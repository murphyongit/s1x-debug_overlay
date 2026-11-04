#!/usr/bin/env python3
"""Top-down route map: collision blocks (optional), trigger boxes, fail/warning boxes, highlighted targets, paths.
usage: route_map.py --log run.txt [--log run2.txt ...] [--collision collision_seoul.txt] --out map.png
       [--box x0 x1 y0 y1] [--from T0] [--to T1] [--target NAME ...] [--zmin Z] [--zmax Z]"""
import argparse, re
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle
from matplotlib.collections import PatchCollection

ap = argparse.ArgumentParser()
ap.add_argument('--log', action='append', required=True)
ap.add_argument('--label', action='append')
ap.add_argument('--collision')
ap.add_argument('--out', required=True)
ap.add_argument('--box', nargs=4, type=float)
ap.add_argument('--from', dest='t0', type=float, default=0)
ap.add_argument('--to', dest='t1', type=float, default=1e9)
ap.add_argument('--target', action='append', default=[])
ap.add_argument('--zmin', type=float, default=-1e9)
ap.add_argument('--zmax', type=float, default=1e9)
ap.add_argument('--title', default='Route map')
a = ap.parse_args()

trig = {}
for l in open(a.log[0], errors='replace'):
    if ' TRIG ' not in l:
        if l[:6] > '0.10 ': break
        continue
    m = re.match(r'\S+ TRIG #(\d+) (\S+) name=(\S+)(?: flags=(.*?))? at (-?\d+) (-?\d+) (-?\d+)(?: size (\d+) (\d+) (\d+))?', l.strip())
    if m and m.group(8):
        slot, cls, nm, fl, x, y, z, sx, sy, sz = m.groups()
        trig[int(slot)] = dict(cls=cls, name=nm, flags=fl or '', x=float(x), y=float(y), z=float(z), sx=float(sx), sy=float(sy), sz=float(sz))

fig, ax = plt.subplots(figsize=(16, 12), dpi=130)
fig.patch.set_facecolor('#14161a'); ax.set_facecolor('#1b1e24')
ax.set_aspect('equal'); ax.tick_params(colors='#9aa1ad'); [s.set_color('#3a3f48') for s in ax.spines.values()]

if a.collision:
    rects, zs = [], []
    for l in open(a.collision):
        if l.startswith('#'): continue
        mx, my, mz, hx, hy, hz = map(float, l.split())
        if mz + hz < a.zmin or mz - hz > a.zmax: continue
        rects.append(Rectangle((mx - hx, my - hy), 2 * hx, 2 * hy)); zs.append(mz + hz)
    pc = PatchCollection(rects, cmap='viridis', alpha=0.55, linewidths=0)
    pc.set_array(zs); ax.add_collection(pc)
    cb = fig.colorbar(pc, ax=ax, fraction=0.03); cb.set_label('top of collision (z)', color='#9aa1ad'); cb.ax.tick_params(colors='#9aa1ad')

for t in trig.values():
    if t['z'] < -5000: continue  # parked (switched off at start)
    f = t['flags']
    fail = re.search(r'left_map|left_squad|left_encounter|board_fail', f)
    warn = re.search(r'leaving_map|leaving_squad|warn', f)
    tgt = any(k in (t['name'] + ' ' + f) for k in a.target)
    if not (fail or warn or tgt): continue
    col = '#ff4d4d' if fail else '#ffb547' if warn else '#5aa9ff'
    ax.add_patch(Rectangle((t['x'] - t['sx'] / 2, t['y'] - t['sy'] / 2), t['sx'], t['sy'], fill=tgt, alpha=0.35 if tgt else 1,
                           edgecolor=col, facecolor=col, linewidth=2.5 if tgt else 1.2))
    if tgt: ax.text(t['x'], t['y'], t['name'] if t['name'] != '-' else f.split(' ')[0], color='white', fontsize=9, ha='center', fontweight='bold')
# targets that are parked at start: draw at their real x,y anyway
for t in trig.values():
    if t['z'] < -5000 and any(k in (t['name'] + ' ' + t['flags']) for k in a.target):
        ax.add_patch(Rectangle((t['x'] - t['sx'] / 2, t['y'] - t['sy'] / 2), t['sx'], t['sy'], fill=True, alpha=0.35, edgecolor='#5aa9ff', facecolor='#5aa9ff', linewidth=2.5))
        ax.text(t['x'], t['y'], (t['flags'].split(' ')[0] or t['name']) + ' (switched on later)', color='white', fontsize=9, ha='center', fontweight='bold')

cols = ['#5fd38d', '#ff9ff3', '#e9ebef', '#7fd6ff']
for i, lg in enumerate(a.log):
    xs, ys = [], []
    for l in open(lg, errors='replace'):
        p = l.split()
        if len(p) > 4 and p[1] == 'P':
            t = float(p[0])
            if t < a.t0 or t > a.t1: continue
            xs.append(float(p[2])); ys.append(float(p[3]))
    lab = a.label[i] if a.label and i < len(a.label) else lg.split('/')[-1]
    ax.plot(xs, ys, color=cols[i % 4], linewidth=1.6, label=lab)
    if xs: ax.plot(xs[0], ys[0], 'o', color=cols[i % 4]); ax.plot(xs[-1], ys[-1], 's', color=cols[i % 4])
if a.box: ax.set_xlim(a.box[0], a.box[1]); ax.set_ylim(a.box[2], a.box[3])
ax.legend(facecolor='#1d2026', edgecolor='#3a3f48', labelcolor='#e9ebef', loc='upper left')
ax.set_title(a.title, color='#e9ebef', fontsize=15, fontweight='bold')
ax.text(0.01, 0.01, 'red = fail box (some only active at certain times; height ignored) · orange = warning box · blue = target triggers · ○ start · □ end of path', transform=ax.transAxes, color='#9aa1ad', fontsize=10)
plt.savefig(a.out, facecolor=fig.get_facecolor(), bbox_inches='tight'); print('saved', a.out)
