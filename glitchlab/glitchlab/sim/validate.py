"""Checks the simulator against recorded runs.

The recorder saves position, view angles, movement flags and the game clock about 10 times a second, but not
the keys pressed. So the check works like this:

1. Constants straight from the logs: gravity and jump apex (fitted on clean jump arcs), steady run, sprint
   and crouch speeds.
2. Replay: walk through each recording 0.1 s at a time. For every gap between two samples, try each key
   combination a keyboard player could hold (forward/back/none x left/right/none, plus a jump press), with
   the view turning smoothly between the two recorded angles. Keep the combination that lands closest to
   the next recorded spot and carry the simulated speed forward. If the simulator's physics are right,
   some key combination should land within a few units nearly every time. With wrong physics (say air
   control twice as strong) the best combination still misses, and the errors grow.
   The floor is taken from the recording (flat at the lower of the two heights), so only flat ground and
   jumps/falls are scored; slopes, stairs, ladders, slides, cutscenes and teleports are skipped.
3. The same replay with deliberately wrong constants, to show the game's values fit best.
"""
from __future__ import annotations

import itertools
import math
from dataclasses import dataclass, replace

import numpy as np

from ..data import RunLog, Sample
from .params import Params
from .pmove import CROUCH, PRONE, STAND, Cmd, Mover, State
from .world import FlatWorld


# ------------------------------------------------------------------ 1. constants from logs
def fit_jump_arcs(logs: list[RunLog]) -> dict:
    fits = []
    for lg in logs:
        S = [s for s in lg.samples if s.game_ms is not None]
        t = np.array([s.game_ms / 1000 for s in S])
        z = np.array([s.z for s in S])
        i = 2
        while i < len(S) - 15:
            dt = t[i] - t[i - 1]
            if not (0.07 < dt < 0.13) or abs(z[i - 1] - z[i - 2]) > 0.6 or z[i] - z[i - 1] < 4:
                i += 1
                continue
            z0 = z[i - 1]
            j = i
            while j < len(S) and z[j] > z0 + 0.5 and j - i < 20 and 0.07 < t[j] - t[j - 1] < 0.13:
                j += 1
            if j - i >= 5 and j < len(S) and abs(z[j] - z0) < 3:
                tt = t[i:j] - t[i - 1]
                zz = z[i:j] - z0
                A = np.vstack([np.ones_like(tt), tt, -0.5 * tt ** 2]).T
                c, v0, g = np.linalg.lstsq(A, zz, rcond=None)[0]
                if np.abs(A @ np.array([c, v0, g]) - zz).max() < 1.5:
                    fits.append((g, c + v0 ** 2 / (2 * g)))
            i = max(j, i + 1)
    if not fits:
        return {"arcs": 0}
    f = np.array(fits)
    return {"arcs": len(f), "gravity_median": round(float(np.median(f[:, 0])), 1),
            "gravity_iqr": [round(float(v), 1) for v in np.percentile(f[:, 0], [25, 75])],
            "apex_median": round(float(np.median(f[:, 1])), 2)}


def steady_speeds(logs: list[RunLog]) -> dict:
    groups: dict[str, list[float]] = {"run": [], "sprint": [], "crouch": []}
    for lg in logs:
        S = [s for s in lg.samples if s.game_ms is not None]
        for a, b, c in zip(S, S[1:], S[2:]):
            d1 = (b.game_ms - a.game_ms) / 1000
            d2 = (c.game_ms - b.game_ms) / 1000
            if not (0.07 < d1 < 0.13 and 0.07 < d2 < 0.13) or abs(b.z - a.z) > 0.3 or abs(c.z - b.z) > 0.3:
                continue
            v1 = math.hypot(b.x - a.x, b.y - a.y) / d1
            v2 = math.hypot(c.x - b.x, c.y - b.y) / d2
            if abs(v1 - v2) > 1:
                continue
            if b.stance & 0x4 and 100 < v2 < 140:
                groups["crouch"].append(v2)
            elif not (b.stance & 0xC):
                if b.flags & 0x4000 and 250 < v2 < 285:
                    groups["sprint"].append(v2)
                elif not b.flags & 0x4000 and 180 < v2 < 200:
                    groups["run"].append(v2)
    return {k: {"samples": len(v), "median": round(float(np.median(v)), 1) if v else None}
            for k, v in groups.items()}


