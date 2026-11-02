#!/usr/bin/env python3
"""Build AW_missions.xlsx: an Index tab linking to each mission's tabs (Overview, Gates, Triggers, Runs).
usage: build_missions_workbook.py <out.xlsx>   (reads tools/missions/*)"""
import os, re, sys
import openpyxl
from openpyxl.styles import Font, PatternFill, Alignment
from openpyxl.worksheet.datavalidation import DataValidation
sys.path.insert(0, os.path.join(os.path.dirname(__file__), 'missions'))
from overviews import MISSIONS, OVERVIEW, SECTION_NAMES, EXTRA

D = os.path.join(os.path.dirname(__file__), 'missions')
F = Font(name='Arial', size=10); FB = Font(name='Arial', size=10, bold=True)
FH = Font(name='Arial', size=10, bold=True, color='FFFFFF'); HF = PatternFill('solid', fgColor='2F3B4C')
WR = Alignment(wrap_text=True, vertical='top'); NOTE = Font(name='Arial', size=9, italic=True)
LINK = Font(name='Arial', size=10, color='0563C1', underline='single')

def head(ws, row, cols, widths=None):
    for i, c in enumerate(cols, 1):
        cell = ws.cell(row, i, c); cell.font = FH; cell.fill = HF; cell.alignment = WR
    if widths:
        for i, w in enumerate(widths, 1): ws.column_dimensions[openpyxl.utils.get_column_letter(i)].width = w

def body(ws, start):
    for row in ws.iter_rows(min_row=start):
        for c in row:
            if c.font != FH and c.font != LINK: c.font = FB if c.font.bold else F
            c.alignment = WR

def back(ws, row=1, col=8):
    c = ws.cell(row, col, '← Index'); c.hyperlink = "#'Index'!A1"; c.font = LINK

wb = openpyxl.Workbook()
idx = wb.active; idx.title = 'Index'
idx['A1'] = 'Advanced Warfare campaign — mission sheets'; idx['A1'].font = Font(name='Arial', size=14, bold=True)
idx['A2'] = 'Click a mission name to jump to it. Each ready mission has 4 tabs: Overview, Gates (every script wait), Triggers (boxes from your recorder logs), Runs (your attempt log).'
idx['A2'].font = NOTE; idx.merge_cells('A2:F2'); idx.row_dimensions[2].height = 28
head(idx, 4, ['#', 'Mission', 'Game file', 'Status', 'Tabs', 'Notes'], [4, 16, 14, 14, 44, 50])

