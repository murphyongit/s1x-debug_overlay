"""Miners that read the decompiled mission scripts (GSC)."""
from __future__ import annotations

import re
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path

from . import Finding, MineContext

FUNC_RE = re.compile(r"^([A-Za-z_0-9]+)\s*\((.*)\)\s*$")


@dataclass
class Func:
    file: str
    name: str
    start: int          # 1-based line of the header
    lines: list[str]    # body lines (including braces)


# Campaign missions by internal map name (file prefix of their scripts). Longer names first so that
# "sanfran" doesn't swallow "sanfran_b".
MISSIONS = ("seoul", "recovery", "lagos", "fusion", "detroit", "greece", "irons_estate", "betrayal", "crash",
            "lab", "sanfran_b", "sanfran", "df_fly", "captured", "finale")


EXTRA_FILES = {"df_fly": ("df_canyon",)}     # script files that belong to a mission but use another prefix


@lru_cache(maxsize=32)
def _index(scripts_dir: str, prefix: str) -> tuple[Func, ...]:
    out = []
    longer = [m for m in MISSIONS if m != prefix and m.startswith(prefix)]
    files = sorted(Path(scripts_dir).glob(f"{prefix}*.gsc"))
    for extra in EXTRA_FILES.get(prefix, ()):
        files += sorted(Path(scripts_dir).glob(f"{extra}*.gsc"))
    for p in files:
        if any(x in p.name for x in ("_fx.gsc", "_lighting.gsc", "_precache.gsc", "_art.gsc", "_fog")):
            continue
        if any(p.name.startswith(m) for m in longer):
            continue
        lines = p.read_text(encoding="utf-8", errors="replace").split("\n")
        heads = [(i, FUNC_RE.match(l).group(1)) for i, l in enumerate(lines) if FUNC_RE.match(l)]
        heads.append((len(lines), None))
        for (s, name), (e, _) in zip(heads, heads[1:]):
            out.append(Func(p.name, name, s + 1, lines[s + 1:e]))
    return tuple(out)


def _funcs(ctx: MineContext, prefix: str | None = None) -> tuple[Func, ...]:
    if not ctx.scripts_dir:
        return ()
    return _index(str(ctx.scripts_dir), prefix or ctx.map)


def _flags_in(lines, kind: str) -> list[str]:
    pat = re.compile(r"flag_%s\(\s*\"([^\"]+)\"" % kind)
    return [m.group(1) for l in lines for m in pat.finditer(l)]


def _callers(funcs, name: str) -> list[str]:
    pat = re.compile(r"(?:thread\s+|::|\s)" + re.escape(name) + r"\s*\(")
    return sorted({f"{f.name} ({f.file}:{f.start})" for f in funcs for l in f.lines if pat.search(l)})


def end_chains(ctx: MineContext) -> list[Finding]:
    """For the function that ends the mission: which flags it waits on, and what sets each one.

    A flag set by a map trigger is the most interesting case (a 'Trepang2 cube'): touching that trigger may
    end the mission, and if the trigger is switched off it is parked 10000 units down, where touching it
    still works."""
    funcs = _funcs(ctx)
    out: list[Finding] = []
    trig_setters: dict[str, list] = {}
    for e in ctx.map_ents:
        if e.script_flag and e.classname.startswith("trigger"):
            trig_setters.setdefault(e.script_flag, []).append(e)
    for f in funcs:
        idx = [i for i, l in enumerate(f.lines) if re.search(r"\bnextmission\b", l)]
        if not idx:
            continue
        waited = _flags_in(f.lines[:idx[0]], "wait")
        notes = [l.strip() for l in f.lines[:idx[0]] if "waittill" in l or "trigger_wait" in l]
        callers = _callers(funcs, f.name)
        if not waited:
            out.append(Finding(
                "end_chains", "sequence_break", f"Mission ends in {f.name} (no flag waits; follow its callers)", 30.0,
                evidence={"function": f"{f.name} ({f.file}:{f.start})", "started_by": callers[:6],
                          "other_waits": notes[:6],
                          "body": [l.strip()[:120] for l in f.lines[:idx[0] + 1] if l.strip() not in ("{", "}")][:12]},
                test_plan="Trace what starts this function (caller list); anything a touch or prompt reaches early "
                          "is a possible shortcut to the end."))
        for flag in waited:
            setters = []
            player_trigger = []
            for g in funcs:
                for j, l in enumerate(g.lines):
                    if re.search(r"flag_set\(\s*\"%s\"" % re.escape(flag), l) or \
                            (re.search(r"addnotetrack_flag\(", l) and f'"{flag}"' in l):
                        setters.append(f"{g.name} ({g.file}:{g.start + j + 1}): {l.strip()[:120]}")
                        # what that setter waits for before setting it: a touch trigger means the player
                        # can set it just by being somewhere
                        waits = [w.strip()[:120] for w in g.lines[:j] if TOUCH_WAIT_RE.search(w)]
                        if waits:
                            player_trigger.append({"setter": g.name, "waits_for": waits[-3:]})
            trigs = trig_setters.get(flag, [])
            score = 40.0
            title = f"Mission end waits on flag '{flag}' ({f.name})"
            loc = None
            if player_trigger and not trigs:
                score = 70.0
                title = f"Mission end flag '{flag}' is set after a trigger or prompt ({player_trigger[0]['setter']})"
            if trigs:
                score = 95.0
                t = trigs[0]
                o = t.origin
                parked = bool(t.flag_true)
                title = f"END TRIGGER: touching a trigger sets '{flag}', which ends the mission"
                loc = [o[0], o[1], o[2] - (10000 if parked else 0)] if o else None
            out.append(Finding(
                "end_chains", "sequence_break", title, score,
                evidence={"function": f"{f.name} ({f.file}:{f.start})", "started_by": callers[:6],
                          "other_waits": notes[:6], "script_setters": setters[:8],
                          "setter_touch_waits": player_trigger[:4],
                          "trigger_setters": [{"classname": t.classname, "origin": t.origin,
                                               "switched_off_until": t.flag_true or None,
                                               "switched_off_when": t.flag_false or None} for t in trigs]},
                test_plan=("Noclip into the trigger at its parked position (z - 10000) and see if the mission ends."
                           if trigs else "Check what sets this flag; only useful if a trigger or early event sets it."),
                location=loc,
                needs="" if (trigs or not ctx.map_ents) else "map entity file has no trigger for this flag"))
    return out