# ------------------------------------------------------------------ 2. replay with key inference
@dataclass
class ReplayStats:
    intervals: int = 0
    scored_flat: int = 0
    scored_air: int = 0
    skipped: int = 0
    resyncs: int = 0
    err_flat: list = None
    err_air: list = None

    def summary(self) -> dict:
        def s(v):
            if not v:
                return None
            a = np.array(v)
            return {"n": len(a), "median": round(float(np.median(a)), 2), "p90": round(float(np.percentile(a, 90)), 2),
                    "within_2": round(float(np.mean(a < 2)), 3), "within_5": round(float(np.mean(a < 5)), 3)}
        return {"intervals": self.intervals, "flat": s(self.err_flat), "air": s(self.err_air),
                "skipped": self.skipped, "resyncs": self.resyncs}


def _stance(s: Sample) -> int:
    if s.stance & 0x8:
        return PRONE
    if s.stance & 0x4:
        return CROUCH
    return STAND


def _yaw_lerp(a: float, b: float, f: float) -> float:
    d = (b - a + 180.0) % 360.0 - 180.0
    return a + d * f


UNMODELLED_FLAGS = 0x10 | 0x800 | 0x8 | 0x100000    # aiming down sights, frozen, ladder, sliding


def unmodelled(s: Sample) -> bool:
    """Movement the simulator doesn't cover yet: ADS, frozen, ladder, slide, crouch/prone, scripted states."""
    return bool(s.flags & UNMODELLED_FLAGS) or bool(s.stance & 0xC) or bool(s.stance & ~0xF)


def _exo_burst(p0: Sample, a: Sample, b: Sample) -> bool:
    """A sideways speed change no key can make (exo dash, knockback, being moved by script).

    Normal air control adds at most ~190 u/s per second (air accel 1 x wishspeed 190); ground control at
    most ~2400 plus friction. Anything far beyond that is something the simulator doesn't model."""
    d0 = max((a.game_ms - p0.game_ms) / 1000, 1e-3)
    d1 = max((b.game_ms - a.game_ms) / 1000, 1e-3)
    v0 = np.array([a.x - p0.x, a.y - p0.y]) / d0
    v1 = np.array([b.x - a.x, b.y - a.y]) / d1
    acc = float(np.linalg.norm(v1 - v0)) / ((d0 + d1) / 2)
    airborne = abs(a.z - p0.z) > 0.3 or abs(b.z - a.z) > 0.3
    return np.linalg.norm(v1) > 420 or np.linalg.norm(v0) > 420 or acc > (600 if airborne else 5000)


def _burst_mask(S: list[Sample], pad: int = 3) -> list[bool]:
    """True near any exo burst (a few samples either side, since a dash lasts a while)."""
    m = [False] * len(S)
    for i in range(1, len(S) - 1):
        if _exo_burst(S[i - 1], S[i], S[i + 1]):
            for k in range(max(0, i - pad), min(len(S), i + pad + 1)):
                m[k] = True
    return m


KEYS = [(f, r) for f in (127, 0, -127) for r in (127, 0, -127)]


def _sim_interval(mover: Mover, st: State, a: Sample, b: Sample, keys, jump: bool, frame_ms: int) -> State:
    s = st.copy()
    total = b.game_ms - a.game_ms
    n = max(1, round(total / frame_ms))
    msec = total / n
    sprint = bool(a.flags & 0x4000)
    stance = _stance(a)
    for k in range(n):
        yaw = _yaw_lerp(a.yaw, b.yaw, (k + 1) / n)
        if jump:
            s.jump_held = False        # jump pressed every frame (like a scroll-wheel bind): jumps on landing
        c = Cmd(keys[0], keys[1], yaw, jump=jump, sprint=sprint, stance=stance, msec=int(round(msec)))
        mover.step(s, c)
    return s


