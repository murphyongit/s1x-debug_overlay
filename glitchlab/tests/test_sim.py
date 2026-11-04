"""Simulator tests: numbers the game's code and the recorded runs agree on."""
from __future__ import annotations

import math

import numpy as np

from glitchlab.sim.params import Params
from glitchlab.sim.pmove import Cmd, Mover, State
from glitchlab.sim.world import BrushWorld, FlatWorld


def _run(m, st, cmd, frames):
    for _ in range(frames):
        m.step(st, cmd)
    return st


def test_run_and_sprint_speeds():
    m = Mover(FlatWorld(0.0))
    st = _run(m, State(np.zeros(3)), Cmd(127, 0, 0.0), 250)
    assert abs(st.speed2d - 190.0) < 0.5
    st = _run(m, st, Cmd(127, 0, 0.0, sprint=True), 250)
    assert abs(st.speed2d - 266.0) < 0.5


def test_diagonal_run_is_not_faster():
    m = Mover(FlatWorld(0.0))
    st = _run(m, State(np.zeros(3)), Cmd(127, 127, 0.0), 300)
    assert abs(st.speed2d - 190.0) < 0.5


def test_jump_apex_and_airtime():
    m = Mover(FlatWorld(0.0))
    st = State(np.zeros(3))
    zs = []
    for i in range(300):
        m.step(st, Cmd(0, 0, 0.0, jump=(i == 1)))
        zs.append(st.origin[2])
    assert abs(max(zs) - 39.0) < 0.2
    air_ms = sum(1 for z in zs if z > 0.2) * 4      # resting height is 0.125 above the floor
    assert abs(air_ms - 2 * Params().jump_speed / 800 * 1000) < 12
    assert any(e[1] == "land" for e in st.events)


def test_jump_needs_a_fresh_press():
    m = Mover(FlatWorld(0.0))
    st = State(np.zeros(3))
    jumps = 0
    for _ in range(600):
        m.step(st, Cmd(0, 0, 0.0, jump=True))       # held the whole time
    jumps = sum(1 for e in st.events if e[1] == "jump")
    assert jumps == 1


def test_air_strafe_gains_speed_slowly():
    """Holding strafe and turning toward it in the air adds speed (classic strafe jumping), but slowly:
    air accel 1 with wishspeed 190 can't add more than ~190 u/s per second."""
    m = Mover(FlatWorld(-1e6))
    st = State(np.zeros(3), np.array([300.0, 0.0, 0.0]))
    yaw = 0.0
    for _ in range(250):                              # 1 s
        v = st.velocity
        speed = math.hypot(v[0], v[1])
        heading = math.degrees(math.atan2(v[1], v[0]))
        # best wish angle: cos = (190 - 190*dt) / speed, holding D (wish = yaw - 90)
        ang = math.degrees(math.acos(min(1.0, (190 - 190 * 0.004) / speed)))
        yaw = heading - ang + 90
        m.step(st, Cmd(0, 127, yaw))
    gain = st.speed2d - 300
    assert 60 < gain < 190


def test_walls_steps_and_ceilings():
    w = BrushWorld.boxes([((-1000, -1000, -10), (1000, 1000, 0)),     # floor
                          ((100, -100, 0), (200, 100, 16)),            # 16 high step
                          ((300, -100, 0), (310, 100, 200))])          # wall
    m = Mover(w)
    st = State(np.array([0, 0, 0.125]))
    top = 0.0
    for _ in range(500):
        m.step(st, Cmd(127, 0, 0.0))
        top = max(top, st.origin[2])
    assert abs(top - 16.125) < 0.05                   # stepped up the 16-unit step
    assert abs(st.origin[0] - (300 - 16 - 0.125)) < 0.05   # stopped at the wall, box edge against it
    assert st.walking


def test_too_high_step_blocks():
    w = BrushWorld.boxes([((-1000, -1000, -10), (1000, 1000, 0)), ((100, -100, 0), (200, 100, 24))])
    m = Mover(w)
    st = _run(m, State(np.array([0, 0, 0.125])), Cmd(127, 0, 0.0), 300)
    assert st.origin[0] < 100 - 16 + 0.01


def test_slope_walkable_threshold():
    """A brush top tilted past normal z 0.7 can't be stood on: you slide off it."""
    # a 50-degree ramp (normal z ~0.64) as a single triangle pair
    tri1 = ((0, -100, 0), (100, -100, 119.2), (100, 100, 119.2))
    tri2 = ((0, -100, 0), (100, 100, 119.2), (0, 100, 0))
    planes, bounds = [], []
    for t in (tri1, tri2):
        p, b = BrushWorld.slab_from_triangle(t)
        planes.append(p)
        bounds.append(b)
    p, b = BrushWorld.slab_from_triangle(((-500, -500, -1), (500, -500, -1), (500, 500, -1)))
    w = BrushWorld(planes, bounds)
    m = Mover(w)
    st = State(np.array([60.0, 0.0, 120.0]))
    for _ in range(400):
        m.step(st, Cmd(0, 0, 0.0))
    assert not st.walking or st.origin[2] < 80      # never comes to rest standing on the ramp


def test_world_from_full_collision_dump(tmp_path):
    """A floor brush written in the overlay's AWCOL1 format loads as solid ground."""
    import struct
    from glitchlab import data as D
    out = bytearray(b"AWCOL1\0\0") + struct.pack("<I", 0)
    lo, hi = (-500, -500, -10), (500, 500, 0)
    x0, y0, z0 = lo
    x1, y1, z1 = hi
    faces = [  # outward-facing quads of a box
        [(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)],
        [(x0, y0, z0), (x0, y1, z0), (x1, y1, z0), (x1, y0, z0)],
        [(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)],
        [(x0, y1, z0), (x0, y1, z1), (x1, y1, z1), (x1, y1, z0)],
        [(x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0)],
        [(x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1)],
    ]
    out += b"B" + struct.pack("<IH", 1, len(faces))
    for f in faces:
        out += struct.pack("<HB", 0xFFFF, len(f))
        for v in f:
            out += struct.pack("<3f", *v)
    out += b"E"
    p = tmp_path / "c.bin"
    p.write_bytes(bytes(out))
    full = D.load_collision_full(p)
    assert full.brushes == [(1, 0, 6)]
    w = BrushWorld.from_full_collision(full)
    m = Mover(w)
    st = State(np.array([0.0, 0.0, 50.0]))
    for _ in range(300):
        m.step(st, Cmd(0, 0, 0.0))
    assert st.walking and abs(st.origin[2] - 0.125) < 0.01
