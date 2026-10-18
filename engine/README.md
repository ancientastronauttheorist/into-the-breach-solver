# ITB engine

A clean-room C++ rules engine for Into the Breach, built to back a provably
optimal per-turn solver. The goal is "perfect turns": given the board a
player can see, find the best actions, simulating through the end of the
enemy phase. Ranking uses strict priority tiers: grid and buildings, then
mech survival, then objectives, then kills, then position.

The behavior comes from reading the game (the unstripped Linux build
21601364 in Ghidra, plus the shipped Lua). This directory contains only
original code and written rules. Game files, decompiler output and Ghidra
projects stay local and are never committed.

## Build and test

```bash
cmake -S engine -B engine/build -G Ninja
cmake --build engine/build
engine/build/itb_tests
```

Dependencies are fetched and pinned by hash: Lua 5.1.5 (the game's Lua
version), doctest and nlohmann/json.

Some tests run the game's own scripts. By default they look for the local
copy at `.local_decompile/builds/linux_21601364`; set `ITB_GAME_DIR` to use
another install, i.e. any directory that contains `scripts/scripts.lua`.
Without a game install, those tests warn and skip.

## Tools

```bash
engine/build/itb_inspect recordings/<run>/m07_turn_01_solve_input.json  # render a recorded board
engine/build/itb_inspect --pawns                                        # every pawn definition
engine/build/itb_inspect --scripts                                      # script load report
engine/build/itb_inspect --corpus recordings                            # load all recorded boards
engine/build/itb_inspect --weapons recordings                           # run weapon Lua on recorded boards
engine/build/itb_inspect --replay recordings                            # replay recorded actions vs the game
engine/build/itb_inspect --replay recordings --turns                    # whole turns, through the enemy phase
engine/build/itb_inspect --score recordings/<run>/m07_turn_01_solve_input.json  # score a recorded plan
engine/build/itb_inspect --solve recordings/<run>/m07_turn_01_solve_input.json --time 10 --threads 8
engine/build/itb_inspect --solve recordings --sample 60 --time 120 --threads 8 --json out.jsonl
engine/build/itb_inspect --solve recordings --threat-tier --threat-eval --json on.jsonl  # experimental threat tier
engine/build/itb_inspect --threat-check recordings                       # threat proxy vs engine and games, cost
engine/build/itb_inspect --solve state.json --lookahead [--lookahead-samples 8] # experimental next-turn tier
engine/build/itb_inspect --solve state.json --grid-survival                     # Grid Defense odds when every line can lose the grid
engine/build/itb_inspect --survival-cert state.json --time 900 --threads 4     # an upper bound on those odds (certificate)
engine/build/itb_inspect --solve state.json --grid-survival --survival-cap 0.5 --time 30 --max-time 120 --min-tiers 7  # certificate pre-pass
engine/build/itb_inspect --lookahead-study state.json --compare [--samples 8] [--next-time 4]  # plans vs next turn
engine/build/itb_inspect --two-turn state.json --arm la8 --arm sum8 --plan solve.json [--next-seeds 16]  # plans over two turns
engine/build/itb_inspect --two-turn deployed.json --deploy --deploy-top 6 [--spawn F1] # placements over two turns
engine/build/itb_inspect --predict state.json --actions @actions.json --branches   # live validation
engine/build/itb_inspect --ai recordings [--samples 64] [--show N] [--verbose]       # enemy AI planning vs the live saves
engine/build/itb_live deploy deployed_state.json --time 45 --threads 8            # deployment search (JSON)
engine/build/itb_live solve state.json --time 10 --threads 8                      # live play (JSON)
engine/build/itb_inspect --diff-weapons recordings --random 300   # C++ weapon ports vs their Lua
engine/build/itb_inspect --diff-weapons --ports                   # which weapon methods run natively
```

`--solve` runs the perfect-turn search (stage 9) and prints the plan in
A1-H8 notation, its worst-case score, whether it is proven optimal (else the
upper bound and how many leading tiers are proven), search statistics, and
the old bot's recorded plan scored the same way. On a directory it takes
`--sample N` (evenly spaced boards), `--shard I/N`, `--verbose`, `--json FILE`
and ends with proof rates, times to prove and how our plans compare with the
recorded ones. `--beam W` sets the beam width (0 = off); `--lua-counts`
prints every Lua call by weapon table, method and the Lua function that ran
(`@native` for calls a C++ port answered). `--max-time S --min-tiers K`
turns on the adaptive budget ("Perfect-turn search", Anytime): `--time` is
then the base time, kept only if the first K tiers are proven by then.

`--diff-weapons` fires every ported weapon table (one per distinct
behaviour; `--all-tables` for all) from every pawn of every board, at every
tile, once in Lua and once natively, and compares target areas, every
SpaceDamage field, Lua errors, writes, console output and random draws
(exit status 1 on any difference). `--weapon ID` limits it to one table.

`--replay` takes `--show N`, `--weapon ID`, `--trace RUN/MISSION/TURN`
(boards and effects of one turn), `--json FILE` (every mismatch) and
`--no-sync`. With `--turns` it replays each recorded turn whose plan it can
follow to the end, runs the enemy phase and compares with the game's board
at the start of the next turn (see "Validating the enemy phase").

## Live play

`scripts/live_play.py` plays the running game with this engine, through the
Lua bridge. It never acts on import, refuses to act when the bridge heartbeat
is stale or `bridge_ext_disabled` is set, and every command takes `--dry-run`.

```bash
python3 scripts/live_play.py status                 # bridge + board summary
python3 scripts/live_play.py deploy [--tiles C5,D6,E5]
python3 scripts/live_play.py turn --time 30 --max-time 120 --min-tiers 5 --threads 8
python3 scripts/live_play.py end-turn
python3 scripts/live_play.py mission --max-turns 10  # turn + end-turn, stops at the first anomaly
python3 scripts/live_play.py restore --run 20261011_222439 --mission 0 --turn 1  # back to a turn start
```

- **After a game restart, load the save twice.** A save continued right after
  the game starts gets the Grid Defense base of the settings file's
  difficulty, not the save's (Grid survival, below), so an Unfair run would
  play with 15% it should not have. We don't take that bonus: after any
  restart, Continue, then load once more (Reset Turn, or menu → Continue)
  before playing. `status` prints a WARNING while the bridge reports the
  bogus base (`grid_defense_base` 15 on difficulty 3). Run 22 (2026-10-14,
  won) had the bonus: 40% instead of 25%.
- **Checkpoints and restore.** At each turn start `turn` copies the game's
  own save files (`profile_Alpha/saveData.lua`, `undoSave.lua`) into the run
  as `m<MM>_turn_<TT>_saveData.lua` / `_undoSave.lua`. `restore` quits the
  game, backs up the profile's current save files
  (`.local_runs/live_play/restore_backups/`), puts the checkpoint back,
  relaunches through the offline Steam client and clicks Continue on the
  title screen (the bridge is silent there), then compares the loaded board
  with that turn's solve input. The game reads its save files only at
  launch (Continue after the pause menu's Main Menu resumes the run still in
  memory, whatever the profile now holds), hence the relaunch. `restore
  --reload` then loads the save once more (the pause menu by its gear icon
  at `--gear-xy`, default (43, 28), Esc as a fallback: Esc once did not
  open it, live 2026-10-16; both bring the game window to the front first;
  once the heartbeat stalls, the pause menu's MAIN MENU at (530, 438), no
  confirm dialog; Continue on the title) and fails while the bridge still reports a wrong Grid Defense
  base (`grid_defense_base` / `grid_base_difficulty` against the save's
  difficulty). Continue resumes the run at the start of
  that turn, Vek intents and spawn points included, so the same turn can be
  replayed with another plan or other solver settings, and the game referees
  the outcome. `turn --plan FILE` plays a given plan (itb_live's action list,
  or a solve reply with a `plan`) instead of solving; it is predicted and
  checked step by step like the engine's own, and a mismatch re-solves.

- **turn** solves the live board (`Visibility::Player`), then executes the
  plan one sub-action at a time (`MOVE_NATIVE`; `ATTACK` /
  `TWO_CLICK_ATTACK` with the slot of the exact weapon id from
  `weapon_slots`; `REPAIR`). Execution is native only: it sends
  `EXEC_MODE native` first (an older bridge is refused), and the bridge
  then plays every action as the game's own `Pawn:FireWeapon` with the mech
  selected (`SetPawn`, as a click does; weapon scripts read the global
  `Pawn`), refuses it while the board or the mech is busy, acks an error
  when the game does not fire (`FireWeapon` returned 0/2/3), and acks only
  once the board and the mech are idle. No fallback to `MOVE`/SetSpace, no
  emulated transit damage, flips or repairs (those exist only behind
  `EXEC_MODE legacy`, the old Python bot's opt-in via
  `ITB_BRIDGE_EXEC_MODE=legacy`). After each, it takes a settled bridge dump and
  compares it with the predicted board: units by uid (type, tile, HP, fire,
  acid, frozen, shield, web), units the engine created by type/tile/HP, and
  building HP. The save-based `grid_power` is stale mid-turn and is not
  compared. A mismatch re-solves from the live board (`--max-resolves`),
  with the same budget. The budget is adaptive: `--time` (default 10 s) is
  the base, kept when the plan is proven in its first `--min-tiers` tiers
  (default 5: grid, buildings, mechs lost, objectives failed, mech HP);
  otherwise the search goes on, proving only those tiers, up to
  `--max-time` (default 120 s). `--min-tiers 0` always stops at `--time`.
  Native `Pawn:FireWeapon` does not apply the UI's per-turn limits, so
  before executing a plan (and each re-solve) the driver refuses one in
  which a unit does more than the UI allows ("Action model" below): move
  then one weapon or Repair; Shifty / Post_Move pilots one more move after
  acting; Double_Shot two actions without moving. Each unit's moves and
  actions this turn are kept in the run manifest (`unit_actions`); a unit
  that acted and is active again gets its pilot bonus in re-solves
  (`bonus_shift`, or `moved` for Double_Shot), which the bridge cannot read.
  A pilot skill with a PowerCost counts only while the pilot is powered
  (see "Action model"): the bridge exports the save's `pilot.power` cells
  and the live `pilot.skill_active`; for older bridges the driver fills the
  cells from the save's squad pilots (`power_source: "save"`). After each
  action it also waits until the bodies of the units it killed have left
  the board: a dead non-corpse pawn stays in its tile while its death
  animation plays (about 1.2 s plus a 0.5 s fade-out, not "busy" in the
  player's turn), and the next action would still push it into whatever is
  behind it (live 2026-10-16: a Wall killed by one shot bumped a building
  on the next). The bridge lists them in `dying_bodies`
  (`Board:GetPawn(p, false)`); without the list the driver waits 2.5 s
  after any kill.
- **Ranking options** (all off by default; each is passed to every solve and
  re-solve of the turn): `--threat-tier`, `--threat-kills`,
  `--objectives-first`, `--mech-grid N [--mech-grid-turns]
  [--mech-grid-pilot]` and `--lookahead [--lookahead-samples K]
  [--lookahead-reply greedy|idle]` ("Turn score" below), and
  `--grid-survival` ("Grid survival" below). With
  `--lookahead` the plan line adds the next-turn estimate of the plan's end
  board, and the tier counts of `--min-tiers` include NextTurn (with
  `--threat-tier`, `--min-tiers 7` reaches mech HP). `--grid-survival` adds
  no tier to the counts; the plan line shows the odds and the Grid Defense
  chance used. A bridge without `grid_overflow` (before 2026-10-14) gets it
  from the profile's `saveData.lua` (`grid_overflow` in the state's
  `patches`). `--survival-cap [--survival-cap-share F]
  [--survival-cap-strict]` (with `--grid-survival`; off by default) runs
  the certificate as a pre-pass on a turn where the survey finds no line
  that surely keeps the grid, and stops the solve once the plan reaches the
  proven ceiling ("Survival cap" under Grid survival); the plan line then
  says "odds PROVEN OPTIMAL at the ceiling".
- **end-turn** predicts the enemy phase from the board as left, sends
  `END_TURN` (it deactivates the mechs, so the "units can still act" dialog
  never appears; on this build the bridge cannot end the turn itself and
  answers `NEEDS_MCP_CLICK`), clicks End Turn (`--end-turn-xy`, default
  fullscreen 1360x768 `128,89`; `--no-click` asks you to), checks the phase
  changed, waits for the next player turn and compares grid, HP, statuses,
  survivors and mech tiles. Vek tiles and webs are not compared (the AI
  moves and webs after the point the engine stops); new units are reported
  as spawns. Outcomes the engine cannot know are notes, not anomalies: a
  Vek that picks up an acid pool moving while the AI plans; a unit burning
  in the prediction that the game shows put out on a tile smoked after it
  (a Mosquito smokes the target it queues on while the AI plans, so a burning
  mech there is put out; live m78; the compact boards list `smoke_tiles`;
  `itb_inspect --ai` reproduces the game's board there); a Vek burning
  after the phase on another tile, one the game shows burning or lava (it
  walked into the fire while the AI planned; live run 41, Final Cave turn
  3: a Scorpion2 onto F2), or a Burrower put out on another tile (the AI
  moves it by burrowing, which puts fire out; live run 43, JellyBoss turn
  1: a Burrower2 the BurnbugBoss's flames lit on D5 burrowed to F7); Grid Defense
  (the prediction takes every roll as not resisted, its worst case: with
  chance nodes in the prediction, a building it damaged that the game shows
  with more HP, at most its HP before the phase and on a tile with a roll,
  with the grid higher by exactly the HP kept; when something else differs
  too, the driver predicts again with those rolls resisted, `"resist":
  [[x, y], ...]` (`itb_live predict --resist`, each entry resists the next
  roll on its tile; saved as `..._prediction_resisted.json`), and takes
  that prediction if it matches: a resist can change more than the
  building, e.g. Void Shocker shocks a Vek whose only hit was resisted,
  fuzz 20261018_082428 sample 1); a Soldier Psion or Psion
  Abomination emerging from a hidden spawn (every Vek of its team +1 HP at
  once); another outcome of a hidden choice (the prediction's
  `hidden_choices`: an environment order or choice, a mission hook's
  random pick such as the Final Cave's bomb drop; it takes branch 0 of
  each): on a difference the driver predicts again with `"branches": 64`
  (`itb_live predict --branches N`: `enemy_branches`, each other outcome's
  `choices`, `after_enemy` and `enemy_phase`, saved as
  `..._prediction_branches.json`) and takes the first that matches,
  carrying its boards on as the prediction (fuzz 20261017_000242 sample 4,
  Mission_Final_Cave turn 3: the new bomb fell on the Scorpion at C6, the
  prediction's on the rock at E6). Anything else stops `mission`.
- **Queued shots** come from the engine where the bridge's are stale. The
  bridge reads them from the save (written at turn start) and only shifts
  them with the pawn; no Lua API exposes a pawn's live queued shot, so a
  shot the turn cleared (a Vek smoked, frozen or in water; live m36: a
  Beetle smoked by Aerial Bombs, then pulled out of the smoke) or flipped
  still shows. `itb_live` boards carry each unit's `queued` shot; after each
  step the driver keeps the predicted one for every turn-start unit the live
  board confirms (type, tile, HP, statuses), in the run's `manifest.json`
  (`engine_queued`, that mission and turn only), and puts it into the state
  of every re-solve and of the end-of-turn prediction (`<uid>.queued` in the
  file's `patches`; the bridge's dump is archived beside it as `..._raw`),
  with a note for each shot cleared or retargeted.
- **Underground Burrowers** come from the engine too. A hurt Burrower dives
  off the board until the AI's move brings it up, and the bridge lists no
  such pawn (live m20 Mission_Reactivation: Burrower1#1683 dove after a
  laser hit). `itb_live` boards list it under `underground`; the driver
  keeps the entry for a turn-start unit the live board no longer lists
  (`engine_underground` in `manifest.json`, the whole mission, dropped once
  the bridge lists the uid again), adds it to the state of every re-solve
  and of the end-of-turn prediction (`"underground": true` at the tile it
  dove from; `<uid>.underground` in `patches`), and counts it as a known
  unit: one that resurfaces after the enemy phase is matched by uid (type
  and HP; a note, not a spawn).
- **Shielded buildings** come from the engine too. On vanilla the bridge
  cannot read a structure's shield (no `Board:IsShield`;
  `tile_shields_live` false), so a building the plan shielded looked bare
  in the End Turn snapshot (live 20261014_175032 m14, Mission_Survive turns
  1 and 2: Science_Shield on F4 and G4; the end-of-turn prediction lost
  them and the grid, the game and the solve's own forecast kept both).
  `itb_live` boards list `shield_tiles` (shielded buildings and mountains);
  after each step the driver keeps those whose building the live board
  shows at the predicted HP (`engine_shields` in `manifest.json`, that
  mission and turn) and marks them `"shield": true` on the state's tiles
  for every re-solve and the end-of-turn prediction (`tile_shields` in
  `patches`; a tile's `shield` loads on buildings and mountains). Shields
  the enemy phase leaves standing carry into the next turn only when the
  game matched the prediction. A bridge with `tile_shields_live` is taken
  as is. On a mission's turn 1 the bridge may mark shields itself from
  RegionData (`tile_shields_static_baseline`: the turn start's map, which
  keeps a shield a hit popped); once the driver has a record for the turn
  (a step played), the record replaces those marks (`tile_shields_cleared`
  in `patches`; fuzz 20261016_230406 sample 14, a restored
  Mission_BeltRandom turn 1: the plan popped G4's and F6's shields, the
  end-of-turn prediction read them stale and kept G4 against Firefly2's
  shot, the game lost it and 1 grid).
- **Mission_Reactivation's thaw list** is kept from what a player sees,
  never from the mission's own `Enemies` list (hidden, like the AI seed).
  The driver starts it from the frozen enemies at deployment or at the
  start of turn 1 (a turn-0 list is narrowed at turn 1 to the ones still
  frozen), keeps it per mission in `manifest.json` (`reactivation`), and
  gives it to every solve and prediction as `reactivation_visible_list`.
  After each enemy phase, the prediction's `thawed` events name the
  draw's candidates E (listed units alive and frozen at the draw): with
  |E| < 2 the mission drew its whole list, which empties; with |E| = 2 both
  leave it; with more, the two the game shows unfrozen leave it (a spawn
  bump after the draw also breaks ice: such a unit is ambiguous, and more
  than two left unfrozen keep the list as is). Entries the draws dropped
  unseen stay listed (worst case). A mission joined later than turn 1
  gives no list, and the engine takes every frozen enemy as a candidate.
  Each solve notes where the list differs from the bridge's `Enemies`
  (diagnostics only).
- **deploy** uses the bridge's `drop_zone` (refused tiles are already left
  out, and `DEPLOY` refuses them anyway). It places the mechs with the
  heuristic first (unplaced mechs are not in the bridge's dump; once placed,
  the board lists them with their weapons, HP and pilots), then runs the
  engine's deployment search on that board (`itb_live deploy`, below; the
  heuristic placement is one of its candidates; `--search-time`, default
  45 s, `--threads`) and moves the mechs to the tiles it picks (a mech whose
  tile another mech still holds waits; a swap goes through a free tile). If
  the engine cannot search the board (no engine, an error) the heuristic
  placement stays, with a note. `--heuristic` skips the search, `--tiles`
  places by hand. It checks every mech's tile in the state (the final board
  is `m<MM>_turn_00_deployed_02.json` when the search moved them) and leaves
  Confirm to you. `deploy` takes the ranking options of `turn` / `mission`
  (`--threat-tier ... --lookahead`); given, the search's turn-1 solves rank
  as the turns will (off by default: plain tiers, as before; see
  "Enemy AI planning and deployment").
- Bridges without `weapons_exact` (before the extension) need
  `--loadout FILE` (`{"PunchMech": ["Prime_Punchmech_B", ...]}`); without
  `moved` the driver marks the units it moved.

Every state and prediction lands in `recordings/live/<YYYYMMDD_HHMMSS>/`
(git-ignored; force-add a run to keep it): `m<MM>_turn_<TT>_solve_input.json`
(turn start, the old recordings' wrapper) and `..._solve.json` (the executed
plan in the old format), so `itb_inspect --corpus/--solve/--moves` read them;
`--replay` skips them (no per-step predictions in the old simulator's
format). Other states are `m<MM>_turn_<TT>_<label>_<NN>.json`, each
prediction is beside its input as `..._prediction.json`, and `events.jsonl`
logs every command and ack. `recordings/live/2026-10-09/` holds the first
live session's bridge states (old bridge: base weapon ids); `itb_tests`
loads every state under `recordings/live/`.

The engine side is `itb_live` (`tools/live_tool.hpp`), JSON in and out:

```bash
engine/build/itb_live solve state.json --time 10 [--max-time 120 --min-tiers 5] --threads 8 [--out FILE] [--pretty]
engine/build/itb_live predict state.json --plan @plan.json
engine/build/itb_live board state.json
engine/build/itb_live actions state.json [--plan @prefix.json] [--no-effects]   # the fuzzer's move list
engine/build/itb_live deploy state.json [--time 45] [--threads 8] [--samples 12] [--finalists 3]
          [--screen-samples 3] [--solve-time 0.5] [--seed 1] [--placements @p.json] [--always @p.json]
engine/build/itb_live serve --threads 8   # one JSON request per stdin line (what live_play.py uses)
```

`solve` returns the plan (`Plan::actions`), `steps` (one per move, weapon
or repair, each with the predicted `board` after it, its status, chance
nodes and timing flags; `weapon_index` is the slot in the unit's weapons),
`start` / `after_player` / `after_enemy` boards, `enemy_phase` (events,
`emerged_unknown`, `mission_ended`, exactness, `chance_nodes` and the
`grid_defense` rolls with their tile and grid at stake), `worst_case` and
`upper_bound` by tier, `proven_optimal`, `proven_components`,
`chance_exact`, `timed_out`, `extended` (the adaptive budget ran past the
base time), `contingent`, `stats` and `warnings`. Requests take
`time_limit`, `node_limit`, `beam_width`, `threads`, and for the adaptive
budget `max_time` and `min_tiers`; the ranking options are `threat_tier`,
`threat_kills`, `objectives_first`, `mech_grid`, `mech_grid_turns`,
`mech_grid_pilot` (then also `mech_rates` by uid), `lookahead`
(`lookahead_samples`, `lookahead_reply` "greedy" | "idle", `lookahead_seed`;
then `worst_case` has `next_turn`, `stats` has `lookahead_evals`,
`lookahead_hits` and `lookahead_s`, and `next_turn` gives the plan's own
end board's estimate: worst grid, mechs and building HP lost and each AI
sample's) and `grid_survival` (then `worst_case` has `grid_survival`, a
probability, and the reply has `grid_defense`: the `percent` used and its
`source`, "state" or "default"). Solve only, with `grid_survival`:
`survival_cap` (true), `survival_cap_share` (default 0.5) and
`survival_cap_strict`; the reply then has `survival_cap` (`ran`,
`survey_proof`, `m`, `k`, `bound`, `proven`, `proofs`, `time_s`, `share`).
CLI: `--mech-grid-pilot`, `--lookahead
[--lookahead-samples K] [--lookahead-reply greedy|idle]`, `--grid-survival`,
`--survival-cap`, `--survival-cap-share F`, `--survival-cap-strict`. The
simulation follows the plan's default chance outcome (no Grid Defense
resist, first branch), reseeding Lua as the solver does. Boards are compact:
`grid_power`, `buildings` (x, y, hp) and living on-board `units` sorted by
uid (uid, type, x, y, hp, team, mech, fire, acid, frozen, shield, web;
`leader` for a psion: its mutation number), plus `underground`,
`acid_tiles`, `smoke_tiles` and `shield_tiles` (shielded buildings and
mountains, [x, y]).

`actions` plays a plan prefix exactly as `predict` does (same options, same
Lua seeding), then lists every sub-action the engine accepts from there:
the solver's Action model ("Perfect-turn search", Action model), enumerated
as its search does (moves to the Move skill's area, every non-passive slot
at every target incl. second clicks, Repair), each run once on a copy of
the board. Each entry has `sub`, `uid`, `weapon`, `slot`, `target`,
`target2`, `target_kind` (what the target tile holds: `unit_player`,
`building`, `water`, ...), `end`, and (unless `--no-effects`) its `effect`:
HP lost and kills by team, the actor's own HP, pushes, bumps, falls,
drownings, units created, statuses changed, building / grid / mountain
loss, tiles changed, chance nodes, timing flags, Lua errors, unmodelled
writes, mission hooks. Refused and empty ones are only counted
(`rejected`, by status). The reply also has `board` (after the prefix) and
`units` (`can_move` / `can_act` per controllable unit); `refused` >= 0 if
the prefix itself was refused.

### Fuzzing

`scripts/fuzz_play.py` plays random but UI-legal turns in the game and
checks every step against the engine. Live play only executes the solver's
choices, so a rare interaction the engine mis-models as bad is never tried;
the fuzzer tries exactly those.

```bash
python3 scripts/fuzz_play.py --dry-run --samples 500                 # engine only, never touches the game
python3 scripts/fuzz_play.py --dry-run --run 20261016_130634 --mission 2 --turn 2 --samples 20
python3 scripts/fuzz_play.py --samples 20 --minutes 60               # live: quits and relaunches the game per sample
python3 scripts/fuzz_play.py --put-back recordings/fuzz/<session> [--dry-run]  # the profile's own saves back
python3 scripts/coverage_report.py [--unseen]                        # what the recordings never exercised
python3 scripts/fuzz_play.py --dry-run --coverage --samples 60       # inject what they never exercised
python3 scripts/fuzz_play.py --unfired --samples 10                  # inject the weapons never fired, and fire them
python3 scripts/fuzz_play.py --loadout '0:Prime_Lasermech_A,Passive_FlameImmune;pilot=Pilot_Rock' --samples 5
python3 scripts/loadout.py --check                                   # the save model vs every recorded checkpoint, and rewrites of each
```

- **Turn starts** are the recorded `m<MM>_turn_<TT>_solve_input.json` of
  `recordings/live/<run>/` (`--recordings`, `--run`); live mode needs their
  `_saveData.lua` checkpoint. A mission's last turn is left out
  (`--last-turns`: its enemy phase ends the mission or the run). Starts
  are drawn without replacement: a shuffled pass over all of them, then a
  new pass, said so (`--mission/--turn`: that one every time). Live mode
  restores only a start its recording shows playable (`offline_reason`: a
  player turn with an active mech, not a last turn, and a sequence of
  `--tries` the engine predicts keeps `--min-grid` and a mech from the
  recorded solve input); one that fails, or fails the same checks after its restore, is
  never drawn again (`skipped_<why>` in the outcomes, `rejected_starts` in
  `session.json`).
- **Moves** come from `itb_live actions` on the board after the prefix,
  filtered by what the UI allows (`ui_sequence_ok`: move then act;
  Shifty / Post_Move one move after acting; Double_Shot two actions
  without moving; an unpowered pilot skill gives nothing), and executed as
  live play executes them (`play_step`: EXEC_MODE native, MOVE_NATIVE,
  ATTACK / TWO_CLICK_ATTACK by slot, REPAIR; no weapon or Repair from smoke
  or water). The choice is weighted: a (unit, kind, weapon) group first
  (weapons 3, moves 1.5, Repair 0.4), then a target by its effect's oddity
  (friendly fire, building hits, bumps, falls, kills, created units,
  terrain and status changes, and most for Lua errors, unmodelled writes,
  frame-timing flags and mission hooks), damped by how often the session
  played the same kind (`--uniform`: none of this). A turn ends with no
  legal sub-action left, after `--max-steps` (12), or at random
  (`--stop-prob` 0.06 per step).
- **Grid safety.** Only sequences the engine predicts keep `--min-grid`
  (2) grid and a living mech through every step and the enemy phase
  (worst case, no resist) are played (up to `--tries` 5, else the start is
  skipped): at 0 grid the game leaves for the Timeline Lost screens, and
  with no mech left it gives no player turn with an active mech again
  (fuzz 20261016_230406 sample 10, Mission_ScarabBoss turn 3: the engine
  had all three mechs dead, End Turn never came back).
- **Compared** after each step with `diff_boards` and live play's carried
  state (queued shots, Burrowers underground, structure shields; the
  recorded turn start's shields, underground units and
  Mission_Reactivation list seed each sample's run); End Turn and the
  enemy phase are `live_play.end_turn` itself (`--turns N`: N turns from
  each start). Created units are commanded through the uid map. A
  restored game's bridge has no turn-start building snapshot (it takes
  one when a turn begins), so it estimates no grid lost during the turn:
  the driver records the turn start's grid buildings (`grid_start` in the
  manifest) and `patch_state` takes the grid from them. Teleporter pads
  come from the checkpoint save the sample loaded when the bridge has none
  (`mission_save` in the manifest; right after a restore the profile's
  saveData.lua can hold none); live play reads the profile's saveData.lua,
  then undoSave.lua, then the bridge log's capture, only one made since the
  bridge last started (an older one can be another mission's). A step
  after a chance node whose only difference is a building the game kept
  (a Grid Defense resist) is no finding (`step_resisted`); the rest is
  re-predicted.
- **Findings** go to `recordings/fuzz/<session>/findings.jsonl` (git-ignored):
  `step`, `chance` (a step whose prediction drew a chance node, e.g. a Grid
  Defense roll: the rest is re-predicted from the live board and played
  on), `enemy_phase`, `game_refused` (the bridge refused a sub-action the
  engine allowed), `ui_blocked`, `uid_unmatched`, `engine_inconsistent`,
  each with the source turn, the plan up to it, the differences, both
  boards and the files: the sample's run directory `s<NNN>/` (states,
  predictions, `events.jsonl`) and `plan_to_step<N>.json`, so
  `itb_live predict <start> --plan @<plan>` reproduces the engine side.
  Any difference but `chance` ends the sample without End Turn
  (`--continue-after-diff` re-predicts and goes on).
