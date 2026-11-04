"""Exo moves (boost jump, hover, air dash) measured from recordings, with a train/test check.

The exo movement isn't in the plain movement code (its tuning comes from game data, not dvars), so these
models are fitted to what the recordings show. Only clean intervals are used (sim/clean.py removes noclip,
teleports, reloads and scripted scenes). Recordings are split by file: even-numbered files fit the model,
odd-numbered files test it, so the reported errors are on runs the fit never saw.

Models (all vertical speeds in u/s, + is up):
* boost jump: vertical speed is SET to  boost_base + boost_carry * (vertical speed before)
* hover: vertical speed is pulled up at hover_catch u/s per second until it reaches hover_floor, then rises
  slowly (hover_rise u/s per second) and drops back to hover_floor every hover_period seconds - a sawtooth
  averaging about -44 u/s. Sideways control while hovering is the normal air control.
* air dash: sideways speed held at dash_speed in the dash direction for dash_time, then set to dash_end
  (240). Gravity keeps acting during the dash.
"""
from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np

from ..data import RunLog, Sample
from .clean import label


@dataclass
class ExoParams:
    # fitted on half of the Induction recordings (53 files), tested on the other half; see check()
    boost_base: float = 539.1       # test error 3.7 u/s (49 boosts)
    boost_carry: float = -0.048
    hover_floor: float = -64.8      # test error 3.1 u/s per sample (202 hovers); average sink -44 u/s
    hover_rise: float = 70.0
    hover_period: float = 0.59
    hover_catch: float = 540.0      # rate a fast fall is slowed at when hover starts (39 cases)
    dash_speed: float = 580.0       # distance over a dash within 17 units (median, 148 dashes)
    dash_time: float = 0.32
    dash_end: float = 240.0         # Atlas and Induction: 64% of air dashes end at 235-245


def _clean_runs(logs: list[RunLog]):
    for lg in logs:
        S = [s for s in lg.samples if s.game_ms is not None]
        if len(S) < 50:
            continue
        yield lg, S, label(S)


# ------------------------------------------------------------------ boost jump
def find_boosts(logs) -> list[tuple[float, float]]:
    """(vertical speed just before, just after) for mid-air boost jumps: a gravity arc, a sudden upward
    kick, then a gravity arc again. Arcs are fitted on both sides and the boost time is where they meet."""
    out = []
    for _lg, S, ivs in _clean_runs(logs):
        for k in range(4, len(ivs) - 4):
            if any(iv.label != "ok" for iv in ivs[k - 3:k + 4]):
                continue
            a1, a2, a3 = ivs[k - 2].acc_z, ivs[k - 1].acc_z, ivs[k].acc_z
            if a1 is None or a2 is None or a3 is None or abs(a1 + 800) > 150 or abs(a2 + 800) > 150 or a3 < 1200:
                continue
            post_acc = [ivs[m].acc_z for m in (k + 2, k + 3)]
            if any(p is None or abs(p + 800) > 150 for p in post_acc):
                continue
            pre = [S[ivs[k - 3].i - 1]] + [S[ivs[m].i] for m in range(k - 3, k)]
            post = [S[ivs[m].i] for m in range(k, k + 4)]

            def fit(ss):
                t = np.array([s.game_ms / 1000 for s in ss])
                z = np.array([s.z for s in ss])
                A = np.vstack([np.ones_like(t), t]).T
                return np.linalg.lstsq(A, z + 400 * t ** 2, rcond=None)[0]

            cp, cq = fit(pre), fit(post)
            if abs(cq[1] - cp[1]) < 1:
                continue
            tb = (cp[0] - cq[0]) / (cq[1] - cp[1])
            if not (pre[-1].game_ms / 1000 - 0.02 <= tb <= post[0].game_ms / 1000 + 0.02):
                continue
            vz_pre, vz_post = cp[1] - 800 * tb, cq[1] - 800 * tb
            if vz_post > 450:                 # ~250 is a normal jump off the ground (a bunny hop), not a boost
                out.append((float(vz_pre), float(vz_post)))
    return out


