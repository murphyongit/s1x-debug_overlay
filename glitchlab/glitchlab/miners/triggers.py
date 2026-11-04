"""Miners over map triggers (map entity text + trigger sizes from recorder logs)."""
from __future__ import annotations

import math

from . import Finding, MineContext
from ..data import PARK_OFFSET

FAIL_WORDS = ("left_encounter", "fail", "missionfail", "kill", "death", "out_of_bounds")


def _size_for(ctx: MineContext, origin, parked: bool):
    sizes = ctx.trigger_sizes()
    if not origin:
        return None
    key = (round(origin[0]), round(origin[1]))
    if key in sizes:
        return sizes[key]
    best = None
    for (x, y), s in sizes.items():
        d = abs(x - origin[0]) + abs(y - origin[1])
        if d <= 2 and (best is None or d < best[0]):
            best = (d, s)
    return best[1] if best else None


GRAVITY = 800.0
PLAYER_H = 72.0
PLAYER_HALF_W = 16.0


def void_fall_check(void, centre, size) -> dict:
    """What a straight fall from the void entry into a parked trigger box needs.

    * time to reach the top of the box and the fall speed there (gravity 800, no air drag assumed)
    * the sideways speed needed to be over the box by then (distance to its nearest edge / time)
    * the lowest FPS at which one frame's drop still overlaps the box (drop per frame < box height + 72);
      below that FPS you can fall straight through it without it firing"""
    top = centre[2] + size[2] / 2
    drop = void[2] - top
    if drop <= 0:
        return {"note": "box top is above the void entry"}
    t = math.sqrt(2 * drop / GRAVITY)
    v = GRAVITY * t
    dx = max(0.0, abs(centre[0] - void[0]) - size[0] / 2 - PLAYER_HALF_W)
    dy = max(0.0, abs(centre[1] - void[1]) - size[1] / 2 - PLAYER_HALF_W)
    side = math.hypot(dx, dy)
    return {"seconds_to_box_top": round(t, 2), "fall_speed_at_box": round(v),
            "sideways_units_to_box_edge": round(side), "sideways_speed_needed": round(side / t),
            "min_fps_to_register": math.ceil(v / (size[2] + PLAYER_H))}


def parked_triggers(ctx: MineContext) -> list[Finding]:
    """Triggers switched off at mission start (script_flag_true). The game parks them 10000 units down; touching
    them there still sets their flag and skips the condition. Ranked by what the flag does."""
    out = []
    void = ctx.known_spots.get("void_entry")
    for e in ctx.map_ents:
        if not e.classname.startswith("trigger") or not e.flag_true or not e.script_flag:
            continue
        o = e.origin
        if not o:
            continue
        parked = (o[0], o[1], o[2] - PARK_OFFSET)
        size = _size_for(ctx, o, True)
        flag = e.script_flag
        score = 30.0
        why = "progress flag"
        if flag in ("outro_start",) or "end" in flag and "mission" in flag:
            score, why = 98.0, "ends the mission"
        elif any(w in flag for w in FAIL_WORDS):
            score, why = 5.0, "fail / warning flag (avoid)"
        elif any(w in flag for w in ("complete", "enter_helicopter", "elevator", "start", "obj_")):
            score, why = 50.0, "objective / progression flag"
        horiz = None
        fall = None
        if void:
            horiz = math.hypot(parked[0] - void[0], parked[1] - void[1])
            if score > 20 and horiz < 2500:
                score += 10
            if size:
                fall = void_fall_check(void, parked, size)
        out.append(Finding(
            "parked_triggers", "script_trigger",
            f"Hidden switched-off trigger sets '{flag}' ({why})", score,
            evidence={"classname": e.classname, "real_position": o, "parked_position": parked,
                      "box_size": size, "switched_on_by_flag": e.flag_true, "switched_off_by_flag": e.flag_false or None,
                      "sideways_distance_from_void_entry": round(horiz) if horiz is not None else None,
                      "void_fall": fall},
            test_plan=f"Noclip to {parked[0]:.0f} {parked[1]:.0f} {parked[2]:.0f} and check that '{flag}' fires "
                      "(recorder SNOTIFY). Then look for a legit fall into the void that passes through it.",
            location=list(parked)))
    return out


def fail_trigger_thickness(ctx: MineContext) -> list[Finding]:
    """Fail / out-of-bounds triggers and the speed needed to skip them with the low-FPS trick.

    A trigger is missed only if one frame's movement exceeds (thickness + 32). Uses the box's smallest
    horizontal side (an upper bound on thickness: rotated triggers can be thinner)."""
    out = []
    seen = set()
    for log in ctx.logs:
        for t in log.triggers.values():
            if not t.size or not any(w in t.flags for w in ("left_encounter", "board_fail", "out_of_bounds")):
                continue
            key = (round(t.mid[0]), round(t.mid[1]), round(t.mid[2]))
            if key in seen:
                continue
            seen.add(key)
            thick = min(t.size[0], t.size[1])
            need = {fps: round((thick + 32) * fps) for fps in (1, 2, 5, 10)}
            warn = "out_of_bounds_warning" in t.flags and "left_encounter" not in t.flags
            score = (60 if thick < 80 else 35 if thick < 250 else 15) - (20 if warn else 0)
            out.append(Finding(
                "fail_trigger_thickness", "script_trigger",
                f"{'Warning' if warn else 'Fail'} box #{t.slot} is at most {thick:.0f} thick", float(max(score, 1)),
                evidence={"flags": t.flags, "box_mid": t.mid, "box_size": t.size,
                          "speed_needed_by_fps": need},
                test_plan=f"At 1 FPS you need about {need[1]} u/s across it (5 FPS: {need[5]}); pass through it "
                          "on its thin side and check the recorder for an ENTER line.",
                location=list(t.mid)))
    return out
