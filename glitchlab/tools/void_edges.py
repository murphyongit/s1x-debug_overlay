"""Per-map geometry pass: bottomless void edges near where the player has been, and how hard it is to fall
from each edge into each parked (hidden) trigger. Writes allmaps_<map>.pkl."""
import sys, glob, math, pickle
sys.path.insert(0, '/home/claude/s1x-debug_overlay/glitchlab')
import numpy as np
from scipy import ndimage
from glitchlab import data as D
from glitchlab.miners import MineContext
from glitchlab.miners import triggers as TM, geometry as GM

SD = '/mnt/user-data/uploads/AW/speedrun_data/'
SC = '/tmp/claude-0/-home-claude/5b5ed90d-297f-579c-9a1b-58fa066c8e68/scratchpad/sim/'
CELL = 32.0
SOLID = 0x1 | 0x10000 | 0x20000


def run(mp):
    col = D.load_collision_full(SD + f'collision_full_{mp}.bin')
    logs = sorted(glob.glob(SD + f'{mp}_2026110[45]*.txt'))
    lg = D.load_run_log(logs[-1])
    S = [s for s in lg.samples if s.game_ms is not None]
    pos = np.array([(s.x, s.y, s.z) for s in S])
    spawn = pos[0]
    # bounds: around the recorded positions and walkable geometry
    ents = D.load_map_ents(SD + f'map_ents_{mp}.txt')
    eo = np.array([e.origin for e in ents if e.origin and abs(e.origin[2]) < 8000 and max(abs(e.origin[0]), abs(e.origin[1])) < 60000])
    allp = np.vstack([pos, eo]) if len(eo) else pos
    allp = allp[np.isfinite(allp).all(1)]
    lo = allp.min(0) - 2000; hi = allp.max(0) + 2000
    print(mp, 'grid', lo.round(), hi.round(), flush=True)
    X0, Y0 = lo[0], lo[1]
    nx, ny = int((hi[0] - X0) / CELL) + 1, int((hi[1] - Y0) / CELL) + 1
    cover = np.zeros((nx, ny), bool)
    top = np.full((nx, ny), -1e9)

    def cells(a, b):
        if not (np.isfinite(a).all() and np.isfinite(b).all()):
            return 0, -1, 0, -1
        i0 = max(int((a[0] - X0) // CELL), 0); i1 = min(int((b[0] - X0) // CELL), nx - 1)
        j0 = max(int((a[1] - Y0) // CELL), 0); j1 = min(int((b[1] - Y0) // CELL), ny - 1)
        return i0, i1, j0, j1

    # brushes: bounding boxes (convex, so box is a fair stand-in for cover); top = highest up-facing face
    for contents, f0, n in col.brushes:
        if not contents & SOLID:
            continue
        pts = np.array([p for _, _, poly in col.brush_faces[f0:f0 + n] for p in poly])
        if not len(pts):
            continue
        a, b = pts.min(0), pts.max(0)
        if b[2] < -5000 or a[2] > hi[2] + 2000:
            continue
        i0, i1, j0, j1 = cells(a, b)
        if i1 < i0 or j1 < j0:
            continue
        cover[i0:i1 + 1, j0:j1 + 1] = True
        top[i0:i1 + 1, j0:j1 + 1] = np.maximum(top[i0:i1 + 1, j0:j1 + 1], b[2])
    for mat, contents, model, tri in col.triangles:
        if not contents & SOLID:
            continue
        t = np.array(tri)
        a, b = t.min(0), t.max(0)
        if b[2] < -5000 or a[2] > hi[2] + 2000:
            continue
        i0, i1, j0, j1 = cells(a, b)
        if i1 < i0 or j1 < j0:
            continue
        xs = X0 + (np.arange(i0, i1 + 1) + 0.5) * CELL; ys = Y0 + (np.arange(j0, j1 + 1) + 0.5) * CELL
        gx, gy = np.meshgrid(xs, ys, indexing='ij')
        A, B, C = t; v0 = B - A; v1 = C - A; d = v0[0] * v1[1] - v0[1] * v1[0]
        if abs(d) < 1e-6:
            continue
        px = gx - A[0]; py = gy - A[1]
        u = (px * v1[1] - py * v1[0]) / d; v = (v0[0] * py - v0[1] * px) / d
        inn = (u >= -0.05) & (v >= -0.05) & (u + v <= 1.1)
        if not inn.any():
            continue
        cover[i0:i1 + 1, j0:j1 + 1] |= inn
        nrm = np.cross(v0, v1); nz = abs(nrm[2]) / (np.linalg.norm(nrm) + 1e-9)
        if nz >= 0.7:
            z = A[2] + u * v0[2] + v * v1[2]
            sub = top[i0:i1 + 1, j0:j1 + 1]
            top[i0:i1 + 1, j0:j1 + 1] = np.where(inn, np.maximum(sub, z), sub)
    void = ~cover
    # cracks of 1-2 cells are rasterisation noise; a real hole survives one erosion
    void = ndimage.binary_opening(void, iterations=1)
    walk = top > -1e8
    visited = np.zeros((nx, ny), bool)
    ix = ((pos[:, 0] - X0) // CELL).astype(int); iy = ((pos[:, 1] - Y0) // CELL).astype(int)
    ok = (ix >= 0) & (ix < nx) & (iy >= 0) & (iy < ny); visited[ix[ok], iy[ok]] = True
    reach = ndimage.binary_dilation(visited, iterations=24)   # within ~770 units of where you went
    edge = ndimage.binary_dilation(void, iterations=1) & walk & ~void
    frame = np.zeros_like(edge); frame[2:-2, 2:-2] = True
    zok = (top > pos[:, 2].min() - 3000) & (top < pos[:, 2].max() + 3000)
    edge &= frame & zok
    edge_all = edge
    edge_near = edge
    ei, ej = np.nonzero(edge_near)
    ex = X0 + (ei + 0.5) * CELL; ey = Y0 + (ej + 0.5) * CELL; ez = top[ei, ej]
    # cluster edge cells into spots
    lab, n = ndimage.label(edge_near, structure=np.ones((3, 3)))
    spots = []
    for k in range(1, n + 1):
        m = lab[ei, ej] == k
        if m.sum() < 2:
            continue
        kk = np.argmin(np.hypot(ex[m] - spawn[0], ey[m] - spawn[1]))
        spots.append(dict(cells=int(m.sum()), x=round(float(ex[m][kk])), y=round(float(ey[m][kk])),
                          z=round(float(ez[m][kk])), zmin=round(float(ez[m].min())), zmax=round(float(ez[m].max())),
                          from_spawn=round(float(np.hypot(ex[m][kk] - spawn[0], ey[m][kk] - spawn[1]))),
                          near_path=bool(reach[ei[m], ej[m]].any())))
    spots.sort(key=lambda s: s['from_spawn'])
    # parked triggers vs void edges
    ctx = MineContext(map=mp, map_ents=ents, logs=[lg], scripts_dir=None)
    parked = TM.parked_triggers(ctx)
    pk = []
    allx, ally, allz = ex, ey, ez
    # include every void edge in the map (not only near your path) for the parked-trigger search
    ai, aj = np.nonzero(edge)
    ax = X0 + (ai + 0.5) * CELL; ay = Y0 + (aj + 0.5) * CELL; az = top[ai, aj]
    for f in parked:
        real = f.evidence['real_position']; park = f.evidence['parked_position']
        size = f.evidence.get('box_size') or [0, 0, 0]
        # is there geometry directly above the parked spot? (a straight fall from a hole in the map)
        pi = int((park[0] - X0) // CELL); pj = int((park[1] - Y0) // CELL)
        above_void = bool(0 <= pi < nx and 0 <= pj < ny and void[pi, pj])
        best = None
        for X, Y, Z, near in ((ax, ay, az, False),):
            if not len(X):
                continue
            dh = np.hypot(X - park[0], Y - park[1])
            dz = np.maximum(Z - park[2], 1)
            t = np.sqrt(2 * dz / 800.0)
            need = np.maximum(dh - max(size[0], size[1]) / 2 - 16, 0) / t
            k = int(np.argmin(need))
            cand = dict(edge=(round(float(X[k])), round(float(Y[k])), round(float(Z[k]))), dist=round(float(dh[k])),
                        fall=round(float(dz[k])), secs=round(float(t[k]), 1), speed=round(float(need[k])), near_path=near)
            if best is None or (near and cand['speed'] < 400) or cand['speed'] < best['speed'] - 50:
                best = cand if (best is None or cand['speed'] < best['speed'] or near) else best
        pk.append(dict(title=f.title, flag=f.evidence.get('switched_on_by_flag'), real=real, park=park, size=size,
                       above_void=above_void, best=best))
    # lowest floor under the play area: is there a catch-all floor like Atlas's -576?
    under = top[reach & walk]
    def safe(f):
        try:
            return f()
        except Exception as e:
            print(mp, 'skipped', e, flush=True); return []
    slopes = safe(lambda: GM.steep_slopes(MineContext(map=mp, map_ents=ents, logs=[lg], full=col)))
    seams = safe(lambda: GM.elevator_seams(MineContext(map=mp, map_ents=ents, logs=[lg], full=col)))
    out = dict(map=mp, spawn=spawn.round().tolist(), cells=(nx, ny), void_frac=float(void[reach].mean()),
               spots=spots, parked=pk, floor_min=float(under.min()) if len(under) else None,
               slopes=[(round(s.score), s.title, [round(v) for v in (s.location or [])]) for s in sorted(slopes, key=lambda s: -s.score)[:25]],
               seams=[(round(s.score), s.title, [round(v) for v in (s.location or [])]) for s in sorted(seams, key=lambda s: -s.score)[:25]])
    pickle.dump(out, open(SC + f'allmaps_{mp}.pkl', 'wb'))
    print(mp, 'spots', len(spots), 'parked', len(pk), 'slopes', len(slopes), 'seams', len(seams), flush=True)


if __name__ == '__main__':
    run(sys.argv[1])
