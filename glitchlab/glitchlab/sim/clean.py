"""Finds the parts of a recording that are real, physics-driven movement, so measurements aren't polluted by
noclip, teleports, checkpoint reloads, pauses or scripted scenes.

The recorder doesn't log noclip directly, so it is recognised by how it moves:
* noclip flies along the view direction with no gravity: the climb/drop angle of the movement matches the
  view pitch, while the vertical speed doesn't change the way gravity would make it (-800 u/s per second)
* steady climbing (over 80 u/s) or steady fast sinking (over 150 u/s) with no gravity can't be normal
  movement either (hover sinks at about 50 u/s and isn't affected)
* noclip is 3x run speed: sustained sideways speed over ~450 for longer than any dash
A stretch of flight around each detection is cut out, plus a second either side.

Everything else that is cut:
* gaps that aren't ~0.1 s (reloads make the game clock jump back, pauses and loads make it skip)
* position jumps faster than 1500 u/s (teleports, devsave loads, respawns)
* scripted states the recorder marks in the stance word (bits above 0xF) and frozen controls
"""
from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np

from ..data import Sample

GRAVITY = 800.0
GROUND_SPEEDS = (123.5, 190.0, 266.0, 344.0)


@dataclass
class Interval:
    i: int                 # index of the end sample in the sample list (interval is S[i-1] -> S[i])
    dt: float
    vx: float
    vy: float
    vz: float
    acc_z: float | None    # change in vertical speed vs the previous interval, per second
    label: str = "ok"      # ok, gap, teleport, noclip, scripted

    @property
    def vh(self) -> float:
        return math.hypot(self.vx, self.vy)


def intervals(S: list[Sample]) -> list[Interval]:
    out: list[Interval] = []
    for i in range(1, len(S)):
        a, b = S[i - 1], S[i]
        if a.game_ms is None or b.game_ms is None:
            out.append(Interval(i, 0.0, 0, 0, 0, None, "gap"))
            continue
        dt = (b.game_ms - a.game_ms) / 1000
        if not (0.07 < dt < 0.16):
            out.append(Interval(i, dt, 0, 0, 0, None, "gap"))
            continue
        vx, vy, vz = (b.x - a.x) / dt, (b.y - a.y) / dt, (b.z - a.z) / dt
        iv = Interval(i, dt, vx, vy, vz, None)
        if math.sqrt(vx * vx + vy * vy + vz * vz) > 1500:
            iv.label = "teleport"
        elif (b.stance & ~0xF) or (b.flags & 0x800):
            iv.label = "scripted"
        prev = out[-1] if out else None
        if prev is not None and prev.label not in ("gap", "teleport"):
            iv.acc_z = (vz - prev.vz) / ((dt + prev.dt) / 2)
        out.append(iv)
    return out


def _on_slope(iv: Interval, flags: int) -> bool:
    """Walking, sprinting or sliding along a slope keeps the full 3D speed at a ground value."""
    v3 = math.sqrt(iv.vh ** 2 + iv.vz ** 2)
    return bool(flags & 0x100000) or any(abs(v3 - g) < 6 for g in GROUND_SPEEDS)


def _noclip_like(iv: Interval, pitch: float, flags: int) -> bool:
    """Moving along the view line with no gravity. Pitch is positive looking down; noclip moving forward
    goes the way you look (down when looking down), moving backward (pm_flags 0x20) the opposite way."""
    if iv.label != "ok" or iv.acc_z is None:
        return False
    if abs(iv.vz) < 40 or abs(pitch) < 3:
        return False
    if flags & (0x4000 | 0x100000 | 0x2 | 0x2000):   # sprinting, sliding, crouched or jumped: not noclip
        return False
    if abs(iv.acc_z + GRAVITY) < 250:         # vertical speed changing like gravity: a real fall or jump
        return False
    # walking or sliding down/up a slope also moves at a steady climb angle, but at exactly a ground speed
    # (the game keeps the full 3D speed when following a slope): run 190, sprint 266, slide ~344, crouch 124
    v3 = math.sqrt(iv.vh ** 2 + iv.vz ** 2)
    if any(abs(v3 - g) < 6 for g in GROUND_SPEEDS):
        return False
    backward = bool(flags & 0x20)
    expect_up = (pitch > 0) if backward else (pitch < 0)
    if (iv.vz > 0) != expect_up:
        return False
    elev = math.degrees(math.atan2(abs(iv.vz), max(iv.vh, 1e-6)))
    return abs(elev - abs(pitch)) < 4


def label(S: list[Sample], pad_s: float = 1.0) -> list[Interval]:
    """Intervals with a label each; only 'ok' ones are safe to measure physics on."""
    ivs = intervals(S)
    n = len(ivs)
    hits = [False] * n
    for k, iv in enumerate(ivs):
        hits[k] = _noclip_like(iv, S[iv.i].pitch, S[iv.i].flags)
    # moving up, or down fast, at a steady rate with no gravity: noclip (any flags), lifts, script moves
    for k, iv in enumerate(ivs):
        if iv.label == "ok" and iv.acc_z is not None and abs(iv.acc_z) < 300 and (iv.vz > 80 or iv.vz < -150) \
                and not _on_slope(iv, S[iv.i].flags):
            hits[k] = True
    # 3 in a row
    core = [False] * n
    for k in range(1, n - 1):
        if hits[k - 1] and hits[k] and hits[k + 1]:
            core[k - 1] = core[k] = core[k + 1] = True
    # sustained fast flight: over 450 sideways for 7+ samples in a row (dashes last ~0.3 s)
    run = 0
    for k, iv in enumerate(ivs):
        run = run + 1 if (iv.label == "ok" and iv.vh > 450) else 0
        if run >= 7:
            for j in range(k - run + 1, k + 1):
                core[j] = True
    # grow each detection over the surrounding flight (anything not on the ground and not a gravity arc)
    flagged = core[:]
    for k in range(n):
        if not core[k]:
            continue
        for step in (-1, 1):
            j = k + step
            while 0 <= j < n:
                iv = ivs[j]
                if iv.label != "ok" or core[j]:
                    break
                grounded = abs(iv.vz) < 5
                ballistic = iv.acc_z is not None and abs(iv.acc_z + GRAVITY) < 250
                if grounded or ballistic:
                    break
                flagged[j] = True
                j += step
    # pad by time
    t = [S[iv.i].t for iv in ivs]
    out = [iv for iv in ivs]
    marks = [k for k in range(n) if flagged[k]]
    if marks:
        lo = 0
        for k in range(n):
            while lo < len(marks) and t[marks[lo]] < t[k] - pad_s:
                lo += 1
            if lo < len(marks) and abs(t[marks[lo]] - t[k]) <= pad_s:
                if out[k].label == "ok":
                    out[k].label = "noclip"
            elif lo + 1 < len(marks) and abs(t[marks[lo + 1]] - t[k]) <= pad_s and out[k].label == "ok":
                out[k].label = "noclip"
    return out


def clean_mask(S: list[Sample], pad_s: float = 1.0) -> np.ndarray:
    """mask[i] is True when the interval ending at sample i (and the one before it) are clean."""
    ivs = label(S, pad_s)
    m = np.zeros(len(S), dtype=bool)
    for iv in ivs:
        m[iv.i] = iv.label == "ok"
    return m


def summary(S: list[Sample]) -> dict:
    ivs = label(S)
    c: dict[str, int] = {}
    for iv in ivs:
        c[iv.label] = c.get(iv.label, 0) + 1
    return c
