"""Noclip filtering and the exo-move models."""
from __future__ import annotations

import math
from dataclasses import replace

import numpy as np

from glitchlab.data import Sample
from glitchlab.sim import clean as C
from glitchlab.sim.params import Params
from glitchlab.sim.pmove import Cmd, Mover, State
from glitchlab.sim.world import FlatWorld


def _samples(points, pitch=0.0, flags=0):
    out = []
    for k, (x, y, z) in enumerate(points):
        out.append(Sample(k * 0.1, x, y, z, pitch, 0.0, flags, 2, 60.0, 1000 + k * 100))
    return out


def test_noclip_flight_is_cut_but_jumps_and_slopes_stay():
    pts = []
    # 2 s walking on flat ground
    for k in range(20):
        pts.append((k * 19.0, 0.0, 0.0))
    x0 = pts[-1][0]
    # 2 s of noclip: 570 u/s along a view pitched 30 degrees down -> drops at 285 u/s, no gravity
    for k in range(1, 21):
        pts.append((x0 + k * 57 * math.cos(math.radians(30)), 0.0, -k * 57 * math.sin(math.radians(30))))
    S = _samples(pts, pitch=30.0)
    labs = [iv.label for iv in C.label(S)]
    assert labs[25:38].count("noclip") >= 12

    # a normal jump arc and a walk down a 20-degree slope at run speed: nothing cut
    pts = [(k * 19.0, 0.0, 0.0) for k in range(10)]
    for k in range(1, 7):
        t = k * 0.1
        pts.append((pts[9][0] + k * 19.0, 0.0, 249.8 * t - 400 * t * t))
    for k in range(1, 20):
        pts.append((pts[15][0] + k * 19.0 * math.cos(math.radians(20)), 0.0,
                    pts[15][2] - k * 19.0 * math.sin(math.radians(20))))
    S = _samples(pts, pitch=20.0)
    assert all(iv.label == "ok" for iv in C.label(S))


def test_teleport_and_reload_gaps_are_cut():
    pts = [(k * 19.0, 0.0, 0.0) for k in range(10)] + [(5000.0, 0.0, 0.0)] + \
          [(5000.0 + k * 19, 0.0, 0.0) for k in range(1, 10)]
    S = _samples(pts)
    S[15].game_ms = S[14].game_ms - 4000          # checkpoint reload: clock goes back
    labs = [iv.label for iv in C.label(S)]
    assert labs[9] == "teleport" and labs[14] == "gap"


def _exo_mover():
    P = replace(Params(), boost_enabled=True, hover_enabled=True, dash_enabled=True)
    return Mover(FlatWorld(-1e6), P), P


def test_boost_sets_vertical_speed():
    m, P = _exo_mover()
    st = State(np.zeros(3), np.array([0.0, 0.0, -300.0]))
    m.step(st, Cmd(boost=True))
    assert abs(st.velocity[2] - (P.exo.boost_base + P.exo.boost_carry * -300.0) + 800 * 0.004) < 2
    m.step(st, Cmd(boost=False))
    m.step(st, Cmd(boost=True))                   # second press in the same air time: nothing
    assert st.velocity[2] < 600 and sum(1 for e in st.events if e[1] == "boost") == 1


def test_hover_sinks_slowly_in_a_sawtooth():
    m, P = _exo_mover()
    st = State(np.zeros(3), np.array([200.0, 0.0, -400.0]))
    zs = []
    for _ in range(int(4 / 0.004)):
        m.step(st, Cmd(hover=True))
        zs.append(st.origin[2])
    # after catching the fall, average sink is floor + rise * period / 2 (about -44 u/s)
    sink = (zs[-1] - zs[-1 - 500]) / 2.0
    E = P.exo
    assert abs(sink - (E.hover_floor + E.hover_rise * E.hover_period / 2)) < 3


def test_dash_then_240():
    m, P = _exo_mover()
    st = State(np.zeros(3), np.array([350.0, 0.0, 0.0]))
    m.step(st, Cmd(dash=(0, 127)))
    assert abs(st.speed2d - P.exo.dash_speed) < 1
    for _ in range(int(0.3 / 0.004)):
        m.step(st, Cmd())
    assert abs(st.speed2d - P.exo.dash_end) < 5


def test_atlas_has_dash_but_no_boost_or_hover():
    P = Params.for_mission("recovery")
    assert P.dash_enabled and not P.boost_enabled and not P.hover_enabled
    m = Mover(FlatWorld(-1e6), P)
    st = State(np.zeros(3), np.array([0.0, 0.0, -100.0]))
    m.step(st, Cmd(boost=True, hover=True))
    assert st.velocity[2] < -100