- **Restore by relaunch.** The game reads its save files only at launch:
  Continue after the pause menu's Main Menu resumes the run still in memory
  (live 2026-10-16), and the title's Continue stays greyed when there was no
  run at launch. So each sample restores as `live_play.py restore` does,
  with its functions: quit the game, put the checkpoint's save files in the
  profile, relaunch (`launch_game`: `open steam://run/590380` through the
  offline Steam client, nothing else of Steam), click Continue on the title
  screen until a fresh player turn shows, then load once more against the
  Grid Defense restart quirk (as `restore --reload`: the gear at
  `--gear-xy`, default (43, 28), Esc as a fallback; once the heartbeat
  stalls, i.e. the pause menu is open, its MAIN MENU at `--main-menu-xy`,
  default (530, 438); Continue). The board is checked
  against the recorded turn start and the Grid Defense base against the
  save's difficulty; either one wrong stops the session. Title-screen
  clicks go out only right after a launch or a Main Menu, and a retry only
  while the bridge has stayed silent since the last click (it is silent on
  the island map too). The profile's own save files are copied to
  `<session>/profile_backup/` the first time (after the game has exited)
  and put back at every end (done, a mission that ended, a silent bridge,
  a failed restore): quit, swap, relaunch; with a run in them, one Continue
  (after `END_CONTINUE_DELAY`, 25 s), then the bridge gets `END_WAIT` (60 s):
  a mission that opens is loaded once more and its base checked; a silent
  bridge (the island map, where Esc does nothing) gets the second load
  blind: the map's gear at `--map-gear-xy`, default (38, 27), MAIN MENU,
  Continue, with a fixed `MAP_CLICK_WAIT` (5 s) after each click, since
  nothing can confirm them (said so in the output). A
  backup without `saveData.lua` (no run in progress) is put back as it is
  and the game left on the title screen. If the put-back itself fails,
  `--put-back <session>` does it again (`--dry-run`: only says what).
  A sample takes about 1 to 1.5 min of restore (quit, launch, 15 s before
  Continue, the load, the second load) plus the turn.
- **One driver at a time.** `live_play.py` turn / end-turn / mission /
  deploy / restore and the fuzzer hold an exclusive lock
  (`<bridge dir>/itb_driver.lock`, flock: released when the process ends).
  The fuzzer also refuses while another driver runs (`live_play.py`
  mission / turn / deploy / end-turn / restore, `drive2.sh`,
  `drive_final.sh`, `game_loop.py auto_turn`, another fuzzer; not dry
  runs), when the bridge acked a command or the current live run logged an
  event within `--idle-seconds` (300), or when the game is not running.
- **--dry-run** never touches the bridge: it generates and predicts on the
  recorded states and checks the engine against itself (each picked
  sub-action replays through `predict` to the board the enumeration's
  prefix gave, no refusal), writing `samples.jsonl` and the summary: the
  action mix, weapons, target kinds, effects, and the engine's own notes
  (chance nodes, inexact enemy phases, Lua errors, timing flags).
- **Loadouts** (`--loadout SPEC`, repeatable, one per sample in turn;
  `--coverage`). Before a start is played its checkpoint is rewritten
  (`scripts/loadout.py`) so the mechs carry other weapons, upgrades,
  passives and pilots. SPEC: `<mech>:<primary>[,<secondary>][;pilot=<id>]`
  groups, `;`-separated; mech = squad index (the bridge uid); exact ids
  (`_A`/`_B`/`_AB`); `*` keeps a slot, one weapon drops the secondary,
  `<mech>:pilot=<id>` changes only the pilot. In a mission the mechs are
  the battle region's `map_data` pawns (SquadData has `skip_pawns`):
  `primary`/`secondary` (base id), `_power` (PowerCost, +1 off class:
  `Skill::IsClassPenalty`, a TechnoVek mech pays for every other weapon,
  any other mech for another class but "", Unique, Death, Enemy),
  `_power_class`, `_mod1`/`_mod2` (`GetUpgradeCost` cells; an upgrade is on
  with its first cell), `_uses` (a new limited weapon: Limited, +1
  Conservative, +1 Ammo), `pilot` (id, name, skills, `power`), `reactor`
  (free/used normal, bonus and undo cores; cells 1/2/3), `health` /
  `max_health` (must be `Pawn::ComputeHealthTotal`, else the game resets
  it and heals to full: Zoltan 1, else type Health + Health/Skilled 2 each
  + 2 healthPower + 3 Rock + Networked Armor 1/2 squad-wide + 1 health
  psion; damage taken is kept), `bForceField` (Zoltan's turn-start shield),
  `iMutation` (Psionic Receiver: the board's psion). GameData.current
  repeats weapons and pilots. Both saveData.lua and undoSave.lua are
  rewritten; `loadout.py --check` recomputes all of it on every recorded
  checkpoint, then rewrites each twice (every mech with nothing changed:
  it still fits, every weapon and pilot skill is on or off as before and
  the recorded bridge state verifies; and a pilot injected, each with a
  PowerCost and Zoltan in turn: powered, Zoltan 1/1) (8415 mechs in 2805:
  all match; `--no-round-trip` skips the rewrites). Legal loadouts only: what a
  run can hold (drops.lua shop/pod lists, the squads' starting weapons,
  upgrades the weapon has, CreatePilot pilots), no weapon base twice in the
  squad or its storage, no named pilot twice, Vek pilots stay in their
  TechnoVek mech, the old pilot's XP and skills (blacklisted ones become
  Move / Popular / Invulnerable), missing cores bought. The engine is asked
  first, on the recorded start with the loadout (`loadout_solve_input.json`):
  a new weapon needs targets in `itb_live actions` without Lua errors or
  unapplied Lua writes (none from that start: `inconclusive`, tried
  elsewhere); passives and pilots by name (`passive_of`, the engine's own
  strings, the game's `IsPassiveSkill` tests; Networked Armor and Ammo via
  the bridge's HP and uses); `no_effect`: not modelled, but outside a
  fuzzed turn (Pilot_Medic's extra Reset Turn, ...); `unsupported`: not
  played. Live, after the load the bridge must show the loadout
  (`weapons_exact`, `weapon_slots` power and `powered`, pilot, HP, the
  pilot's `skill_active`, Zoltan's shield; each on or off as injected: a
  kept weapon or pilot the player never powered, at `{0}`, stays off, as
  `Pilot::IsAbility` grants a Skill with a PowerCost only while its first
  cell is set; a Zoltan mech is 1/1, which bridges before 2026-10-17 showed
  as 1/type Health, `live_play.fill_zoltan_max_hp`), else `loadout_rejected` (a finding; the
  next sample restores anew). The bridge reads weapons and pilots from the
  save, so the game's own word is HP (a wrong max_health is reset), the
  live IsAbility and shields, and every step. Each sample keeps
  `s<NNN>/loadout.json` and the rewritten saves; samples, findings and
  `session.json` (`loadout_items`: each item's status; `coverage_left`)
  carry the loadout. `--coverage` gives each living mech an unseen weapon
  of its class first (else off class), a passive as secondary while one is
  left, and an unseen pilot; an item goes once (unplayed or inconclusive
  ones come back, 3 times at most). `--unfired` gives each living mech two
  active weapons the recordings never fired (carried or not; no passives
  or pilots), and one goes only once the game fired it. With either, the
  choice of sub-actions favours a never-fired weapon 25 times over as a
  group (`FIRE_BOOST`), and the turn does not end at random while one can
  still fire; the summary says which injected weapons fired.
- `scripts/coverage_report.py` lists the player weapons (upgrades apart),
  passives, pilots, level-up skills, missions and enemy types the recorded
  turn starts hold (and the weapons the executed plans fired) against the
  game's scripts (`ITB_GAME_DIR`; enemies via `itb_inspect --pawns`):
  names only. Live runs and live fuzz sessions (`--fuzz`, default
  `recordings/fuzz`; dry runs left out) are separate columns; weapon totals
  count what a run can hold, the rest listed apart.

## Python bindings

The live bot (`src/solver/cpp_solver.py`) uses the engine through the module
`itb_engine` (`python/itb_engine_py.cpp`, pybind11 2.13.6 fetched by hash),
built only with `-DITB_BUILD_PYTHON=ON`, for the interpreter CMake finds:

```bash
cmake -S engine -B engine/build -G Ninja -DITB_BUILD_PYTHON=ON -DPython_EXECUTABLE="$(command -v python3)"
cmake --build engine/build   # -> engine/build/python/itb_engine.cpython-39-darwin.so
```

JSON in, JSON out. Every call takes the bridge state as JSON text (what the
bridge writes; a recording wrapper with `data.bridge_state` works too) and
loads it with the recording loader, plus pilot abilities and repair skills
(as `itb_inspect --solve` does) and `can_move: false` (the bot's mark for a
mech that moved and has not acted: `Pawn::moved`).

```python
import itb_engine, json
e = itb_engine.Engine(game_root="", threads=8, difficulty=0)  # "" = ITB_GAME_DIR / build default
r = json.loads(e.solve(bridge_json, json.dumps({"time_limit": 8.0})))
r["plan"]            # [{"uid", "move": [x,y]|None, "kind": "none"|"weapon"|"repair",
                     #   "weapon", "target", "target2", "description"}], Plan::actions
r["worst_case"]      # tiers by name; "upper_bound", "proven_optimal",
                     # "proven_components", "chance_exact", "contingent", "stats", "warnings"
r["simulation"]      # the plan run step by step with every chance node's default outcome:
                     # steps[i].after_move / after_action, post_player_board,
                     # final_board (after the enemy phase), enemy_phase, score
e.simulate(bridge_json, plan_json)   # the same for any plan
e.evaluate(bridge_json, plan_json)   # evaluate_plan: worst case over every chance outcome
e.load(bridge_json)                  # the board as loaded, loader warnings
```

Boards are serialized in the bridge's shape (`terrain` names and
`terrain_id`, `building_hp`, `fire`/`smoke`/`acid`/`shield`/`pod`; units with
`x`/`y`/`hp`/statuses/`active`/queued shot; `boosted` includes the Boost
psion, as `Pawn:IsBoosted()` does), so the bot reads them with
`Board.from_bridge_data` and diffs them against the live game exactly as it
did the old solver's predictions. Pawns the engine removed are absent;
fallen and burrowed pawns are marked `off_board`. `ENGINE_VERSION` names the
prediction semantics (bump it when the same board predicts differently) and
`BUILD_GIT` the commit built. Each `Engine` holds one engine and Lua state
per thread; extra engines load in parallel (about 1.3 s for one, 2.7 s for
eight).

## How game data is loaded

`GameData::load` runs the game's scripts in Lua 5.1, in the same
`GetScripts()` order the game uses. It supplies:

- the constants and value classes the binary binds natively (`Point`,
  `SpaceDamage`, `SkillEffect`, the list types), with luabind's rules;
- inert stubs for the remaining native functions the scripts call while
  loading (UI classes only).

The stubs are discovered automatically and reported, and the load verifies
that no stub shadows a Lua definition. Pawn definitions are read with normal
Lua inheritance. That covers every `AddPawn` entry plus every global table
derived from `Pawn`: 232 definitions in build 21601364.

## Status

Done so far (build order from the decompile):

- **Stage 1: core state and data.**
   - `core.hpp`: constants and A1–H8 notation.
   - `board.hpp`: tiles, pawns, and the pawn-list order that `Board::AddPawn`
     produces.
   - `game_data.hpp`: all pawn definitions, animation definitions and tuning
     values, read from the game's scripts.
   - `recording.hpp`: all 469 recorded boards load.
- **Stage 2: per-tile damage and status rules** (`tile_rules.hpp`).
   - The game's ordered `DamageSpace` steps in all three modes (weapon,
     push/bump, and the explosion mode corpse explosions use).
   - Pawn damage: shields, frozen, turn shield, armor and ACID.
   - Terrain, buildings and grid, cracks, items/mines.
   - Fire, smoke, acid and freeze interactions, terrain dangers, and the
     game's per-frame tile and pawn rules, either one frame at a time for the
     executor (skipping busy pawns) or until the board stops changing.
   - Drowning (`BoardSpace::CheckTerrainDangers` @00926090) takes only an
     idle pawn. A pawn mid-push still gets the rest of the tile's dangers, but
     it does not sink: a hit that breaks cracked ice (or melts ice with fire,
     the Flamethrower) under a unit and pushes it sends it to the next tile
     alive (live 2026-10-11 m26 turn 2: a Bouncer's hit on a Centipede). If
     the push is blocked, the pawn drowns once the push ends.
   - Grid Defense resists are a chance node: the caller resolves each roll, and
     every roll is logged. Pushes, spawns and Lua scripts are recorded for the
     later stages.
   - Webs are tile state, as in the game (`BoardSpace::SetGrappled` /
     `ClearGrapple`): the emitting tile keeps the directions it webs
     (`Tile::web_out`), the held tile counts its webs (`Tile::web_in`), and
     `Pawn::webbed` is the pawn's flag. A pawn webbed by two Scorpions stays
     webbed until both let go (smoke, a flip or freezing on the emitter, or
     the web check: emitter tile without an enemy, held tile without a pawn,
     Disable_Immunity). A relocated pawn loses the flag, the web stays on the
     tile, and a pawn standing on a held tile is webbed again once idle.
     A spider egg (`SpiderAtk1/2` land a `WebbEgg1`, then `AddGrapple` from
     its tile to each neighbour) webs only who stands next to it as it lands:
     `SetGrappled(d)` (@0091b300) skips a neighbour that is not grappleable,
     so a pawn arriving later is not caught, and the web breaks when the egg
     leaves its tile. All webs go when the enemy phase starts
     (`BoardSpace::NextTurn(6)` @0091b950).
- **Stage 5: movement** (`movement.hpp`).
   - Pathing profiles, move budget and pilot move skills. A recorded speed
     keeps what `base_move` does not compute live as a standing bonus; the
     loader puts each pilot's ability (`GameData::pilot_ability`, its Lua
     Skill) on the pawn first, so Youth_Move (+3 on turn 1) and
     Arrogant_Boost are not counted twice (live 2026-10-13: a 10-tile move
     planned with an 8-tile budget).
   - The game's reachability search and its weighted A* walk path (float32,
     (f, x, y) tie-break).
   - Walks, leaps, charges, teleports and burrows. With a stage 2 context,
     Injured goes through the stage 2 health change and the tile a move ends
     on is settled.
   - Every first move the old bot executed in a recording is reachable here:
     `itb_inspect --moves recordings`.
   - The `Move` skill's pawn abilities: `Pawn:IsAbility` (Lua and the native
     port alike) reads the pilot's skill through the one name table of
     `pilot_skill_ability`. Adjacent_Heal (Rosie Rivets, `Pilot_Caretaker`)
     heals each adjacent player-team pawn but the mover by 1 after the move
     (−1 damage, so like any heal it also clears fire, frozen and ACID,
     even at full HP). It had no ability bit until live run 34 (2026-10-15,
     Mission_Final turn 4): the game healed the TrimissileMech next to her
     from 1 to 2 HP, the engine predicted 1.
