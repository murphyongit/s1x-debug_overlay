"""Tests on small synthetic files (no game data needed). Run:  python -m pytest -q"""
from __future__ import annotations

import math
import struct
from pathlib import Path

import pytest

from glitchlab import data as D
from glitchlab.db import Store
from glitchlab.miners import MineContext, geometry, logs, scripts, triggers


# ------------------------------------------------------------------ data loaders
def test_run_log_parses_samples_triggers_and_reloads(tmp_path: Path):
    p = tmp_path / "recovery_x.txt"
    p.write_text(
        "0.000 RUN tool version 1.26  map recovery\n"
        "0.000 TRIG #1714 trigger_multiple_flag_set name=- flags=outro_start at 1506 -2115 -10456 size 903 936 178\n"
        "0.100 P 1 2 3 4 5 0x10 0 60 1000\n"
        "0.200 P 0 0 0 0 0\n"            # reload moment: zeros are skipped
        "0.300 RELOAD checkpoint\n"
        "0.400 SNOTIFY outro_start\n")
    lg = D.load_run_log(p)
    assert lg.map == "recovery" and lg.tool_version == "1.26"
    assert len(lg.samples) == 1 and lg.samples[0].game_ms == 1000 and lg.samples[0].flags == 0x10
    t = lg.triggers[1714]
    assert t.flags == "outro_start" and t.size == (903.0, 936.0, 178.0) and t.mid[2] == -10456.0
    assert lg.reloads == [0.3]
    assert lg.notifies("outro_start") == [0.4]


def test_map_ents(tmp_path: Path):
    p = tmp_path / "ents.txt"
    p.write_text('{\n171 "trigger_multiple_flag_set"\n742 "1506 -2115 -456"\n31182 "outro_start"\n'
                 '31188 "training_enable_end"\n}\n{\n171 "worldspawn"\n}\n')
    ents = D.load_map_ents(p)
    assert len(ents) == 2
    e = ents[0]
    assert e.classname == "trigger_multiple_flag_set" and e.origin == (1506, -2115, -456)
    assert e.script_flag == "outro_start" and e.flag_true == "training_enable_end"


def _write_awcol(path: Path):
    out = bytearray(b"AWCOL1\0\0")
    out += struct.pack("<I", 1) + struct.pack("<II", 1, 0) + b"clip_metal".ljust(64, b"\0")
    # one brush, one 45-degree face (normal z ~0.707 is walkable-ish; use a 0.5 slope instead)
    face = [(0, 0, 0), (100, 0, 0), (100, 100, 173.2), (0, 100, 173.2)]
    out += b"B" + struct.pack("<IH", 1, 1) + struct.pack("<HB", 0, len(face))
    for v in face:
        out += struct.pack("<3f", *v)
    out += b"T" + struct.pack("<HIh", 0xFFFF, 1, -1) + struct.pack("<9f", 0, 0, 0, 1, 0, 0, 0, 1, 0)
    out += b"E"
    path.write_bytes(bytes(out))


def test_collision_full_roundtrip(tmp_path: Path):
    p = tmp_path / "collision_full_recovery.bin"
    _write_awcol(p)
    full = D.load_collision_full(p)
    assert full.materials[0].name == "clip_metal" and full.brush_count == 1
    assert len(full.brush_faces) == 1 and len(full.brush_faces[0][2]) == 4
    assert len(full.triangles) == 1 and full.triangles[0][0] == 0xFFFF


def test_collision_full_rejects_other_files(tmp_path: Path):
    p = tmp_path / "x.bin"
    p.write_bytes(b"NOTAWCOL")
    with pytest.raises(ValueError):
        D.load_collision_full(p)


# ------------------------------------------------------------------ database rules
def test_experiments_are_append_only():
    s = Store()
    e = s.add_experiment("recovery", "manual", hypothesis="h", inputs={"keys": "w"})
    s.set_outcome(e, "negative")
    with pytest.raises(ValueError):
        s.set_outcome(e, "positive")          # outcomes are never rewritten
    child = s.derive_experiment(e, hypothesis="h2")
    row = s.con.execute("SELECT parent_id, hypothesis, outcome FROM experiments WHERE id=?", (child,)).fetchone()
    assert row == (e, "h2", None)
    assert s.con.execute("SELECT hypothesis FROM experiments WHERE id=?", (e,)).fetchone()[0] == "h"


def test_discovery_upsert_dedupes_and_stages():
    s = Store()
    a = s.upsert_discovery("recovery", "movement", "x", "fp1", score=10)
    b = s.upsert_discovery("recovery", "movement", "x", "fp1", score=20)
    assert a == b and len(s.discoveries("recovery")) == 1 and s.discoveries()[0]["score"] == 20
    with pytest.raises(ValueError):
        s.set_stage(a, "works_trust_me")
    with pytest.raises(ValueError):
        s.upsert_discovery("recovery", "nonsense", "y", "fp2")


