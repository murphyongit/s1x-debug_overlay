"""Command line: import recorder logs, run the static miners, write a report.

    python -m glitchlab.cli mine   --config config.json --map recovery
    python -m glitchlab.cli import --config config.json --map recovery
    python -m glitchlab.cli report --config config.json --map recovery
"""
from __future__ import annotations

import argparse
import json
import logging
import time
from pathlib import Path

from . import data as D
from .db import Store
from .miners import MineContext, all_miners, script_miners
from .miners.scripts import MISSIONS

log = logging.getLogger("glitchlab")


def load_config(path: str | Path) -> dict:
    cfg = json.loads(Path(path).read_text())
    cfg.setdefault("db", "glitchlab.sqlite")
    return cfg


def build_context(cfg: dict, map_name: str, current_map: bool = True) -> MineContext:
    """current_map: the game folder's map_ents_0.txt belongs to this map (it is rewritten for whichever
    mission was loaded last), so only use it when mining the mission you're playing; other missions need
    an explicit "map_ents": {"<map>": "<file>"} entry in the config."""
    game = Path(cfg["game_dir"])
    sd = game / "speedrun_data"
    ctx = MineContext(map=map_name)
    ents = cfg.get("map_ents", {}).get(map_name) or (game / "map_ents_0.txt" if current_map else None)
    if ents and Path(ents).exists():
        ctx.map_ents = D.load_map_ents(ents)
    logs = sorted(sd.glob(f"{map_name}_*.txt"))
    limit = cfg.get("max_logs", 12)
    for p in logs[-limit:]:
        try:
            ctx.logs.append(D.load_run_log(p))
        except Exception as e:  # a corrupt log shouldn't stop the miners
            log.warning("skipping %s: %s", p.name, e)
    b = sd / f"brushes_{map_name}.txt"
    if b.exists():
        ctx.walls = D.load_brushes_txt(b)
    full = sd / f"collision_full_{map_name}.bin"
    if full.exists():
        ctx.full = D.load_collision_full(full)
    mv = sd / f"movement_{map_name}.txt"
    if mv.exists():
        ctx.movement = D.load_movement(mv)
    if cfg.get("scripts_dir"):
        ctx.scripts_dir = Path(cfg["scripts_dir"])
    ctx.known_spots = cfg.get("known_spots", {}).get(map_name, {})
    return ctx


def cmd_mine(cfg, map_name, store: Store, miners=None, current_map=True) -> list:
    ctx = build_context(cfg, map_name, current_map)
    log.info("context: %d map entities, %d logs, %d wall pieces, full collision %s, scripts %s",
             len(ctx.map_ents), len(ctx.logs), len(ctx.walls), "yes" if ctx.full else "no",
             ctx.scripts_dir or "none")
    exp = store.add_experiment(map_name, "static_miner", hypothesis="static scan of saved game data",
                               start_conditions={"logs": [l.path.name for l in ctx.logs],
                                                 "full_collision": ctx.full is not None})
    found = []
    for miner in miners or all_miners():
        t0 = time.time()
        try:
            res = miner(ctx)
        except Exception as e:
            log.exception("miner %s failed", miner.__name__)
            store.add_observations(exp, [(None, "miner_error", {"miner": miner.__name__, "error": str(e)})])
            continue
        log.info("%-24s %4d findings (%.1fs)", miner.__name__, len(res), time.time() - t0)
        for f in res:
            did = store.upsert_discovery(map_name, f.category, f.title, f.fingerprint, score=f.score,
                                         evidence={**f.evidence, "miner": f.miner, "needs": f.needs},
                                         test_plan=f.test_plan, location=f.location)
            store.link(did, exp)
        found += res
    store.set_outcome(exp, "positive" if found else "negative", notes=f"{len(found)} findings")
    return found


def write_report(store: Store, map_name: str, path: Path, top: int = 15) -> None:
    rows = store.discoveries(map_name)
    by = {}
    for r in rows:
        by.setdefault(r["evidence"].get("miner", "?") if r["evidence"] else "?", []).append(r)
    lines = [f"# Glitch miner report: {map_name}", "",
             f"{len(rows)} candidates. Scores are rough priorities for testing, not proof that anything works.", ""]
    order = sorted(by, key=lambda m: -max(r["score"] or 0 for r in by[m]))
    for m in order:
        lines += [f"## {m} ({len(by[m])})", ""]
        for r in by[m][:top]:
            loc = " at " + " ".join(f"{v:.0f}" for v in r["location"]) if r["location"] else ""
            lines.append(f"- **{r['score']:.0f}** {r['title']}{loc}  [{r['stage']}]")
            ev = {k: v for k, v in (r["evidence"] or {}).items() if k not in ("miner",) and v not in (None, "", [], {})}
            if ev:
                lines.append(f"  - evidence: `{json.dumps(ev)[:600]}`")
            if r["test_plan"]:
                lines.append(f"  - test: {r['test_plan']}")
        lines.append("")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines))


def cmd_import(cfg, map_name, store: Store) -> int:
    """Each recorder log becomes one experiment (source recorder_log) with sampled positions + events."""
    sd = Path(cfg["game_dir"]) / "speedrun_data"
    n = 0
    for p in sorted(sd.glob(f"{map_name}_*.txt")):
        if store.con.execute("SELECT 1 FROM experiments WHERE source='recorder_log' AND notes=?", (p.name,)).fetchone():
            continue
        lg = D.load_run_log(p)
        eid = store.add_experiment(map_name, "recorder_log", tool_version=lg.tool_version,
                                   hypothesis="human play session", outcome="inconclusive", notes=p.name)
        obs = [(s.t, "position", [s.x, s.y, s.z, s.pitch, s.yaw, s.game_ms]) for s in lg.samples[::5]]
        obs += [(t, k.lower(), r[:200]) for t, k, r in lg.events if k in ("SNOTIFY", "ENTER", "EXIT", "RELOAD")]
        store.add_observations(eid, obs)
        n += 1
    return n