def fit_boost(pairs) -> tuple[float, float]:
    p = np.array(pairs)
    A = np.vstack([np.ones(len(p)), p[:, 0]]).T
    base, carry = np.linalg.lstsq(A, p[:, 1], rcond=None)[0]
    return float(base), float(carry)


# ------------------------------------------------------------------ hover
def find_hovers(logs, min_samples: int = 12) -> list[tuple[list[tuple[float, float]], list[float]]]:
    """Each hover: list of (interval start, end) game times and the vertical speed over each interval."""
    out = []
    for _lg, S, ivs in _clean_runs(logs):
        k = 0

        def h(iv):
            return iv.label == "ok" and -80 < iv.vz < -15 and (iv.acc_z is None or -330 < iv.acc_z < 130)

        while k < len(ivs):
            if h(ivs[k]):
                j = k
                while j < len(ivs) and h(ivs[j]):
                    j += 1
                if j - k >= min_samples:
                    tt = []
                    for iv in ivs[k:j]:
                        t1 = S[iv.i].game_ms / 1000
                        tt.append((t1 - iv.dt, t1))
                    out.append((tt, [iv.vz for iv in ivs[k:j]]))
                k = j
            else:
                k += 1
    return out


def _saw_avg(tt, floor, rise, period, phase):
    out = []
    for t0, t1 in tt:
        ts = np.linspace(t0, t1, 30)
        out.append(float(np.mean(floor + rise * np.mod(ts - phase, period))))
    return np.array(out)


def hover_rms(h, P: ExoParams) -> float:
    """Best phase only (the one thing a model can't know is when the cycle started)."""
    tt, vz = h
    vz = np.array(vz)
    best = min(float(np.sqrt(np.mean((_saw_avg(tt, P.hover_floor, P.hover_rise, P.hover_period, ph) - vz) ** 2)))
               for ph in np.arange(0, P.hover_period, 0.02))
    return best


def fit_hover(hovers, limit: int = 40) -> tuple[float, float, float]:
    best = None
    for period in np.arange(0.55, 0.66, 0.01):
        for rise in (55, 60, 65, 70, 75, 80, 85):
            errs, floors = [], []
            for tt, vz in hovers[:limit]:
                vz = np.array(vz)
                b = None
                for ph in np.arange(0, period, 0.02):
                    m = _saw_avg(tt, 0.0, rise, period, ph)
                    f0 = float(np.mean(vz - m))
                    r = float(np.mean((vz - m - f0) ** 2))
                    if b is None or r < b[0]:
                        b = (r, f0)
                errs.append(b[0])
                floors.append(b[1])
            e = float(np.mean(errs))
            if best is None or e < best[0]:
                best = (e, float(np.median(floors)), float(rise), float(period))
    return best[1], best[2], best[3]


# ------------------------------------------------------------------ air dash
def find_dashes(logs) -> list[dict]:
    """Air dashes: sideways speed jumps over 450 from under 400 while airborne the whole time, and ends near
    240. Returns speed per 0.1 s interval from the one before the dash to two after it ends."""
    out = []
    for _lg, S, ivs in _clean_runs(logs):
        k = 3
        while k < len(ivs) - 6:
            iv = ivs[k]
            if iv.label == "ok" and 450 < iv.vh < 1200 and ivs[k - 1].vh < 400 and \
                    all(x.label == "ok" for x in ivs[k - 3:k + 6]):
                j = k
                while j < len(ivs) and ivs[j].vh > 420:
                    j += 1
                seg = ivs[k - 1:j + 3]
                if all(abs(x.vz) > 3 for x in seg) and 232 <= seg[-1].vh <= 248 and len(seg) >= 5:
                    out.append({"speeds": [x.vh for x in seg], "dts": [x.dt for x in seg]})
                k = j
            k += 1
    return out