- **Stages 3 and 4: pushes, deaths and the SkillEffect executor**
  (`executor.hpp`, `timing.hpp`).
   - A frame-exact simulation of how the game resolves an effect: the six
     phases of the game's frame, an integer frame clock at a fixed frame rate,
     and every timer replayed in the game's float32 arithmetic. Frames where
     nothing can happen are skipped.
   - The step is what `CFPS::OnLoop` measures, which is a hair less than the
     real frame time: it takes the frame's length from its second clock read
     of the frame before to its first read of this one, so it misses its own
     bookkeeping in between (`kCfpsMeasureGap`, 100 ns; any value from 10 ns
     to 10 µs gives the same frame counts). At a steady 60 fps the step is
     just under 1/60, so a timer that is a whole number of frames takes one
     more update than float32(1/60) (which rounds up) would give: a 0.4 s
     push lands on update 25 and a 0.3 s delay fires on the 19th countdown.
     Live 2026-10-13 run 17 (m08 turn 1) shows it: a Pierce Shot pushes the
     shooter's own mech, 0.3 s after launch, into the tile of a Moth the shot
     killed. The mech moved in unhurt, so the Moth's body was past half its
     death animation (36 updates) when the push landed, which only the
     slower step gives (the old step landed it on update 35: a bump).
   - `Board::ApplyEffect` and the stacked-effect queue (FULL/PROJ/positive
     delays), projectiles, artillery, lasers, melee lunges, walks, leaps,
     charges, teleports and burrows.
   - Pushes and bumps with the game's same-frame ordering, dying bodies that
     block for half their death animation, death effects (ACID pools, Fast
     Decay, Lua hooks), corpse explosions, psion leaders and body removal.
   - A death animation fades out: `Pawn::SetupAnimation` gives a non-corpse
     pawn's `<Image>d` animation `SetFadeOut(0.5)`, so `Animation::Update`
     holds its last frame 0.5 s longer before it is done (the body stays that
     much longer, and is busy in the enemy phase). The frame tracker always
     loops (`Animation::LoadData` sets its loop flag), so during the hold it
     keeps running from its overshoot and `GetTotalProgress` falls back to
     (n − 1) / n: a one-frame death animation restarts from 0 and its body
     blocks pushes again for the first half of the hold. Live 2026-10-13
     run 17 (m36 Mission_Barrels, turn 2): a Pierce Shot killed an ACID vat
     (`barrel1d`, one frame of 0.5 s) and pushed the shooter's own mech into
     its tile 42 updates later. The game bumped the mech (the vat's progress
     was back at 0.37), where a Digger2 in the same spot 42 updates after
     its death (8 frames) is passed through.
   - `Board:Bounce` (the script `SkillEffect:AddBounce` adds) is not only
     cosmetic: the tile's `BounceAnim` runs 0.5 s and `BoardSpace::IsBusy`
     counts it (busy code 2), so FULL_DELAY chunks, death effects and the
     quiescent-frame work (kill credit, corpse detonation, the end of the
     last shot) wait for it (`Resolver::bounce`). A bounce of at most 9 is
     skipped on a tile whose first pawn spans two tiles. Live 2026-10-14
     run 23 (m00 Mission_Dam, turn 3): a Vice Fist throw killed a Mosquito
     with an unanimated damage entry; without the bounce the last shot ended
     before the death was processed, the kill went to nobody and Viscera
     Nanobots did not heal the Judo, as the game did.
   - Boost and Vek Hormones are baked in when an effect is computed.
   - Critical Shields (`Passive_CritDefense`, AE, PowerCost 0): an
     unresisted hit on a populated building that leaves the grid at exactly
     1 raises event 17 (`PowerBar::Damage` @008f4230). Events are current
     for one frame, the frame after they are raised, so the next frame's
     `Board::OnLoop` (@00955070, after the weapon animations, before the
     tiles) shields every tile whose terrain is still BUILDING (objective
     and unique buildings too; not mountains, not rubble). It fires every
     time a hit ends at 1, in either phase; a resisted roll or a hit to 0
     does not fire it, and a second hit in the same frame is not blocked.
     As for every passive, only the skill's power counts, not whether its
     mech lives. The bounds stay sound: shields never raise grid power or
     building HP, they only stop losses.
   - Void Shocker (`Passive_VoidShock`, AE; the `_A` upgrade exists only
     in the localization: always 1 damage, and a second carrier adds
     nothing). When `Board::OnLoop` (@00955070) ends the last shot (no
     effect active any more), it checks the shot's "hit something" flag:
     set by `Pawn::Damage` when a hit takes HP from a living pawn (any
     team, the attacker included, bumps too; never a Wall, RockThrown or
     BombRock) and by `BoardSpace::DamageSpace` when a building loses HP
     (a Grid Defense resist is no hit; mountains, ice and rubble are not
     buildings). A shot that hit nothing, is not "Move", and whose owner
     is enemy-team (Vek and bots), has no queued shot left (the instant
     part of a telegraphing skill is spared) and is not `VoidShockImmune`
     (Blobbers, Spiders, eggs, psions, Shamans, three bosses) costs the
     owner 1 HP: a raw `ModifyHealth` (shield, ice, armor and ACID do not
     matter), with an ANIM_DELAY bolt on its tile; it can kill (an
     ordinary death, with no shooter). A `TC_Enrage` shot (Science_TC_
     Enrage, the mech's) shocks the enraged unit instead, whatever its
     type. A shot keeps its flag while the same owner, team and name
     re-record it (delayed remainders); a new record clears it
     (`RulesContext::shot_hit`). Fuzz 2026-10-18 s1: a Leaper whose bite a
     building resisted and a Centipede that spat at an empty tile took 1
     each. A death effect (`Pawn::TriggerDeathEffect`: Lua death effects,
     ACID pools, Fast Decay) is a follow-up and keeps the record. A corpse
     detonation (`Pawn::DetonateCorpse`: Explosive Vek, the Blast and Boss
     psions' explosions, the Arachnid Psion's egg, Burns) is not: it
     carries the shooter's owner and team but no name, so it starts a new
     record and is judged on its own. A Vek whose kill only left a spider
     egg, or set off an explosion that hit nothing, is shocked (fuzz
     20261018_082428 s7: Scorpion2 killed a Burnbug under the Arachnid
     Psion and took 1).
   - Outcomes that hinge on two events within one frame are reported as
     timing-sensitive; Grid Defense rolls and spider-egg picks are logged as
     chance nodes.
- **Stage 6: the Lua host** (`lua_host.hpp`).
   - Runs the game's own weapon scripts (`GetTargetArea`, `GetSkillEffect`,
     queued Vek attacks, two-click weapons) and death effects on an engine
     Board, as the accuracy reference for C++ ports.
   - Native bindings follow luabind's rules: exact-arity overloads, strict
     argument types, aliasing `SpaceDamage&` / field references, one shared
     instance metatable (so `tostring(p)` and pawn `==` raise, as in game).
   - `random_int`, `random_bool` and `math.random` share a glibc `rand()`
     clone. Live seeds are hidden, so seeded results (e.g. the Large Goo
     split) are one sample of a random outcome.
   - Board queries read the engine Board directly; mutating bindings are
     reported (`LuaWrite`), not applied.
   - All 559 weapon ids run on the test boards without Lua errors, and
     every mech weapon and queued Vek attack in the recordings runs cleanly:
     `itb_inspect --weapons recordings`.
   - C++ ports of the hottest weapon scripts (`weapon_ports_list.cpp`; Lua
     stays the reference): 62 Lua functions (`GetTargetArea` /
     `GetSkillEffect` of Move, repair, the base tank / artillery / laser
     classes, 40-odd squad and Vek weapons, a few mission units and the
     deployable tanks): 574 weapon methods, 99.4% of those calls on the
     60-board sample. A port is
     bound to a weapon table only when the method resolves to the very Lua
     function it mirrors (source file, line and an FNV-1a hash of its text,
     likewise for the helpers it stands in for: `GetProjectileEnd`,
     `Laser_Base:AddLaser`, `CallMethod`, ...) and every table field it
     reads has the expected type; it uses the same native bindings as the
     Lua (`lua_native.hpp`) and returns "not handled" wherever the Lua would
     raise, so that call runs in Lua. `LuaHost::set_native_weapons(false)`
     (or `ITB_LUA_WEAPONS=1`) runs everything in Lua. Checked by
     `itb_inspect --diff-weapons` (469 recorded boards and 300 random boards,
     51 M skill effects: no difference) and a fast subset in the tests.

- **Stage 6 integration: shots end to end** (`engine.hpp`).
   - `Engine::move / fire_weapon / repair / fire_queued` run the game's
     own skill: Lua target area and `GetSkillEffect` (two-click weapons
     through `TranslateFirstClick`, `GetSecondTargetArea`,
     `GetFinalEffect`), `PrepareEffect` and `CheckAlterations`, the
     executor, then the `Pawn::FireWeapon` bookkeeping.
   - `sScript`s and `GetDeathEffect` run in the same Lua host against the
     board as it is at that frame; their writes (`Board:AddEffect`,
     `SetTerrain`, `Pawn:SetFrozen`, `Game:ModifyPowerGrid`, ...) are
     applied, the rest reported. Death effects that draw random numbers
     are logged as chance nodes.
- **Replay validation** (`itb_inspect --replay recordings`).
   - Replays every recorded player action from the board the old bot
     solved, and compares each move and attack with what the game did: the
     old bot's prediction plus the differences its verifier recorded, on
     exactly the fields that verifier compared. Results are split by how
     the bridge executed the action (native `FireWeapon`, `AddEffect`, or
     the bridge's own emulation) and by whether the old simulator was right.
   - Known recording artefacts are labelled, not counted as engine errors:
     multi-tile pawns, the old simulator's `active`, unrecorded weapon
     upgrades (a powered variant reproduces the game exactly), the bridge's
     SKIP behaviour.

- **Stage 7: the enemy phase** (`enemy_phase.hpp`, `environment.hpp`).
   - `Engine::end_turn(board, TurnContext)` runs End Turn up to the end of
     the enemy's spawns, in the game's order: Pawn::EndTurn on player pawns
     (Networked Shielding off), webs released, the six status-tick phases
     (one pawn per idle step, fresh list per phase, UpdateLeaders after each),
     environment steps, queued shooters (first in list order, re-aimed from
     the current tile, smoke/death/ice/water cancel), then the victory check,
     the mission's enemy NextTurn and the spawn cursor (blocked spawns take a
     push-mode hit and keep their marker; spawns on water/chasm are dropped),
     then "Temp Units Die" (below). Every step resolves to idle on one frame
     clock; mission per-frame hooks (UpdateMission) run at every simulated
     frame. It stops before AI planning. `Engine::play_turn` runs the
     player's actions first.
   - "Temp Units Die" (`Board::KillTempUnits` @009460b0): once the last
     spawn attempt has resolved and before the AI plans, every living pawn
     whose type has Lua `TempUnit = true` is killed (`Pawn::Kill`), in list
     order and whatever its team, after a 1 s delay. `Board::SpawnQueued`
     arms it, so it does not happen when the mission ends at the victory
     check (the final turn). It is an ordinary death, not a removal: death
     effect, an ACID pool for an ACID pawn, `Explodes` and `Corpse` all apply
     as for any death. The only shipped temp unit is the Bombling Mech's
     walking bomb (`DeployUnit_Bomby`: no corpse, no death explosion; its
     self-destruct is an active skill), so an unused bomb just disappears
     and its neighbours take nothing (live 2026-10-11, m71). Event
     `TempUnitDied`. Scoring: the bomb is player-team and not a mech, so its
     death is neither a mech loss nor a kill; the only tier it touches is
     position (a living bomb next to an enemy cost -1 there).
   - `PhaseResult`: events, every chance node (Grid Defense, environment
     orders and choices, Lua death-effect randomness, spider eggs, mission
     picks), timing flags, Lua diagnostics, spawns of unknown type, and
     `exact = false` when the recorded data does not pin the result down.
   - Environments sit behind `Environment` (the mission's LiveEnvironment
     plus its combat hooks), dispatched on `mission_id`; a Lua-backed
     implementation can replace the native ones once the bridge exports the
     mission instance (spec stage 7 section 4). Table below.
   - The per-frame hooks run in the player's turn too: the game calls
     `Mission:BaseUpdate` from `BoardPlayer::OnLoop` (0x008c9760) on every
     frame of every state but the finished one. With `ActionOptions::mission`
     set, `Engine::move / fire_weapon / repair` run the environment's
     `update` on every frame of the action (the environment re-reads the
     pawns it watches from the board first, `Environment::bind`). A repair
     under a living Storm Generator keeps ACID; a dam the player destroys
     floods at once (a hook's effect has the Lua default owner -1, not
     ENV_EFFECT, so the Vek the flood drowns are the dam killer's kills and
     XP); a dead shield generator drops every shield before the
     next action. `action_options(TurnContext)` builds these options:
     `play_turn`, the solver, `itb_live`, the Python module, `--predict` and
     `--replay` all play actions with them. Missions without a per-frame
     hook take no hook at all (`native_environment_has_update`).
   - The Soldier psion's +1 is tracked per pawn (`Pawn::health_bonus`):
     pawns that appear while it lives get it, pawns another psion does not
     affect keep a stale one. The Psion Tyrant reaches every player-team pawn.
   - Recordings: the mission data stage 7 reads (`Recording::mission`) and
     AE pilot level-up skills (Thick Skin, Technician).
   - Pilot experience (`itb/pilot_xp.hpp`, bridge `pilot.xp`): a non-mech
     enemy-team death counts for the last shooter (`EventSystem` LastShot,
     cleared at the first frame without an active effect; an effect without
     an owner keeps it). Unless the victim is Minor it gives XP equal to its
     max HP: to the shooter if its id is 0-2, otherwise split over the living
     mechs with a pilot (the remainder is random: `ChanceKind::XpSplit`, a
     node only when it decides a level-up). XP applies at the quiescent frame
     (`Pawn::UpdateKills`), 25 XP to level 1, 50 to level 2, excess lost.
     The new skill applies at once: Health / Skilled +2 max HP and +2 HP,
     Opener (first turn) / Closer (last turn) Boost, Conservative +1 use,
     Thick Skin, Technician; Move, Skilled, Opener, Closer and Pain move is
     read live by `base_move`. Kills also feed Experienced (+2 XP), Adrenaline,
     Viscera Nanobots (`Passive_Leech`, heal; `Passive_Psions` is Psion
     Leech) and KO_Boost.

- **Stage 8: the turn score** (`score.hpp`, `objectives.hpp`).
   - Strict tiers: grid and buildings, mechs, mission objectives, kills,
     position (see "Turn score" below).
   - Every shipped mission's objectives and the nine bonus objectives, in the
     game's reward units, from the recorded mission fields
     (`MissionData::objectives`).
   - `itb_inspect --score` scores a recorded plan; `--replay --turns` checks
     the objective counters against the game.
- **Stage 9: the perfect-turn search** (`solver.hpp`, `board_hash.hpp`).
   - The best worst-case plan through the enemy phase, over interleaved
     sub-actions of every unit, chance adversarial and enumerated; anytime
     with sound upper bounds, proven optimal when the search completes.
   - `itb_inspect --solve`: plans on recorded boards against the old bot's.
- **Stage 10: the enemy AI's planning and the deployment search**
  (`ai_phase.hpp`, `deploy.hpp`; "Enemy AI planning and deployment" below).
   - `Engine::ai_phase`: every Vek's move and queued attack after the enemy
     phase (AiController / AiPlanner and the Lua scoring), seeded like the
     game; `itb_inspect --ai` checks it against the live saves.
   - `deploy_search` / `itb_live deploy`: where to drop the mechs, through
     the enemy phase before turn 1 and the AI's sampled answers, each turn-1
     board solved.

### Deployable units

The support weapons that drop a player-controlled tank
(`weapons_deploy.lua`: `DeploySkill_Tank`, `_AcidTank`, `_ShieldTank`,
`_PullTank`, `_IceTank`, each with `_A` / `_B` / `_AB`) work end to end:
engine, solver, loader and live play. No squad starts with one (they come
from shops and rewards); the AE Bombermechs and Arachnophiles create player
units the same way (`Ranged_DeployBomb`'s walking bomb, `Ranged_Arachnoid`'s
spider), and the live runs of 2026-10-11 to 10-14 played 59 turns in which a
bomb created that turn moved and acted, every step matching the game.