def replay(log: RunLog, params: Params | None = None, frame_ms: int = 8, max_intervals: int | None = None,
           start: int = 0, windows: list[tuple[float, float]] | None = None, trace: list | None = None,
           max_scored: int | None = None) -> ReplayStats:
    """windows: only score these (log time start, end) ranges, e.g. the parts of a run without noclip.
    trace: if a list is given, (t, error, keys, jump, category) is appended for every scored interval."""
    P = replace(params or Params(), frame_ms=frame_ms)
    mover = Mover(FlatWorld(0.0), P)
    S = [s for s in log.samples if s.game_ms is not None]
    st: State | None = None
    stats = ReplayStats(err_flat=[], err_air=[])
    burst = _burst_mask(S)
    prev: Sample | None = None
    for i in range(max(1, start), len(S) - 1):
        if max_intervals and stats.intervals >= max_intervals:
            break
        if max_scored and len(stats.err_flat) + len(stats.err_air) >= max_scored:
            break
        a, b = S[i], S[i + 1]
        p0 = S[i - 1]
        if windows and not any(w0 <= p0.t and b.t <= w1 for w0, w1 in windows):
            st = None
            continue
        dt = (b.game_ms - a.game_ms) / 1000
        dt0 = (a.game_ms - p0.game_ms) / 1000
        stats.intervals += 1
        jumpy = math.dist((a.x, a.y, a.z), (b.x, b.y, b.z)) / max(dt, 1e-3) > 900
        if not (0.07 < dt < 0.16) or not (0.07 < dt0 < 0.16) or jumpy or abs(b.z - a.z) > 60 \
                or any(unmodelled(x) for x in (p0, a, b)) or burst[i] or burst[i + 1]:
            stats.skipped += 1
            st = None
            continue
        dz_prev, dz = a.z - p0.z, b.z - a.z
        flat = abs(dz_prev) < 0.3 and abs(dz) < 0.3
        # air: part of a falling/rising curve (vertical speed changes about like gravity says)
        acc = (dz / dt - dz_prev / dt0) / ((dt + dt0) / 2)
        air = abs(dz) > 0.3 and -1100 < acc < -500
        if st is None:
            # velocity at sample a from the two gaps before it (never from b, the sample being predicted):
            # second-order backward difference sideways; vertically the last gap plus half a gap of gravity
            vz = (a.z - p0.z) / dt0
            if abs(dz_prev) >= 0.3:
                vz -= P.gravity * dt0 / 2
            vx, vy = (a.x - p0.x) / dt0, (a.y - p0.y) / dt0
            pm = S[i - 2] if i >= 2 else None
            if pm is not None and 0.07 < (p0.game_ms - pm.game_ms) / 1000 < 0.16:
                d1 = (p0.game_ms - pm.game_ms) / 1000
                vx += ((a.x - p0.x) / dt0 - (p0.x - pm.x) / d1) * dt0 / (dt0 + d1)
                vy += ((a.y - p0.y) / dt0 - (p0.y - pm.y) / d1) * dt0 / (dt0 + d1)
            st = State(np.array([a.x, a.y, a.z]), np.array([vx, vy, vz]))
            st.walking = abs(dz_prev) < 0.3
            st.ground_normal = np.array([0.0, 0.0, 1.0]) if st.walking else None
            st.jump_held = False
            stats.resyncs += 1
        target = np.array([b.x, b.y, b.z])
        best = None
        floors = [min(a.z, b.z)]
        # a hop inside this gap (fell, landed somewhere lower than both samples, jumped again): the landing
        # height isn't recorded, so try a few
        bounce = not st.walking and st.velocity[2] < 0 and dz > -st.velocity[2] * dt * 0.5
        if bounce:
            floors += [min(a.z, b.z) - k for k in (4, 8, 12, 18, 26, 36)]
        for fz in floors:
            mover.world = FlatWorld(fz)
            for keys, jump in itertools.product(KEYS, (False, True)):
                if jump and not ((st.walking and dz > 2) or bounce):
                    continue
                s2 = _sim_interval(mover, st, a, b, keys, jump, frame_ms)
                e = float(np.linalg.norm(s2.origin - target))
                if best is None or e < best[0]:
                    best = (e, s2)
        if bounce and best[1].velocity[2] > 0:
            air = True
        err, st = best
        if trace is not None and (flat or air):
            trace.append((b.t, round(err, 2), "flat" if flat else "air"))
        if flat:
            stats.err_flat.append(err)
        elif air:
            stats.err_air.append(err)
        else:
            stats.skipped += 1
        if err > 24 or not (flat or air):
            st = None          # lost track (or terrain we can't model with a flat floor): restart here
    return stats


def compare_params(runs: list[tuple[RunLog, list[tuple[float, float]] | None]], variants: dict[str, Params],
                   frame_ms: int = 8, max_scored: int = 800) -> dict:
    """runs: (log, windows) pairs; windows None = whole log."""
    out = {}
    for name, P in variants.items():
        flat, air, rs = [], [], 0
        for lg, win in runs:
            st = replay(lg, P, frame_ms, windows=win, max_scored=max_scored)
            flat += st.err_flat
            air += st.err_air
            rs += st.resyncs
        out[name] = ReplayStats(err_flat=flat, err_air=air, resyncs=rs).summary()
    return out


