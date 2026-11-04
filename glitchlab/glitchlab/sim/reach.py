"""How far a jump or fall can carry you, using the simulator in open air (no collision).

Strafing model: hold one strafe key and turn the view each frame to the angle that adds the most speed
(the same thing a good strafe jumper does with the mouse). 'straight' = hold forward, no turning.
"""
from __future__ import annotations

import math

import numpy as np

from .params import Params
from .pmove import Cmd, Mover, State
from .world import EmptyWorld, FlatWorld


def best_strafe_yaw(velocity, P: Params, dt: float, side: int = 1) -> float:
    """View yaw (degrees) that makes the strafe key add the most speed this frame. side 1 = right key."""
    vx, vy = velocity[0], velocity[1]
    speed = math.hypot(vx, vy)
    heading = math.degrees(math.atan2(vy, vx))
    ws = P.speed
    if speed < 1:
        return heading + 90 * side
    c = (ws - P.air_accel * dt * max(ws, P.accel_min_control)) / speed
    ang = math.degrees(math.acos(max(-1.0, min(1.0, c))))
    # the right key pushes toward yaw - 90; aim that push 'ang' degrees off the current heading
    return heading - side * ang + 90 * side


def fly(start_speed: float, drop: float = 0.0, jump: bool = True, strafe: bool = True, P: Params | None = None,
        frame_ms: int = 4, up_speed: float | None = None, max_seconds: float = 30.0) -> dict:
    """Open-air flight from a flat takeoff until you've dropped `drop` units below the takeoff height.

    start_speed: horizontal speed at takeoff (266 sprint, more after a bunny hop chain)."""
    P = P or Params()
    m = Mover(EmptyWorld(), P)
    vz = (P.jump_speed if jump else 0.0) if up_speed is None else up_speed
    st = State(np.zeros(3), np.array([start_speed, 0.0, vz]))
    dt = frame_ms / 1000
    t = 0.0
    peak = 0.0
    side = 1
    while t < max_seconds:
        if strafe:
            # switch strafe side whenever the heading has swung 10 degrees off the takeoff direction, so the
            # path zig-zags forward instead of curving round (what a player does with A/D and the mouse)
            heading = math.degrees(math.atan2(st.velocity[1], st.velocity[0]))
            if heading < -10:
                side = -1
            elif heading > 10:
                side = 1
            yaw = best_strafe_yaw(st.velocity, P, dt, side)
            cmd = Cmd(0, 127 * side, yaw, msec=frame_ms)
        else:
            cmd = Cmd(127, 0, 0.0, msec=frame_ms)
        m.step(st, cmd)
        t += dt
        peak = max(peak, st.origin[2])
        if st.origin[2] <= -drop and st.velocity[2] < 0:
            break
    return {"seconds": round(t, 3), "distance": round(float(np.hypot(st.origin[0], st.origin[1])), 1),
            "end_speed": round(st.speed2d, 1), "peak_height": round(peak, 1),
            "fall_speed": round(float(-st.velocity[2]), 1)}


def jump_table(P: Params | None = None) -> list[dict]:
    """Distances for the takeoff speeds and drops that come up most in routing."""
    rows = []
    for speed in (190, 266, 300, 340, 380):
        for drop in (-30, 0, 50, 150, 300):
            for strafe in (False, True):
                r = fly(speed, drop, True, strafe, P)
                rows.append({"takeoff_speed": speed, "land_height": -drop, "strafe": strafe, **r})
    return rows