for n, (name, mp) in enumerate(MISSIONS, 1):
    r = 4 + n
    idx.cell(r, 1, n); idx.cell(r, 3, mp)
    ready = mp in OVERVIEW
    c = idx.cell(r, 2, name)
    idx.cell(r, 4, 'Ready' if ready else 'Not started')
    if not ready:
        idx.cell(r, 6, 'Scripts available in the public dump; brief not built yet.')
        continue
    tabs = [f'{name} Overview', f'{name} Gates', f'{name} Triggers', f'{name} Runs']
    c.hyperlink = f"#'{tabs[0]}'!A1"; c.font = LINK
    idx.cell(r, 5, ' · '.join(t.replace(name + ' ', '') for t in tabs))
    for cc in range(1, 7): idx.cell(r, cc).fill = PatternFill('solid', fgColor='D9F2E1')

    ov = OVERVIEW[mp]
    # Overview
    ws = wb.create_sheet(tabs[0]); back(ws, 1, 6)
    ws['A1'] = f'{name} (game file: {mp}) — overview'; ws['A1'].font = Font(name='Arial', size=14, bold=True)
    ws['A2'] = ov['source']; ws['A2'].font = NOTE; ws.merge_cells('A2:E2'); ws.row_dimensions[2].height = 30
    nav = ws.cell(3, 1, 'Go to:'); nav.font = FB
    for i, t in enumerate(tabs[1:], 2):
        l = ws.cell(3, i, t.replace(name + ' ', '')); l.hyperlink = f"#'{t}'!A1"; l.font = LINK
    head(ws, 5, ['Section (in play order)', 'What progress waits on', 'Ally / scripted-walk gates (Will-type bug risk)', 'Speedrun notes / things to test'], [26, 56, 46, 56])
    for i, row in enumerate(ov['rows'], 6):
        for j, v in enumerate(row, 1): ws.cell(i, j, v)
    rr = ws.max_row + 2
    for title, rows in EXTRA.get(mp, []):
        t = ws.cell(rr, 1, title); t.font = FB
        head(ws, rr + 1, ['Part', 'How it ends', 'Skippable?'])
        for i, row in enumerate(rows, rr + 2):
            for j, v in enumerate(row, 1): ws.cell(i, j, v)
        rr = ws.max_row + 2
    body(ws, 6); ws.freeze_panes = 'A6'

    # Gates
    ws = wb.create_sheet(tabs[1]); back(ws, 1, 7)
    head(ws, 1, ['Section', 'Function', 'Source (file:line)', 'Kind', 'Code', 'Note'], [26, 34, 30, 26, 80, 44])
    notes = {'Ally must arrive (anim_reach)': 'Ally must physically arrive. Shared "arrived" signal — same bug class as Will in Induction.',
             'Kill gate (clear enemies)': 'Progress waits on enemies dying.', 'Player trigger': 'You must touch/use something.',
             'Mission fail': 'Fail condition.', 'Teleport': 'Ally/you moved instantly.', 'Mission end': 'Split end.'}
    for line in list(open(os.path.join(D, f'{mp}_gates.tsv')))[1:]:
        sec, fn, src, kind, code = line.rstrip('\n').split('\t')
        if kind == 'Fixed timer':
            m = re.search(r'wait[ (]\s*([0-9.]+)', code)
            if m and float(m.group(1)) < 1.0: continue
        ws.append([SECTION_NAMES.get(mp, {}).get(sec, sec), fn, src, kind, code, notes.get(kind, '')])
    body(ws, 2); ws.freeze_panes = 'A2'; ws.auto_filter.ref = f'A1:F{ws.max_row}'

    # Triggers
    ws = wb.create_sheet(tabs[2]); back(ws, 1, 12)
    head(ws, 1, ['Slot', 'Class', 'Name', 'Flags', 'Centre x', 'Centre y', 'Centre z', 'Size x', 'Size y', 'Size z', 'Category'], [7, 40, 34, 60, 10, 10, 10, 9, 9, 9, 14])
    src_log = None
    for l in open(os.path.join(D, f'{mp}_trig.txt')):
        m = re.match(r'\S+ TRIG #(\d+) (\S+) name=(\S+)(?: flags=(.*?))? at (-?\d+) (-?\d+) (-?\d+)(?: size (\d+) (\d+) (\d+))?', l.strip())
        if not m: continue
        slot, cls, nm, flags, x, y, z, sx, sy, sz = m.groups(); flags = flags or ''
        if 'script_brushmodel' in cls and nm == '-': continue
        cat = ('Fail' if re.search(r'left_encounter|board_fail|left_map|left_squad', flags) else
               'Warning' if re.search(r'warn|leaving_map|leaving_squad', flags) else
               'Use prompt' if 'trigger_use' in cls else 'Flag' if flags else 'Other')
        ws.append([int(slot), cls, nm, flags, int(x), int(y), int(z), int(sx) if sx else None, int(sy) if sy else None, int(sz) if sz else None, cat])
    body(ws, 2)
    for r2 in range(2, ws.max_row + 1):
        cat = ws.cell(r2, 11).value
        if cat in ('Fail', 'Warning'):
            for cc in range(1, 12): ws.cell(r2, cc).fill = PatternFill('solid', fgColor='FBE0DE' if cat == 'Fail' else 'FFF1CC')
    ws.freeze_panes = 'A2'; ws.auto_filter.ref = f'A1:K{ws.max_row}'
    ws.cell(ws.max_row + 2, 1, 'Source: TRIG list from your recorder log at mission start. Positions are box centres in game units; z near -8000 to -10000 = switched off / parked until a script turns it on.').font = NOTE

    # Runs
    ws = wb.create_sheet(tabs[3]); back(ws, 1, 11)
    ws['A1'] = 'One row per attempt. Blue cells are yours to edit; the grey row shows the format.'; ws['A1'].font = NOTE
    head(ws, 2, ['Date', 'Attempt #', 'Route / what I tried', 'Checkpoint 1 (time)', 'Checkpoint 2 (time)', 'Checkpoint 3 (time)', 'Final time', 'Result', 'Notes'], [12, 9, 34, 14, 14, 14, 11, 16, 44])
    ex = ['2026-11-03', 1, 'Example: normal route', '0:52', '3:10', '6:05', '9:20', 'Finished', 'Example row — replace']
    for j, v in enumerate(ex, 1): ws.cell(3, j, v).font = Font(name='Arial', size=10, italic=True, color='808080')
    for r2 in range(4, 104):
        for cc in range(1, 10): ws.cell(r2, cc).font = Font(name='Arial', size=10, color='0000FF')
    dv = DataValidation(type='list', formula1='"Finished,Restarted,Mission failed,Crashed"', allow_blank=True)
    ws.add_data_validation(dv); dv.add('H4:H103'); ws.freeze_panes = 'A3'

idx.freeze_panes = 'A5'
body(idx, 5)
for n, (name, mp) in enumerate(MISSIONS, 1):
    if mp in OVERVIEW: idx.cell(4 + n, 2).font = LINK
wb.save(sys.argv[1]); print('saved', sys.argv[1], len(wb.sheetnames), 'tabs')
