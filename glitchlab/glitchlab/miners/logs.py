"""Miners over recorded runs: where checkpoints put you, and movement that looks unusual."""
from __future__ import annotations

import math

from . import Finding, MineContext

JUMP_HEIGHT = 39.0


def checkpoint_positions(ctx: MineContext) -> list[Finding]:
    """Where each checkpoint reload put the player (first sample after a RELOAD line)."""
    out = []
    seen = []
    for log in ctx.logs:
        for rt in log.reloads:
            after = next((s for s in log.samples if s.t > rt + 0.05), None)
            if not after:
                continue
            p = (after.x, after.y, after.z)
            if any(math.dist(p, q) < 64 for q in seen):
                continue
            seen.append(p)
            out.append(Finding("checkpoint_positions", "checkpoint",
                               f"Checkpoint restore point at {p[0]:.0f} {p[1]:.0f} {p[2]:.0f}", 20.0,
                               evidence={"log": log.path.name, "reload_at": rt,
                                         "game_ms_after": after.game_ms},
                               test_plan="Check whether this checkpoint can be saved while you are somewhere useful "
                                         "(e.g. out of bounds) and reloaded later as a warp.",
                               location=list(p)))
    return out


def _segments(log):
    """Continuous pieces of a run between reloads/teleports (big position jumps)."""
    seg = []
    for a in log.samples:
        if seg:
            b = seg[-1]
            dt = (a.game_ms - b.game_ms) / 1000 if a.game_ms is not None and b.game_ms is not None else a.t - b.t
            if dt <= 0 or dt > 0.6 or math.dist((a.x, a.y, a.z), (b.x, b.y, b.z)) / max(dt, 1e-3) > 1500:
                if len(seg) > 5:
                    yield seg
                seg = []
        seg.append(a)
    if len(seg) > 5:
        yield seg


def _dt(a, b):
    if a.game_ms is not None and b.game_ms is not None:
        return (b.game_ms - a.game_ms) / 1000
    return b.t - a.t


def speed_records(ctx: MineContext) -> list[Finding]:
    """Flags moments in recorded runs that beat normal movement: jumps higher than 39 units from a flat
    takeoff, and fast upward motion that isn't noclip-like (noclip shows sustained climbing)."""
    out = []
    for log in ctx.logs:
        for seg in _segments(log):
            for i in range(2, len(seg) - 12):
                a0, a, b = seg[i - 2], seg[i - 1], seg[i]
                if abs(a.z - a0.z) > 0.5 or b.z - a.z < 3:
                    continue      # want: flat for a sample, then rising
                peak = max(s.z for s in seg[i:i + 12])
                land = seg[i + 11].z
                rise = peak - a.z
                climb_rate = max((seg[j + 1].z - seg[j].z) / max(_dt(seg[j], seg[j + 1]), 1e-3)
                                 for j in range(i - 1, i + 6))
                if rise > JUMP_HEIGHT + 4 and climb_rate < 300:
                    out.append(Finding(
                        "speed_records", "movement",
                        f"Jump rose {rise:.0f} units (normal {JUMP_HEIGHT:.0f})", min(80.0, 30 + rise - JUMP_HEIGHT),
                        evidence={"log": log.path.name, "t": a.t, "takeoff": [a.x, a.y, a.z], "peak": peak,
                                  "landed_z": land, "max_climb_rate": round(climb_rate)},
                        test_plan="Look at what you jumped from / into here: a slope, an edge or an NPC can give "
                                  "extra height; repeat it with the recorder on.",
                        location=[a.x, a.y, a.z]))
    # keep the best per 200-unit area
    best = {}
    for f in out:
        k = (round(f.location[0] / 200), round(f.location[1] / 200))
        if k not in best or f.score > best[k].score:
            best[k] = f
    return sorted(best.values(), key=lambda f: -f.score)[:25]