- **The weapon.** `Deployable` (`weapons_base.lua`): LineArtillery's target
  area with `OnlyEmpty` (2 to 8 tiles in a line, none blocked for ground
  units: no pawn, mountain, building, water or chasm), `Limited = 1` (one
  use per battle; the bridge's `weapon_slots` give the uses left), and one
  artillery shot whose SpaceDamage carries `sPawn = self.Deployed`. The
  upgrades are pawn types (`Deploy_TankA` +2 Health, `Deploy_TankB` the
  2-damage shot, `Deploy_PullTankB` flying, ...; the Ice Tank's `_A` and
  `_B` point at each other's pawns), so the unit's type carries them.
- **The unit.** `Board::DamageSpace` (B6) creates it with its type's default
  team (TEAM_PLAYER), and `Board::AddPawn` sets it active
  (`Pawn::SetActive(true)`, which refuses only a pawn whose one skill is
  Move and whose move speed is 0): it moves and fires in the turn it lands,
  then every turn after, one move and one action like a mech without a
  pilot. It is not a mech (`Pawn::SetMech` is for squad units), has no
  Repair, and the engine numbers it `Board::next_uid`.
- **Score.** Its death is no mech loss and costs no star (it is not a
  mission unit); Vek target it like a mech (`Skill:ScoreList`: any
  enemy-team pawn that is neither dead nor a TempUnit scores ScoreEnemy).
  The position tier counts it (+2 alive, +1 per HP), so a dead tank never
  scores better than a living one next to a Vek (the -1 for an enemy next
  to a player non-mech unit would otherwise reward losing it).
- **Ports.** `Deployable:GetSkillEffect`, `Deploy_AcidTankShot`,
  `Deploy_ShieldTankShot` and `Deploy_ShieldTankShot2` run natively (the
  other tank shots were already: TankDefault, Science_Pullmech); `itb_inspect
  --diff-weapons recordings --random 300` finds no difference for them.
- **Live play.** The deploy is an ordinary `ATTACK` (the mech's slot of the
  exact weapon id). The bridge lists a unit created this turn at once,
  active, with its type's SkillList as `weapons` and no `weapons_exact` /
  `weapon_slots` (the save has no record of it until the next turn); the
  game numbers it itself. `live_play.py turn` matches it after the deploy
  step by type and tile (`unit_uid_map`) and sends the tank's `MOVE_NATIVE`
  and `ATTACK` under the game's uid (slot from `weapons`). The map is
  explicit and one to one for every unit created that turn: earlier matches
  keep their live uids, equal numbers map to themselves, and a created
  unit's step is never sent under the engine's raw uid (re-solve if it has
  no match). The two counters drift apart, so the engine's second unit can
  carry the game's uid of the first (live 2026-10-16: two tanks, then two
  spiders, refused moves). From the next turn on it is an ordinary unit of
  the board. No bridge change was needed.
- Tests: `test_deployables.cpp` (every upgrade's unit, the drop area and the
  one use, acting the turn it lands, each tank's shot, score, Vek targeting,
  the solver dropping a tank and firing it, keeping it alive),
  `test_live.cpp` (the live bomb turn replayed against the game's board, a
  deploy tank through `itb_live`), `test_weapon_ports.cpp`, and
  `tests/test_live_play.py` (the uid mapping and commands).

### Environments and mission hooks

"Exact" means: reproduced from what today's recordings contain. Newer bridge
fields (`environment_tides_index`, `environment_wind_dir`,
`mission_final_volcano`, `mission_final_cave`, `mission_hacking_*_id`) are
used when present.

| mission (env) | native behaviour | from recorded data |
|---|---|---|
| Mission_Airstrike (Env_Airstrike) | plane, then DAMAGE_DEATH on the 5-tile cross | exact (centre = the mark with 4 marked neighbours) |
| Mission_Lightning (Env_Lightning) | 4 strikes, DAMAGE_DEATH, one per step | strike order is hidden RNG: chance node `EnvOrder` over the k! orders of occupied strikes (empty strikes commute) |
| Mission_Crack (Env_Seismic) | 3 path tiles become chasms, in path order | exact when the path direction follows from last turn's chasm or the board edge; else chance node (2 orders, only if 2+ strikes are occupied) |
| Mission_Cataclysm (Env_Cataclysm) | column collapses top to bottom, 0.2 s per tile, buildings spared | exact (column from the marks; no marks = nothing can change) |
| Mission_Tides (Env_Tides) | row Index floods, building shadows, mountains DAMAGE_DEATH first | exact (row from the marks or the recorded index) |
| Mission_Terratide (Env_Terratide) | row 7-Index smoked from the bottom, no shadow | exact |
| Mission_SnowStorm (Env_SnowStorm) | 3x3 block frozen in one frame | exact (`environment_freeze`) |
| Mission_Wind (Env_RandomWind) | two columns pushed from the downwind edge, 0.2 s after occupied rows | exact with `environment_wind_dir`; old recordings: chance node `EnvChoice` (Down/Up) + EnvInexact |
| Mission_Belt / BeltRandom (Env_Belt) | belts push in `Belts` list order (downstream first), 0.2 s after occupied belts | exact with the bridge's `env_instance` Belts/BeltsDir; else from the tiles, exact unless chains feed each other; a belt breaking mid-phase is EnvInexact (CheckBelts quirk) |
| Mission_Final (Env_Volcano) | rocks: fire artillery DAMAGE_DEATH; lava: tile to lava; super-volcano repaired; NextTurn at turn 0 (the enemy phase after the deployment, before the spawns and the AI): the 7 pylons drop, a populated 2-HP building on every tile of zone `pylons` | exact with `mission_final_volcano`; old: mode and order from the tile pattern (EnvInexact for a single tile); pylons from the bridge zones (none recorded: EnvInexact, no drop) |
| Mission_Final_Cave (Env_Final) | rocks drop (DAMAGE_DEATH, ground), tentacles (DAMAGE_DEATH, lava); bomb re-drop (UpdateMission on an idle board: in the enemy phase not before the status ticks and the environment's steps are done, so a bomb the Psion Tyrant kills comes back after the strikes, struck tiles allowed; a drop on a Vek kills it) | exact with `mission_final_cave`; old: phase from the turn number (EnvInexact), phase-2 order a chance node; bomb drop a chance node `MissionRandom` |
| Mission_AcidStorm | every pawn ACID every frame while the generator lives | exact |
| Mission_Shields | new pawns shielded; generator death drops every shield | assumes every recorded pawn already had its shield (`ShieldedUnits` not recorded) |
| Mission_Hacking | facility death turns the bot into Snowtank1_Player | exact with ids; old recordings infer the bot (EnvInexact if several) |
| Mission_Dam | dam death floods two columns, 0.3 s per row | exact |
| Mission_Train / Armored_Train | train fires after every Vek; wreck replaces a dead train | queued move inferred (bridge omits team-1 shots) |
| Mission_Satellite | queued launch: DAMAGE_DEATH around, rocket flies away; NextTurn powering | exact with `queued_launch` |
| Mission_Volatile | last enemy retreats | exact with `is_infinite_spawn` |
| Mission_Reactivation | NextTurn draws from the mission's `Enemies` list (the units it placed frozen) until 2 are thawed or the list is empty; a drawn unit thaws only if alive and frozen, and every drawn entry leaves the list, so a unit the player re-freezes after its thaw stays frozen | player-visible list only (the mission's own `Enemies`, exported as `mission_state.instance.Enemies`, is hidden and never read): the state's `reactivation_visible_list` from live_play.py (units frozen at the start minus those the mission was seen to thaw) rides on the pawns (`Pawn::reactivation_listed`); candidates are listed alive frozen units, chance node `MissionRandom` over their pairs; discarded draws stay listed when 2 thaw (which ones is hidden; worst case); each `thawed` event names the draw's candidates (`detail` "candidates u1,u2,...", `amount` their count). Without the list (old recordings, a mission joined late): every frozen enemy is a candidate |
| anything else with marks | none | `EnvUnsupported` (NanoStorm, Sandstorm, LandChange are unshipped) |

Conveyor directions come from the live belts: the Env_Belt instance's
Belts/BeltsDir, else the `conveyor<dir>.png` custom tiles. The bridge's
per-tile `conveyor` field scans the whole save, which holds every region's
map, and on an island with another belt map it reports phantom belts and
wrong directions (live m34: B4 and C4 read as pushing left, not down), so
the loader replaces it (warning "conveyor field disagrees with the live
belts") whenever the bridge extension or a conveyor sprite is present.

Spawn types are hidden (decided when queued, not shown): spawns without a
type in `TurnContext::spawn_types` emerge without a pawn
(`PhaseResult::emerged_unknown`). The bridge reports spawns in scan order,
not queue order; when that can matter (a blocked spawn among several) the
result says so (EnvInexact).

### Validating the enemy phase

`itb_inspect --replay recordings --turns` (corpus of 469 recorded turns):
324 turns have a plan the per-action replay follows to its end; 231 of them
have a trustworthy next-turn board (the board read when the next player turn
began, `m*_turn_<t+1>_solve_input.json`, or the `post_enemy` summary when that
board was read after the next turn had begun). 77 final turns have neither,
9 have captures that disagree on grid power.

Between the end of the spawns (where the engine stops) and that capture the
game ran AI planning and movement. Differences that step explains are
classified, not counted: instant smoke on queued targets (Mosquitoes), pawns
created at planning (Digger walls, spider eggs, blobs, totems, hatching), ACID
pools picked up by AI moves, Vek stepping on mines, UpdateSpawning additions
(Holes, Factory, Spider boss, Acid). So are recording artefacts: lava the old
bridge reported as water, pilot level-ups, and the Storm Generator upgrade
(the bridge reports `Passive_Electric` without `_A`), and the old bot's
webs on every neighbour of a spider egg (its board reader guessed them; the
game webs only who stood there as the egg landed). Hidden choices and
Grid Defense rolls are tried as branches; emerged spawn types are taken from
the next board.

Turns whose player steps all matched the game: 223, of which 152 exact
(68.2%) and 70 more with only explained differences (99.6%); the one left is
a post_enemy-only comparison (building HP total off by 1, grid power
matching). Turns that needed a mid-turn sync: 8, 50.0%.

### Turn score (stage 8)

`score_turn(before, after, ctx, phase)` (`score.hpp`) compares the board at
the start of the player's turn with the board after the enemy phase. Tiers
are strict and compared lexicographically; the search takes the worst case
over chance outcomes, the score judges one concrete outcome.

1. **Grid and buildings.** [With `--grid-survival`: first the odds that the
   grid survives the turn, see "Grid survival" under the search.] Grid
   power, then structure HP over building tiles, both as net changes. Unpopulated and objective buildings count; a
   destroyed building (rubble, or water for a building on water) has left
   the sum with all its HP. A grid gain counts for the player (none happens
   mid-turn in the shipped missions).
2. **Mechs and stars.** Mechs alive, then objective stars failed this turn
   (`objectives.hpp`), then mech HP, over the mechs on the board at the
   start, both net: a repair offsets damage and a revived corpse counts +1
   (the game's own `Board:GetMechDamage`, which BONUS_MECHS reads, is max HP
   minus current HP). A mech fallen into a chasm is lost with its HP. Stars
   rank above mech HP because a lost star is gone for good while mech HP is
   restored after every mission (on the 540 recorded turns: 9 stars saved
   for 11 mech HP, nothing lost in a higher tier).
3. **Objective progress** (`objectives.hpp`).
4. **Kills.** Enemies killed, then enemy HP removed (net, so psion
   regeneration counts against the player). Enemies are team-6 pawns that
   are not neutral: Vek and enemy bots alike (both threaten buildings the
   same way); neutral team-6 pawns are mission props (hacked building, storm
   and shield generators, acid vats, minefield bots) scored as objectives.
   Only enemies alive at the start count, so this turn's spawns are not
   scored; kills by the enemy phase itself (fire, other Vek, environments)
   count like the player's. A retreat (minor flag set, HP 0) is not a kill.
5. **Position** (`position_terms`), a tie-break on the end board only:
   -3 per mech on fire (not fire-immune), -2 per mech with ACID, -2 per
   frozen mech, -1 per mech on smoke (not IgnoreSmoke or Disable_Immunity),
   -2 per mech at 1 HP; -1 per (enemy, adjacent building) pair (-2 for an
   objective building) and per enemy next to a player-team non-mech unit;
   +1 per enemy on fire or frozen; +2 per living deployed unit and +1 per
   HP it has (a support tank or an Arachnoid spider, `is_deployed_unit`; see
   "Deployable units"). One pass over the pawns. Spawn tiles are left out:
   a blocked spawn stays queued and hurts its blocker again next turn, so
   occupying one is not a clear gain.

**Next-turn threat (experimental, off by default).** `ScoreKey::Threat`
sits between objective stars and mech HP and is scored only with
`ScoreOptions::threat_tier` (`SolveOptions::threat_tier`, `itb_inspect
--solve --threat-tier`, itb_live / Python `"threat_tier": true`,
`live_play.py --threat-tier`, `ITB_THREAT_TIER=1` for the old bot). It is
minus the enemies on the scored board (the end of the enemy phase, just
before AI planning) that could attack a building in the coming AI phase
(`threat.hpp`): scored enemies (Minor pawns and bots included) that are
alive, not frozen and have a weapon that damages tiles, standing (after an
AI move: `GetReachablePoints` with their profile and speed, or staying; not
on smoke unless smoke-immune) where their weapon's reach class covers a
building with HP that is not frozen. Reach classes come from the weapon
name: footprints (melee, self-centred blasts, diagonals, artillery 2..N with
their extra tiles), projectiles (first obstacle that will still be there:
mountains, buildings, every pawn but enemies about to move; with splash for
the Centipede), lasers (through pawns up to a building or mountain), and
none (egg, blob and totem throwers, hatching). Unknown names get every tile
in line. Enemies that could attack a unit a mission objective protects
(the Terraformer, trains, tanks, bombs, satellites...) are counted apart
with those units as the targets, and each takes `kUnitThreatWeight` (2^26)
off the slot, above objective progress: such a unit is a star with few HP
(run 8 lost the Terraformer to two Vek that turn 1 left next to it while it
terraformed; the replay ranked this way killed them and kept it). Off, the tier is 0, its TierBounds cap is 0, and it is left out of
printed scores, JSON score vectors and tier counts (`tier_used`,
`components_for_tiers`), so every result is unchanged; on, tier counts
include it (`--min-tiers 5` = grid through threat). `--kills-tier` puts
the kill count in that slot instead (an experiment: kills above mech HP;
with `--threat-tier` too, kills - 100 x threats: threats first, then kills).
`--objectives-first` (`SolveOptions::objectives_first`, itb_live / Python
`"objectives_first"`, `live_play.py --objectives-first`) ranks objective
progress above the threat: the slot becomes 16384 x objective progress plus
the threat part (`kObjectivesFirstWeight`), so a step toward a bonus star is
never traded for a threat. It stays one tier, so the order is grid,
buildings, mechs lost, stars failed, [objective progress, threat, (kills)],
mech HP, ..., and `--min-tiers 6` keeps mech HP proven.
`itb_inspect --threat-check DIR` checks the proxy against the engine and
the recorded games and times it (see "Threat tier measurement").

**Next-turn lookahead (experimental, off by default).** `ScoreKey::NextTurn`
sits between objective stars and the threat tier and is scored only with
`SolveOptions::lookahead` (`itb_inspect --solve --lookahead`, itb_live /
Python `"lookahead": true`, `live_play.py --lookahead`). It is minus what
the board a turn leaves costs on the next turn (`lookahead.hpp`):

1. From the end of the turn's enemy phase, spawns of a type the player
   cannot see (they emerged without a pawn) get a pawn of an enemy type
   visible on the turn's board (a sample of the hidden type).
2. The AI plans the next turn (`Engine::ai_phase`, stage 10) with a sampled
   seed (the live seed is hidden; `LookaheadOptions::seed`, sample k uses
   `deploy_sample_seed(seed, k)`), each enemy's last target being the one
   it showed at the start of this turn; then the next player turn starts.
3. Reply: a greedy policy plays the next turn: rounds in which every unit
   that has not played tries its unit turns (move, then weapon or repair)
   aimed at the queued attacks (targets on or next to an attacker, its
   target or the line between them, a projectile line through an attacker;
   moves onto those tiles), each run through the enemy phase, and the best
   one by (loss, then queued attackers and their HP) is kept while it
   improves. Its loss (grid lost, then mechs lost, then building HP, from
   the next turn's start) is achievable, so an upper bound on what that
   board forces (`--lookahead-reply idle`: no reply at all).
4. The worst loss over `lookahead_samples` seeds (default 8), packed as
   `-(1024 x grid + 64 x mechs + building HP)` (`next_turn_value`, clamped;
   floor `kNextTurnFloor`). No next turn (the mission's last turn, or the
   phase ended the mission): 0.

Where it ranks: below this turn's grid, buildings, mechs lost and stars
failed, which are certain and permanent, while NextTurn is an estimate (a
sample of the AI's seeds and of hidden spawn types, a reply that is only
greedy); above the threat tier, which it refines (the AI's actual targets
and the mechs' answers instead of reachability), and above mech HP,
objective progress, kills and position, since a next-turn grid point lost
is permanent for the run while those are not or are worth less. Within the
term grid comes first, then mechs, then building HP, as next turn's own
order would rank them. Chance inside the estimate takes its default outcome
(no Grid Defense resist: the worst for buildings); the AI's own chance is
the seed sample. Off, the tier is 0, its cap is 0, and it is left out of
printed scores, JSON vectors and tier counts like the threat tier
(`TierSet`: `tier_used`, `used_tier_count`, `components_for_tiers`), so
results are unchanged. "Next-turn lookahead measurement" has the numbers.

**Mech exchange (experimental, off by default).** A mech lost is gone for
every remaining turn of the mission (and its pilot dies), while the strict
order gives it up for any grid point. `ScoreOptions::mech_grid` = N >= 0
(`SolveOptions::mech_grid`, `itb_inspect --solve --mech-grid N`, itb_live /
Python `"mech_grid": N`, `live_play.py --mech-grid N`, `ITB_MECH_GRID=N` for
the old bot) makes a mech worth more than N grid power and less than N + 1:
the GridLost slot becomes `2 x grid + (2N + 1) x mechs` (`mech_grid_slot`),
minus `kGridOutPenalty` (2^20) when the turn ends with no grid power, so the
last grid point is never traded. Building HP and the later tiers keep their
places (MechsLost stays as a now redundant component); N = 0 only lifts a
mech above building HP. `mech_grid_turns` (`--mech-grid-turns`,
`"mech_grid_turns"`, `ITB_MECH_GRID_TURNS=1`) caps N by the turns left after
this one (`total_turns - turn`), so on a mission's last turn a mech is worth
less than one grid point again. The rate is fixed per solve (the root
board's turn). `mech_grid_pilot` (`--mech-grid-pilot`, `"mech_grid_pilot"`)
puts a floor under each mech's rate: min(N, its pilot's level 0-2), turns
left or not (`mech_rate`). A dead pilot is gone for the rest of the run
(the mech comes back next mission with no pilot, its levels, skills and
the XP toward them lost), so on a mission's last turns a levelled pilot is
worth more than the turns left say: level 1 more than one grid point (less
than two), level 2 more than two, a level-0 pilot as before. Rates then
differ per mech: the slot's mechs part is the sum over the root's mechs of
(2 r_m + 1) x (alive after - alive before) (`mech_grid_slot_w`; with equal
rates the same number as before), and its bound sums 2 r_m + 1 over the
root's dead mechs (only those can be revived). Printed and JSON scores carry the combined slot as "grid"
(`itb_inspect` adds the plan's plain tiers, JSON `"plain"`; itb_live adds
`"mech_grid_rate"` and `live_play.py` prints the slot formula). Off, nothing
changes (byte-identical `--corpus/--moves/--weapons/--replay [--turns]`,
node-budget solves identical in every counter). See "Mech exchange
measurement".

Pawn identity: engine uids of removed pawns can be handed out again within a
turn (new pawns take one more than the largest uid still on the board), so a
pawn on `after` is the same pawn only if its type matches and it did not
emerge from a spawn this phase (`same_pawn`).

**Objective units.** A *star* is the game's reward unit: each objective's
`Objective(text, value)` reputation, power or asset reward (every bonus
objective is 1). `ObjectivesFailed` is stars lost for good this turn: latched
failures when they happen, end-of-mission checks on the turn the mission
ends (`PhaseResult::mission_ended`; without a phase, `turn >= total_turns`).
Objectives are weighted by their stars, not equally: that is how the game
pays them. `ObjectiveProgress` is progress toward stars not yet earned, in
`kStar` = 840 units per star (lcm 1..8, so "1 of 5 kills" is exact); it can
be negative (fires put out, bots thawed). Counted objectives (kills,
blocked spawns, Mission_Force mountains, Mission_Repair platforms) are
paced: their progress is scaled by the units still needed per turn left
(this one included, at most 4), so the objective behind schedule wins a
trade with one that can wait (`paced`, objectives.cpp). Time pods are not in the game's
objective list but are weighted as 1 star: lost when broken, secured when a
player unit picks one up.

| objective | failed (stars) | progress | data | exact |
|---|---|---|---|---|
| time pod | broken: 1 | collected: 1 star | tiles | yes |
| BONUS_ASSET (AssetLoc) | building damaged (`IsDamaged`): 1 | - | `objective_name` Str_* | yes |
| Mission_Critical: Solar/Wind/Power/Factory | each building damaged: 1 | - | `objective_name` Mission_* | yes |
| BONUS_KILL_FIVE | end: KilledVek < target | kills toward the target | `mission_kill_target`, `mission_kills_done` | yes |
| BONUS_PACIFIST | KilledVek crosses the limit | - | `mission_kill_limit`, kills | yes |
| BONUS_GRID | grid damage since deployment reaches 3 | - | PowerStart: turn-1 grid (later turns: this turn only) | turn 1 |
| BONUS_MECHS | end: mech damage >= 4 | (tier 2) | board | yes |
| BONUS_BLOCK | after the last turn with spawns (TurnLimit - 1): BlockedSpawns < 3 | blocked spawns (`SpawnBlocked`) | `mission_blocked_spawns` | yes |
| BONUS_DEBRIS | end: an egg sack left | sack destroyed: 1/2 | BonusDebris pawns | yes |
| BONUS_SELFDAMAGE | end: a mech infected | mite removed: 1/3 | `infected` | yes |
| BONUS_KILL | end: an enemy left | - | board | yes |
| Tanks, Civilians, Bomb, BotDefense (2 units) | each unit dies: 1 | - | pawn types | yes |
| Artillery, Filler, Volatile (no kill; a retreat is fine), Final_Cave bomb | dies: 1 | - | pawn types | yes |
| Train / Armored_Train (2) | stopped: 1; wreck dies: 1 | - | pawn types | yes |
| Satellite (2) | rocket destroyed: 1 | launched: 1 star | pawn types | yes |
| Dam, Shields, AcidStorm, Hacking tower, bosses | end: target alive | destroyed: 1 star | pawn types (`Mission_XBoss` -> `XBoss*`, Jelly_Boss) | yes |
| Hacking bot | bot dies (not its player-team swap): 1 | - | `mission_hacking_bot_id` or the shielded Cannon Bot | with ids |
| Disposal | unit dies: 1; end: mountains left: 1 | mountains destroyed | board | yes |
| Terraform | unit dies: 1; end: grass left: 1 | grass turned to sand | grass zone not exported | no |
| Force | end: Mountains < 2 | mountains destroyed (EVENT_MOUNTAIN_DESTROYED) | `mission_mountains_destroyed` | yes |
| AcidTank (2) | end: 2 - stars(AcidKills) | acid kills (1st: 1 star, 4th: 2) | `mission_kills_done` = AcidKills | yes |
| Barrels (2) | end: each vat left: 1 | vat destroyed: 1 star | AcidVat pawns | yes |
| BoomBots (2) | end: 2 - stars(destroyed) | bots destroyed (2: 1 star, 4: 2) | `_Boom` pawns, 4 placed | yes |
| ForestFire (2) | end: 2 - stars(fires) | fires on the board (8: 2 stars) | tiles | yes |
| Repair | end: RepairPickups < 3 | platforms used by player units | `repair_platforms_used` | yes |
| FreezeBldg | end: thawed < 5 | buildings thawed or destroyed | `freeze_building_tiles` | with tiles |
| FreezeBots (2) | bot dies: 1; end: bot not frozen: 1 | bot frozen: 1 star | pawn types | yes |
| BlobBoss | end: dead blobs < 5 | blobs killed | dead blobs estimated from the living | no |
| Missiles (2) | - | - | Missile_Unit shots not modelled | no |
| Final (survive) | - (no reward) | - | - | - |

Bonus objectives come from the bridge's `bonus_objective_ids` when exported
(51 of 469 recorded boards); otherwise from what other fields reveal: a kill
target (KILL_FIVE; on Mission_AcidTank it is the mission's own goal), a kill
limit (PACIFIST), an Str_* building (ASSET), egg sacks (DEBRIS), infected
mechs (SELFDAMAGE). GRID, MECHS and BLOCK cannot be inferred; GRID and MECHS
only add a threshold to what tiers 1 and 2 already rank.

Native counters follow the decompile: `Pawn::ProcessDeath` raises
EVENT_ENEMY_KILLED for every team-6 non-minor death (bots included, any
killer) and EVENT_ACID_DESTROYED for every team-6 non-mech death (minor ones
too) with ACID at that moment, in either phase; Mission.KilledVek only counts
while KILL_FIVE or PACIFIST is active (Mission:BaseUpdate), AcidKills always.
The dead pawn leaves the board with its ACID, so acid kills are counted where
deaths are processed (`Simulation::process_death` into the running
`Board::acid_destroyed`) and a turn's share is after - before: ACID gained and
the kill in one turn counts, and so does a pawn created during the turn.

Validation (`itb_inspect --replay recordings --turns`, the 216 replayed
turns with a trustworthy next board, 15 more against post_enemy): the
engine's kill count matches the next turn's `mission_kills_done` on all 19
turns where the game counts kills; repair platforms 4/4; objective buildings
standing 231/231; pods 231/231; infected mechs 216/216. Every scored failure
(8: a Cannon Bot, an Archive tank, three Volatile Vek, two trains stopped,
a pod) happened in game too. Acid kills (`--score` on the live
Mission_AcidTank turns, whose bridge exports AcidKills): 6/6 agree with the
next turn's count; the May run's m07 (older bridge, no AcidKills) adds up
to the 4 its outcome ("ACID objective completed") needs.
`itb_inspect --score <recording>` prints one recorded plan's score, every
objective line and the position terms beside the game's outcome.

Integration notes:

- A tile's occupant order is arrival order (`Pawn::arrival`). Boards loaded
  from recordings have no arrival history, so their shared tiles fall back to
  board-list order.
- Lua writes are applied when the script returns, so a script does not see
  its own writes (no shipped weapon script reads back what it wrote).
- A hurt burrower dives (stage 2 H4: any HP loss, except a weapon hit on a
  cracked tile): `RulesContext::burrow_dives` records it and the executor
  (`FrameHooks::start_dive`) plays the dive (`Durations::burrow_dive`), busy,
  on its tile: later hits of the same chunk still find it, and a FULL_DELAY
  chunk waits for it. When the dive ends it leaves the board (`Pawn::pos`
  invalid, previous tile in `movement.prev_pos`, queued shot and fire gone),
  in the player's turn as in the enemy phase, so the next action never sees
  it. It stays alive (no kill credit) until the AI resurfaces it. itb_live
  lists such pawns under `"underground"` (x, y = the tile they dove from), and
  a state unit with `"underground": true` loads as one; `live_play.py`
  carries them over, since the bridge omits them. [I] A burrower killed
  during its dive stays where it died.
- `Engine::end_turn` leaves `Board::player_phase` false and does not apply
  the next player turn's start (Zoltan shields, Opener/Closer, last-turn
  spawn clearing): score those as next-turn context.
- Multi-tile pawns (Lua `ExtraSpaces`: Dam_Pawn (1,0), the trains and their
  wrecks (0,1)) stand on both tiles, as natively: the pawn is listed in the
  BoardSpace of its tile and of each extra tile (`Pawn::SetSpace` 0x0087dcb0
  moves them all through `Board::MovePawn` 0x0093abd0; `Board::RemovePawn`
  0x0093a980 clears them all). `Pawn::pos` stays the main tile
  (`Pawn:GetSpace`); `Pawn::extra_dx/extra_dy` hold the offset (from the
  definition, or the bridge's `extra_spaces`) and `Pawn::occupies` /
  `extra_tile` answer per tile. So the extra tile blocks moves, spawns and
  projectiles, `GetPawn` / `IsPawnSpace` find the pawn there, and each
  SpaceDamage on it hits the pawn (two SpaceDamages, one per tile, hit it
  twice); a push into it bumps the pawn, and the tile rules (water, chasm,
  pods, ACID) see it there. The bridge's `"is_extra_tile"` entries are
  checked against the offset on load. Every shipped definition has at most
  one extra space and is `Pushable = false`; pushing a multi-tile pawn is not
  modelled beyond that.

### Perfect-turn search (stage 9)

`solve_turn(engine, board, ctx, options)` (`solver.hpp`) returns the plan
with the best worst-case `score_turn`, evaluated through the end of the
enemy phase, from what the player can see (unknown spawn types emerge as
unknown).

**Values.** A node is a board during the player's turn. Its value is
`V(B) = max(E(B), max_a min_o V(B·a·o))`: end the turn now (`E`, the minimum
of `score_turn` over every chance outcome of the enemy phase), or give a
unit a sub-action `a` whose chance outcomes `o` the adversary picks. Since
"end the turn now" is an option at every node, every prefix of a plan is a
plan. Chance during the player's own actions branches the plan: the player
sees the outcome before choosing the next action (`Plan::contingent`: the
returned actions follow the default outcome; re-solve after any other).

**Action model.** At any point any controllable unit (`Pawn::controlled()`,
alive) may: move to a tile of its Lua `Move` target area if `can_move`
(not moved, or a Shifty / Post_Move bonus move); fire any non-passive
weapon at any tile of its target area (two-click weapons: every tile of the
second target area, unless `IsTwoClickException`); repair (mechs; the
pilot's repair skill, over its target area). Sub-actions of different units
interleave freely; the engine's `Pawn::FireWeapon` bookkeeping decides what
a unit may still do (Double_Shot, Shifty, Post_Move) and `check_legal` is
the ground truth: refused sub-actions and ones with no effect are dropped.
Move undo is not modelled (an undone move equals not moving).

A weapon or Repair needs `can_act` (`movement.hpp`), the game's
`Pawn::IsActable` (0087a200): active, alive, powered, not on smoke (unless
immune), not submerged (water or lava, not flying) and no bonus shift
pending; and not frozen. That is the UI's gate: the weapon box arms a weapon
or Repair only while it holds (`PawnBox::SwapSkill`, `OnKeyAction`,
`OnMouseAction`; `PawnBox::Update` disarms one each frame once it does not),
and its "blocked" overlay reads `Pawn::IsBonusShift`. `Pawn::FireWeapon`
itself checks only frozen, so the bridge's native calls are no check. What
follows for the pilots (FireWeapon's bookkeeping, stage 5 §2.4):

- **Shifty** (Sidestep): a weapon or Repair re-activates the pawn with a
  bonus shift of 1, which replaces its move: one 1-tile move, never a
  second action. The move spends the bonus and ends the pawn's turn.
- **Post_Move** (Fire-and-Forget): the same with a bonus shift of the base
  move, moved before or not.
- **Double_Shot**: after a weapon or Repair with no move yet, the pawn is
  marked moved and active again: exactly one more action, no move. After a
  move, one action.
- Every other pawn: one action, which ends its turn.
- A Skill whose pilot has a Lua `PowerCost` (Silica's Double_Shot 2;
  Gana's Deploy_Anywhere, Archimedes' Post_Move, Prospero's Flying 1) is
  only on while the pilot is powered in the mech bay: `Pilot::IsAbility`
  (0x00859010) checks the pilot's `PowerState` (0x0051e290: non-empty, first
  cell set) when `PowerCost > 0`. The loader leaves such a pilot's Skill
  out (and the pilot out of `Recording::pilots`) when the recording says it
  is off: the bridge's `pilot.skill_active` (`Pawn:IsAbility`), else the
  save's `pilot.power` cells (`{0, 0}` = unpowered). Unknown counts as on
  (live 2026-10-16, run 37: an unpowered Silica's mech was planned to fire
  twice; the game ended its turn after one shot).

**Disable_Immunity** (Evasion, Camila Vera, `Pilot_Soldier`; "Mech
unaffected by Webbing and Smoke") is read in three places, all
`kPilotDisableImmunity`: `Pawn::IsSmoked` (00870f40) is false for her, so
she attacks and repairs from smoke (IsActable) and smoke would not drop a
queued shot; `BoardSpace::IsGrappleImmunity` (009005b0) makes the web check
in `BoardSpace::OnLoop` break any web held on her tile once she is not busy;
and `Pawn::SetGrappled` (0086e5f0) only swaps the voice line
(Mech_WebBlocked). A web attack therefore still sets the flag for the rest
of its frame, then the settle frees her (`check_webs`), so she can move next
turn, and a web left on a tile does not catch her when she steps on it. It
is a property of her pawn alone: other mechs on smoke or webbed beside her
are unaffected, her attacks are unchanged, and tile-level smoke rules still
reach her (`BoardSpace::OnLoop` puts out a burning pawn on smoke, and
Nanofilter Mending spends smoke to heal her, both without IsSmoked). The
position tier does not count her as a mech on smoke.

Before 2026-10-13 the engine checked only `active` for weapons, so a
re-activated Shifty pawn could fire and repair again without limit, and a
mech on smoke or in water could attack. The game, through native
`FireWeapon`, took all of it: live run 20261012_172449 had 20 turns whose
plan gave one Shifty mech more than one action (up to 44), and the live runs
of 2026-10-09 to 10-12 executed 249 weapon/Repair steps from smoke (84) or
water (165), in 148 turns. Not modelled: IsActable's "has a
usable weapon" clause, and a frozen mech's Repair (the UI allows it:
`FireWeapon` and `PawnBox::Update` exempt the Repair slot from the frozen
check; the engine still refuses it).

**Chance.** Every hook the engine exposes (Grid Defense, `choose` for
environment orders/choices and mission picks, spider eggs, death-effect
seeds) goes through one driver that numbers the calls of a run. The default
outcome (no resist, branch 0, the pawn's own seed) runs first; then each
recorded call's other options, depth-first, by re-running with the earlier
choices forced. Every leaf of the chance tree is reached once, so resists
are enumerated, not assumed bad (`test_solver.cpp` has a board where a
resist is the worst case). An evaluation stops as soon as its minimum falls
to the incumbent (it can no longer matter). Death-effect seeds are hidden:
a death effect that draws random numbers (only the Goo bosses in the
shipped scripts) is tried with a sample of seeds and the result is never
claimed proven. The Lua stream is reseeded before every engine run, so a
run is a function of the board and the choices.

**Search.** Depth-first alpha-beta over these max (player) and min
(chance) nodes; results are intervals `[lo, hi]` that hold the value
(exact when equal). The incumbent (best value already guaranteed from the
root) raises every alpha.
- Transposition table on a 128-bit content hash of the board
  (`board_hash.hpp`: every field; arrival stamps as ranks among the pawns
  sharing a tile, which is all the engine compares; move-undo flags left
  out, as nothing the search does reads them). Orders of sub-actions that
  reach the same board are one subproblem.
- End-of-turn values memoized on the End Turn hash (the search hash minus
  the player-turn flags `Engine::end_turn` resets first: player units'
  `active`, bonus move, Kickoff bonus).
- Children (deduplicated by board) are ordered by the value of ending the
  turn right after them; boards that threaten buildings get fixed first.
  A child the tier bound cuts gets no end-of-turn value for ordering.
- Same-effect shots: a weapon's target only reaches the board through the
  effect it builds (`Engine::pure_weapon_effect`: prepared, alterations
  applied, no random draw or Lua error; `SkillEffect::target` is never read
  once prepared, and effects with a queued part keep their target). Two
  targets of one unit's slot with equal effects (every `SpaceDamage` field)
  give the same outcomes, so the second is not run: projectiles and charges
  aimed anywhere along a line, artillery shots that land the same way. The
  effect computed for the check is handed to the run
  (`ActionOptions::known_effect`). Nodes, enemy phases, TT hits and values
  are unchanged (120 sample boards at a node budget, one thread: identical
  in every one of those counters); `ITB_VERIFY_EFFECTS=1` runs every skipped
  or known-effect shot the old way and warns on any other board,
  `ITB_SAME_EFFECT=0` turns it off.
- Spent turns: a weapon or Repair whose one outcome is the board it was
  fired from, except that its unit is no longer active (and the undo flags),
  is not a child (`SolveOptions::skip_spent_turns`, `ITB_SKIP_SPENT=0` turns
  it off; `SolveStats::spent_turns`). A Repair at full HP or a shot at empty
  ground did that, and its subtree repeated the parent's "that unit never
  acts" part under other hashes. Sound: if X and Y differ only in unit A's
  `active` (X true, Y false) and the undo flags, V(Y) <= V(X) by induction:
  `Engine::end_turn` resets both first, so the end boards are equal; Y's
  sub-actions are other units' (A can neither move nor act) and run the
  same from X, since `active` and the undo flags are read only by A's own
  legality checks, Lua `Pawn:IsActive` / `IsUndoPossible` (no shipped script
  calls IsActive; IsUndoPossible only in the tutorial) and the undo
  machinery. So a plan through such a child is matched by the same plan
  without that sub-action, which never passes through a skipped child.
  Exact equality only: a spent Boost, a used limited charge or a `moved`
  flag is a change. Values are unchanged (`test_solver.cpp`: tiny boards
  with and without it, and against the naive enumeration); plans can differ
  only among plans of equal value (the skipped no-op was one of them).
- Early stop of the enemy phase (`TurnContext::stop_early`): once the board
  between two steps of the phase (after the status ticks, each environment
  step, each queued shot) has a tier bound `<= alpha`, the outcome is cut
  there: grid power and building HP never rise, so the rest of the phase
  cannot lift it. The end-of-turn memo marks such values (`LeafMemo::
  stopped`, only the bound); the beam's ranking, and child ordering outside
  the tier proof of the adaptive budget, take full phases.
- A beam pre-pass over whole unit turns (optional move, then optional
  action) keeps the best states for every set of units that have played
  and seeds the incumbent; plans whose value appears only once every unit
  has played survive it. With helper threads, thread 0 widens it (x3 per
  round, up to width 1000) for up to 40% of the time limit, or of
  `max_time_s` under the adaptive budget (live m41 turn 6: cut off at 12 s
  of a 30 s base, the search spent 300 s without finding the round that
  saves every building; given 40% of the cap, it finds it at 56 s).
- Threads (`SolveOptions::helper_engines`, one engine and Lua state per
  thread) share the table, the memo and the incumbent; a child another
  thread is searching waits for a second pass.

**Bounds.** `ub(B)` is componentwise at least the score of any plan from
B, hence a lexicographic upper bound; a node or child with `ub <= alpha` is
cut. Per tier (each argued in `solver.cpp`): grid power never rises
(`Game:ModifyPowerGrid` gains come only from Support_KO_GridCharger, which
disables this bound), building HP never rises (no weapon, death or native
environment script builds a building), mechs revived <= mechs dead at the
start, net mech HP <= the HP missing at the start (+1 per mech with Psion
Leech), objective stars lost <= 0, the next-turn loss estimate <= 0 (a loss
is never negative), next-turn threat <= 0 (no tighter cap is
cheaply sound: any enemy may still be killed, pushed, frozen or smoked, and
spawns blocked), kills and HP removed <= the enemies and
their HP at the start. Objective progress and position have no derived cap
(`SolveOptions::tier_caps` can supply them), except with `objectives_first`:
then `objective_progress_cap` (`objectives.cpp`) bounds every objective line
by what the root board allows (pods present, targets alive, counters
clamped at their targets) and caps both the progress and the threat slot
(0 of 601 recorded turns exceed it; the solver warns if a leaf ever does). Grid and building losses are
therefore committed once a player action causes them. With the mech
exchange the grid slot's bound is 2 x the grid bound + (2N + 1) x the mechs
bound (both coefficients positive), less the grid-out penalty on a board
already at grid 0 when grid power cannot rise.

**Lookahead in the search.** With `SolveOptions::lookahead` the leaf value
E(B) is computed lazily: each chance outcome of the enemy phase is scored
with NextTurn at its cap (0), an upper bound, and the usual cut (the
minimum so far <= alpha) runs on those bounds. Outcomes that survive are
taken in increasing order of their bound; one whose bound with NextTurn at
its floor cannot go below the minimum so far is skipped; for the others the
lookahead's samples run one at a time, each lowering the outcome's
NextTurn (the worst so far), and as soon as the outcome is <= alpha the
remaining samples are skipped and the leaf reports [-inf, that value]
(sound: more samples can only lower it). Sample values are cached by the
end board (a shared table), so a later visit resumes where an earlier one
stopped. Child ordering and the beam's ranking only take bounds
([floor, cap] intervals, no lookahead); the beam evaluates the best 3
chance-free states of each level exactly to seed incumbents. The tier's
cap in `TierBounds` is 0. So intervals stay sound, a completed search
proves the plan optimal with respect to the estimate (`test_solver.cpp`
checks the lookahead against the naive enumeration on tiny boards, both
folds), and `evaluate_plan` includes the estimate (the plan re-check
matches). An estimate costs tens of milliseconds, so the search is slower;
to keep that from costing the tiers above it, `LookaheadOptions::base_share`
(25%) of the budget first goes to a search without the lookahead, and its
plan, scored with the lookahead by `evaluate_plan` (open loop: a value it
surely reaches), seeds the incumbent of the lookahead search, which gets the
rest of the budget (time, or nodes; the seed evaluation is charged to it,
"Time budget" below, and its sample values go to the lookahead search's
table). That search can only return something
better under the full order, so grid, buildings, mechs and stars are never
worse than the base search's at that share of the budget (before seeding,
2 of 60 turns lost grid to it at 20 s and 2 threads). A sample stops when
the budget runs out (the leaf reports the bound so far). The base search's
tables stay alive for the lookahead search, whose values are at most the
base search's (NextTurn only falls below its cap): a node or end-of-turn
value whose base upper bound is `<= alpha` is cut at once, and the result
carries the tighter of the two root bounds, so tiers the base search proved
stay proven (`SolveStats::base_proven_components`; before, a lookahead
search cut by the budget reported its own, often 0).

**Grid survival (off by default).** Chance is taken at its worst, so with
the grid low every line may end with the grid at 0 in its worst outcome.
Those lines tie in the grid tier, and the tiers below it (building HP,
mechs, ...) pick among them, although one may need a single Grid Defense
resist to keep the grid and another three. `SolveOptions::grid_survival`
(`itb_inspect --solve --grid-survival`, itb_live / Python `"grid_survival":
true`, `live_play.py --grid-survival`) puts `ScoreKey::GridSurvival` in front
of the grid tier: the probability that the grid is still above 0 at the end
of the turn, in units of 10^-9 (`kSurvivalOne`).

- Per outcome `score_turn` gives it 1 (the grid survived) or 0. The value of
  a chance node folds its outcomes over its chance tree (the calls each run
  made, `ChanceTree`): a Grid Defense roll weighs the resisted branch by p and
  the other by 1 - p, a game's random pick (`MissionRandom`: the Final Cave's
  bomb drop, the Reactivation thaw; a spider egg's tile) averages its
  branches, every other call takes its worst branch (environment orders and
  hidden parameters, XP splits, sampled death-effect seeds). Rolls are
  independent. This holds for the enemy phase (the end-of-turn value) and
  for chance during the player's own actions (a resist when the player's
  shot or bump hits a building): the player then sees the roll and plays on
  (the plan is contingent), so the node is the expectation over its
  outcomes' values.
- The rest of a chance node's value is its worst outcome's, among the
  outcomes with odds above 0 when the node's odds are above 0 (how the turn
  goes given the grid survives: an outcome that surely ends the run does not
  count), else among all of them (as without the odds).
- Rounding guard: the odds are exactly 1 only if no outcome can lose the grid,
  else at most 1 - 10^-9. So a line that surely keeps the grid scores 1,
  beats every line that can lose it and ranks among such lines exactly as
  before: the odds only decide between lines that can all lose the grid,
  i.e. when the best worst case is a lost game. Then they rank first, above
  building HP, mechs, stars and mech HP (if the grid goes the run ends and
  nothing below matters), and in front of the mech exchange's grid slot (a
  line is not traded for a mech either). Equal odds (0 for all when nothing
  can resist) fall back to the old order.
- The Grid Defense chance p: the game's `PowerBar::GetDefense` (0x008ed5c0)
  is a base, plus the overpower bonus (`ModifyPowerGrid` past a full grid:
  +2 per point while below 10, then +1, at most 25; the save's
  `GameData.overflow`), plus 3 per squad pilot whose learned level-up skills
  include Grid (`SquadControl::GetDefBonus`). This is the HUD's GRID DEFENSE
  figure. The base is 15, or 0 when `Settings::GetDifficulty()` is Unfair
  at the moment the power bar is built (its constructor, 0x008ed180), and
  nothing saves or reloads it. It is built with each `GameMap`: by
  `Game::StartGame` (a new run: the difficulty just chosen) and by
  `Game::LoadGame` (Continue, Reset Turn), which builds the map and runs the
  save file before `GameMap::LoadGame` sets the save's difficulty. So a run
  continued right after the game starts gets the base of the settings
  file's difficulty (`settings.lua`, 1 when absent), not the save's: run 22
  (Unfair, continued after a restart with Hard in `settings.lua`) showed
  overflow + 15 on the HUD, 15% to 40%, while runs 21 and 23 (Unfair, new
  runs, no restart) showed the overflow alone, at most 25%. The bridge sees
  the build (`startNewGame`, and the save file's first `CreateEffect` after
  a new `GameData` table) and exports `grid_defense_base` (0 or 15) with
  `grid_base_difficulty`, and `grid_overflow` from the save; the loader
  (`load_grid_defense`) adds the base (without `grid_defense_base`: by the
  save's difficulty, right for new runs and after Reset Turn) and the Grid
  pilots into `MissionData::grid_defense` (a state may also give
  `grid_defense` in percent). Unknown (older states): `Board::grid_defense`,
  15. It only feeds the odds: the chance nodes still branch whenever
  `Board::grid_defense` > 0 and predictions follow the default outcome (no
  resist), as before.
- Off, GridSurvival is 0 everywhere, its cap 0, and it is left out of printed
  scores and JSON vectors, so every output is unchanged (byte-identical
  `--corpus/--moves/--weapons/--replay [--turns]`). It is never counted as a
  tier: on, the first tier is (odds, grid), so `--min-tiers` means the same.

In the search: the tier bound of the odds is 1, or 0 once the board's grid
is 0 (grid power never rises). While the incumbent surely keeps the grid
(odds 1), an outcome that loses it puts a node below the incumbent at once,
as GridLost did: leaves cut on the first such outcome, the enemy phase may
stop early, and chance nodes keep the old windows. The beam and child
ordering always rank by the old key (`LeafMemo::key`, the worst outcome),
which also finds high odds sooner on lost-cause boards (fewer buildings hit,
fewer rolls). A search without a seed starts with a survey
(`search_survival`): the incumbent floored at odds 1, so only lines that
surely keep the grid are searched, as without the odds. If it finds one,
that is the whole search with the whole budget (every line that can lose the
grid ranks below it). If it finds none within 25% of the budget, or proves
there is none, the search with the odds runs on the rest.

Below odds 1 every outcome's share counts. A leaf's odds are bounded from
the outcomes run so far (the others counted as surviving, except the
resisted branch of a roll made with the grid already at 0, which loses it
too: `ChanceCall::dead`, without a Grid Charger; 4-9% fewer enemy phases on
three doomed recorded turns at 60000 nodes, same values; the fold is tried
after 1, 2, 4, ... outcomes) and its phases run to their end. A chance node
in the player's actions is searched Star1-style (`chance_odds`): each
outcome in a window of its own, without the incumbent's raise (`local`), cut
only where its odds alone would put the node's odds below alpha's with the
other outcomes at their upper bounds (outcomes not searched yet at their
tier bound, so an outcome with no grid left counts 0 without a search);
after one pass either an outcome was cut and the node's odds are below
alpha's, or every outcome is exact. Alpha-beta over max, min and these
weighted nodes stays sound (every interval is), and `test_solver.cpp` checks
the solver, with and without table, bounds, ordering and beam, against a
naive enumeration of the same definition: 30 tiny boards with the grid at
1-2 and Grid Defense 0-40% (21 where every line can lose the grid, 16 with
chance in the player's actions; 1769 Star1 windows), 6 more with the
lookahead or the mech exchange, plus a hand-built board where the old order
picks the two-roll line and the odds the one-roll line.

Cost (`itb_inspect --solve --nodes 20000 --threads 1`, 9 recorded Final and
Cave boards, on and off). Seven where some line keeps the grid: the same
values, 0-2.4% fewer enemy phases than without the odds (the survey cuts
lines that lose the grid at once; one board identical in every counter),
times within noise. Two where every line can lose it (Cave turn 2 at grid 1
of `20261013_192343` and `20261014_045942`): 45x and 6x the enemy
phases for the same nodes (each leaf weighs every outcome, no cut by building
HP and below), so in a time budget those searches see fewer nodes. Their tier
bound for the odds is 1, so they are never proven (the adaptive budget runs
to `--max-time`); the search is anytime.

Run 21, Cave (`recordings/live/20261014_045942`, Unfair, Grid Defense 25%:
overpower bonus 25, no Grid pilot; live flags, 4 threads, 300 s, other jobs
running). Turn 1 (grid 3): a line keeps the grid, odds 1, and the plan and
value are the live plan's (the survey found it; the odds change nothing).
Turn 2 (grid 1): every line can lose the grid. The live plan needs one
resist, in the player's own action (the Hornet Mech kills the Blob on D3;
its explosion hits C3's building: odds 0.25, or 0 if chance in the player's
actions were taken at its worst). With the odds the solver also lands at
0.25 (no better line found in 300 s; not proven): one Grid Defense roll in
the enemy phase (the Scarab's 2-damage shot at E2's building), and given
that roll it keeps every building and leaves one enemy able to reach a
protected unit, where the live plan's surviving outcome left three (an
earlier 300 s run found a 0.25 line leaving none).

**Certificate** (`itb_inspect --survival-cert state.json [--time S]
[--threads N] [--plain] [--json FILE]`; a diagnostic, no search changes).
An upper bound on every plan's odds, to set next to the best odds found.
- The roll rule (`tile_rules.cpp` damage_building): one Grid Defense roll
  per damaging hit (a weapon's or death effect's SpaceDamage, a bump) that
  reaches a populated building after shields and ice have taken theirs: a
  plain building with `populated`, or a unique building at full HP. The
  roll's amount is min(damage, building HP) >= 1; unresisted, the building
  loses that HP and grid power that much (floored at 0); resisted, nothing
  happens (no HP, grid or Auto-Shield change), so a resisted building can be
  hit and roll again. Grid power falls through nothing else (no shipped
  script calls `ModifyPowerGrid` with a loss), and only rises with a Grid
  Charger on the board (`TierBounds`: monotone otherwise).
- First resist. Up to its first resisted roll, an outcome of a plan follows
  the plan's no-resist path (the same choices, the same picks of the other
  chance nodes). If that path loses the grid at its m-th roll, an outcome
  keeps the grid only if one of its first m rolls is resisted, so the odds
  are at most 1 - (1 - p)^m: the rolls are independent, every worst-case
  chance node only lowers the fold (any fixed pick of the adversary bounds
  it), and the game's random picks average over branches that each obey the
  bound. Each unresisted roll costs at least 1 grid, so m <= G.
- The proof. `SolveOptions::cert_roll_budget` M runs the search in a world
  where the turn's first M rolls are not resisted and the rest are (no
  branch; a run starting at grid g gets M - (G - g), never more than the
  turn has left). With grid survival on, odds 0 for every plan there (the
  search proven in its first tier: `--min-tiers 1`, the adaptive budget)
  means every plan's no-resist path loses the grid within M rolls, for every
  random pick and some worst-case pick. The tool proves M = G, G - 1, ...
  while the bound stays at or above the recorded plan's odds, and prints
  1 - (1 - p)^M for the smallest M proven and whether the recorded plan
  (`_solve.json`, open-loop) reaches it. `--plain` also proves D, the
  building HP the best no-resist line loses (plain tiers, M unbounded).
- Grid 1. An outcome then keeps the grid only if every roll is resisted:
  it is the plan's all-resist path, of probability p^(its rolls).
  `SolveOptions::cert_resist_first` K resists the first K rolls of each
  sub-action and enemy phase before the budget (M = 1); that world follows
  the all-resist path while it has at most K rolls, so odds 0 there for
  every plan bounds every plan's odds by p^(K + 1). The tool tries K = 1,
  2, ... after M = 1.
- `test_solver.cpp` checks the bound against the naive odds on 24 tiny
  boards (16 certified, 15 of them at the naive odds exactly).

Recorded doomed turns (4 threads, up to 1500 s per proof, a live run on the
same machine; p from the state; "recorded" is the live plan, open-loop):

| Turn | G | p | proofs | bound | recorded | |
|---|---|---|---|---|---|---|
| run 21 Cave t2 (`20261014_045942` m41) | 1 | 0.15 | M=1 763 s | 0.15 | (no plan file; one resist) | optimal |
| run 24 Final t3 (`20261014_151230` m40) | 1 | 0.28 | M=1 72 s, K=1 101 s | 0.0784 | 0.0784 | optimal |
| run 25 Survive t3 (`20261014_175032` m14) | 1 | 0 | M=1 open at 1500 s | 1 | 0 | open |
| run 27 Factory t3 (`20261014_222734` m14) | 2 | 0.08 | M=2 19 s | 0.1536 | 0.1536 | optimal |
| run 26 Cave t3 (`20261014_190250` m41) | 3 | 0.28 | M=3 361 s; M=2 fails (158 s) | 0.6268 | 0.4816 | open |

Run 26: some line's no-resist path takes three 1-grid rolls to empty the
grid, so M = 2 is false and the first-resist bound stops at 0.6268; closing
it needs a bound on the outcomes after a first resist (not done). The old run took 1508 s
to find 0.4816 and never bounded it below 1.
- Why not D and a roll count. A bound from "survival needs k = ceil((D -
  G + 1) / h) resists among n rolls" needs every outcome to lose at least
  D without its resists, which is not true in general: a resist can keep a
  1 HP building standing in front of a 2 HP one, so a second shot hits the
  first again instead of passing over the rubble. And no static count caps
  n, since a resisted building rolls again on every hit. For k = 1 the first
  resist argument above is sound and needs no n (m <= G replaces it).

