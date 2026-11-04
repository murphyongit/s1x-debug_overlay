"""Player movement, one client frame at a time, following the game's movement code.

Structure and order match the S1x SP exe (PmoveSingle at 0x1403cc6e0):
  ground check -> jump check -> PM_WalkMove (0x1403cbdf0) or PM_AirMove (0x1403c4180) -> ground check.
PM_Accelerate (0x1403c3f60) is the IW version: accel * dt * max(wishspeed, 100), capped at the speed still
missing along the wish direction. PM_Friction (0x1403c6b50): drop = max(speed, 100) * 5.5 * dt on ground.
Slide/step moves are Quake 3's PM_SlideMove / PM_StepSlideMove with the IW step-down rule (0x1403ce830):
after stepping, landing on a surface you can't stand on cancels the step if you jumped, or if it is
steeper than normal z 0.3.

Not modelled yet (needs more exe reading or in-game tests): sliding (crouch while sprinting), mantling,
ladders, exo abilities (boost jump, dash, overdrive speed), knockback, swimming.
"""
from __future__ import annotations

import math
from dataclasses import dataclass, field, replace

import numpy as np

from .params import Params
from .world import World

STAND, CROUCH, PRONE = 0, 1, 2


@dataclass
class Cmd:
    """One frame of input, like the game's usercmd: moves are -127..127 (keyboard = full)."""
    forward: int = 0
    right: int = 0
    yaw: float = 0.0           # degrees, game convention (0 = +x, 90 = +y)
    jump: bool = False         # held this frame (a jump needs a fresh press)
    sprint: bool = False
    stance: int = STAND
    msec: int = 4


@dataclass
class State:
    origin: np.ndarray
    velocity: np.ndarray = field(default_factory=lambda: np.zeros(3))
    walking: bool = False                      # on ground you can stand on
    ground_normal: np.ndarray | None = None    # set when touching any ground (even too steep)
    jumped: bool = False                       # PMF_JUMPING: set on jump, cleared on landing
    jump_held: bool = False
    stance: int = STAND
    fall_start_z: float | None = None          # highest point since leaving the ground
    time_ms: int = 0
    events: list = field(default_factory=list)  # (time_ms, kind, data)

    def copy(self) -> "State":
        return replace(self, origin=self.origin.copy(), velocity=self.velocity.copy(),
                       ground_normal=None if self.ground_normal is None else self.ground_normal.copy(),
                       events=[])

    @property
    def speed2d(self) -> float:
        return float(math.hypot(self.velocity[0], self.velocity[1]))


def clip_velocity(v: np.ndarray, n: np.ndarray, overbounce: float) -> np.ndarray:
    backoff = float(np.dot(v, n))
    backoff = backoff * overbounce if backoff < 0 else backoff / overbounce
    return v - n * backoff


def _height(P: Params, stance: int) -> float:
    return (P.height_stand, P.height_crouch, P.height_prone)[stance]


def _accelerate(st: State, wishdir, wishspeed, accel, dt, P: Params):
    current = float(np.dot(st.velocity, wishdir))
    add = wishspeed - current
    if add <= 0:
        return
    a = accel * dt * max(wishspeed, P.accel_min_control)
    st.velocity += wishdir * min(a, add)


def _friction(st: State, dt, P: Params):
    v = st.velocity.copy()
    if st.walking:
        v[2] = 0.0
    speed = float(np.linalg.norm(v))
    if speed < 1.0:
        st.velocity[0] = st.velocity[1] = 0.0
        if st.walking:
            st.velocity[2] = 0.0
        return
    drop = 0.0
    if st.walking:
        control = max(speed, P.stopspeed)
        drop = control * P.friction * dt
    new = max(0.0, speed - drop) / speed
    st.velocity *= new


def _axes(yaw_deg: float):
    y = math.radians(yaw_deg)
    fwd = np.array([math.cos(y), math.sin(y), 0.0])
    right = np.array([math.sin(y), -math.cos(y), 0.0])
    return fwd, right


def _cmd_scale(cmd: Cmd, speed: float) -> float:
    f, r = cmd.forward, cmd.right
    mx = max(abs(f), abs(r))
    if mx == 0:
        return 0.0
    total = math.sqrt(f * f + r * r)
    return speed * mx / (127.0 * total)


