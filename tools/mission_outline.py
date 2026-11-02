#!/usr/bin/env python3
"""Condensed outline of a campaign mission's scripts: per function, the waits, triggers,
flags, scripted moves, teleports and fail/end calls, in source order.
usage: mission_outline.py <decompiled maps dir> <mapname> [entry_func ...]"""
import re, sys, os
d, m = sys.argv[1], sys.argv[2]
files = [f for f in os.listdir(d) if f.startswith(m) and f.endswith('.gsc') and not any(x in f for x in ('_fx','_aud','_lighting','_precache','_art','_fog','_anim.gsc'))]
funcs = {}
for f in sorted(files):
    cur = None
    for i, line in enumerate(open(os.path.join(d, f), encoding='utf-8', errors='replace')):
        mm = re.match(r'^([A-Za-z_0-9]+)\s*\(.*\)\s*$', line)
        if mm: cur = mm.group(1); funcs[cur] = (f, i + 1, []); continue
        if cur: funcs[cur][2].append((i + 1, line.rstrip()))
KEY = re.compile(r'flag_wait|flag_set|flag_clear|waittill|waittill_trigger|trigger_with_name|activate_trigger|anim_reach|anim_single|anim_loop|forceteleport|teleport|missionfail|nextmission|thread [a-z_]+|notify\(|setgoal|wait\s*\(|wait [0-9]|endon|spawn_ai|spawn_vehicle|playerlinkto|enableplayerweapons|setmovespeed|player_speed', re.I)
def show(name, depth=0, seen=set()):
    if name in seen or name not in funcs: return
    seen.add(name)
    f, ln, body = funcs[name]
    print('  ' * depth + f'## {name}  ({f}:{ln})')
    for n, l in body:
        if KEY.search(l):
            s = l.strip()
            s = re.sub(r'(common_scripts|maps)\\[a-z_\\]*::', '', s)
            print('  ' * depth + f'   {n}: {s[:170]}')
    for n, l in body:
        for c in re.findall(r'thread ([A-Za-z_0-9]+)\(', l) + re.findall(r'^\s*([A-Za-z_0-9]+)\(\s*\)\s*;', l):
            if c in funcs and funcs[c][0].startswith(m): show(c, depth + 1, seen)
for e in (sys.argv[3:] or ['main']): show(e)
