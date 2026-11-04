# glitchlab

A separate tool for finding glitch and route candidates in Call of Duty: Advanced Warfare (S1x) campaign
missions. It reads what the debug overlay already saves and the decompiled mission scripts. It never touches
the game, the game files or the overlay. Nothing it finds is treated as real until it has been reproduced
in the game.

## What it does now (phase 1: static mining)

It runs a set of "miners" over saved data. Each one returns **candidates**, each with:

- a score (rough test priority, 0–100)
- the evidence it is based on
- a test plan

Results go into a SQLite database and a Markdown report.

| Miner | Looks at | Finds |
|---|---|---|
| `parked_triggers` | map entities | triggers switched off at mission start. The game parks these 10000 units down, and touching them there still sets their flag. Each entry includes the fall-speed and FPS numbers for reaching it from a known void entry. |
| `fail_trigger_thickness` | recorder trigger list | fail and out-of-bounds boxes, with the speed needed per FPS to pass through them using the low-FPS trick |
| `end_chains` | scripts and map entities | what the mission-end function waits on, and what sets each of those flags. A flag set by a map trigger is scored highest. |
| `delayable_prompts` | scripts | scenes using the waiting `setup_player_for_scene(1)` (stored-teleport candidates). Includes flags set before the wait. |
| `npc_speed_rules` | scripts | NPC speed rules that depend on the player (like Gideon's catch-up) |
| `player_moving_scripts` | scripts | every script `setorigin`, `playerlinkto` or `unlink` of the player |
| `checkpoint_positions` | run logs | where each checkpoint reload puts you |
| `speed_records` | run logs | jumps that rose well above the normal 39 units without noclip-like climbing |
| `steep_slopes` | collision | 0.3 ≤ normal z < 0.7 surfaces (speed-redirect slopes), aimed at a target if one is set |
| `elevator_seams` | full collision | narrow creases between facing steep surfaces (classic elevator spots) |

The geometry miners use the overlay's `collision_full_<map>.bin`. Overlay v1.25+ writes this file
automatically about 20 s after a mission loads. Without it, they fall back to `brushes_<map>.txt`, which
only has invisible walls and hidden objects.

## Setup

You need Python 3.10+ and numpy. pytest is optional.

```
cd glitchlab
pip install numpy
copy config.example.json config.json      (then edit the paths)
```

`config.json`:

- `game_dir`: the AW folder, the one that contains `speedrun_data\`
- `scripts_dir`: the decompiled `maps` folder from the S1 GSC dump
- `map_ents`: optional, per-map entity files. `map_ents_0.txt` in the game folder is rewritten for whichever
  mission was loaded last, so it is only used for the map you name with `--map`.
- `known_spots`: optional, per map. `void_entry` is a place you can fall out of the world. `target` is a
  spot to aim slopes at.

## Commands

```
python -m glitchlab.cli mine    --config config.json --map recovery     # all miners, one mission
python -m glitchlab.cli scripts --config config.json --all              # script miners, every mission
python -m glitchlab.cli import  --config config.json --map recovery     # recorder logs -> experiments
python -m glitchlab.cli report  --config config.json --map recovery
```

Reports are written to `reports/<map>_miner_report.md`.

Internal map names:

| Map name | Mission |
|---|---|
| seoul | Induction |
| recovery | Atlas |
| lagos | Traffic |
| fusion | Fission |
| detroit | Aftermath |
| greece | Manhunt |
| betrayal | Utopia |
| irons_estate | Sentinel |
| crash | Crash |
| lab | Bio Lab |
| sanfran | Collapse |
| sanfran_b | Armada |
| df_fly | Throttle |
| captured | Captured |
| finale | Terminus |

## Database rules

- **Experiments are append-only.** Inputs and start conditions are never edited. An outcome is set once.
  A changed attempt is a new experiment made with `derive_experiment(parent)`.
- **Discovery stages:** `candidate → recorded → reproduced → validated → speedrun_viable`, or `rejected`.
  Miners only ever create candidates. Promotion happens when a run in the game shows it.
- Discoveries are de-duplicated by fingerprint (miner + title + location), so re-running a miner updates
  the existing entries instead of adding copies.

## Engine facts used

These come from the exe and from logs:

| Fact | Value |
|---|---|
| Walkable surface | normal z ≥ 0.7 |
| Speed redirect off steep surfaces | only when not jumping and normal z ≥ 0.3 (bouncefix check) |
| Get-unstuck | 26 nudge directions, upward first, then a 1.25-unit drop test |
| Fall damage | 200 / 350 units (`bg_fallDamageMinHeight` / `MaxHeight`) |
| Gravity | ~800 |
| Player box | 32 × 72 |
| Run speed | ~190 u/s |
| Sprint speed | ~265–270 u/s |
| Jump height | 38.9 |

A trigger only fires on a frame where you overlap it. One frame's movement must therefore be smaller
than (box size + player size) in the direction you are moving.

## Tests

```
python -m pytest -q              # or, without pytest:
python tests/run_without_pytest.py
```

## Movement simulator (`glitchlab/sim`)

The simulator steps one player through the game's movement code, one client frame at a time:

- ground check
- jump
- walk or air move
- slide and step against the collision world

Every constant is either read from the SP exe (the address is noted in `sim/params.py`) or measured from
recorded runs. Where both sources exist, they agree.

- `sim/world.py` contains the collision worlds:
  - `FlatWorld`: one floor
  - `BrushWorld.boxes`: hand-made boxes
  - `BrushWorld.from_full_collision`: the real map, from the overlay's full collision dump
- `sim/pmove.py` contains `Mover.step(state, Cmd(...))`, which runs one frame.
  `Cmd(forward, right, yaw, jump, sprint, stance, msec)` is one frame of input, with moves from -127 to 127.
- `sim/reach.py`: `fly(takeoff_speed, drop, strafe=True)` gives how far a jump or fall carries you, with
  perfect A/D-and-mouse strafing or with straight W.
- `sim/validate.py` checks the simulator against your recordings:

  ```
  python -m glitchlab.cli simcheck --config config.json --map recovery
  ```

  The report covers:

  - constants fitted from the recordings: gravity, jump apex, and run, sprint and crouch speeds
  - an **air-control check**: the fastest speed gain players actually reached while strafing in the air,
    next to the simulator's limit for different air-control strengths
  - air-dash speeds (measured; dashes aren't simulated yet)
  - a jump-distance table
  - a replay

  For the replay: the recorder doesn't save key presses. So for each 0.1 s, the check tries every keyboard combination
  and keeps the one that lands closest to the next recorded spot. `--variants` also replays with
  deliberately wrong constants. The replay can't tell close values apart well, because the key search
  covers small differences. The air-control check is the sharper test. In the config,
  `"clean_windows": {"<log file>": [[t0, t1], ...]}` limits scoring to the parts of a run without noclip
  or teleports.

**Exo moves** (`sim/exo.py`) are fitted to recordings, because their tuning lives in game data rather
than dvars:

- **Boost jump:** vertical speed is set to about 539 − 0.048 × the speed before.
- **Hover:** a sawtooth. Vertical speed drops to −65, rises at 70 u/s², and resets every 0.59 s, so you
  sink about 44 u/s on average. Air control is normal while hovering.
- **Air dash:** about 580 u/s for 0.32 s, then 240.

Each move is fitted on half of the recordings and tested on the other half:

```
python -m glitchlab.cli exocheck --config config.json --map seoul
```

Turn the moves on per mission with `Params.for_mission(map)`. Atlas has dash only.

**Noclip and other non-physics movement** is removed before anything is measured (`sim/clean.py`). The
recorder doesn't log noclip, so it is recognised by how it moves:

- flying along the view line with no gravity
- steady climbing, or fast steady sinking
- 3x run speed for longer than a dash

Teleports, reloads, pauses and scripted scenes are removed too.

Not modelled yet:

- sliding
- mantling
- ladders
- overdrive
- knockback
- swimming

The check skips those moments, using the movement flags and speed bursts in the recordings.

## Planned phases

1. **Lab module.** A separate in-game DLL that only loads when `lab.ini` exists. It would teleport the
   player, set velocity and replay inputs, and talk to this tool over a local socket. Without the ini file,
   the overlay and the game behave exactly as now.
2. **Movement simulator.** Checked against recorded runs before it is trusted.
3. **Verification queue.** The top candidates get tested automatically, several times each. Only results
   that reproduce get promoted.
