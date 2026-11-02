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
idx['A2'] = 'Click a mission name to jump to its tab. Each mission is one tab: Overview, Run log, Script gates and Triggers, with jump links at the top.'
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
    tab = name
    c.hyperlink = f"#'{tab}'!A1"; c.font = LINK
    idx.cell(r, 5, 'Overview · Run log · Script gates · Triggers (one tab)')
    for cc in range(1, 7): idx.cell(r, cc).fill = PatternFill('solid', fgColor='D9F2E1')
    ov = OVERVIEW[mp]
    ws = wb.create_sheet(tab)
    for i, w in enumerate([26, 34, 46, 56, 60, 40, 12, 14, 40], 1):
        ws.column_dimensions[openpyxl.utils.get_column_letter(i)].width = w
    ws['A1'] = f'{name} (game file: {mp})'; ws['A1'].font = Font(name='Arial', size=14, bold=True)
    back(ws, 1, 6)
    ws['A2'] = ov['source']; ws['A2'].font = NOTE; ws.merge_cells('A2:E2'); ws.row_dimensions[2].height = 30
    ws.cell(3, 1, 'Jump to:').font = FB
    anchors = {}
    def section(title):
        rr = ws.max_row + 2
        cell = ws.cell(rr, 1, title); cell.font = Font(name='Arial', size=12, bold=True)
        anchors[title] = rr
        return rr + 1
    # Overview
    rr = section('Overview')
    head(ws, rr, ['Section (in play order)', 'What progress waits on', 'Ally / scripted-walk gates (Will-type bug risk)', 'Speedrun notes / things to test'])
    for i, row in enumerate(ov['rows'], rr + 1):
        for j, v in enumerate(row, 1): ws.cell(i, j, v)
    for title, rows in EXTRA.get(mp, []):
        rr = section(title)
        head(ws, rr, ['Part', 'How it ends', 'Skippable?'])
        for i, row in enumerate(rows, rr + 1):
            for j, v in enumerate(row, 1): ws.cell(i, j, v)
    body(ws, 4)
    # Run log
    rr = section('Run log')
    ws.cell(rr, 1, 'One row per attempt. Blue cells are yours to edit; the grey row shows the format.').font = NOTE
    head(ws, rr + 1, ['Date', 'Attempt #', 'Route / what I tried', 'Checkpoint 1 (time)', 'Checkpoint 2 (time)', 'Checkpoint 3 (time)', 'Final time', 'Result', 'Notes'])
    ex = ['2026-11-03', 1, 'Example: normal route', '0:52', '3:10', '6:05', '9:20', 'Finished', 'Example row — replace']
    for j, v in enumerate(ex, 1): ws.cell(rr + 2, j, v).font = Font(name='Arial', size=10, italic=True, color='808080')
    for r2 in range(rr + 3, rr + 53):
        for cc in range(1, 10): ws.cell(r2, cc).font = Font(name='Arial', size=10, color='0000FF')
    dv = DataValidation(type='list', formula1='"Finished,Restarted,Mission failed,Crashed"', allow_blank=True)
    ws.add_data_validation(dv); dv.add(f'H{rr + 3}:H{rr + 52}')
    ws.cell(rr + 52, 1, ' ')
    # Gates
    rr = section('Script gates')
    ws.cell(rr, 1, 'Every point where the scripts wait. Kinds: kill gate, ally must arrive, player trigger, flag/event wait, fixed timer, teleport, fail, end.').font = NOTE
    head(ws, rr + 1, ['Section', 'Function', 'Source (file:line)', 'Kind', 'Code', 'Note'])
    notes = {'Ally must arrive (anim_reach)': 'Ally must physically arrive. Shared "arrived" signal — same bug class as Will in Induction.',
             'Kill gate (clear enemies)': 'Progress waits on enemies dying.', 'Player trigger': 'You must touch/use something.',
             'Mission fail': 'Fail condition.', 'Teleport': 'Ally/you moved instantly.', 'Mission end': 'Split end.'}
    g0 = ws.max_row + 1
    for line in list(open(os.path.join(D, f'{mp}_gates.tsv')))[1:]:
        sec, fn, src, kind, code = line.rstrip('\n').split('\t')
        if kind == 'Fixed timer':
            m = re.search(r'wait[ (]\s*([0-9.]+)', code)
            if m and float(m.group(1)) < 1.0: continue
        ws.append([SECTION_NAMES.get(mp, {}).get(sec, sec), fn, src, kind, code, notes.get(kind, '')])
    body(ws, g0)
    # Triggers
    rr = section('Triggers')
    ws.cell(rr, 1, 'From your recorder log at mission start. Red = fail box, yellow = warning box. Positions are box centres in game units; z near -8000 to -10000 = parked until a script switches it on.').font = NOTE
    head(ws, rr + 1, ['Category', 'Name', 'Class', 'Flags', 'Centre x y z', 'Size x y z', 'Slot'])
    t0 = ws.max_row + 1
    for l in open(os.path.join(D, f'{mp}_trig.txt')):
        m = re.match(r'\S+ TRIG #(\d+) (\S+) name=(\S+)(?: flags=(.*?))? at (-?\d+) (-?\d+) (-?\d+)(?: size (\d+) (\d+) (\d+))?', l.strip())
        if not m: continue
        slot, cls, nm, flags, x, y, z, sx, sy, sz = m.groups(); flags = flags or ''
        if 'script_brushmodel' in cls and nm == '-': continue
        cat = ('Fail' if re.search(r'left_encounter|board_fail|left_map|left_squad', flags) else
               'Warning' if re.search(r'warn|leaving_map|leaving_squad', flags) else
               'Use prompt' if 'trigger_use' in cls else 'Flag' if flags else 'Other')
        ws.append([cat, nm, cls, flags, f'{x} {y} {z}', f'{sx} {sy} {sz}' if sx else '', int(slot)])
    body(ws, t0)
    for r2 in range(t0, ws.max_row + 1):
        cat = ws.cell(r2, 1).value
        if cat in ('Fail', 'Warning'):
            for cc in range(1, 8): ws.cell(r2, cc).fill = PatternFill('solid', fgColor='FBE0DE' if cat == 'Fail' else 'FFF1CC')
    for i, (title, row) in enumerate(anchors.items(), 2):
        l = ws.cell(3, i, title); l.hyperlink = f"#'{tab}'!A{row}"; l.font = LINK
    ws.freeze_panes = 'A4'

idx.freeze_panes = 'A5'
body(idx, 5)
for n, (name, mp) in enumerate(MISSIONS, 1):
    if mp in OVERVIEW: idx.cell(4 + n, 2).font = LINK
wb.save(sys.argv[1]); print('saved', sys.argv[1], len(wb.sheetnames), 'tabs')