**Survival cap** (`SolveOptions::survival_cap_share`, off by default;
`itb_inspect --solve --grid-survival --survival-cap [SHARE]`, itb_live
`"survival_cap": true` / `--survival-cap`, `live_play.py --grid-survival
--survival-cap [--survival-cap-share F]`). The certificate as a pre-pass of
the live solve (`survival_cap.cpp`, wired in `search_survival`):
- When. Only after the survey (above) found no line that surely keeps the
  grid. A survey that finds one returns as before: the pre-pass never runs
  and every output is unchanged. The survey's proof that there is none
  (it completes within its share) is often too slow (4 to over 30 s on the
  doomed turns below, against a live share of 25% of 7.5 s), so the
  pre-pass's first proof, M = G, is the proof that the turn is doomed:
  every plan's no-resist path loses the grid, so every line can. If it
  fails (a plan's no-resist path keeps the grid) or runs out of time, the
  pre-pass stops and the search goes on as without it with the time left.
  A turn with a line that surely keeps the grid which the survey missed in
  its share therefore pays that first proof (it stops at the first plan
  whose no-resist path keeps the grid, at most the pre-pass budget).
  `--survival-cap-strict` (`survival_cap_strict`) runs the pre-pass only
  after the survey's own proof.
- Proofs. M = G, G - 1, ... while proven, then with the grid at 1 K = 1,
  2, ...; each is a search of the certificate's world (`cert_roll_budget`,
  `cert_resist_first`) ranked by the odds alone (the other ranking options
  change no plan's odds), with no beam, the adaptive budget proving the
  first tier from the start and `survival_goal` 1 (a plan with odds above 0
  disproves the world at once). Together they get `survival_cap_share` of
  the whole budget (`max_time` with the adaptive budget, else `time`;
  nodes alike), never more than is left of it. They are charged to the
  whole budget only: with the adaptive budget the search after them keeps
  its base time (`time` less the survey's time) and its extension still
  ends by `max_time`; with one budget (`time` alone) both come out of it.
  (Before 2026-10-15 the pre-pass also came out of the base time: on run 27
  Factory t3 at one thread the survey and pre-pass took 5.8 s of the base
  search's 7.5 s, it got 1.7 s and stopped at the goal with no tier proven
  below the odds.)
- The ceiling. The ceiling B bounds the odds of every plan from the root,
  not those of a board deeper in the search: after a chance node in the
  player's own actions (a resisted roll) the turn may keep the grid for
  sure. So it is never a tier bound (`tier_caps` would cap every node,
  chance outcomes included, and cut the best line). It is
  `SolveOptions::survival_goal` instead: B less a rounding guard, at which
  the adaptive budget counts the plan proven (`Shared::meets_target`: it
  stops at the base time, or at once past it), and it caps the reported
  bound's odds (B plus the guard). The guard (`kSurvivalCapGuard`, 26
  units of 10^-9): each fold over a chance tree rounds once (half a unit)
  and a plan nests at most one fold per sub-action (48) plus the enemy
  phase's, so the search's odds of a plan are within 24.5 units of its
  exact odds; a plan whose odds reach B - guard is optimal in the odds to
  within 5.3 * 10^-8.
- Result: `SolveResult::survival_cap` (`ran`, `survey_proof`, `m`, `k`,
  `bound`, `proven`: the plan's odds reach the goal, `proofs`, `log`,
  `time_s`). It proves the odds only: `proven_components` and
  `proven_optimal` keep their meaning (the tiers below the odds, the grid
  of the outcomes that keep it and on, are searched until the base time).
- With the lookahead the goal stops the base search only (it is the search
  that proves the tiers above NextTurn, and the odds are proven). The
  lookahead search after it, seeded with the base plan (odds at the goal),
  refines the tiers below the odds and keeps its own adaptive budget: it
  stops at its base time only if its first tiers meet the root's bound,
  else it runs on to `max_time` (`solve_turn`; the whole solve stays
  within it). The odds stay proven: the lookahead search only returns a
  plan better in the full order, and the ceiling bounds every plan.
- `test_solver.cpp`: 30 tiny boards (15 doomed, 14 of them at their
  ceiling; a third through the lookahead's two searches) give the same values and plans with and
  without it, the naive odds never above the cap and, with an adaptive
  budget that stops at the goal, the naive odds; a doomed board where the
  search stops at the proven 0.25 before its end (without it the search runs
  to its end), and with the lookahead (base time 0.05 s, `max_time` 120 s,
  7 tiers) whose lookahead search still runs to its end past the goal (the
  same value as without the pre-pass, proven optimal); a board that keeps
  the grid untouched (same counters).

Live flags (`--time 30 --max-time 120 --min-tiers 7 --threat-tier
--objectives-first --mech-grid 2 --mech-grid-turns --mech-grid-pilot
--lookahead --grid-survival`, 3 threads where live uses 8, with other jobs
running), with `--survival-cap 0.5` and without:

| Turn | G | p | pre-pass (60 s at most) | with: odds, total | without: odds, total | plan |
|---|---|---|---|---|---|---|
| run 27 Factory t3 (`20261014_222734` m14) | 2 | 0.08 | M=2 proven, M=1 disproven: 2.4 s | 0.1536 = ceiling, **proven**, 30.9 s | 0.1536, 120.9 s, unproven | next_turn -1025 vs -64 |
| run 30 MosquitoBoss t4 (`20261015_072606` m28) | 2 | 0.24 | M=2 proven, M=1 disproven: 0.05 s | 0.24 (ceiling 0.4224), search complete 0.4 s | 0.24, complete 0.3 s | same |
| run 24 Final t3 (`20261014_151230` m40) | 1 | 0.28 | M=1 proven, K=1 open: 60 s | 0.0784 (ceiling 0.28), 123 s | 0.0784, 125 s | same |
| run 21 Cave t2 (`20261014_045942` m41) | 1 | 0.15 | M=1 open: 60 s | 0.15, 126 s | 0.15, 125 s | same |
| run 26 Cave t3 (`20261014_190250` m41) | 3 | 0.28 | M=3 open: 60 s | 0.4816, 124 s | 0.4816, 128 s | same |
| run 25 Survive t3 (`20261014_175032` m14) | 1 | 0 | M=1 open: 60 s | 0, 123 s | 0, 122 s | same |
| run 28 Cave t4 (`20261014_233706` m41) | 1 | 0.20 | M=1 open: 60 s | 0.0016 (p^4), 120 s | 0.0016, 120 s | same |

Same odds on all seven, the same plan on six. Run 27 stopped at the base
time once its plan reached the ceiling, so the tiers below the odds got 30 s
instead of 120 s: its NextTurn was worse there (the next turn's estimate,
given the grid survives, which it does with odds 0.15). Where the pre-pass
proves nothing the search after it has 60 s fewer and still found the same
plans. (A total a few seconds past 120 s is the lookahead search's last
evaluations, with or without the pre-pass.)

Since then the goal stops only the base search (above). The same live
flags at 1 thread, `--survival-cap 0.3`, against the cap off and the
previous rule (goal in both searches):

| Turn | cap 0.3 now | cap off | previous rule |
|---|---|---|---|
| run 27 Factory t3 | 0.1536 **proven**, next_turn -1025, mech_hp 0, 120.6 s | 0.1536 (first 4 tiers proven), same plan, 120.8 s | 0.1536 proven, next_turn -1025, mech_hp -1, 30.7 s |
| run 30 MosquitoBoss t4 | 0.24, complete, 0.6 s | same | same |
| run 24 Final t3 | 0.0784 (M=1 open at 36 s), 122.9 s | 0.0784, same plan, 122.8 s | 0.0784 (M=1 proven 34.8 s), 123.1 s |

Run 27 now gets the plan of the search without the cap (the previous rule's
plan lost a mech HP). At 1 thread its next_turn is -1025 with the cap off
too (the -64 above was at 3 threads). The cap off proves its first 4 tiers
there (the base search completes in 9.3 s); with the cap the survey and the
pre-pass take 7.5 s of the 7.5 s base time, the base search stops at the
goal and only the odds are proven (since fixed: the pre-pass is charged to
`max_time` only, and run 27 with the cap proves the same 4 tiers, "Time
budget" below). Run 24's M = 1 proof sits at the edge of
the 36 s share (proven in one run, open in the other).

With more time (`--max-time 900 --survival-cap 0.9`, 3 threads): run 24
proves M=1 in 19.6 s and K=1 in 16.4 s (K=2 disproven at once), ceiling
0.0784 = its plan's odds: **proven, 40.3 s in all** (the 60 s run above
shared the machine with a build); run 28's M=1 is still open after 812 s
(its plan needs four resists, p^4, so it would also need K=3). The
certificate tool's own proofs took 19 s (run 27 M=2) and 72 + 101 s (run 24)
with 4 threads: the pre-pass's proofs (no beam, a plan with odds above 0
ends a proof at once, the first tier's proof from the start) take 2.4 s and
36 s.

**Anytime.** With a time or node budget the result is the best plan found,
`proven_optimal = false`, an `upper_bound` (the root's interval, sound
because every interval is) and `proven_components`: the number of leading
tiers in which the plan is proven optimal (no plan has a better prefix).
`proven_optimal` is only set when a search ran to its end with every chance
node enumerated.

**Adaptive budget.** `SolveOptions::min_proven_tiers` (k) and `max_time_s`
(both set, `max_time_s > time_limit_s > 0`): at `time_limit_s` the search
stops only if the best plan is proven in its first k tiers; otherwise it
keeps going until it is, or until `max_time_s` (`SolveResult::extended`).
Before the search completes the root's bound is the root's tier bound
(unsearched children may still reach it), so "proven in k tiers" means the
plan's first k components equal that bound's, a check `improve` makes for
each new incumbent (an atomic flag; the stop check reads it). Past the base
time every alpha is the incumbent with tiers k.. raised to `INT32_MAX`: a
node or chance outcome that cannot beat the plan's first k tiers is cut,
including everything that would only improve later tiers, so the proof of
the first k tiers no longer waits for the rest. Raising alpha only adds
cutoffs, and every cut reports a sound interval, so bounds stay sound; a
search completed this way proves the first k tiers (and is
`proven_optimal` only if its bound meets the plan in every tier). Later
tiers keep the plan found by then. Without both options nothing changes.
`test_solver.cpp` checks it on tiny boards against the full search (the
first k tiers agree, k = 1, 2, 4, 5) and the stop at the base time and at
the cap.

Four hard live boards (`recordings/live/20261009_233724/`, 3 units, 8
threads, other jobs running; live play had stopped them at 30 s unproven in
the first 0, 0, 0 and 4 tiers), `--time 30 --max-time 120 --min-tiers 5`:
`m22_turn_04` proven optimal at 38 s (a plain 120 s search: 44 s),
`m20_turn_02` at 79 s (84 s), `m12_turn_03` and `m10_turn_02` still short at
120 s (as is a plain 120 s search); with a 300 s cap they reach the target at
127 s (5 tiers) and 252 s (proven optimal). Every plan's value equals the
30 s plan's: on these boards the extra time buys the proof, not a better
plan.

**Time budget** (2026-10-15). A solve with a time budget ends by its whole
budget, `max_time_s` with the adaptive budget, else `time_limit_s`, with
everything it runs between the searches included (`solver.cpp` "Time
budget"; `SolveStats::seed_eval_s`, `recheck_s`, `recheck_cut`, printed by
`--solve` as "outside the searches").
- The searches stop 1% of the whole budget early (at most 2 s), kept back
  for the closing re-check of the returned plan (`evaluate_plan`, a
  diagnostic for the "re-running the plan gives" warning) and the result.
  The re-check runs until the whole budget's end; cut there it is skipped
  (`recheck_cut`, no warning).
- The survey, the survival cap pre-pass, the base search, the seed
  evaluation and the lookahead search are charged as they run: the
  lookahead search gets what is left after the seed. The seed evaluation
  stops running samples at the searches' end; a leaf without its samples
  counts at the NextTurn floor, and if even its enemy phases run past the
  whole budget the base search's value with NextTurn at the floor seeds
  instead. Both are lower bounds of the plan's value (NextTurn only falls
  from its cap 0 to the floor; which outcomes count depends on the odds
  alone), so the seed stays sound and the search after it can only return
  a plan better under the full order. The re-check accepts a value above
  such a seed's.
- With the lookahead both evaluations run the samples only for the chance
  leaves whose NextTurn can still change the plan's value: with the
  missing leaves at the floor and at the cap the value lies between the
  two, and the leaves are run in increasing order of their score until
  the two meet. They share sample values with the lookahead search (the
  seed's go to its table; the re-check reads it). A sample is a function of
  its end board, so values are unchanged.
- Past the deadline a solve only finishes what it is running: one enemy
  phase or lookahead sample per thread, and the result. The caller's work
  comes on top (`itb_live` predicts the plan's board, `--solve` scores the
  recorded plan with the eager `evaluate_plan`).

Before, the base plan's seed evaluation and the closing re-check ran
outside the budget: each is every chance leaf of the plan times 8 samples,
on one thread whatever the solve's thread count. Measured with the live
flags (`--time 30 --min-tiers 7 --threat-tier --objectives-first --mech-grid
2 --mech-grid-turns --mech-grid-pilot --lookahead --grid-survival`), one
thread, `nice`, other jobs on the machine; solve time (`time_s`), base =
82fed1e6:

| turn | budget | base | now | plan |
|---|---|---|---|---|
| run 28 Cave t1 (`20261014_233706` m41) | `--max-time 300` | 519.3 s | 298.2 s (seed 40.8 s, re-check 0.1 s) | odds 0.5304, grid -8, buildings -4, mechs 0, stars -1 in both; next_turn -1030 vs -1092 (the old lookahead search had 75 s, now 34 s) |
| run 32 Cave t3 (`20261015_132930` m41) | `--max-time 300 --survival-cap 0.3` | 325.7 s | 298.1 s (seed 9.1 s, re-check 0.01 s) | the same value |
| run 27 Factory t3 (`20261014_222734` m14) | `--max-time 120 --survival-cap 0.3` | 120.7 s, odds proven, 0 tiers below | 118.9 s, odds proven, 4 tiers | the same value |
| run 27 Factory t3 | `--max-time 120` | 120.7 s, 4 tiers | 118.9 s, 4 tiers | the same value |

Run 28's base search runs to its 75% share (no tier proven) and its plan's
leaves need 40.8 s of samples (the Cave bomb's 16 tiles times the rolls;
the old seed evaluation and re-check, every leaf's samples each, took the
old solve 219 s past its cap). Run 27 with the cap: the survey (1.9 s) and
the pre-pass (3.0 s) no longer come out of the base search's 7.5 s, so it
completes (all ten plain tiers, as with the cap off) instead of getting
1.7 s and stopping at the proven odds. Every check is identical:
`--corpus`, `--moves`, `--weapons`, `--replay`, `--replay --turns` byte for
byte against 82fed1e6; 40 sample boards at 20 000 nodes (plain flags) and 20
at 4 000 nodes with the live flags, with and without `--survival-cap 0.3`,
the same in every reported field but the times (no time budget: no
deadline, the shared samples change only the lookahead counters).
`test_solver.cpp` "time budget": a lookahead solve at 0.4 s (and 0.1 s
without the adaptive budget) on `board_m07_turn01` ends within 0.3 s of its
budget (the unbounded re-check alone takes 0.25 s there, the seed
evaluation as much); tiny lookahead boards give the same values, plans and
nodes with a time budget as without; the pre-pass leaves the search after
it its base time less the survey's.

**Validation.** `test_solver.cpp`: hand-built boards with known optima
(save a building, the greedy kill that loses a building, two threats and two
mechs), chance nodes whose worst case is not the default outcome (lightning
order, a resisted building that causes a bump), anytime bounds, threads
against one thread, and a brute-force cross-check: 30 random tiny boards
(mixed mechs and Vek, buildings, water; 26 of them with chance branches)
against a naive exhaustive enumeration written independently of the
solver, with and without table, bounds, ordering and beam: identical values.

**Recorded boards** (`itb_inspect --solve recordings --sample 60 --time 120
--threads 8`, Apple M6, 12 cores, other jobs running): 46 of 60
boards (76.7%) proven optimal within 120 s; 36.7% within 10 s, 13.3% within
1 s; median time to prove 13.3 s, p95 57 s (median 0.46 M nodes). By active
units: every 1- and 2-unit board, 39/48 with 3 units, 1/4 with 4, 0/2 with 5.
The unproven 14 are still proven optimal in their first 2-5 tiers (5: grid,
buildings, mechs, mech HP, stars). Against the old bot's executed plan,
scored the same way (worst case over every chance outcome): ours better on
45 boards (grid 13, mech HP 25, kills 5, objectives 1, position 1), equal
on 13, worse on none; one recorded plan is refused by the engine (a move
outside the Lua move area). The first plan is found at once (beam), the
returned one after a median 0.17 s (p95 12.6 s).

Whole corpus at 10 s (`--solve recordings --time 10 --threads 8`, 469
boards): 158 proven (33.7%; 12.2% within 1 s; median 2.2 s, p95 8.0 s);
every 1- and 2-unit board, 117/389 with 3 units, none of the 39 with 4-5.
Unproven boards: 239 proven in their first 5 tiers, 14 in none. Against the
recorded plans: ours better on 362 (mech HP 218, grid 43, kills 34,
objectives 29+6, vek HP 14, position 9, mechs 7, buildings 2), equal on 95,
worse on 2 (neither proven: the search had not reached them yet; at 120 s
both are proven optimal and at least as good as the recorded plan), 8
recorded plans refused by the engine (targets outside the Lua target area,
moves by a unit that cannot move).

**Cost.** One thread: a sub-action ~17 us, an enemy phase ~33 us before the
speed-ups below. They change no result: `--replay` and `--replay --turns`
output is byte-identical, the tests pass, and node-budget solves of the
60-board sample (`--nodes 30000` and `--nodes 150000`, one thread) are
identical in every counter (nodes, sub-actions, enemy phases, TT hits,
scores, bounds). Instructions spent in the search at 20 k nodes, one thread
(`/usr/bin/time -l`, startup subtracted): `m17_turn_04` 33.9 G -> 8.8 G,
`20260713_052159_731/m07_turn_01` 76.2 G -> 20.9 G, the 5-unit
`20260508_134925_472/m02_turn_04` 89.7 G -> 21.7 G (3.7-4.1x). Proving
`m17_turn_04` on one thread: 131 s -> 27 s (back to back, other jobs
running).
- Lua: the solver passes the target area it already computed to
  `fire_weapon` (`ActionOptions::known_area`, only for areas computed without
  a Lua error or a random draw); the Lua `Pawn` global is assigned lazily;
  `GetTwoClick` is cached per weapon; the default `Pawn:GetDeathEffect` is
  skipped; sScript chunks are compiled once; and the hottest weapon scripts
  run as C++ ports (stage 6 above): 99.4% of the target-area and
  skill-effect calls on the sample never enter Lua (whose garbage collection
  alone was 17-21% of the time).
- Executor: `update_tiles` only visits tiles that may not be quiet (a mask of
  occupied tiles, webs, non-inert tile states and finished animations, kept
  up to date after every tile that acted), with exact early returns for
  quiet occupied tiles and skipped no-op `note_deaths`; the per-frame board
  snapshot reuses one buffer and compares tiles with `memcmp` (`Tile` has no
  padding); animation timelines are cached per engine; `PawnSim` lookups use
  a small open-addressing index; the resolution hooks are moved, not copied.
- Hashing: tiles are hashed as raw bytes in two multiply-fold lanes.
- Tables: once `tt_max_entries` entries are stored, a new entry replaces the
  deepest entry among its shard's neighbouring buckets instead of being
  dropped (at 120 s with 8 threads, eight sample boards used to sit at the
  cap and search every transposition again; `m17_turn_04` with the tables
  capped at 50 k entries: 608 k nodes with replacement, unfinished after
  14 CPU minutes without). Below the cap nothing changes.

Sample at 8 threads (`--sample 60 --threads 8`, same machine, back to back,
heavy foreign load: load average 16-20 on 12 cores, 2 of them fast):
proven within 10 s 11 -> 19 boards (18.3% -> 31.7%), within 120 s 29 -> 45
(48.3% -> 75.0%; 3-unit boards 23/48 -> 39/48; 4-5 units 0/6 in both, still
proven in their first 3-5 tiers); thread time per sub-action 260 us -> 89 us
and per enemy phase 546 us -> 206 us under that load. Every board proven by
both has the same value; the new plans are better on 2 boards and worse on
none.

Remaining time and limits: the executor is ~40% (frames of walks, pushes and
projectiles; one `Simulation` per resolution), the enemy phase ~25-45%, Lua
~10% (unported weapons, sScripts, `Board:Bounce` scripts), hashing ~5%.
About half of the sub-actions reach a board already in the table or a
sibling's board: interleavings of different units' attacks and repairs
(`m17_turn_04`: weapon after weapon 225 k, repair after weapon 128 k, move
after weapon 104 k, move after move only 11 k). Merging them before running
them needs a proof that two shots commute, i.e. the read and write sets of
the frame-exact executor; moves alone are too small a share to be worth an
independence rule. The tier bounds stop at ObjectiveProgress (no cap), so
below the first five tiers the search is exhaustive: the unproven boards are
proven in their first 3-5 tiers. With 8 threads per-operation costs rise
2-3x (2 super, 4 performance and 6 efficiency cores, shared caches, foreign
load).

**Final-mission turns** (2026-10-14; Mission_Final m40 and Mission_Final_Cave
m41 of the live runs, the live flags `--time 30 --max-time 300 --min-tiers 7
--threat-tier --objectives-first --mech-grid 2 --mech-grid-turns
--mech-grid-pilot --lookahead`, 6 threads, other jobs running; profile
counters in `itb_inspect --solve` single-board output and JSON). Live, many
of these turns ended "optimal in the first 0 tiers" after 300 s.
- Shape: 3 mechs against 6-8 enemies, a wide and shallow tree: 2-2.7
  children per expanded node, most nodes at depth 5-6 (whole or nearly whole
  plans; run 13 m41 turn 3: 0.05 M nodes at depth 3, 0.57 M at 4, 3.4 M at
  5, 9.1 M at 6 after 2400 s). Each node costs one end-of-turn value (one
  enemy phase, more under chance) and its children's sub-actions.
- Where every plan loses grid (the enemy phase alone takes it: run 13 m41
  turn 3 loses 2 grid; the Unfair runs 15 and 19 lose their last grid
  points), the grid bound (grid power never rises) cuts only lines whose own
  actions lose grid, so proving even the first tier visits every distinct
  end board. Run 13 m41 turn 3: 14.5 M nodes in 2400 s without completing
  (the 2 M-entry tables overflow; 8 M entries: no gain at 300 s, RSS 3.5 ->
  4.9 GB). No cheap sound bound is tighter: any queued attack can be changed
  by a push, kill, block, smoke, ice or shield anywhere on the board.
- Sub-actions: with line weapons (Pierce, Hydrant, Scarab, Napalm squads)
  40-75% of the shots gave a sibling's board, now skipped as same-effect
  shots (up to 21 M per 300 s solve).
- Enemy phases: inside the tier proof most outcomes are cut (98% of the
  end-of-turn values here), now mostly before the phase ends. Outside it,
  Grid Defense rolls and the Cave's bomb re-drop (a MissionRandom choice
  over up to 16 inner tiles) multiply phases (run 15 m41 turn 4: 8 per
  end-of-turn value).
- The lookahead search re-derived what the base search knew, and when the
  budget cut it, reported its own root bound: tiers the base search had
  proven were lost from the report.

Live flags, 300 s cap, 6 threads, the base commit and this one back to back
per board (P = proven optimal; tiers as reported):

| board | base: tiers, nodes | now: tiers (base search), nodes | value |
|---|---|---|---|
| run 9 m41 t6 | 0, 14.9 M | 0 (0), 23.2 M | next-turn -128 -> -192 (unproven) |
| run 9 m41 t7 | 0, 8.5 M | P at 267 s, 11.4 M | same |
| run 13 m41 t3 | 0, 7.9 M | 0 (0), 17.5 M | same |
| run 15 m41 t1 | 0, 8.5 M | 0 (0), 19.0 M | same |
| run 15 m41 t4 | 0, 5.4 M | 0 (0), 7.9 M | threat better |
| run 17 m40 t1 | 0, 7.8 M | P at 226 s, 10.4 M | same |
| run 17 m41 t1 | 0, 8.2 M | P at 273 s, 13.2 M | same |
| run 19 m41 t1 | 0, 1.8 M | 0 (0), 2.3 M | same |
| run 19 m41 t2 | 0, 2.1 M | 0 (0), 3.4 M | threat better |
| run 4 m41 t5 | P at 155 s | P at 67 s | same |
| `20261011_222439` m41 t2 | P at 193 s | P at 124 s | same |
| `20261012_113135` m41 t3 | P at 192 s | P at 135 s | same |
| run 17 m41 t3 | P at 125 s | P at 74 s | same |

Proven 4 -> 7 of 13; proven by both: the same value, in 50-65% of the
time. Nodes per 300 s 1.3-2.2x. The unproven six lose grid in every line
(the first four lose the last grid points on Unfair); their plans keep the
same grid, building, mech and star tiers. Thread time per sub-action and
per enemy phase fell 25-65% (same load, back to back). Peak memory 3.2 ->
3.9 GB (run 9 m41 t6: the base search's tables stay alive for the
lookahead search). Checks: the 100-board sample (`--sample 100`, 3 threads
each, side by side) at `--time 10` without and `--time 5` with the
lookahead: every tier proven by both builds agrees (88 and 83 boards; 9
boards proven optimal by both, all with the same value); the new build
proves more tiers (444 -> 456, 444 -> 448) and finds a better plan on 15
boards, a worse one on 1 (unproven).

**Final-mission profile, round 2** (2026-10-15; the live turns that ended
"optimal in the first N tiers" at the 300 s cap in runs 21-29: Cave t1 of
runs 21, 26, 28, 29, Cave t2 of run 26, t3/t4 of run 23, t4 of run 29,
Final t1 of run 23 and t2 of run 24; live flags plus `--grid-survival`;
measured with one thread, other jobs on the machine).
- Two kinds of turn. (A) Every line loses grid (r28 t1, r29 t1, r24 t2,
  r23 m40 t1; 2-4 grid in the worst case): the root's grid bound stays 0, so
  even the first tier needs every distinct board of the turn. A sub-problem
  after the first unit's turn (the recorded step states) is proven optimal
  in 9 k-400 k nodes (4-60 s on one thread); the root has hundreds of first
  unit turns, so the whole proof is 10^7+ nodes (run 24 t2: 21.6 M). (B)
  The beam finds a plan perfect in the first four tiers within seconds
  (r29 t4, r26 t1/t2, r23 t3/t4, r21 t1), so the base search's four tiers
  are proven at once and give the lookahead search no cuts; that search
  then visits every board whose first four bounds are perfect, with few
  next-turn estimates (r26 t1: 71 in 112 s). Either way the proof
  enumerates boards: about one enemy phase per expanded node (70-89% of
  them stopped early by the bound), 2.1-2.5 children and 3.4-4.1 sub-action
  runs per node.
- Where the time goes (sampled): enemy phases 35-55%, sub-actions 25-46%,
  `pure_weapon_effect` 5-12%, Lua garbage collection 8-13%, the executor's
  frame loop most of the engine time (`Simulation::update_tiles` 16-23%,
  of which the tile-state cache check 5%; the board copy and comparison
  per frame 7-10%), hashing 4%, locks and table work small. Of the
  sub-action runs, about a third reach a board already in the table (moves:
  57% on r29 t1) and 15-35% a sibling's board; Repair was 95% duplicates or
  turn-only no-ops.
- Spent turns (above): 7-35% fewer nodes on the Cave sub-problems below,
  32% on the corpus sample's proven boards. The per-frame board comparison now compares the pawn list
  bytewise first (`Board::operator==`: 4-5% fewer instructions), and the Lua
  collector waits for 10x the live heap (`LuaHost`, about 40 MB per engine;
  collection was 7.7% of one profile at the default pause and 2.9% at 40x;
  2-15% fewer cycles at 10x, the same instructions).
- With one thread the grid survival survey can miss the sure line that 8
  threads find (r28 t1), and the search with the odds then runs 23-45 enemy
  phases per leaf (Grid Defense rolls times the Cave's 16 bomb tiles). That
  solve also ran 240-280 s past its 300 s cap: the plan is re-checked
  (`evaluate_plan`, every chance leaf times 8 lookahead samples) outside
  the budget, and so was the base plan's seed evaluation (fixed since:
  "Time budget" under the search).

Before and after (base = 662e64ca; one thread, `nice`, one process at a
time, other jobs on the machine). Every check is identical: `--corpus`,
`--moves`, `--weapons`, `--replay`, `--replay --turns` byte for byte; with
`ITB_SKIP_SPENT=0`, 40 sample boards at 20 000 nodes identical in every
counter; 60 sample boards at 150 000 nodes (plain flags): proven 17 -> 21,
every board proven by both with the same value, on those nodes 694 k -> 469 k
and sub-actions 1.84 M -> 1.38 M with the same enemy phases, tiers proven
340 -> 360.

| sub-problem (live flags, proven optimal) | base: time, nodes | now: time, nodes |
|---|---|---|
| r28 m41 t1 after step 2 | 4.3 s, 27.8 k | 3.8 s, 24.6 k |
| r29 m41 t1 after step 1 | 20.4 s, 109 k | 13.7 s, 84.3 k |
| r24 m40 t2 after step 1 | 56.2 s, 399 k | 54.3 s, 370 k |
| r23 m40 t1 after step 3 | 6.2 s, 12.5 k | 5.3 s, 8.9 k |
| r29 m41 t4 after step 2 | 11.3 s, 165 k | 7.9 s, 150 k |
| r26 m41 t2 after step 2 | 70.6 s, 983 k | 63.2 s, 672 k |
| r23 m41 t4 after step 2 | 180.0 s, 2.44 M | 154.9 s, 1.83 M |
| r21 m41 t1 after step 2 | 30.9 s, 80.9 k | 26.4 s, 52.9 k |
| total | 380 s, 4.21 M | 330 s, 3.19 M |

All eight: the same value and the same plan. The ten whole turns at the
live budget on one thread (`--time 30 --max-time 300`, an eighth of the
live compute): the same proven tiers in both builds (0 on the four kind-A
turns, 4-6 on the others), the same plan and value on nine; on r28 t1 the
new build found better odds (0.530 against 0.525). Not measured: thread
scaling (the machine allowed one thread).

Limitations: limited-use weapons are assumed available unless the bridge
recorded their uses (`Pawn::uses`, bridge extension); proofs are relative to the engine's model (`PhaseResult::exact`
false for EnvInexact missions is reported as a warning); 128-bit hash
collisions are ignored; past `tt_max_entries` entries the tables replace old
entries (deepest first).

