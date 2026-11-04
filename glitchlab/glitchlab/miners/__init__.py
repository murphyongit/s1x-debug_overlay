"""Static "glitch miners": scanners that look through saved game data for spots worth testing.

Each miner takes a MineContext and returns Finding objects. A finding is a *candidate* only - nothing here
claims a glitch works; it says where to look and why, with the evidence from the data.
"""
from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass, field
from pathlib import Path

from .. import data as D


@dataclass
class Finding:
    miner: str
    category: str               # one of db.CATEGORIES
    title: str
    score: float                # 0..100 priority (rough; for ordering only)
    evidence: dict = field(default_factory=dict)
    test_plan: str = ""
    location: list[float] | None = None
    needs: str = ""             # what data or test is still missing, if anything

    @property
    def fingerprint(self) -> str:
        key = json.dumps([self.miner, self.title, self.location], sort_keys=True)
        return hashlib.sha1(key.encode()).hexdigest()


@dataclass
class MineContext:
    map: str
    map_ents: list[D.MapEntity] = field(default_factory=list)
    logs: list[D.RunLog] = field(default_factory=list)
    walls: list[D.WallPiece] = field(default_factory=list)
    full: D.FullCollision | None = None
    movement: dict[str, float] = field(default_factory=dict)
    scripts_dir: Path | None = None       # decompiled GSC "maps" folder
    known_spots: dict[str, list[float]] = field(default_factory=dict)   # user-reported places, e.g. void entry

    def trigger_sizes(self) -> dict[tuple[int, int], tuple[float, float, float]]:
        """(round x, round y) of each trigger's box centre at mission start -> box size, from any run log."""
        out = {}
        for log in self.logs:
            for t in log.triggers.values():
                if t.size:
                    out[(round(t.mid[0]), round(t.mid[1]))] = t.size
        return out


def script_miners():
    """The miners that only need the decompiled scripts (run for every mission with `scripts --all`)."""
    from . import scripts
    return [scripts.end_chains, scripts.delayable_prompts, scripts.npc_speed_rules, scripts.player_moving_scripts]


def all_miners():
    from . import scripts, triggers, geometry, logs
    return [triggers.parked_triggers, triggers.fail_trigger_thickness, scripts.end_chains,
            scripts.delayable_prompts, scripts.npc_speed_rules, scripts.player_moving_scripts,
            logs.checkpoint_positions, logs.speed_records, geometry.steep_slopes, geometry.elevator_seams]