def cmd_simcheck(cfg, map_name, out: Path, variants: bool = True) -> dict:
    """Checks the movement simulator against this map's recorded runs and writes a report."""
    from dataclasses import replace
    from .sim import validate as V
    from .sim.params import Params
    sd = Path(cfg["game_dir"]) / "speedrun_data"
    windows = cfg.get("clean_windows", {})
    runs = []
    for p in sorted(sd.glob(f"{map_name}_*.txt")):
        lg = D.load_run_log(p)
        if sum(1 for s in lg.samples if s.game_ms is not None) < 200:
            continue                                   # older recordings without the game clock
        runs.append((lg, windows.get(p.name)))
    logs = [r[0] for r in runs]
    P = Params()
    from .sim.reach import fly
    res = {"jump_arcs": V.fit_jump_arcs(logs), "steady_speeds": V.steady_speeds(logs),
           "dash": V.dash_profile(logs),
           "params": P.as_dict(), "runs": [lg.path.name for lg in logs]}
    var = {"game values": P}
    if variants:
        var.update({"air control x2": replace(P, air_accel=2.0), "air control x0.5": replace(P, air_accel=0.5),
                    "friction 8 (not 5.5)": replace(P, friction=8.0), "friction 4": replace(P, friction=4.0),
                    "ground accel 6 (not 9)": replace(P, accel_stand=6.0),
                    "ground accel 12": replace(P, accel_stand=12.0),
                    "gravity 700": replace(P, gravity=700.0), "run speed 210": replace(P, speed=210.0)})
    res["replay"] = V.compare_params(runs, var)
    lines = [f"# Movement simulator check: {map_name}", "",
             f"Recordings: {', '.join(res['runs'])}", "",
             "## Constants measured from the recordings", "",
             f"- Jump arcs fitted: {res['jump_arcs'].get('arcs')}; gravity {res['jump_arcs'].get('gravity_median')} "
             f"(middle half {res['jump_arcs'].get('gravity_iqr')}); jump apex {res['jump_arcs'].get('apex_median')}",
             *(f"- Steady {k}: {v['median']} u/s ({v['samples']} samples)" for k, v in res["steady_speeds"].items()),
             f"- Air dashes: {res['dash']}",
             "", "## Jump distances (simulated, open air, flat takeoff)", "",
             "| takeoff speed | land at | straight (hold W) | strafing (A/D + mouse) | speed on landing (strafing) |",
             "|---|---|---|---|---|",
             *(f"| {v} | {'same height' if d == 0 else (f'{-d} higher' if d < 0 else f'{d} lower')} | "
               f"{fly(v, d, strafe=False)['distance']} | {fly(v, d)['distance']} | {fly(v, d)['end_speed']} |"
               for v in (190, 266, 340) for d in (-30, 0, 150, 300)),
             "", "## Replay (0.1 s at a time, best key combination each time)", "",
             "Error = how far the simulated player is from the recorded spot 0.1 s later. Lower is better.", "",
             "| Physics | flat ground: median / 90% / share within 2 u | in the air: median / 90% / share within 5 u |",
             "|---|---|---|"]
    for k, v in res["replay"].items():
        f, a = v["flat"] or {}, v["air"] or {}
        lines.append(f"| {k} | {f.get('median')} / {f.get('p90')} / {f.get('within_2')} (n={f.get('n')}) | "
                     f"{a.get('median')} / {a.get('p90')} / {a.get('within_5')} (n={a.get('n')}) |")
    lines += ["", "## Simulator constants", "", "```", json.dumps(res["params"], indent=1), "```"]
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines))
    return res


def main(argv=None):
    ap = argparse.ArgumentParser(prog="glitchlab")
    ap.add_argument("command", choices=["mine", "import", "report", "scripts", "simcheck"])
    ap.add_argument("--config", default="config.json")
    ap.add_argument("--map", help="internal map name, e.g. recovery (Atlas), seoul (Induction)")
    ap.add_argument("--all", action="store_true", help="scripts: run the script miners for every mission")
    ap.add_argument("--out", default=None)
    a = ap.parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    cfg = load_config(a.config)
    store = Store(cfg["db"])
    if a.command == "scripts":
        maps = MISSIONS if a.all else [a.map]
        out_dir = Path(a.out or "reports")
        for m in maps:
            found = cmd_mine(cfg, m, store, miners=script_miners(), current_map=False)
            write_report(store, m, out_dir / f"{m}_miner_report.md")
            print(f"{m}: {len(found)} findings")
        store.close()
        return
    if not a.map:
        ap.error("--map is required")
    if a.command == "simcheck":
        cmd_simcheck(cfg, a.map, Path(a.out or f"reports/{a.map}_sim_check.md"))
        print("simulator check written")
        store.close()
        return
    if a.command == "mine":
        found = cmd_mine(cfg, a.map, store)
        write_report(store, a.map, Path(a.out or f"reports/{a.map}_miner_report.md"))
        print(f"{len(found)} findings; report written")
    elif a.command == "import":
        print(f"imported {cmd_import(cfg, a.map, store)} logs")
    else:
        write_report(store, a.map, Path(a.out or f"reports/{a.map}_miner_report.md"))
    store.close()


if __name__ == "__main__":
    main()