**Threat tier measurement** (experimental tier, "Turn score"; 2026-10-11,
all 733 recorded boards of the main checkout, other jobs running).

The proxy (`itb_inspect --threat-check recordings`): of 1060 queued attacks
that hit a building (found by running the enemy phase with only that attack
kept), the proxy counts 1058 on the board where they were queued (the two
misses are Bouncers whose push bumps a pawn into a building; pushes are not
modelled). As a prediction, on 535 turns followed by the next recorded turn
(the engine's board after the enemy phase vs the attacks the game's AI then
queued): of 1073 enemies on both boards, the proxy counts 720, 410 hit a
building next turn, 406 of them counted (recall 99.0%, precision 56.4%
against a base rate of 38.2%). It is a cautious upper bound, not a forecast.
Cost: `count_threats` 1.9 us median per board after the enemy phase, one
enemy phase 33.5 us; node-budget solves of 30 Hard boards (20 k nodes, one
thread): 89.4 -> 93.0 us per node (+4%).

Solves at `--time 10 --max-time 60 --min-tiers 5 --threads 4`, off vs
`--threat-tier` side by side (on: 5 tiers = grid through threat). Totals
over the boards; the old tiers of the threat plans are compared with the
off plans, threats are each plan's worst case scored with the tier.

| | Hard (306) | Hard, final missions (27) | other (sample of 150) |
|---|---|---|---|
| plans changed | 173 | 15 | 82 |
| threats next turn | 573 -> 389 | 63 -> 52 | 175 -> 88 |
| mech HP | -38 -> -185 | -25 -> -39 | 32 -> -10 |
| objective progress (stars) | -16.7 | 0 | -11.0 |
| kills / Vek HP | +39 / +48 | +5 / +10 | +23 / +25 |
| position | -100 | -6 | -22 |
| proven optimal | 106 -> 112 | 3 -> 8 | 69 -> 86 |
| proven in 5 tiers | 238 -> 204 | 14 -> 10 | 143 -> 142 |
| proven through mech HP | 238 -> 145 | 14 -> 9 | 143 -> 133 |
| mean time, at the 60 s cap | 23.5 -> 29.2 s, 68 -> 102 | 39.5 -> 47.2 s, 13 -> 17 | 11.5 -> 13.5 s, 7 -> 8 |