class Mover:
    def __init__(self, world: World, params: Params | None = None):
        self.world = world
        self.P = params or Params()

    # ------------------------------------------------------------------ traces
    def _trace(self, st: State, start, end):
        return self.world.trace(start, end, self.P.half_width, _height(self.P, st.stance))

    def ground_trace(self, st: State):
        end = st.origin - np.array([0, 0, 0.25])
        tr = self._trace(st, st.origin, end)
        if tr.fraction == 1.0 or tr.allsolid:
            st.walking = False
            st.ground_normal = None
            return
        # moving up off the surface (jumping): not on ground
        if st.velocity[2] > 0 and float(np.dot(st.velocity, tr.normal)) > 10:
            st.walking = False
            st.ground_normal = None
            return
        st.ground_normal = tr.normal
        st.walking = tr.normal[2] >= self.P.walkable_nz

    # ------------------------------------------------------------------ moves
    def slide_move(self, st: State, dt: float, gravity: bool) -> bool:
        P = self.P
        v = st.velocity.copy()
        end_v = v.copy()
        if gravity:
            end_v[2] -= P.gravity * dt
            v[2] = (v[2] + end_v[2]) * 0.5
            if st.ground_normal is not None:
                v = clip_velocity(v, st.ground_normal, P.overclip)
        planes = []
        if st.ground_normal is not None:
            planes.append(st.ground_normal)
        sp = np.linalg.norm(v)
        if sp > 1e-9:
            planes.append(v / sp)
        time_left = dt
        bump = 0
        for bump in range(4):
            end = st.origin + v * time_left
            tr = self._trace(st, st.origin, end)
            if tr.allsolid:
                v[2] = 0.0
                st.velocity = v
                return True
            if tr.fraction > 0:
                st.origin = tr.endpos
            if tr.fraction == 1.0:
                break
            time_left -= time_left * tr.fraction
            if len(planes) >= 5:
                st.velocity = np.zeros(3)
                return True
            if any(np.dot(tr.normal, p) > 0.99 for p in planes):
                v = v + tr.normal
                continue
            planes.append(tr.normal)
            for i, p in enumerate(planes):
                if np.dot(v, p) >= 0.1:
                    continue
                cv = clip_velocity(v, p, P.overclip)
                ce = clip_velocity(end_v, p, P.overclip)
                for j, p2 in enumerate(planes):
                    if j == i or np.dot(cv, p2) >= 0.1:
                        continue
                    cv = clip_velocity(cv, p2, P.overclip)
                    ce = clip_velocity(ce, p2, P.overclip)
                    if np.dot(cv, p) >= 0:
                        continue
                    dr = np.cross(p, p2)
                    ln = np.linalg.norm(dr)
                    if ln < 1e-9:
                        continue
                    dr /= ln
                    cv = dr * float(np.dot(dr, v))
                    ce = dr * float(np.dot(dr, end_v))
                    for k, p3 in enumerate(planes):
                        if k in (i, j) or np.dot(cv, p3) >= 0.1:
                            continue
                        st.velocity = np.zeros(3)
                        return True
                v, end_v = cv, ce
                break
        st.velocity = end_v if gravity else v
        return bump != 0

    def step_slide_move(self, st: State, dt: float, gravity: bool):
        P = self.P
        start_o, start_v = st.origin.copy(), st.velocity.copy()
        if not self.slide_move(st, dt, gravity):
            return
        down_o, down_v = st.origin.copy(), st.velocity.copy()
        tr = self._trace(st, start_o, start_o - np.array([0, 0, P.step_size]))
        if start_v[2] > 0 and (tr.fraction == 1.0 or tr.normal[2] < P.walkable_nz):
            return                                       # no stepping up while still rising
        up = self._trace(st, start_o, start_o + np.array([0, 0, P.step_size]))
        if up.allsolid:
            return
        step = up.endpos[2] - start_o[2]
        st.origin, st.velocity = up.endpos.copy(), start_v.copy()
        self.slide_move(st, dt, gravity)
        tr = self._trace(st, st.origin, st.origin - np.array([0, 0, step]))
        if tr.fraction < 1.0 and tr.normal[2] < P.walkable_nz and (st.jumped or tr.normal[2] < P.bounce_min_nz):
            st.origin, st.velocity = down_o, down_v      # IW: no step onto an unwalkable surface here
            return
        if not tr.allsolid:
            st.origin = tr.endpos
        if tr.fraction < 1.0:
            st.velocity = clip_velocity(st.velocity, tr.normal, P.overclip)
        # keep whichever went further horizontally (the step can only help)
        if np.hypot(*(down_o - start_o)[:2]) > np.hypot(*(st.origin - start_o)[:2]) + 1e-6:
            st.origin, st.velocity = down_o, down_v

    def walk_move(self, st: State, cmd: Cmd, dt: float):
        P = self.P
        _friction(st, dt, P)
        fwd, right = _axes(cmd.yaw)
        n = st.ground_normal
        fwd = clip_velocity(fwd, n, P.overclip)
        right = clip_velocity(right, n, P.overclip)
        fwd /= max(np.linalg.norm(fwd), 1e-9)
        right /= max(np.linalg.norm(right), 1e-9)
        fm, rm = float(cmd.forward), float(cmd.right)
        sprinting = cmd.sprint and cmd.forward > 0 and st.stance == STAND
        if sprinting:
            rm *= P.sprint_strafe_scale
        c2 = Cmd(int(fm), int(rm))
        speed = P.speed * (P.sprint_scale if sprinting else 1.0)
        speed *= (1.0, P.crouch_scale, P.prone_scale)[st.stance]
        wishvel = fwd * fm + right * rm
        wl = np.linalg.norm(wishvel)
        if wl > 1e-9:
            wishdir = wishvel / wl
            wishspeed = _cmd_scale(c2, speed) * wl
            accel = (P.accel_stand, P.accel_crouch, P.accel_prone)[st.stance]
            _accelerate(st, wishdir, wishspeed, accel, dt, P)
        # keep speed while following the ground slope
        sp = np.linalg.norm(st.velocity)
        st.velocity = clip_velocity(st.velocity, n, P.overclip)
        nl = np.linalg.norm(st.velocity)
        if nl > 1e-9:
            st.velocity *= sp / nl
        if st.velocity[0] == 0 and st.velocity[1] == 0:
            return
        self.step_slide_move(st, dt, gravity=False)

    def air_move(self, st: State, cmd: Cmd, dt: float):
        P = self.P
        _friction(st, dt, P)
        fwd, right = _axes(cmd.yaw)
        wishvel = fwd * cmd.forward + right * cmd.right
        wl = np.linalg.norm(wishvel)
        if wl > 1e-9:
            wishdir = wishvel / wl
            wishspeed = _cmd_scale(cmd, P.speed) * wl
            _accelerate(st, wishdir, wishspeed, P.air_accel, dt, P)
        if st.ground_normal is not None:
            st.velocity = clip_velocity(st.velocity, st.ground_normal, P.overclip)
        self.step_slide_move(st, dt, gravity=True)

    def check_jump(self, st: State, cmd: Cmd) -> bool:
        if not cmd.jump:
            st.jump_held = False
            return False
        if st.jump_held:
            return False
        st.jump_held = True
        if not st.walking or st.stance == PRONE:
            return False
        st.velocity[2] = self.P.jump_speed
        st.walking = False
        st.ground_normal = None
        st.jumped = True
        st.fall_start_z = float(st.origin[2])
        st.events.append((st.time_ms, "jump", st.origin.round(1).tolist()))
        return True

    # ------------------------------------------------------------------ one frame
    def step(self, st: State, cmd: Cmd) -> State:
        dt = cmd.msec / 1000.0
        st.stance = cmd.stance
        was_ground = st.walking
        self.ground_trace(st)
        if not st.walking and was_ground and st.fall_start_z is None:
            st.fall_start_z = float(st.origin[2])
        self.check_jump(st, cmd)
        if st.walking:
            self.walk_move(st, cmd, dt)
        else:
            self.air_move(st, cmd, dt)
        self.ground_trace(st)
        if not st.walking:
            z = float(st.origin[2])
            st.fall_start_z = z if st.fall_start_z is None else max(st.fall_start_z, z)
        elif st.fall_start_z is not None:
            fell = st.fall_start_z - float(st.origin[2])
            if fell > self.P.fall_damage_min:
                st.events.append((st.time_ms, "fall_damage", round(fell, 1)))
            st.events.append((st.time_ms, "land", round(fell, 1)))
            st.fall_start_z = None
            st.jumped = False
        st.time_ms += cmd.msec
        return st

    def run(self, st: State, cmds) -> list[State]:
        out = []
        for c in cmds:
            self.step(st, c)
            out.append(st.copy())
        return out
