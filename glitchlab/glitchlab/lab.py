"""Lab mode: experiments the game runs for us (overlay v1.27+).

The overlay only does this when speedrun_data\\lab\\lab.ini exists. Each queued experiment moves the player to a
spot, sets a velocity, and records every game frame until a time limit or a height limit (so a test never
falls into the end cube or a fail trigger). No keys are pressed by the lab: these are physics/geometry tests
(does this hole really let you fall, where does a launch at this speed land, does the simulator agree).

Files (all in <game>\\speedrun_data\\lab):
  lab.ini        - presence turns lab mode on
  queue.txt      - one experiment per line: id x y z vx vy vz seconds [stop_z]
  result_<id>.txt- "ms x y z vx vy vz pm_flags" per game frame, ending with "END <reason>"
  done.txt       - ids already run (delete a line to run it again)
"""
from __future__ import annotations

import math
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np


@dataclass
class LabExp:
    id: str
    pos: tuple[float, float, float]
    vel: tuple[float, float, float] = (0.0, 0.0, 0.0)
    seconds: float = 2.0
    stop_z: float = -1e9
    note: str = ""

    def line(self) -> str:
        p, v = self.pos, self.vel
        return (f"{self.id} {p[0]:.2f} {p[1]:.2f} {p[2]:.2f} {v[0]:.2f} {v[1]:.2f} {v[2]:.2f} {self.seconds:.2f} "
                f"{self.stop_z:.1f}")


@dataclass
class LabResult:
    id: str
    header: str
    ms: np.ndarray            # game ms since start
    pos: np.ndarray           # (n, 3)
    vel: np.ndarray           # (n, 3)
    flags: np.ndarray
    end: str = ""

    @property
    def final(self) -> np.ndarray:
        return self.pos[-1] if len(self.pos) else np.full(3, np.nan)


def write_queue(lab_dir: Path, exps: list[LabExp], append: bool = True) -> Path:
    lab_dir.mkdir(parents=True, exist_ok=True)
    q = lab_dir / "queue.txt"
    with open(q, "a" if append else "w") as f:
        for e in exps:
            if e.note:
                f.write(f"# {e.id}: {e.note}\n")
            f.write(e.line() + "\n")
    return q


def read_result(path: Path) -> LabResult:
    header, rows, end = "", [], ""
    for line in Path(path).read_text(errors="replace").splitlines():
        if line.startswith("#"):
            header = line
        elif line.startswith("END"):
            end = line[3:].strip()
        else:
            p = line.split()
            if len(p) >= 8:
                rows.append([float(v) for v in p[:7]] + [int(p[7], 16)])
    a = np.array(rows) if rows else np.zeros((0, 8))
    rid = Path(path).stem[len("result_"):]
    return LabResult(rid, header, a[:, 0], a[:, 1:4], a[:, 4:7], a[:, 7].astype(int), end)


def read_results(lab_dir: Path) -> dict[str, LabResult]:
    return {r.id: r for r in (read_result(p) for p in sorted(Path(lab_dir).glob("result_*.txt")))}


# ------------------------------------------------------------------ plans
def plan_holes(points, prefix="hole", above=48.0, seconds=2.0) -> list[LabExp]:
    """Drop straight down from just above each spot: did the player fall through (no floor) or land?"""
    return [LabExp(f"{prefix}{k:02d}", (x, y, z + above), (0, 0, 0), seconds, stop_z=-900.0,
                   note=f"drop test at {x:.0f} {y:.0f}") for k, (x, y, z) in enumerate(points)]


def plan_launches(points, target, speeds=(266.0, 380.0, 600.0), jump=249.8, seconds=6.0, stop_z=-9000.0,
                  prefix="launch") -> list[LabExp]:
    """Launch from each spot toward a target at several speeds (with a jump's upward speed), no keys held.
    Stops at stop_z so it never reaches parked triggers lower down (end cube, fail boxes near -9840)."""
    out = []
    for k, (x, y, z) in enumerate(points):
        a = math.atan2(target[1] - y, target[0] - x)
        for s in speeds:
            out.append(LabExp(f"{prefix}{k:02d}_{int(s)}", (x, y, z + 2), (s * math.cos(a), s * math.sin(a), jump),
                              seconds, stop_z, note=f"launch at {s:.0f} toward {target[0]:.0f} {target[1]:.0f}"))
    return out


def plan_friction(spot, directions=(0, 90, 180, 270), speed=400.0, prefix="slide") -> list[LabExp]:
    """Ground physics check: start standing on flat ground already moving at 400 u/s, no keys: friction alone
    should stop you in a predictable distance (simulator: about 64 units from 400)."""
    x, y, z = spot
    return [LabExp(f"{prefix}{d:03d}", (x, y, z + 0.25), (speed * math.cos(math.radians(d)),
                                                          speed * math.sin(math.radians(d)), 0.0), 1.5,
                   note=f"friction stop from {speed:.0f} heading {d}") for d in directions]


# ------------------------------------------------------------------ comparison with the simulator
def simulate(exp: LabExp, world, params=None, frame_ms: int = 4):
    """Same start state in the simulator, no keys. Returns (ms, positions)."""
    from .sim.params import Params
    from .sim.pmove import Cmd, Mover, State
    P = params or Params()
    m = Mover(world, P)
    st = State(np.array(exp.pos, float), np.array(exp.vel, float))
    m.ground_trace(st)
    ms, pos = [0], [st.origin.copy()]
    t = 0
    while t < exp.seconds * 1000 and st.origin[2] >= exp.stop_z:
        m.step(st, Cmd(msec=frame_ms))
        t += frame_ms
        ms.append(t)
        pos.append(st.origin.copy())
    return np.array(ms), np.array(pos)


def compare(result: LabResult, exp: LabExp, world, params=None) -> dict:
    sm, sp = simulate(exp, world, params)
    if not len(result.ms):
        return {"id": exp.id, "error": "no samples"}
    sim_at = np.array([sp[min(np.searchsorted(sm, t), len(sp) - 1)] for t in result.ms])
    err = np.linalg.norm(sim_at - result.pos, axis=1)
    return {"id": exp.id, "game_end": result.final.round(1).tolist(), "sim_end": sim_at[-1].round(1).tolist(),
            "end_error": round(float(err[-1]), 1), "max_error": round(float(err.max()), 1),
            "fell_through": bool(result.final[2] < exp.pos[2] - 300), "sim_fell_through": bool(sim_at[-1][2] < exp.pos[2] - 300),
            "frames": int(len(result.ms)), "end": result.end}
