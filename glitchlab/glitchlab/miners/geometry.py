"""Miners over collision geometry.

With only the brush export (invisible walls + hidden objects) these run on that subset; once the overlay's
full collision dump (collision_full_<map>.bin) exists they run on every solid surface.

Physics facts used (from the game's movement code, see README "Engine facts"):
* walkable surface: normal z >= 0.7 (IW default); steeper = you slide
* MW2-style bouncefix: speed is NOT redirected off a non-walkable surface while the "jumped" flag is set or
  when normal z < 0.3  ->  redirect works for 0.3 <= nz < 0.7 when you walked off an edge (no jump)
* get-unstuck nudges are 1 unit, upward first, then a 1.25-unit drop check
"""
from __future__ import annotations

import math

import numpy as np

from . import Finding, MineContext

PLAYER_W = 32.0
WALKABLE_NZ = 0.7
BOUNCE_MIN_NZ = 0.3
GRAVITY = 800.0


def _polys(ctx: MineContext):
    """Yields (polygon corners, contents, source) for every solid face we have."""
    if ctx.full is not None:
        for contents, _mat, pts in ctx.full.brush_faces:
            yield pts, contents, "brush"
        for _mat, contents, _model, pts in ctx.full.triangles:
            yield list(pts), contents, "tri"
        return
    for w in ctx.walls:
        for f in w.faces:
            yield f, w.contents, "wall" if not w.model else "model"


def _normal_area(pts):
    p = np.asarray(pts, dtype=float)
    n = np.zeros(3)
    for i in range(len(p)):
        n += np.cross(p[i], p[(i + 1) % len(p)])
    area = np.linalg.norm(n) / 2
    if area < 1e-6:
        return None, 0.0
    return n / (2 * area), area


def steep_slopes(ctx: MineContext) -> list[Finding]:
    """Surfaces too steep to stand on but shallow enough to redirect speed (0.3 <= normal z < 0.7), facing up.

    Falling onto one after walking off an edge (no jump) should turn fall speed into sideways speed along
    the slope's downhill direction: roughly v_fall * nz * sqrt(1 - nz^2) horizontally (max at 45 degrees)."""
    out = []
    target = ctx.known_spots.get("target")     # e.g. Atlas end cube centre
    for pts, contents, src in _polys(ctx):
        if not (contents & 0x10001):
            continue
        n, area = _normal_area(pts)
        if n is None or area < 2000:
            continue
        nz = n[2]
        if not (BOUNCE_MIN_NZ <= nz < WALKABLE_NZ):
            continue
        c = np.mean(np.asarray(pts, dtype=float), axis=0)
        downhill = np.array([n[0], n[1]])
        dl = np.linalg.norm(downhill)
        if dl < 1e-6:
            continue
        downhill /= dl
        gain = nz * math.sqrt(1 - nz * nz)       # fraction of fall speed turned sideways
        score = 20 + min(30, area / 2000) + 30 * gain
        ev = {"centre": [round(v) for v in c], "normal_z": round(float(nz), 3), "area": round(area),
              "downhill_dir": [round(float(downhill[0]), 3), round(float(downhill[1]), 3)],
              "sideways_fraction_of_fall_speed": round(gain, 2), "source": src}
        if target is not None:
            to = np.array([target[0] - c[0], target[1] - c[1]])
            dist = float(np.linalg.norm(to))
            align = float(np.dot(to / max(dist, 1e-6), downhill))
            ev["points_toward_target"] = round(align, 2)
            ev["target_distance"] = round(dist)
            if align > 0.8:
                score += 25
        out.append(Finding("steep_slopes", "movement",
                           f"Redirect slope at {c[0]:.0f} {c[1]:.0f} {c[2]:.0f} (nz {nz:.2f})", float(score),
                           evidence=ev,
                           test_plan="Walk (don't jump) off an edge above it so you land on it fast; watch the "
                                     "speedometer for a sideways burst along the downhill direction.",
                           location=[float(c[0]), float(c[1]), float(c[2])],
                           needs="" if ctx.full is not None else "only invisible walls checked until the full "
                                                                 "collision dump exists"))
    out.sort(key=lambda f: -f.score)
    return out[:60]


def elevator_seams(ctx: MineContext) -> list[Finding]:
    """Narrow wedges between two steep surfaces that face each other and are closer than the hitbox width:
    the kind of spot where standing up or landing can leave you overlapping solid, so the 1-unit upward
    unstuck nudge can lift you. Needs the full collision dump to be meaningful."""
    if ctx.full is None:
        return [Finding("elevator_seams", "collision_oob", "Elevator seam scan waiting for full collision", 0.0,
                        needs="collision_full_<map>.bin from overlay v1.24+")]
    faces = []
    for pts, contents, _src in _polys(ctx):
        if not (contents & 0x10001):
            continue
        n, area = _normal_area(pts)
        if n is None or area < 50 or n[2] >= WALKABLE_NZ or n[2] < -0.2:
            continue
        p = np.asarray(pts, dtype=float)
        faces.append((n, p, p.min(axis=0), p.max(axis=0)))
    cell = 64.0
    grid: dict[tuple[int, int, int], list[int]] = {}
    for i, (_n, _p, lo, hi) in enumerate(faces):
        for gx in range(int(lo[0] // cell), int(hi[0] // cell) + 1):
            for gy in range(int(lo[1] // cell), int(hi[1] // cell) + 1):
                for gz in range(int(lo[2] // cell), int(hi[2] // cell) + 1):
                    grid.setdefault((gx, gy, gz), []).append(i)
    pairs = set()
    out = []
    for ids in grid.values():
        if len(ids) > 60:
            continue
        for a_i in range(len(ids)):
            for b_i in range(a_i + 1, len(ids)):
                i, j = ids[a_i], ids[b_i]
                key = (min(i, j), max(i, j))
                if key in pairs:
                    continue
                pairs.add(key)
                na, pa, loa, hia = faces[i]
                nb, pb, lob, hib = faces[j]
                if np.dot(na, nb) > -0.1:          # must face each other (a wedge / crease)
                    continue
                zlo, zhi = max(loa[2], lob[2]), min(hia[2], hib[2])
                if zhi - zlo < 40:                  # needs some vertical overlap to climb
                    continue
                # closest distance between the two faces' sample points
                sa = np.vstack([pa, pa.mean(axis=0)])
                sb = np.vstack([pb, pb.mean(axis=0)])
                d = np.min(np.linalg.norm(sa[:, None, :] - sb[None, :, :], axis=2))
                if d > PLAYER_W:
                    continue
                gap_ok = d > 8                      # a gap narrower than the hitbox, not just a shared edge
                c = (pa.mean(axis=0) + pb.mean(axis=0)) / 2
                score = 30 + min(40, (zhi - zlo) / 10) + (15 if gap_ok else 0)
                out.append(Finding("elevator_seams", "collision_oob",
                                   f"Possible elevator crease at {c[0]:.0f} {c[1]:.0f} {c[2]:.0f}", float(score),
                                   evidence={"height_span": round(float(zhi - zlo)), "gap": round(float(d), 1),
                                             "normals": [na.round(2).tolist(), nb.round(2).tolist()]},
                                   test_plan="Push into the crease and spam crouch / jump at high FPS; watch Z.",
                                   location=c.round(1).tolist()))
    out.sort(key=lambda f: -f.score)
    return out[:80]
