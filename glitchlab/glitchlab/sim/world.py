"""Collision worlds the simulator can move a player box through.

All traces move an axis-aligned box: feet at the origin, half width w, height h (the game's player box,
mins (-16,-16,0) maxs (16,16,72)). The trace follows the Quake 3 / IW brush test the game uses: each brush
plane is pushed out by the box's extent along its normal, then a point is traced against the pushed-out
planes, stopping SURFACE_CLIP_EPSILON (0.125) short of the surface.
"""
from __future__ import annotations

from dataclasses import dataclass

import numpy as np

EPS = 0.125          # SURFACE_CLIP_EPSILON
SOLID_MASK = 0x1 | 0x10000   # solid + playerclip


@dataclass
class Trace:
    fraction: float
    endpos: np.ndarray
    normal: np.ndarray
    startsolid: bool = False
    allsolid: bool = False
    brush: int = -1          # which brush/triangle was hit (index into the world's list), -1 if none


class World:
    def trace(self, start, end, half_w: float, height: float) -> Trace:  # pragma: no cover - interface
        raise NotImplementedError


class EmptyWorld(World):
    """Nothing to hit (open air)."""

    def trace(self, start, end, half_w, height):
        return Trace(1.0, np.array(end, dtype=float), np.zeros(3))


class FlatWorld(World):
    """An infinite floor at height z (optionally with walls), for quick tests and log validation."""

    def __init__(self, floor_z: float = 0.0):
        self.floor_z = floor_z

    def trace(self, start, end, half_w, height):
        s = np.asarray(start, dtype=float)
        e = np.asarray(end, dtype=float)
        n = np.array([0.0, 0.0, 1.0])
        d1 = s[2] - self.floor_z
        d2 = e[2] - self.floor_z
        if d1 < 0 and d2 < 0:
            return Trace(0.0, s.copy(), n, startsolid=True, allsolid=True)
        if d1 < 0:
            return Trace(0.0, s.copy(), n, startsolid=True)
        if d2 >= EPS or d2 >= d1:
            return Trace(1.0, e.copy(), np.zeros(3))
        f = max(0.0, (d1 - EPS) / (d1 - d2))
        return Trace(f, s + (e - s) * f, n, brush=0)