Grid and building differences (+5 for the threat tier on Hard, one more
mech lost on one board) all come from boards neither search proved, i.e.
budget noise. The objective progress lost is progress toward bonus stars
that now ranks below the threat (a dam or vat not hit, fewer bots frozen).
On 60 evenly spaced Hard boards (other runs alongside): off 111 threats,
116 kills, mech HP +1; threat tier 75, 126, -26; `--kills-tier` (kills
above mech HP) 91, 140, -27; both flags (threats, then kills) 72, 140, -39.

Round 2 (`--objectives-first`; 394 Hard boards: the 306 above plus run 4,
`live/20261011_011242`): off at `--min-tiers 5` against `--threat-tier
--objectives-first` ("threat") and `--threat-tier --kills-tier
--objectives-first` ("threats, kills") at `--min-tiers 6` (through mech HP),
`--time 10 --max-time 60 --threads 4`, the three side by side on a machine
other jobs kept at load ~30 (absolute times are inflated; compare the arms).
Repeating the off arm on run 4 gave the same value on 87 of 88 boards.

| Hard (394) | off | threat | threats, kills |
|---|---|---|---|
| threats next turn | 738 | 547 | 544 |
| mech HP | -49 | -246 | -306 |
| objective progress (stars) | 80.2 | 102.1 | 102.1 |
| kills / Vek HP | 781 / 2460 | 842 / 2542 | 889 / 2645 |
| position | -435 | -574 | -585 |
| proven optimal | 65 | 109 | 127 |
| proven through the tier slot (5) | - | 187 | 170 |
| proven through mech HP | 232 | 150 | 149 |
| mean time, at the 60 s cap | 32.2 s, 163 | 44.2 s, 245 | 45.1 s, 245 |

Objective progress now only rises (38 boards, +21.9 stars, for 56 mech HP
there; it now also outranks mech HP). Elsewhere 141 mech HP buy 165 fewer
threats. Higher tiers differ on 7 boards, all unproven in those tiers (budget
noise, both ways). Run 4 (88): threats 157 -> 127 / 126, mech HP -14 -> -62 /
-68, progress +5.5 / +6.5 stars, kills +15 / +22, proven through mech HP 48
-> 29 / 30. Its 7 turns unproven even in grid at 120 s live stay unproven in
grid in every arm, with the same grid result; the tier changes 2 of them
below grid (m04 turn 1: one threat fewer, one kill and a third of a star
more, for 2 mech HP).

**Mech exchange measurement** (`--mech-grid`, "Turn score"; 2026-10-11,
after live Hard run 5 was lost). Mission_BotDefense (`live/20261011_011242`
m68) turn 1 was proven optimal with the Drill Mech dead (worst case grid 0,
building HP 0, mechs -1, mech HP -8; the same plan with and without the
threat tier, proven in 28 s at 8 threads, so no plan keeps every mech at no
building cost). `--mech-grid 7` proves the cheapest plan that keeps all
three: 1 grid and 1 building HP (a Moth's shell on a building, Grid Defense
may still resist it), mech HP 0 instead of -8, the same kill and Vek HP;
`--mech-grid 1 --mech-grid-turns` with the live flags picks it too (31 s).
With two mechs the mission then lost 3 + 2 grid and both stars, and the
next mission lost the last 2 grid on its first turn.

Corpus (all 878 recorded boards: 391 Easy, 35 Normal, 451 Hard, 1 Unfair;
`--time 3 --max-time 60 --min-tiers 3 --threads 4`, then the boards that
lose a mech again with 180 s): the plan loses a mech on 14 boards (1.6%; 8
of the 451 Hard), plus 2 where it was budget noise (a longer search keeps
every mech at no cost). The cheapest plan that keeps every mech
(`--mech-grid 7`) costs 1 grid and 1 building HP on 10 of the 14, 2 on 2,
and does not exist on 2; it mostly keeps more mech HP as well. 7 of the 14 are on the mission's
last turn; of the 7 before it, 5 cost 1 grid (BotDefense t1/4, ScarabBoss
t3/4 and CentipedeBoss t3/4 on Hard, Tanks t1/4 also with 2 stars, AcidStorm
t2/4) and 1 costs 2 (Mission_Final t3/4).

What a lost mech costs, from the recorded games (grid at each turn start):

| turns played with | Hard: turns, grid lost per turn | all: turns, per turn |
|---|---|---|
| 3 mechs | 332, 0.148 | 637, 0.127 |
| 2 mechs | 4, 1.25 | 13, 0.85 |

On Hard, missions that kept 3 mechs lost 0.34 grid between the start of
turn 2 and the start of their last turn (109 missions); the two Hard
missions that lost a mech before then lost 3 (m68, plus 2 more in its last
turn) and 2 (`live/20261010_174709` m26, AcidStorm). Few turns and
confounded (harder boards lose mechs), but every estimate puts a mech lost
with L turns left at roughly 0.7-1.1 grid per turn left, plus its pilot.

Arms with the live flags (`--threat-tier --objectives-first --time 10
--max-time 60 --min-tiers 6 --threads 4`), on the 16 boards where the
screen lost a mech:

| | off | `--mech-grid 1/2 --mech-grid-turns` | `--mech-grid 2` |
|---|---|---|---|
| grid / building HP | -8 / -8 | -13 / -13 | -24 / -24 |
| mechs lost | 15 | 10 | 2 |
| mech HP | -50 | -34 | -9 |
| stars failed | 2 | 4 | 4 |

`--mech-grid 1 --mech-grid-turns` and `--mech-grid 2 --mech-grid-turns`
choose the same plans: 1 grid for a mech on BotDefense t1, ScarabBoss t3,
CentipedeBoss t3 (unproven), AcidStorm t2 and Tanks t1 (which also fails 2
stars: mechs already outrank stars). Mission_Final t3/4 needs 2 grid with
1 turn left and keeps the plain plan; last turns are unchanged. Without the
cap (`--mech-grid 2`) last turns trade too, and budget-limited boards trade
grid for a mech a longer search keeps for free.

Elsewhere the exchange changes nothing it could: with the plain optimum
keeping every mech, no plan has a better slot. Cost, on 60 evenly spaced
Hard boards with node budgets (150 k nodes, one thread each, both arms side
by side): identical node counts on 59, 11 proven in both, 17 proven in 6
tiers in both, the same speed per sub-action. 3 plans differ, all unproven
and from search order: the end-of-turn values that order children and the
beam rank a state with a mech about to die lower, so a plan that passes
through one is found later (`live/20261011_011242` m32 turn 3: both proven
optimal with the same value, at 110 s off and 149 s with the exchange,
best plan found at 0.5 s and 116 s). A timed comparison on the same boards
was dominated by machine load (3x slower per sub-action in one arm).
Suggested live trial: `live_play.py ... --mech-grid 2 --mech-grid-turns`
(a mech is worth up to min(2, turns left) grid, never the last grid point);
it is still off by default.

The pilot floor (`--mech-grid-pilot`) on live run 13 (Hard,
`live/20261013_024040`) Mission_FreezeBldg turn 4 of 4, where the plan
lost the 1-HP burning Pierce Mech (pilot level 1, 46 XP) and a star with
grid 7/7: with the floor its rate is 1 (more than one grid point), and the
search still proves the trade optimal (39 s, 2 threads): keeping the mech
costs 2 grid in the worst case (no Grid Defense resist; with rate 2 the
search keeps the mech and the star for those 2 grid). A plan that keeps a
levelled pilot for one grid point is now taken.

