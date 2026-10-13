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
engine/build/itb_inspect --predict state.json --actions @actions.json --branches   # live validation
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

- **Checkpoints and restore.** At each turn start `turn` copies the game's
  own save files (`profile_Alpha/saveData.lua`, `undoSave.lua`) into the run
  as `m<MM>_turn_<TT>_saveData.lua` / `_undoSave.lua`. `restore` quits the
  game, backs up the profile's current save files
  (`.local_runs/live_play/restore_backups/`), puts the checkpoint back,
  relaunches through the offline Steam client and clicks Continue on the
  title screen (the bridge is silent there), then compares the loaded board
  with that turn's solve input. Continue resumes the run at the start of
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
- **end-turn** predicts the enemy phase from the board as left, sends
  `END_TURN` (it deactivates the mechs, so the "units can still act" dialog
  never appears; on this build the bridge cannot end the turn itself and
  answers `NEEDS_MCP_CLICK`), clicks End Turn (`--end-turn-xy`, default
  fullscreen 1360x768 `128,89`; `--no-click` asks you to), checks the phase
  changed, waits for the next player turn and compares grid, HP, statuses,
  survivors and mech tiles. Vek tiles and webs are not compared (the AI
  moves and webs after the point the engine stops); new units are reported
  as spawns. Outcomes the engine cannot know are notes, not anomalies: a
  Vek that picks up an acid pool moving while the AI plans; Grid Defense
  (the prediction takes every roll as not resisted, its worst case: with
  chance nodes in the prediction, a building it damaged that the game shows
  with more HP, at most its HP before the phase and on a tile with a roll,
  with the grid higher by exactly the HP kept); a Soldier Psion or Psion
  Abomination emerging from a hidden spawn (every Vek of its team +1 HP at
  once). Anything else stops `mission`.
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
- **deploy** uses the bridge's `drop_zone` (refused tiles are already left
  out, and `DEPLOY` refuses them anyway), checks every mech's tile in the
  state, and leaves Confirm to you.
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
budget `max_time` and `min_tiers`. The
simulation follows the plan's default chance outcome (no Grid Defense
resist, first branch), reseeding Lua as the solver does. Boards are compact:
`grid_power`, `buildings` (x, y, hp) and living on-board `units` sorted by
uid (uid, type, x, y, hp, team, mech, fire, acid, frozen, shield, web;
`leader` for a psion: its mutation number).

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
   - Pathing profiles, move budget and pilot move skills.
   - The game's reachability search and its weighted A* walk path (float32,
     (f, x, y) tie-break).
   - Walks, leaps, charges, teleports and burrows. With a stage 2 context,
     Injured goes through the stage 2 health change and the tile a move ends
     on is settled.
   - Every first move the old bot executed in a recording is reachable here:
     `itb_inspect --moves recordings`.
- **Stages 3 and 4: pushes, deaths and the SkillEffect executor**
  (`executor.hpp`, `timing.hpp`).
   - A frame-exact simulation of how the game resolves an effect: the six
     phases of the game's frame, an integer frame clock at a fixed frame rate,
     and every timer replayed in the game's float32 arithmetic. Frames where
     nothing can happen are skipped.
   - `Board::ApplyEffect` and the stacked-effect queue (FULL/PROJ/positive
     delays), projectiles, artillery, lasers, melee lunges, walks, leaps,
     charges, teleports and burrows.
   - Pushes and bumps with the game's same-frame ordering, dying bodies that
     block for half their death animation, death effects (ACID pools, Fast
     Decay, Lua hooks), corpse explosions, psion leaders and body removal.
   - Boost and Vek Hormones are baked in when an effect is computed.
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
     stays the reference): 58 Lua functions (`GetTargetArea` /
     `GetSkillEffect` of Move, repair, the base tank / artillery / laser
     classes, 40-odd squad and Vek weapons and a few mission units): 545
     weapon methods, 99.4% of those calls on the 60-board sample. A port is
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
     floods at once; a dead shield generator drops every shield before the
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
| Mission_Final (Env_Volcano) | rocks: fire artillery DAMAGE_DEATH; lava: tile to lava; super-volcano repaired | exact with `mission_final_volcano`; old: mode and order from the tile pattern (EnvInexact for a single tile) |
| Mission_Final_Cave (Env_Final) | rocks drop (DAMAGE_DEATH, ground), tentacles (DAMAGE_DEATH, lava); bomb re-drop (UpdateMission on an idle board: in the enemy phase not before the status ticks and the environment's steps are done, so a bomb the Psion Tyrant kills comes back after the strikes, struck tiles allowed; a drop on a Vek kills it) | exact with `mission_final_cave`; old: phase from the turn number (EnvInexact), phase-2 order a chance node; bomb drop a chance node `MissionRandom` |
| Mission_AcidStorm | every pawn ACID every frame while the generator lives | exact |
| Mission_Shields | new pawns shielded; generator death drops every shield | assumes every recorded pawn already had its shield (`ShieldedUnits` not recorded) |
| Mission_Hacking | facility death turns the bot into Snowtank1_Player | exact with ids; old recordings infer the bot (EnvInexact if several) |
| Mission_Dam | dam death floods two columns, 0.3 s per row | exact |
| Mission_Train / Armored_Train | train fires after every Vek; wreck replaces a dead train | queued move inferred (bridge omits team-1 shots) |
| Mission_Satellite | queued launch: DAMAGE_DEATH around, rocket flies away; NextTurn powering | exact with `queued_launch` |
| Mission_Volatile | last enemy retreats | exact with `is_infinite_spawn` |
| Mission_Reactivation | NextTurn thaws 2 random frozen enemies | chance node `MissionRandom` over the pairs |
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

1. **Grid and buildings.** Grid power, then structure HP over building
   tiles, both as net changes. Unpopulated and objective buildings count; a
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
   frozen mech, -1 per mech on smoke, -2 per mech at 1 HP; -1 per (enemy,
   adjacent building) pair (-2 for an objective building) and per enemy next
   to a player-team non-mech unit; +1 per enemy on fire or frozen. One pass
   over the pawns. Spawn tiles are left out: a blocked spawn stays queued and
   hurts its blocker again next turn, so occupying one is not a clear gain.

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
board's turn). Printed and JSON scores carry the combined slot as "grid"
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
Leech), objective stars lost <= 0, next-turn threat <= 0 (no tighter cap is
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
