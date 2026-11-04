# Hand-written overview rows per mission: (section, waits on, ally/scripted-walk gates, notes)
MISSIONS = [
 ("Induction", "seoul"), ("Atlas", "recovery"), ("Traffic", "lagos"), ("Fission", "fusion"),
 ("Aftermath", "detroit"), ("Manhunt", "greece"), ("Utopia", "betrayal"), ("Sentinel", "irons_estate"),
 ("Crash", "crash"), ("Bio Lab", "lab"), ("Collapse", "sanfran"), ("Armada", "sanfran_b"),
 ("Throttle", "df_fly"), ("Captured", "captured"), ("Terminus", "finale"),
]

SECTION_NAMES = {
 "seoul": {"seoul_start": "Gangnam: start → FOB → hotel", "shopping_district_main": "Shopping district → canal → finale",
           "setup_dont_leave_failure": "Fail: left map", "setup_dont_leave_squad": "Fail: left squad"},
 "recovery": {"setup_funeral": "Funeral", "setup_training_sequence_1": "Training sim 1", "setup_tour": "Tour",
              "setup_training_sequence_2": "Training sim 2", "setup_outro": "Outro",
              "mission_out_of_bounds_fail": "Fail: left encounter", "mission_warn_out_of_bounds_fail": "Warning: out of bounds"},
}

