#!/usr/bin/env python3
"""List every 'gate' in a mission's scripts (what progress waits on), tagged by kind.
usage: mission_gates.py <decompiled maps dir> <mapname> <out.tsv> entry_func [entry_func ...]"""
import re, sys, os
d, m, out = sys.argv[1], sys.argv[2], sys.argv[3]
entries = sys.argv[4:]
skip = ('_fx','_aud','_lighting','_precache','_art','_fog','_anim.gsc','_vo.gsc')
funcs = {}
for f in sorted(os.listdir(d)):
    if not (f.startswith(m) and f.endswith('.gsc')) or any(x in f for x in skip): continue
    cur = None
    for i, line in enumerate(open(os.path.join(d, f), encoding='utf-8', errors='replace')):
        mm = re.match(r'^([A-Za-z_0-9]+)\s*\(.*\)\s*$', line)
        if mm: cur = mm.group(1); funcs[cur] = (f, i + 1, []); continue
        if cur: funcs[cur][2].append((i + 1, line.rstrip()))
kinds = [
 ('Kill gate (clear enemies)', r'waittill_aigroup(cleared|count)|waittill_dead|waittill_drones_dead'),
 ('Ally must arrive (anim_reach)', r'anim_reach'),
 ('Player trigger', r'waittill_trigger|trigger_wait|trigger_with_name|waittill\(\s*"trigger"'),
 ('Flag wait', r'flag_wait'),
 ('Event wait', r'\bwaittill\b|waittill_any|waittill_either'),
 ('Fixed timer', r'^\s*wait[ (]\s*[0-9.]+'),
 ('Teleport', r'forceteleport|teleport\('),
 ('Mission fail', r'missionfail'),
 ('Mission end', r'nextmission'),
 ('Cutscene / scripted anim', r'anim_single|playerlinkto'),
]
rows = []
def walk(name, section, depth, seen):
    if name in seen or name not in funcs or (name in entries and name != section) or depth > 6: return
    seen.add(name)
    f, ln, body = funcs[name]
    for n, l in body:
        s = l.strip()
        for k, rx in kinds:
            if re.search(rx, s, re.I):
                if k == 'Event wait' and re.search(r'waittill\(\s*"(death|damage|goal|trigger)"', s) is None and 'level' not in s and 'player' not in s:
                    pass
                rows.append((section, name, f'{f}:{n}', k, re.sub(r'(common_scripts|maps)\\[a-z_\\]*::', '', s)[:220]))
                break
    for n, l in body:
        for c in re.findall(r'(?:thread\s+)?([A-Za-z_0-9]+)\(', l):
            if c in funcs and c != name: walk(c, section, depth + 1, seen)
for e in entries: walk(e, e, 0, set())
with open(out, 'w') as fo:
    fo.write('Section\tFunction\tSource\tKind\tCode\n')
    for r in rows: fo.write('\t'.join(r) + '\n')
print(len(rows), 'rows')