**Next-turn lookahead measurement** (`--lookahead`, "Turn score";
2026-10-13). `itb_inspect --lookahead-study` solves a recorded turn with the
live flags (`--threat-tier --objectives-first --mech-grid 2
--mech-grid-turns`) with and without the lookahead, plays each plan through
its enemy phase, lets the AI plan the next turn with independent seeds (not
the solver's) and measures what each next-turn board forces: a real solve
of the next turn (4 s, 2 threads; plain grid, mechs and building HP of its
plan) and the greedy reply. The greedy reply matches the 4 s solve on most
boards (on Civilians turn 2 below, 6 of 8 seeds; it is an upper bound and
missed two combinations); a sample costs about 3 ms of AI planning and
20-40 ms of reply.

Turns before the losses of the lost Unfair runs (`--time 30`, 2 threads,
8 independent seeds per plan, next-turn loss by the real solve; the
lookahead with its defaults):

| turn (run, mission) | this turn | next turn, without | with the lookahead | game's next turn |
|---|---|---|---|---|
| run 12 m00 Artillery t3/4 | 0 lost (both) | 1.50 grid, 0.25 mechs; 8/8 lose | 0 lost in 8/8 | lost 1 grid + a mech |
| run 11 m30 Civilians t2/4 | 0 lost (both) | 1.50 grid, 0.12 mechs; 8/8 | 0.50 grid; 4/8 | lost 1 grid |
| run 10 m00 Volatile t2/4 | -3 grid (both) | 1.00 grid, 0.62 mechs | 1.00 grid, 0 mechs | lost 1 grid |
| run 12 m06 Tanks t2/4 | 0 lost (both) | 0.75 grid; 6/8 | 0.75 grid; 4/8 | lost 1 grid |
| run 11 m30 Civilians t3/4 | -1 grid (both) | 0.12 grid, 0.62 mechs; 6/8 | 0.75 grid, 1.00 mechs; 8/8 | lost 1 grid + a mech |
| run 10 m00 t1, run 11 m30 t1, m32 t1-t2, run 12 m02 t2-t3, m04 t1, m06 t3 | same top tiers | no change in the forced loss (1 grid forced on 4 of them in every sample: no plan avoids it) | | |

Run 12's turn 3 of its first mission is the clearest: both plans lose
nothing this turn; after the live plan the game took 1 grid and a mech on
turn 4 (the run's first losses); the lookahead plan uses the Ice Mech on
the Scorpion at B3 and the Hydrant Mech on the Scarab at E2 instead, and no
sampled AI outcome then forces a loss. Run 11's losses came from
turn-1 boards (deployment) and forced chains (Missiles turn 1 forces 1 grid
on turn 2 in every sample and every plan); run 10 had lost a mech on turn
1. Civilians turn 3 is the sample noise the estimate cannot rule out: its
own 8 seeds preferred a plan that independent seeds rate worse.

Recorded live turns (60 evenly spaced Hard and Unfair turn starts of runs
4-13 that are not a mission's last turn; `--time 20`, 2 threads, both arms
side by side; each end board judged by the greedy reply on 16 independent
AI seeds):

| | without | with `--lookahead` (8 samples, worst) |
|---|---|---|
| plans changed | - | 36 of 60 |
| grid, buildings, mechs, stars this turn | - | never worse (2 better: budget noise) |
| next-turn grid lost, sum of the per-turn means | 22.9 | 14.2 (-38%) |
| next-turn mechs lost, same | 7.4 | 4.4 (-41%) |
| samples that lose anything next turn | 310 of 960 | 194 |
| turns better / worse next turn | - | 25 / 8 |
| mech HP, objective progress, kills (sums) | - | -16, -3.7 stars, 0 |
| lookahead samples, thread time | - | 4549, 118 thread-s (26 ms each) |

Alternatives on the same turns: the sum of 8 samples instead of the worst
13.1 grid / 4.9 mechs (objective progress -4.9 stars), the worst of 16
13.3 / 4.9 (-5.1 stars), all within the noise of each other, so the default
is the cheapest (worst of 8). Two samples overfit: on Civilians turn 2 the
search found a plan that its two seeds rated loss-free and independent seeds
rated 5 grid. Before the base search seeded the lookahead search (below),
2 of the 60 turns lost grid to the slower search.

Cost: the budget is shared, not extended. At 20 s and 2 threads the
lookahead search spent about 10% of its thread time in estimates (median
33 samples per turn, at most 871), the base search 25-75% of the budget, and
the tiers through stars were proven on 53 of 60 turns (52 without). At 8
threads the same work per sample (26 ms of one thread) gives about 300
samples per second; the live budget (`--time 30 --max-time 120`) leaves the
base search 7.5-90 s and the lookahead search the rest. Suggested live
trial: `live_play.py mission ... --lookahead --min-tiers 7` with the live
flags (7 tiers: grid through mech HP with the threat and next-turn tiers).

**Two-turn measurement** (`itb_inspect --two-turn`, 2026-10-13, after
Unfair run 18 went from grid 7 to 1 and lost the Charge Mech on
Mission_Solar, `live/20261013_180011` m24, every turn proven optimal with
the live flags and `--lookahead`). Turn mode solves a turn-start board with
several arms (`base` = the live flags; `laN` / `sumN` / `idleN` = with the
lookahead, worst or sum of N samples, or the idle reply) plus recorded plans
(`--plan`, e.g. the run's `_solve.json`), plays each distinct plan through
its enemy phase and judges the next turn on `--next-seeds` independent AI
seeds by a real live-flags solve of each board (plain grid, mechs, building
HP, stars, mech HP of its plan). Deploy mode (`--deploy`) plays placements
(`--placement`, the state's own, the deployment search's `--deploy-top N`)
through the deployment's AI phase for `--deploy-seeds` seeds, solves each
distinct turn-1 board with one arm and judges turn 2 the same way; turn 1's
spawn tiles are hidden at deployment, `--spawn` adds them (a retrospective
study can take them from the recorded turn-1 board).

The mission: 7 building tiles with 9 HP for grid 7, two of them the Solar
Farms (B3, D7: Mission_Critical, 1 star = 1 grid power at mission end each,
lost when the farm is damaged); Scorpions, two Burrowers, a Spider and a
boosting Jelly. Grid 7 -> 7 -> 6 -> 5 -> 1: turns 2 and 3 were each 3 grid
in the worst case and lost 1 (Grid Defense resisted twice at F3), turn 4
(the last) forced 4 grid and the 1-HP Charge Mech (proven: keeping the
mech cost all 5 grid in the worst case). Findings (2 threads):

- Deployment: the search (re-run with 16 seeds, 4 finalist sets) picks the
  live placement again (B5 Judo, F6 Charge, C6 Grav: the only finalist
  whose sampled AI outcomes cost no grid on turn 1). Over two turns (8
  deployment seeds, turn 1 solved with `la8` 8 s, turn 2 judged on 6 seeds)
  the nine placements tried (it, live_play's heuristic B5/E5/E7 and the
  search's next seven) lose 1.8-2.8 grid in turns 1-2 on average and 5 in
  the worst outcome, every one: the G-side placements (G6/B6/G5, G7/B6/G5)
  lose about 1 grid on turn 1 and 1 on turn 2, the B/F/C ones 0.25 and 1.6.
  No placement avoids the losses; the live one is among the best (1.85).
  With the ranking options (`--threat-tier ... --lookahead`) the search
  picks the same placement (its turn-1 next-turn estimate is the worst of
  the finalists, but it is the only one with no turn-1 grid loss).
- Turn 1: `base`, `la8` (the live plan), `la16`, `la32`, `sum8`, `sum16`
  give four distinct plans, each losing nothing on turn 1; on 16
  independent seeds every one loses grid on turn 2 in 15-16 of them (mean
  2.2-2.5, `la32`'s 3.6, worst 4-5). The board already forced the losses.
- Turn 3 (next turn the last): the live plan (`la8`, also `la16`) leaves
  turn 4 losing 1.31 grid and 0.56 mechs on average (worst 2 grid and 2
  mechs); the `base` / `sum8` / `sum16` plan, the same on turn 3 itself,
  0.69 grid and 0.12 mechs (worst 1 and 1). The game's turn 4 took 4 grid
  and a mech, worse than any sample of either. Turn 2: 1.31 (live) against
  1.19 (`sum8`).

The sum fold looked better here, so it was measured on 139 turn starts of
runs 10-18 (every 7th from three offsets, not last turns; `la8` against
`sum8`, 10 s each, 8 next-turn seeds of 3 s): the plans differ on 32, the
turn itself is the same (grid, mechs, stars; mech HP 109 against 119 lost),
next-turn grid lost (sum of the per-turn means) 25.5 against 24.1, mechs
2.25 against 3.0, turns better by grid + 2 x mechs 8 against 9. No clear
gain: the default stays the worst of 8.

The deployment search with the live ranking options (`itb_live deploy
--threat-tier --objectives-first --mech-grid 2 --mech-grid-turns
--mech-grid-pilot --lookahead`) against the plain one, on 18 live
deployments of runs 10-18 (every 6th; 30 s each on 2 threads; each chosen
placement judged over two turns: 6 fresh deployment seeds, turn 1 solved
with `la8` for 6 s and the recorded turn-1 spawn tiles added, turn 2 on 4
seeds of 2 s): the same placement on 2; on the other 16 the plain search's
placements lose 6.0 grid on turns 0-1 and 4.8 on turn 2 (sums of the
means), 1 star and 0.25 mechs, the ranked search's 10.3 and 2.7, 2.3 stars
and 0.04 mechs; lower grid + 2 x mechs over the two turns 9 against 5. The
0.5 s turn-1 solves leave the lookahead search little time, and trading
certain turn-1 losses for estimated turn-2 ones does not pay: the option
stays off.


The Lua bridge (`src/bridge/modloader.lua`, `ITBX`) adds to every state
dump, each under pcall with failures listed in `bridge_errors`: the
mission's key, class chain and raw instance dumps of the mission and its
LiveEnvironment (`mission_state`, stage 7 spec section 4), zones, the
spawn queue with types in queue order (from whichever save matches the
board), per-unit Lua traits, every equipped weapon with its exact upgraded
id, power and limited uses (`weapons_exact`, `weapon_slots`), pilot level,
XP and skills, `moved`, queued shots of non-enemy units and
`attack_order_all`, PowerStart, BlockedSpawns, the drop zone, ice HP,
building population, custom tiles and an environment strike log. With the
debug flag file it also takes a per-frame phase log and captures the enemy
phase before and after the spawns, and accepts `SCENARIO` (build a test
board with the game's bindings) and `FIRE`; `SNAPSHOT` and `MOVE_NATIVE`
(a player-style move) are always available.

`load_recording` reads all of it when present (469/469 old recordings load
as before). The bridge reports whether a pawn is webbed and at most one
source, a guess (the nearest webber). Its `grappled` field is the game's own
flag (`Pawn:IsGrappled`) and wins over `web`, which passes through an old
guess that misses spider eggs. The loader rebuilds the tile webs from
what the board shows: every neighbour of a webbed pawn's tiles (a train's
second tile too) whose Vek has a webbing attack queued on that tile
(`ScorpionAtk*`, `LeaperAtk*`, `MosquitoAtkB`; the Scorpion Leader's
`ScorpionAtkB` webs all four), or a spider egg. Only when none does is the
reported source used. `turn_context(rec, visibility)` builds the TurnContext; the
spawn types and queue order are hidden information, so only
`Visibility::Full` (validation) passes them on, never the solver's
`Visibility::Player`. `itb_tests` compiles the bridge in Lua 5.1 and runs it
against a strict mock of every binding it calls (`tests/bridge`).

Safety (after the 2026-10-09 freeze): `DEPLOY` refuses tiles the native
deploy UI refuses (`Board::GetDropZone` / `IsAvailable`: Mission_Final
pylons, save `blocked_points`, items, pods, danger, spawn points, ...), and
`deployment_zone` / `drop_zone` leave them out. A mech on a pylon tile hangs
the game when the turn-0 pylon drop lands (`BoardSpace::DamageSpace` loops
on the mech's corpse); the BaseNextTurn wrap also moves such a mech off
before the drop. All extension work runs under a CPU budget (`ITBX.guard`):
a call over 1 s (aborted by a count hook when `debug` is available), or 3
calls over 0.25 s, turn the extension off (`bridge_ext_disabled`) while the
old fields keep coming. Save files are parsed once per version, not per
dump. `grid_lost_this_turn` / `grid_power_estimate` track grid loss inside
a turn from building HP (the save grid only changes at turn boundaries).

Settled states (after the 2026-10-09 cave turns 1 and 6, where a dump read
mid-animation showed Ranged_Ignite's pushed Vek on their old tiles): every
dump carries `busy_state` (`Board:GetBusyState()`, 0 = idle), `board_busy`,
`command_waiting` (a command still waits for its effects), `stable` (neither)
and `dump_seq`. ATTACK, TWO_CLICK_ATTACK, MOVE_ATTACK and REPAIR wait until
the board is idle (15 s budget) in every speed mode; only SetSpace moves keep
the 2 s fast-mode cap. When a dump is not stable, the bridge dumps again on
the first idle frame. Readers should diff only `stable` states; the loader
warns on the others (`Recording::board_busy`, `board_busy` in
`Engine.load`). Unit `max_hp` falls back to the save's `max_health`
(`Pawn:GetMaxHealth` is not bound), so Networked Armor and pilot bonuses
count and a Regen pilot's turn-start heal is predicted.

`itb_inspect --predict <state> [--actions ...] [--branches]` prints the
engine's outcomes for a bridge state; `scripts/live_validate.py` runs a
scenario end to end against the game. See `LIVE_TEST_PLAN.md`.

### Enemy AI planning and deployment (stage 10)

Between the end of the spawns (where `Engine::end_turn` stops) and the next
player turn the game's AI plans (`AiController`, state 2): `StartTurn` makes
every enemy-team or neutral pawn active; `NextPawn` takes the first active
pawn in board-list order (a frozen one is just made inactive); it plans,
moves (`Pawn::ManualMove`) and fires its weapon (`SkillManager::FireWeapon`:
a queued weapon telegraphs, the instant part resolves now: webs, eggs,
blobs, smoke), then is made inactive. `Engine::ai_phase(board, options)`
(`ai_phase.hpp`) does the same on the board `end_turn` left. Planning
(`AiPlanner`), in our words:

- Candidates: `GetReachablePoints` from the move origin minus blocked
  tiles, then the pawn's own tile; a tile is scored if it is enterable for
  the pawn's profile (`IsEnterable`) or is its own tile.
- `ScoreLocation(tile)`, with the pawn temporarily on that tile: the
  position score is Lua `ScorePositioning(tile, pawn)` (0 without calling
  Lua when the pawn's move speed is 0; at most 0 for an injured 1-HP pawn
  that moves); the target score is the best `SkillManager::GetTargetScore`
  over the armed weapon's target area from that tile (none when the pawn
  could not act there, `Pawn::IsActable`: smoke, water). That is Lua
  `Skill:GetTargetScore` (`GetSkillEffect` + `ScoreList`; several Vek
  override it), minus 5 for the target the pawn shot at last (save
  `targetHistory`; a positive Lua score stays at least 1). Ties for the
  best positive score: `random(n)`. Once a non-edge tile has a positive
  target score and a non-negative position score, edge tiles are skipped.
- `SelectPlan`: plans below -10 position (and edge tiles, as above) are
  out; the order is positive position over negative, then target score,
  then position score. Ties: `random(n)`. Then `random_bool(4)`: one time in
  four, when there was a group of best plans before the last improvement, a
  random one of those that is off the edge with a positive target score and
  a non-negative position score is taken instead.
- The weapon is the pawn type's Lua `Weapon` (skill index; skill 0 is Move).

Randomness. `random_seed(aiSeed)` before each pawn plans, and the next
pawn's seed is `unique_rand(seed)` drawn after its planning. The live game
is the macOS build, whose libc `rand()` is the Park-Miller minimal standard
generator (not glibc's, which the Lua host's stream copies): its state is
its last output, so a whole AI phase is one stream from the seed the game
saved at the start of the turn (`aiSeed` of the mission's region in
`saveData.lua`). Besides the planner's own draws, each
`Board:GetDeployLocScore` on a free tile (Spider, Blobber, Shaman scoring)
constructs a temporary `Pawn`, whose constructor draws three numbers
(measured: 1, 2 or 4 fit far worse). The live seed is hidden: replays use
the saved one, the deployment search samples seeds.

Not modelled: `SetPlanPoints`' extra Lua calls (no shipped enemy weapon
draws there), priority targets (tutorial only), the "IsFast" timing
shortcut.

Validation (`itb_inspect --ai recordings`, the live runs to 2026-10-13):

| AI phase | phases | all Vek exact | Vek exact (tile and target) | phase seed chain |
|---|---|---|---|---|
| after turns 1+ (the turn's saved seed; Grid Defense as the next board shows) | 258 | 72.9% | 89.2% of 622 on the board, 83.4% of 541 spawned in the phase | 150/258 end on the next turn's saved seed |
| before turn 1, seed from an earlier save (the island's boards are built ahead, each with its seed) | 24 | 62.5% | 82.0% of 111 | 12/24 |
| before turn 1, hidden seed (64 sampled seeds) | 202 | the game's outcome among the samples: 48.0%; the most common sample: 7.4% | most common sample: 40.1% | - |

The first pawn to plan matches 217 of 228 times (95%); errors compound
because a different draw count shifts every later pawn's seed. Known gaps:
Spider/Shaman/Blobber scores (their `GetDeployLocScore` scoring), and
boards where the enemy phase itself differs. The turn-0 AI is the same code:
its outcomes are many and close to equally likely (the most common sampled
outcome matches the game 7% of the time), which is why the deployment
search samples seeds.

Deployment (`deploy.hpp`). Confirming the deployment runs that enemy phase
(`BoardPlayer::FinalizeDeployment` -> `StartState(1)`, then state 2 and the
AI) before turn 1. `deploy_search(engine, deployed_board, ctx, drop_zone,
options)`:

1. Screen: every set of free drop-zone tiles, one per mech, the mechs in
   uid order (the AI scores a mech by its tile, not by which mech it is),
   played through `end_turn` + `ai_phase` for `screen_samples` sampled
   seeds; each turn-1 board valued by ending turn 1 at once.
2. Finalists: every assignment of the mechs to the best `finalists` sets,
   plus `always` (live_play passes its heuristic placement), `samples`
   seeds each; every distinct turn-1 board solved (`solve_turn`,
   `solve_time` each), valued as what the phase itself cost plus the
   plan's worst case.
3. Ranking: per tier in the solver's order, the worst value over the
   sampled outcomes, then the mean; the AI's choice is chance, taken worst
   case over the sample.

The turn-1 boards have no spawn markers and keep the deployment's
environment data (both decided during that phase and hidden). Mission_Final
raises its seven pylon buildings in that phase (`Mission_Final:NextTurn` at
turn count 0, enemy team: state 2, before the spawns and the AI), so the
deployed board has none of them and the turn-1 boards do; the AI aims at
them and the phase's cost counts what happens to them, not their arrival
(live run 24, m40: without them the search planned turn 1 on a board with
no buildings; the drop tiles are fixed by each of the 15 final-island
maps). Budget:
`time_limit` (the screen takes at most half). Cost: a 14-tile zone has 364
sets; 2 seeds each screened in 17 s on 2 threads (about 23 ms per AI phase
and turn-1 end), and a turn-1 solve needs about 0.5 s to find the beam's
plan, so 3 finalist sets (18 placements) x 12 seeds is about 200 solves:
roughly 4 + 13 s on 8 threads.

Offline (the 202 live deployments to 2026-10-13, 201 searchable: the search on
2 threads with 2 finalist sets, 8 seeds and 0.3 s solves; then the chosen
and live_play's heuristic placement re-valued with 16 fresh seeds each):
the chosen placement's worst sampled outcome is better in 121, equal in 27
and worse in 53 (by the search's own ranking: 140 / 22 / 39); the worst grid
loss is smaller in 60 and larger in 15; expected grid loss per deployment
0.57 against 0.81; every sampled outcome keeps all grid in 67 against 50.
Mechs lost hardly change (expected 0.063 against 0.066).

`itb_live deploy <state>` takes a bridge state with the mechs on the board
and its `drop_zone` / `deployment_zone`: `{"cmd": "deploy", "state",
"time_limit", "threads", "screen_samples", "finalists", "samples",
"solve_time", "solve_nodes", "seed", "placements": [[[x, y], ...], ...],
"always": [...]}`, tiles in ascending mech uid order. It returns `mechs`,
`zone`, `sets`, `best` and the ranked `finalists` (each `tiles`, `visual`,
`by_uid`, `screen` and `value`: `worst_case`, `tier_min`, `mean`,
`outcomes`, `seeds`), `solves`, `time`, `timed_out` and `warnings`.
The solve request's ranking options (`threat_tier`, `objectives_first`,
`mech_grid`, `mech_grid_turns`, `mech_grid_pilot`, `lookahead`, ...; CLI
`--threat-tier ... --lookahead`, `live_play.py deploy` passes its own) go
to `DeployOptions::solve`: the turn-1 solves then rank as the live turns do,
the values carry the threat and next-turn tiers, and the phase's own cost
is scored with the same options (mechs folded into the grid slot by the
exchange; no threat, which is turn 1's end board's). Absent, nothing
changes. Measured in "Two-turn measurement" below: worse placements than
the plain search on the sampled live deployments, so it stays off.

### Open questions for live-game testing

Each has a scenario in `LIVE_TEST_PLAN.md`.

- **Vek attack order.** The decompile says board-list order, which is
  insertion order within the player and non-player groups. Recordings made
  before 2026-06-23 only have a bot-computed uid order, and just 3 later
  boards carry the game's real order. Confirm against the live game,
  especially for re-added pawns.
- **Injured and walking.** AE Injured costs 1 HP per tile change. The code
  suggests every step of a walk counts (and undoing the move costs 1 more);
  check in game.
- **Point plus number.** What `Point + number` means natively. It's only
  seen in UI layout code so far.
- **Estimated durations.** Teleport, burrow, walk-step, air-strike and
  dropper animation lengths are estimates (`timing.hpp`); they only shift
  when things happen. Measure them in game.
- **Frame rate.** Some outcomes depend on the frame rate (the executor flags
  them). Check what frame rate and speed level the game really runs at.
  The live game (60 Hz display, settings `no_limit = 0`) matched a step a
  hair below 1/60 on the one recorded tie (run 17 m08 turn 1, above). Real
  frame times jitter, so a timer that is a whole number of frames long can
  still land a frame early in the game; the executor only flags outcomes
  within one frame on its own clock.
- **Mechs standing in fire (Mission_BurnbugBoss).** Recorded mechs on a
  burning tile were not set on fire, although `CheckAcidFire` runs every
  frame for idle occupants. Find what prevents it.
- **Stage 8 objectives.** The bridge could export Mission.BlockedSpawns,
  PowerStart, the Terraform grass zone and Missile_Unit's shots left; until
  then BONUS_BLOCK, BONUS_GRID after turn 1, Terraform and Missiles are
  approximate. That a pod still intact at mission end is recovered is
  assumed, not checked against the game.
- **Pilot level-ups.** Modelled (below). Unconfirmed in game: a dead mech's
  pilot gaining XP (the engine gives it none), an AI pilot record, and the
  Grid skill's +3 Grid Defense taking effect mid-turn (not modelled; only
  chance odds). The bridge's level, XP and max HP are the save's, written at
  turn start; the loader applies a Health / Skilled level-up that HP above
  the saved maximum shows, but a Move / Opener / ... level-up earlier in the
  same turn is invisible to a re-solve. Adrenaline counts only kills the
  engine saw (the battle's earlier kills are not exported).
- **Enemy phase (stage 7).**
   - Spawn types and queue order are hidden; the bridge could forward the
     save's `spawns` / `spawn_ids` / `spawn_points`.
   - Passive upgrades: six recorded turns only match with the Storm
     Generator's +1, which the bridge does not report.
   - The train's queued move is assumed every turn (the bridge omits team-1
     queued shots); Mission_Shields' `ShieldedUnits` is assumed complete.
   - Lightning's realised strike order (spec stage 7 section 4.4 ledger)
     would confirm the order chance node.
   - A pawn killed by `SetMutation` at the quiescent frame (psion death)
     might leave the board idle for one frame (spec stage 7 O1).
   - Nine turns' `post_enemy` summaries disagree with the next turn's board
     on grid power: check when each is captured.