# ------------------------------------------------------------------ 4. exo dash, measured (not simulated yet)
def dash_profile(logs: list[RunLog]) -> dict:
    """Air dashes in the recordings: speed before, peak, and speed once the dash is over (still airborne).

    A dash is a jump in sideways speed to over 450 from under 400 in one 0.1 s sample."""
    pre, peak, after = [], [], []
    for lg in logs:
        S = [s for s in lg.samples if s.game_ms is not None]
        sp = []
        for a, b in zip(S, S[1:]):
            dt = (b.game_ms - a.game_ms) / 1000
            sp.append((math.hypot(b.x - a.x, b.y - a.y) / dt if 0.07 < dt < 0.13 else None, b.z - a.z))
        i = 2
        while i < len(sp) - 6:
            v = sp[i][0]
            if v and 450 < v < 1000 and sp[i - 1][0] and sp[i - 1][0] < 400:
                j = i
                while j < len(sp) - 1 and sp[j][0] and sp[j][0] > 420:
                    j += 1
                if j + 2 < len(sp) and all(x[0] for x in sp[j:j + 3]) and sp[j + 1][1] < -1 and sp[j + 2][1] < -1 \
                        and abs(sp[j + 1][0] - sp[j + 2][0]) < 6:
                    pre.append(sp[i - 1][0])
                    peak.append(max(x[0] for x in sp[i:j]))
                    after.append(sp[j + 2][0])
                i = j
            i += 1
    if not after:
        return {"dashes": 0}
    a = np.array(after)
    return {"dashes": len(a), "speed_before_median": round(float(np.median(pre))),
            "peak_median": round(float(np.median(peak))), "speed_after_median": round(float(np.median(a))),
            "share_ending_at_235_245": round(float(np.mean((a > 235) & (a < 245))), 2)}


# ------------------------------------------------------------------ 5. air control: observed speed gain vs the limit
def air_gain_check(logs: list[RunLog], accels=(0.5, 1.0, 2.0)) -> list[dict]:
    """How fast players gained sideways speed in the air (0.2 s windows on one jump arc, no dashes), next to
    the most the simulator allows with perfect strafing for different air-control strengths.

    Strafe jumpers push right up against the true limit, so the top of the observed gains should sit just at
    the correct value and never clearly above it."""
    from .reach import fly
    obs = []
    for lg in logs:
        S = [s for s in lg.samples if s.game_ms is not None]
        burst = _burst_mask(S)
        for i in range(2, len(S) - 2):
            p, a, b, c = S[i - 1], S[i], S[i + 1], S[i + 2]
            if burst[i] or burst[i + 1] or any(unmodelled(x) for x in (p, a, b, c)):
                continue
            d0, d1, d2 = ((y.game_ms - x.game_ms) / 1000 for x, y in ((p, a), (a, b), (b, c)))
            if not all(0.07 < d < 0.13 for d in (d0, d1, d2)):
                continue
            a1 = ((b.z - a.z) / d1 - (a.z - p.z) / d0) / ((d0 + d1) / 2)
            a2 = ((c.z - b.z) / d2 - (b.z - a.z) / d1) / ((d1 + d2) / 2)
            if not (-900 < a1 < -700 and -900 < a2 < -700):
                continue
            v0 = math.hypot(a.x - p.x, a.y - p.y) / d0
            v2 = math.hypot(c.x - b.x, c.y - b.y) / d2
            if v0 < 200:
                continue
            T = (c.game_ms + b.game_ms) / 2000 - (p.game_ms + a.game_ms) / 2000
            obs.append((v0, (v2 - v0) / T))
    if not obs:
        return []
    o = np.array(obs)
    rows = []
    for lo, hi in ((200, 260), (260, 320), (320, 380), (380, 450)):
        m = (o[:, 0] >= lo) & (o[:, 0] < hi)
        if m.sum() < 5:
            continue
        g = o[m, 1]
        mid = (lo + hi) / 2
        lim = {}
        for acc in accels:
            r = fly(mid, 1e6, jump=False, strafe=True, P=replace(Params(), air_accel=acc), max_seconds=0.2)
            lim[acc] = round((r["end_speed"] - mid) / 0.2)
        rows.append({"speed": f"{lo}-{hi}", "windows": int(m.sum()), "observed_p98": round(float(np.percentile(g, 98))),
                     "observed_max": round(float(g.max())), "limit_by_air_accel": lim})
    return rows