# ------------------------------------------------------------------ miners
def test_void_fall_check_numbers():
    # Atlas end cube from the infinite-fall roof
    r = triggers.void_fall_check((-2704, -293, 158), (1506, -2115, -10456), (903, 936, 178))
    top = -10456 + 89
    t = math.sqrt(2 * (158 - top) / 800)
    assert r["seconds_to_box_top"] == round(t, 2)
    assert r["fall_speed_at_box"] == round(800 * t)
    assert 3900 < r["sideways_units_to_box_edge"] < 4050       # nearest box edge, minus half the player
    assert r["min_fps_to_register"] == math.ceil(800 * t / (178 + 72))


def _ents_with_parked_end():
    return [D.MapEntity(0, {D.KEY_CLASSNAME: "trigger_multiple_flag_set", D.KEY_ORIGIN: "1506 -2115 -456",
                            D.KEY_SCRIPT_FLAG: "outro_start", D.KEY_SCRIPT_FLAG_TRUE: "training_enable_end"}),
            D.MapEntity(1, {D.KEY_CLASSNAME: "trigger_multiple_flag_set", D.KEY_ORIGIN: "0 0 0",
                            D.KEY_SCRIPT_FLAG: "left_encounter_fail", D.KEY_SCRIPT_FLAG_TRUE: "x"})]


def test_parked_triggers_ranks_mission_end_first():
    ctx = MineContext("recovery", map_ents=_ents_with_parked_end(),
                      known_spots={"void_entry": [-2704, -293, 158]})
    found = triggers.parked_triggers(ctx)
    assert found[0].evidence["parked_position"] == (1506, -2115, -10456)
    best = max(found, key=lambda f: f.score)
    assert "outro_start" in best.title and best.score >= 98
    fail = next(f for f in found if "left_encounter" in f.title)
    assert fail.score < 10


def _gsc(tmp_path: Path) -> Path:
    d = tmp_path / "maps"
    d.mkdir()
    (d / "testmap_code.gsc").write_text(
        "outro()\n{\n    common_scripts\\utility::flag_wait( \"outro_start\" );\n"
        "    maps\\_utility::nextmission();\n}\n\n"
        "ending_scene()\n{\n    common_scripts\\utility::flag_set( \"obj_done\" );\n"
        "    maps\\_shg_utility::setup_player_for_scene( 1 );\n    level.player playerlinkto( var_0 );\n}\n\n"
        "teleport()\n{\n    level.player setorigin( ( 0, 0, 0 ) );\n}\n")
    (d / "testmap_b_code.gsc").write_text("other()\n{\n    maps\\_utility::nextmission();\n}\n")
    return d


def test_script_miners(tmp_path: Path):
    ctx = MineContext("testmap", map_ents=_ents_with_parked_end(), scripts_dir=_gsc(tmp_path))
    ends = scripts.end_chains(ctx)
    end = next(f for f in ends if "outro_start" in f.title)
    assert end.score == 95 and end.location == [1506, -2115, -10456]
    dp = scripts.delayable_prompts(ctx)
    assert len(dp) == 1 and dp[0].evidence["flags_set_before_wait"] == ["obj_done"]
    moves = scripts.player_moving_scripts(ctx)
    assert {f.title.split(": ")[1] for f in moves} == {"ending_scene", "teleport"}


def test_mission_prefixes_dont_overlap(tmp_path: Path, monkeypatch):
    d = _gsc(tmp_path)
    monkeypatch.setattr(scripts, "MISSIONS", ("testmap_b", "testmap"))
    scripts._index.cache_clear()
    names = {f.name for f in scripts._index(str(d), "testmap")}
    assert "other" not in names and "outro" in names
    scripts._index.cache_clear()


def test_steep_slope_found_and_aimed(tmp_path: Path):
    p = tmp_path / "c.bin"
    _write_awcol(p)
    full = D.load_collision_full(p)
    ctx = MineContext("recovery", full=full, known_spots={"target": [50, -5000, 0]})
    s = geometry.steep_slopes(ctx)
    assert len(s) == 1
    ev = s[0].evidence
    assert abs(ev["normal_z"] - 0.5) < 0.01
    assert ev["downhill_dir"][1] < -0.99           # slope faces -y, toward the target
    assert ev["points_toward_target"] > 0.99


def test_speed_records_flags_high_jumps():
    samples = []
    t = 0.0
    z = 0.0
    for i in range(40):
        if 10 <= i < 16:
            z += 25           # rises 150 units at 250 u/s: not noclip-fast, much higher than a jump
        elif 16 <= i < 22:
            z -= 25
        samples.append(D.Sample(t, i * 20.0, 0, z, 0, 0, game_ms=int(t * 1000)))
        t += 0.1
    lg = D.RunLog(Path("fake.txt"), samples=samples)
    out = logs.speed_records(MineContext("recovery", logs=[lg]))
    assert out and "rose 150" in out[0].title