OVERVIEW = {
"seoul": {
 "source": "Decompiled scripts (github.com/mjkzy/s1-gsc-dump, maps/seoul*.gsc), your recorder logs (seoul_2026*.txt, Oct 26 – Nov 3) and our analysis sessions. Times are seconds from the level start (begin_building_traversal) unless noted. Status Nov 3: cutscene skip parked as not practical.",
 "rows": [
  ("Start → drop", "Gain control, run through the top-floor doorway, drop down (land assist). Cormack's trip to the ledge is set off by trig_cormack_trigger_will_first_landassist; Will follows and reaches his doorway goal (35458, 9779) ~12 s in, the ledge ~16–17 s, jumps ~25 s.", "Will and Cormack both use the same ledge animation. Cormack also waits at his ledge until you use the land assist, land on the street (begin_looping_fob_functions) or the dialogue ends — so waiting on the ledge only delays him ~2.5–3 s while you lose 5–6.5 s.", "Will freezing at his doorway (needed for the skip) happened ~1 time in ~200 attempts: a LIVE first-fight soldier must stand on (35445, 9785) and not move. Dead soldiers don't block. Seen a live block only once more (18:33 Nov 3, 1.8 s). Soldier position does not depend on where you stand (136 attempts checked)."),
  ("Street → tent skip", "Camp scene start trigger (flag_start_fob_meet_scene, #2290 at 33254 7154) is the ONLY thing that lets the camp scene play.", "Not touching it: camp scene, Cormack's wave and Will going to the hill never happen. Race unaffected. Full run Nov 2 19:40 went around it and finished normally (no skip).", "Going left (east) of it costs ~15 units, maybe faster than the usual curved line. Normal runs: go around (removes the wave that caused plant soft locks). Skip hunting: go through."),
  ("Tent area triggers", "Rocket strip (start_cinematic_rocket_scene) = scripted rocket only; trigger_hill_setup = does nothing; seoul_streets_02 = lighting + checkpoint save.", "trigger_hill_event_01 turns combat back on for you, Will and Cormack.", "Skip/route around the first three freely. Keep hitting hill_event_01 (skipping it probably lets Cormack run to his door without stopping to shoot → harder race)."),
  ("Tent skip → turret (the Cormack race)", "Cormack's 'allies to FOB' signal fires when he reaches his door spot (~34 s from start). Good if it instead fires at Will's van warp (turret exit).", "Lost race → Will sent to the camp ~8 s after the binoculars → no plant, restart.", "Deadline: start getting into the turret by ~31.3 s from start. Your winning runs: 28.5–31.3 s (0–3 s spare). Won 41 of 72 runs that reached the turret."),
  ("EMP turret", "Fixed EMP charge (~26 s on the WR timer). Hold use to exit as soon as allowed.", "Will is warped to the van at the turret exit.", "Nobody saves time here."),
  ("Middle (rooftops → canal)", "Normal route plus out-of-map skips.", "trig_convention_center_combat_06 sends Will (24788, 12114) and Cormack (24860, 12170) to the centre.", "Don't touch combat_06. 'WILL REACHED HIS CENTRE SPOT' after a won race = likely camp."),
  ("Charges → binoculars", "Binocular scene needs 4 triggers in order: sd_escaped_swarm (16063 20176) → trig_canal_reach_upstairs (15852 20461) → trig_updstairs_window_approach (15368 20700) → window flag trigger (15188 20202, switched on by the upstairs one). Highlighted yellow in overlay v1.14.", "Will heads for the plant spot only after the 6 middle weapon guards are dead (middle_weapon_guards_dead → prep_will_for_finale).", "Pickup → window: best old route 39.6 s, new north route (Nov 3 19:3x) 39.7 s despite ~8 s lost dropping to the lower floor (could be ~7 s faster). Guards MUST die before the binoculars: in all 6 runs with late guard kills, no normal plant prompt."),
  ("Binoculars", "Binocular scene ~31–33 s. Will is sent to the canal at its end.", "Will walks to the ship walk-in spot; on arrival every pending walk finishes at once (shared 'arrived' signal): platform (good), stalled camp walk → snapped to the tent, old balcony walk → snapped to the start balcony.", "Camp wave right after the binoculars in 19 of 47 runs."),
  ("Charges / platform", "Plant prompt ~10 s after the binoculars normally.", "If the camp wave is still coming: planting before it (or <~2.5 s after) soft-locks — Will runs off.", "Overlay v1.14: WAVE COMING → WAVE - WAIT → PLANT NOW."),
  ("Finale cutscene", "Part 1 plant bomb ~8.4 s → Part 2 ~27–39 s → Part 3 ~50 s (fixed timer) → fade → nextmission.", "Part 2 ends as soon as Will's animation stops.", "Skip run 215054 (keep forever): Will frozen at start + race won + wave after binoculars + plant ~2.5 s after the wave → Part 2 skipped (~20–30 s). Estimated odds per unmodded run ≈ 1 in 1,500 → parked."),
  ("Fail rules", "player_left_map boxes = instant fail. player_leaving_map boxes = warning text only. player_left_squad = fail.", "—", "Some fail boxes are only active in parts of the level."),
 ]},
"recovery": {
 "source": "Decompiled scripts (maps/recovery*.gsc), the WR video (10:27.016) with your route notes and your first full run log recovery_20261103_212912.txt (Nov 3). Times: seconds between script events in your log; WR times read off the video (about ±5 s).",
 "rows": [
  ("Funeral", "Fixed cutscene + casket/Irons prompts.", "None.", "Unskippable; trimmed from the WR video."),
  ("Training sim 1 (Camp David)", "WR: jump onto the house ~0:25, out of map into the grass (glowing white gun), follow the map edge (invisible trees further in), blind jump ~1:15 to the base, land in-map.", "Skips almost all of sim 1's kill gates and the jeep ride.", "Your log: many tries, landed in the base about 3.5 min after the first attempt. Missing the blind jump is the main time risk."),
  ("Exo arm repair", "Fixed scene: 3 'move your arm' tests. Your run: 55 s from sitting down to done.", "Technicians are scripted, nothing to wait on.", "Only the walk to the desk is yours (your run: 12 s from the hangar door)."),
  ("Walk to the shooting range", "Doors between areas open / next area loads only when you AND Gideon stand in the same box (tff_ally_check).", "Gideon's walking pace sets this part — that's why the WR follows him.", "Your run: 70 s from repair done to range start; WR about 55 s."),
  ("Shooting range", "3 rounds on fixed script timers; score doesn't matter (WR gets 'Poor').", "—", "Fixed 79 s once started. Only gain: start it sooner."),
  ("Grenade range", "Tutorial waits on your actions (throw threat grenade, hit targets), then you start the minigame yourself; minigame is fixed timers (~43 s).", "Gideon leaves 6 s before the minigame ends (gideon_early_exit_grenade_range).", "Your run: tutorial 30 s, then 68 s testing the range-escape glitch (out at ~-3340 2460, ran to the elevator-room door, back) before starting the minigame (WR about 5–10 s). The escape can't open the elevator early: the room trigger only switches on when the minigame completes, and Gideon leaves on the minigame timer. Untested: start the minigame, escape, stand on the room trigger when it completes (likely 0–3 s at best)."),
  ("Armory / elevator", "Room trigger only switches on after the grenade range is complete. Gideon walks there (~22 s from leaving the range), then plays a fixed 27 s animation and presses the button part-way.", "Gideon anim_reach + fixed animation = the doors.", "Your run: 47 s from range complete to the button, same as the WR (~45 s). Almost certainly can't be cut without moving Gideon."),
  ("Elevator ride → sim 2", "7 s + 8 s scripted ride, then sim 2 starts.", "—", "Fixed."),
  ("Training sim 2 → truck → warbird", "WR: go left of the map, left of a tree ~7:13 to dodge a fail box, truck arrives ~7:21, POTUS leaves, kill the spawned enemies down to the trucks, warbird lands, board it.", "Kill gates (counts) decide when the warbird comes.", "Your run: 94 s from elevator to rescue complete, then 18 s to the outro."),
  ("Fail rules", "training_player_left_encounter box = fail; training_out_of_bounds_warning = warning; jeep board warn/fail boxes; president death = fail.", "—", "Leaving the encounter does NOT fail while flag_player_using_drone is set."),
 ]},
}

EXTRA = {
"seoul": [("Cutscene parts — what can be skipped", [
  ("Part 1 (plant bomb, ~8 s)", "Ends when its shortest actor's animation ends. In the skip, the stray 'landed' script fires exactly at the end of Part 1 — nothing we know stops Will earlier.", "Not skippable with what we know."),
  ("Part 2 (~27 s)", "Ends when Will's animation stops. The stray 'landed' script stops it on the frame Part 2 starts.", "Skipped once (run 215054), ~30 s saved."),
  ("Part 3 (~50 s)", "The script runs Part 3 in the background and waits a FIXED time (length of your viewmodel animation − 2 s), then a 2 s fade, then nextmission. Will isn't even in Part 3.", "Not skippable: the timer is fixed no matter what happens to the animations."),
 ])],
}
