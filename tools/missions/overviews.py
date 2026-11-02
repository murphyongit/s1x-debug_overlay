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
 "source": "Decompiled scripts (github.com/mjkzy/s1-gsc-dump, maps/seoul*.gsc), your recorder logs (seoul_2026*.txt) and our analysis sessions. Times are seconds in your logs unless noted.",
 "rows": [
  ("Start → drop", "Gain control, run to the balcony, drop down (land assist). Small trigger on the top floor (trig_cormack_trigger_will_first_landassist) sends Will to the ledge.", "Cormack and Will each walk to the ledge (anim_reach) then jump. If Will misses his jump, his 'walk to ledge' script stays waiting all level.", "Will missing his jump is step 1 of the ending cutscene skip. Seen in 2 of 59 logged attempts."),
  ("Tent skip → turret (the Cormack race)", "After landing, Cormack walks to his door spot (34529, 8378). Entering the turret-area box warps Will + Cormack to the van ~3.2–4 s later.", "Cormack's door-spot walk. If he gets within ~30 units before the warp, the camp scene starts early (bad roll).", "Win the race: reach the turret area ~3.5 s before Cormack reaches his spot ≈ drop → turret area in about 22–23 s. Overlay v0.88+ shows ENTER BOX IN countdown."),
  ("EMP turret", "Fixed EMP charge (~26 s on the WR timer).", "—", "Nobody saves time here."),
  ("Middle (rooftops → canal)", "Normal route plus out-of-map skips.", "Camp (FOB) scene keeps waiting for Cormack/Will to arrive; it stalls mid-way in a bad roll.", "Convention-centre box: in a bad roll, touching it early (~80 s before binoculars) let the camp scene finish (run 173300, 1 case). In a good roll avoid it."),
  ("Binoculars", "Binocular cutscene ~32 s from entering the room. Will is sent to the canal at its end.", "Will walks ~7 s to the ship walk-in spot. On arrival, every walk he has waiting finishes at once (shared 'arrived' signal): ship walk → platform; stalled camp walk → snapped to the tent; old balcony walk → snapped to the start balcony.", "Camp scene not started (race won): Will on time 5 of 5. Camp scene mid-way (race lost): Will on time 1 of 19."),
  ("Charges / platform", "Charge prompt turns on when Will's walk-in animation ends (it never checks where Will is).", "—", "Binoculars-first projected ~5–10 s faster than WR backwards pace when the race is won (untested)."),
  ("Finale cutscene", "Part 1 plant bomb ~8 s → Part 2 (arm, 'pull', 'jump now') ~27 s → Part 3 (jump, explosion, fall, Cormack) ~50 s → fade → nextmission.", "Part 2 ends as soon as Will's animation stops.", "Skip run 215054: old balcony 'landed' script stopped Will on the frame Part 2 started → Part 2 skipped, ~30 s saved. Part 1 and Part 3 can't be cut this way (see 'Cutscene parts')."),
  ("Fail rules", "player_left_map boxes = instant fail. player_leaving_map boxes = warning text only. player_left_squad = fail.", "—", "Some fail boxes are only active in parts of the level (you crossed two near the turret/canal without failing)."),
 ]},
"recovery": {
 "source": "Decompiled scripts (github.com/mjkzy/s1-gsc-dump, maps/recovery*.gsc) and the trigger list in recorder log recovery_20261029_215647.txt. Not tested in-game yet.",
 "rows": [
  ("Funeral", "funeral_start trigger → eulogy cutscene (fixed) → use the casket (trig_coffin) → walk up to Irons (player_proximity_irons box) → card → funeral_complete.", "None (all scripted animations).", "Mostly fixed cutscene time. Player gates: casket prompt and the Irons proximity box — test how early each can be triggered."),
  ("Training sim 1 — start", "Kill gates: groups training_s1_start and training_s1_enemies_start must be cleared before breach prep.", "Joker anim_reach to the threat-grenade spot.", "Combat speed on the first two groups gates everything after."),
  ("Training sim 1 — living room / breach", "Joker scene, flash_enemies cleared → breach set up → player breach trigger.", "Joker anim_reach to breach approach and patio door.", "President dying = fail."),
  ("Training sim 1 — drones / patio", "Drone wave must die → patio waves (counts down to 1, then cleared).", "Joker patio door anim_reach.", "Several 'kill down to N' gates: kill order matters."),
  ("Training sim 1 — hide / golf / end", "Hide from patrols → golf course → end ambush counts (≤4, ≤5) → use the ending trigger at the jeep.", "—", "Fail boxes around the golf course (see Fail rules)."),
  ("Tour", "Jeep ride (scripted), exo arm repair (3 attempts), firing range, grenade / augmented-reality range.", "Gideon anim_reach at ~6 spots (ready room, range stairs x3, exo demo, grenade range).", "Most Will-type risk in this mission: Gideon must arrive before each scene."),
  ("Training sim 2", "Sniper counts 5→3→1→0 → living room, hallway, bedrooms cleared → breach → patio counts → golf course vehicle counts → ending trigger.", "Gideon patio door anim_reach (in/out).", "Same structure as sim 1."),
  ("Outro", "flag outro_start → 2 s fade → nextmission.", "—", ""),
  ("Fail rules", "training_player_left_encounter box = fail; training_out_of_bounds_warning = warning; jeep board warn/fail boxes; president death = fail.", "—", "Script exception: leaving the encounter does NOT fail while flag_player_using_drone is set (you're flying the drone). Worth testing."),
 ]},
}

EXTRA = {
"seoul": [("Cutscene parts — what can be skipped", [
  ("Part 1 (plant bomb, ~8 s)", "Ends when its shortest actor's animation ends. In the skip, the stray 'landed' script fires exactly at the end of Part 1 — nothing we know stops Will earlier.", "Not skippable with what we know."),
  ("Part 2 (~27 s)", "Ends when Will's animation stops. The stray 'landed' script stops it on the frame Part 2 starts.", "Skipped once (run 215054), ~30 s saved."),
  ("Part 3 (~50 s)", "The script runs Part 3 in the background and waits a FIXED time (length of your viewmodel animation − 2 s), then a 2 s fade, then nextmission. Will isn't even in Part 3.", "Not skippable: the timer is fixed no matter what happens to the animations."),
 ])],
}