TOUCH_WAIT_RE = re.compile(r'trigger_wait\w*\(|waittill\(\s*"trigger"|flag_wait\(\s*"[^"]*trig|'
                           r'istouching\(|_touch\w*\(')

SETUP_WAIT_RE = re.compile(r"setup_player_for_scene\s*(?:\(\s*1|,\s*1)")


def delayable_prompts(ctx: MineContext) -> list[Finding]:
    """Scenes that use the waiting version of setup_player_for_scene (stored-teleport candidates).

    Scored higher when the function sets progress flags or notifies BEFORE the wait (the mission moves on
    while you are still free to move) and when a use-trigger starts it (a prompt you can press)."""
    out = []
    for f in _funcs(ctx):
        idx = [i for i, l in enumerate(f.lines) if SETUP_WAIT_RE.search(l)]
        if not idx:
            continue
        k = idx[0]
        before = f.lines[:k]
        pre_flags = _flags_in(before, "set")
        pre_notify = [l.strip() for l in before if "notify(" in l and "endon" not in l]
        prompt = any('waittill( "trigger"' in l or "trigger_wait" in l for l in before)
        link = next((l.strip() for l in f.lines[k:k + 40] if "playerlinkto" in l), "")
        score = 30 + 10 * min(len(pre_flags), 4) + (15 if prompt else 0) + (10 if link else 0)
        out.append(Finding(
            "delayable_prompts", "script_trigger",
            f"Delayable scene '{f.name}'" + (" (starts from a prompt)" if prompt else ""), float(score),
            evidence={"function": f"{f.name} ({f.file}:{f.start})", "flags_set_before_wait": pre_flags,
                      "notifies_before_wait": pre_notify[:6], "snap_to_scene": link},
            test_plan="Melee, press the prompt, then keep the gun from lowering (stomp/jump spam); move away, "
                      "watch whether the flags above take effect while you're free, then let the gun come up."))
    return out


def npc_speed_rules(ctx: MineContext) -> list[Finding]:
    """Script rules that change an NPC's speed depending on where the player is or looks (like Gideon's)."""
    out = []
    rate = re.compile(r"setanimrate|setmovespeedscale|moveplaybackrate|set_moveplaybackrate|run_overrideanim|"
                      r"set_run_speed|setgoalspeed|\.moveplaybackrate")
    cond = re.compile(r"player_looking_at|player_can_see|distance\(|distance2d\(|length\(|within_fov")
    for f in _funcs(ctx):
        body = "\n".join(f.lines)
        if rate.search(body) and cond.search(body) and ("level.player" in body or "player" in f.name):
            hits = [l.strip() for l in f.lines if rate.search(l) or cond.search(l)][:10]
            out.append(Finding("npc_speed_rules", "route", f"NPC speed depends on the player: {f.name}", 55.0,
                               evidence={"function": f"{f.name} ({f.file}:{f.start})", "lines": hits},
                               test_plan="Read the rule, then play the segment the way that makes the NPC fastest."))
    return out


def player_moving_scripts(ctx: MineContext) -> list[Finding]:
    """Every place a script moves or links the player (teleports, scene links, unlinks)."""
    out = []
    pat = re.compile(r"level\.player\s+(setorigin|forceteleport|playerlinkto\w*|unlink)\s*\(")
    for f in _funcs(ctx):
        if re.search(r"(^|_)(start|debug|skipto|startpoint)", f.name):
            continue
        hits = [(f.start + i + 1, l.strip()) for i, l in enumerate(f.lines) if pat.search(l)]
        if not hits:
            continue
        kinds = {pat.search(l).group(1) for _, l in hits}
        score = 25.0 + (10 if "setorigin" in kinds else 0) + (5 if "unlink" in kinds else 0)
        out.append(Finding("player_moving_scripts", "collision_oob", f"Script moves the player: {f.name}", score,
                           evidence={"function": f"{f.name} ({f.file}:{f.start})",
                                     "lines": [f"{n}: {l[:110]}" for n, l in hits[:8]]},
                           test_plan="Check where the player ends up after the unlink; anything that can start "
                                     "from an unexpected spot or leave you somewhere new is worth a test."))
    return out