class BrushWorld(World):
    """Convex brushes (and triangles turned into thin convex slabs) with a 2D grid for broad-phase lookup."""

    CELL = 128.0

    def __init__(self, planes: list[np.ndarray], bounds: list[tuple[np.ndarray, np.ndarray]],
                 contents: list[int] | None = None, mask: int = SOLID_MASK):
        keep = [i for i in range(len(planes)) if contents is None or (contents[i] & mask)]
        self.index = keep                                  # world piece -> original index
        self.planes = [planes[i] for i in keep]            # each (k, 4): nx ny nz d   (n . p <= d inside)
        self.lo = np.array([bounds[i][0] for i in keep]) if keep else np.zeros((0, 3))
        self.hi = np.array([bounds[i][1] for i in keep]) if keep else np.zeros((0, 3))
        self.grid: dict[tuple[int, int], list[int]] = {}
        for b in range(len(self.planes)):
            for gx in range(int(self.lo[b, 0] // self.CELL), int(self.hi[b, 0] // self.CELL) + 1):
                for gy in range(int(self.lo[b, 1] // self.CELL), int(self.hi[b, 1] // self.CELL) + 1):
                    self.grid.setdefault((gx, gy), []).append(b)
        self._packed: dict[tuple, tuple] = {}

    # ------------------------------------------------------------------ building
    @staticmethod
    def brush_from_faces(faces) -> tuple[np.ndarray, tuple[np.ndarray, np.ndarray]] | None:
        """Planes of a convex brush from its face polygons (outward normals), plus its bounds."""
        rows = []
        pts_all = []
        for pts in faces:
            p = np.asarray(pts, dtype=float)
            if len(p) < 3:
                continue
            n = np.zeros(3)
            for i in range(len(p)):
                n += np.cross(p[i], p[(i + 1) % len(p)])
            ln = np.linalg.norm(n)
            if ln < 1e-9:
                continue
            n /= ln
            rows.append([*n, float(np.dot(n, p.mean(axis=0)))])
            pts_all.append(p)
        if len(rows) < 4:
            return None
        allp = np.vstack(pts_all)
        lo, hi = allp.min(axis=0), allp.max(axis=0)
        rows += _axial(lo, hi)
        return np.array(rows), (lo, hi)

    @staticmethod
    def slab_from_triangle(tri) -> tuple[np.ndarray, tuple[np.ndarray, np.ndarray]] | None:
        a, b, c = (np.asarray(v, dtype=float) for v in tri)
        n = np.cross(b - a, c - a)
        ln = np.linalg.norm(n)
        if ln < 1e-9:
            return None
        n /= ln
        d = float(np.dot(n, a))
        rows = [[*n, d], [*(-n), -d]]
        for p, q, r in ((a, b, c), (b, c, a), (c, a, b)):
            en = np.cross(q - p, n)
            le = np.linalg.norm(en)
            if le < 1e-9:
                continue
            en /= le
            if np.dot(en, r - p) > 0:
                en = -en
            rows.append([*en, float(np.dot(en, p))])
        lo = np.minimum(np.minimum(a, b), c)
        hi = np.maximum(np.maximum(a, b), c)
        rows += _axial(lo, hi)
        return np.array(rows), (lo, hi)

    @classmethod
    def from_full_collision(cls, full, mask: int = SOLID_MASK, region=None) -> "BrushWorld":
        """region: optional ((xmin, ymin, zmin), (xmax, ymax, zmax)) to load only part of the map."""
        planes, bounds, contents = [], [], []

        def inside(lo, hi):
            if region is None:
                return True
            rlo, rhi = region
            return all(hi[i] >= rlo[i] and lo[i] <= rhi[i] for i in range(3))

        if full.brushes:
            for cont, first, count in full.brushes:
                if not (cont & mask):
                    continue
                r = cls.brush_from_faces([f[2] for f in full.brush_faces[first:first + count]])
                if r and inside(*r[1]):
                    planes.append(r[0]); bounds.append(r[1]); contents.append(cont)
        for _mat, cont, _model, tri in full.triangles:
            if not (cont & mask):
                continue
            r = cls.slab_from_triangle(tri)
            if r and inside(*r[1]):
                planes.append(r[0]); bounds.append(r[1]); contents.append(cont)
        return cls(planes, bounds, contents, mask)

    @classmethod
    def boxes(cls, boxes: list[tuple[tuple[float, float, float], tuple[float, float, float]]]) -> "BrushWorld":
        """Axis-aligned boxes (lo, hi): handy for tests and hand-made scenes."""
        planes, bounds = [], []
        for lo, hi in boxes:
            lo, hi = np.asarray(lo, float), np.asarray(hi, float)
            planes.append(np.array(_axial(lo, hi)))
            bounds.append((lo, hi))
        return cls(planes, bounds)

    # ------------------------------------------------------------------ tracing
    def candidates(self, lo, hi) -> list[int]:
        out: set[int] = set()
        for gx in range(int(lo[0] // self.CELL), int(hi[0] // self.CELL) + 1):
            for gy in range(int(lo[1] // self.CELL), int(hi[1] // self.CELL) + 1):
                out.update(self.grid.get((gx, gy), ()))
        if not out:
            return []
        ids = np.fromiter(out, dtype=int)
        m = np.all(self.lo[ids] <= hi, axis=1) & np.all(self.hi[ids] >= lo, axis=1)
        return ids[m].tolist()

    def _pack(self, ids: tuple[int, ...]):
        hit = self._packed.get(ids)
        if hit is not None:
            return hit
        k = max(len(self.planes[i]) for i in ids)
        arr = np.zeros((len(ids), k, 4))
        arr[:, :, 3] = 1e9                       # padding planes: everything is "inside" them
        for j, i in enumerate(ids):
            arr[j, :len(self.planes[i])] = self.planes[i]
        if len(self._packed) > 4096:
            self._packed.clear()
        self._packed[ids] = arr
        return arr

    def trace(self, start, end, half_w, height):
        s = np.asarray(start, dtype=float)
        e = np.asarray(end, dtype=float)
        ext = np.array([half_w, half_w, height / 2.0])
        sc = s + np.array([0, 0, height / 2.0])
        ec = e + np.array([0, 0, height / 2.0])
        lo = np.minimum(sc, ec) - ext - 1
        hi = np.maximum(sc, ec) + ext + 1
        ids = self.candidates(lo, hi)
        if not ids:
            return Trace(1.0, e.copy(), np.zeros(3))
        P = self._pack(tuple(ids))
        n = P[:, :, :3]
        d = P[:, :, 3] + np.abs(n) @ ext
        d1 = n @ sc - d
        d2 = n @ ec - d
        both_in = (d1 <= 0) & (d2 <= 0)
        miss = np.any((d1 > 0) & ((d2 >= EPS) | (d2 >= d1)), axis=1)
        startout = np.any(d1 > 0, axis=1)
        getout = np.any(d2 > 0, axis=1)
        denom = np.where(np.abs(d1 - d2) < 1e-12, 1e-12, d1 - d2)
        entering = (d1 > d2) & ~both_in
        leaving = (d1 <= d2) & ~both_in
        fe = np.where(entering, (d1 - EPS) / denom, -1.0)
        fl = np.where(leaving, (d1 + EPS) / denom, 1.0)
        enter = fe.max(axis=1)
        leave = fl.min(axis=1)
        which = fe.argmax(axis=1)
        # starting inside a brush
        inside = ~startout & ~miss
        if np.any(inside):
            allsolid = bool(np.any(inside & ~getout))
            b = int(np.nonzero(inside)[0][0])
            return Trace(0.0, s.copy(), np.zeros(3), startsolid=True, allsolid=allsolid,
                         brush=self.index[ids[b]])
        ok = ~miss & startout & (enter < leave) & (enter > -1)
        if not np.any(ok):
            return Trace(1.0, e.copy(), np.zeros(3))
        cand = np.where(ok, enter, np.inf)
        b = int(np.argmin(cand))
        f = max(0.0, float(cand[b]))
        if f >= 1.0:
            return Trace(1.0, e.copy(), np.zeros(3))
        normal = n[b, which[b]].copy()
        return Trace(f, s + (e - s) * f, normal, brush=self.index[ids[b]])


def _axial(lo, hi):
    return [[1, 0, 0, hi[0]], [-1, 0, 0, -lo[0]], [0, 1, 0, hi[1]], [0, -1, 0, -lo[1]],
            [0, 0, 1, hi[2]], [0, 0, -1, -lo[2]]]
