"""Movement constants, each with where it came from.

"exe" = read from the game's movement code (module dump of the S1x SP exe, addresses noted).
"logs" = measured from recorded Atlas runs (see validate.py), and agrees with the exe where both exist.
"""
from __future__ import annotations

from dataclasses import dataclass, field, asdict


@dataclass
class Params:
    gravity: float = 800.0            # logs: 97 clean jump arcs, median 799.9 (ps->gravity, g_gravity default)
    jump_height: float = 39.0         # logs: apex median 39.0; jump speed = sqrt(2 * gravity * height) = 249.8
    speed: float = 190.0              # logs: steady run 190.3 (ps->speed / g_speed)
    sprint_scale: float = 1.4         # logs: steady sprint 266.3 / 190
    sprint_strafe_scale: float = 0.667  # exe 0x1406ebd58: right-move scaled while sprinting (PM_WalkMove)
    crouch_scale: float = 0.65        # exe 0x1406b09e8 (PM_CmdScale_Walk stance scale); logs ~123 u/s
    prone_scale: float = 0.15         # exe 0x140692414
    accel_stand: float = 9.0          # exe 0x140692314 (PM_WalkMove)
    accel_crouch: float = 12.0        # exe 0x1406a98b0
    accel_prone: float = 19.0         # exe 0x1406abbfc
    air_accel: float = 1.0            # exe PM_AirMove default 1.0 (2.5 / 4.3 only in a special ps state)
    accel_min_control: float = 100.0  # exe PM_Accelerate: accel * dt * max(wishspeed, 100)
    friction: float = 5.5             # exe 0x1406ebd70 (PM_Friction)
    stopspeed: float = 100.0          # exe 0x1406908e4 (control = max(speed, 100))
    hard_landing_friction: float = 0.3  # exe 0x1406929f0 (PMF 0x80)
    hard_landing_accel: float = 0.25    # exe 0x140690688
    step_size: float = 18.0           # IW default jump_stepSize (string absent in SP exe; standard value)
    walkable_nz: float = 0.7          # exe 0x140690b54
    bounce_min_nz: float = 0.3        # exe PM_StepSlideMove step-down revert rule (0x1406929f0)
    overclip: float = 1.001           # exe PM_AirMove ground clip uses 1 + 0.001
    half_width: float = 16.0          # 32 x 32 hull
    height_stand: float = 72.0
    height_crouch: float = 50.0
    height_prone: float = 30.0
    fall_damage_min: float = 200.0    # exe bg_fallDamageMinHeight
    fall_damage_max: float = 350.0    # exe bg_fallDamageMaxHeight
    frame_ms: int = 4                 # simulated client frame (game clock in logs ticks in 1 ms steps)
    notes: dict = field(default_factory=dict)

    @property
    def jump_speed(self) -> float:
        return (2.0 * self.gravity * self.jump_height) ** 0.5

    def as_dict(self) -> dict:
        d = asdict(self)
        d.pop("notes", None)
        return d