def _dash_model(d, P: ExoParams, phase):
    sp = d["speeds"]
    out, t = [], 0.0
    for dt in d["dts"]:
        ts = np.linspace(t, t + dt, 21)
        v = np.where(ts < phase, sp[0], np.where(ts < phase + P.dash_time, P.dash_speed, P.dash_end))
        out.append(float(v.mean()))
        t += dt
    return np.array(out)


def dash_distance_error(d, P: ExoParams) -> float:
    """Error in distance covered over the whole dash window (best start time inside the first gap)."""
    real = float(np.sum(np.array(d["speeds"]) * np.array(d["dts"])))
    best = None
    for ph in np.arange(d["dts"][0] * 0.0, d["dts"][0], 0.01):
        sim = float(np.sum(_dash_model(d, P, ph) * np.array(d["dts"])))
        e = sim - real
        if best is None or abs(e) < abs(best):
            best = e
    return best


def fit_dash(dashes, limit: int = 150) -> tuple[float, float]:
    best = None
    for V in range(560, 761, 20):
        for T in np.arange(0.16, 0.34, 0.02):
            P = ExoParams(dash_speed=V, dash_time=float(T))
            err = 0.0
            for d in dashes[:limit]:
                sp = np.array(d["speeds"])
                err += min(float(np.sum((_dash_model(d, P, ph) - sp) ** 2)) for ph in np.arange(0, d["dts"][0], 0.01))
            if best is None or err < best[0]:
                best = (err, V, float(T))
    return float(best[1]), best[2]


# ------------------------------------------------------------------ train / test
def check(logs: list[RunLog]) -> dict:
    train = [lg for i, lg in enumerate(sorted(logs, key=lambda l: l.path.name)) if i % 2 == 0]
    test = [lg for i, lg in enumerate(sorted(logs, key=lambda l: l.path.name)) if i % 2 == 1]
    res: dict = {"train_files": len(train), "test_files": len(test)}
    b_tr, b_te = find_boosts(train), find_boosts(test)
    if len(b_tr) > 10 and b_te:
        base, carry = fit_boost(b_tr)
        pred = np.array([base + carry * a for a, _ in b_te])
        real = np.array([b for _, b in b_te])
        res["boost"] = {"fit": {"base": round(base, 1), "carry": round(carry, 3)}, "train": len(b_tr),
                        "test": len(b_te), "test_rms": round(float(np.sqrt(np.mean((pred - real) ** 2))), 1),
                        "test_rms_constant": round(float(np.std(real)), 1)}
    h_tr, h_te = find_hovers(train), find_hovers(test)
    if h_tr and h_te:
        floor, rise, period = fit_hover(h_tr)
        P = ExoParams(hover_floor=floor, hover_rise=rise, hover_period=period)
        rms = [hover_rms(h, P) for h in h_te[:80]]
        flat = [float(np.std(h[1])) for h in h_te[:80]]
        res["hover"] = {"fit": {"floor": round(floor, 1), "rise": rise, "period": round(period, 2)},
                        "train": len(h_tr), "test": len(h_te), "test_rms_median": round(float(np.median(rms)), 2),
                        "test_rms_if_constant_sink": round(float(np.median(flat)), 2),
                        "average_sink": round(floor + rise * period / 2, 1)}
    d_tr, d_te = find_dashes(train), find_dashes(test)
    if d_tr and d_te:
        V, T = fit_dash(d_tr)
        P = ExoParams(dash_speed=V, dash_time=T)
        errs = np.array([dash_distance_error(d, P) for d in d_te])
        res["dash"] = {"fit": {"speed": V, "time": round(T, 2)}, "train": len(d_tr), "test": len(d_te),
                       "test_distance_error_median": round(float(np.median(np.abs(errs))), 1),
                       "test_distance_error_p90": round(float(np.percentile(np.abs(errs), 90)), 1),
                       "dash_extra_distance": round(V * T - 240 * T, 1)}
    return res
