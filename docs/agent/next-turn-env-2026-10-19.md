# Next-turn tier: the environment's next-turn hazards (2026-10-19)

Follow-up to `two-turn-oracle-2026-10-18.md` (recommendation 2) and
`next-turn-fold-2026-10-19.md`. The oracle found that the game's actual
next turn was worse than every sampled next turn on 9 of 70 turns, 7 of
them on missions with an environment. The sampled next turn only ran the
Vek AI (`Engine::ai_phase`). Its enemy phase ran with this turn's mission
data, so the environment struck this turn's tiles again: the same airstrike
cross, the same tide row, seismic tiles that were already chasms. So those
boards looked safer than they are.

## What changed

- **`LookaheadOptions::plan_env`** (off by default in the engine).
  - itb_live / Python: `"lookahead_env": true`.
  - `itb_inspect --solve --lookahead-env`, `itb_live solve --lookahead-env`.
  - `live_play.py --lookahead-env` (default off).
  - two_turn: `"lookahead_env": true` plans the hazards for each sampled
    next turn and lists them as `env_hazard`.
- **`plan_next_environment`** (environment.hpp, environments.cpp). It plays
  the environment's planning step. In the game that step runs in state 2 of
  the enemy phase, after the spawns and before the AI moves
  (`Mission:PlanEnvironment`). It runs on the end board and returns the
  mission data the bridge would record at the start of the next turn:

  | environment | next turn's hazard (from the shipped Lua) | random / hidden |
  |---|---|---|
  | Env_Airstrike | a centre in x 3..5, y 1..6 whose cross has no building or pod (water allowed) | centre sampled |
  | Env_Lightning | one tile per quarter, no building, pod or water | tiles sampled |
  | Env_Seismic | the next 3 tiles of the path | the path is hidden: its far end is sampled among the edge endpoints (2..5 on each edge) that every marked step got closer to, and the walk continues from the last mark (closest neighbour, +0.5 on rows 0/7, ties sampled) |
  | Env_Cataclysm | the next column to the left, without chasms and buildings | none |
  | Env_Tides / Env_Terratide | `Index` + 1 (row; the tide spares buildings and the tiles they shadow) | none |
  | Env_RandomWind | direction, plus two columns in 1..5 that are not this turn's | sampled |
  | Env_SnowStorm | the 3x3 block of a centre that is not this turn's | centre sampled; the unused centres are hidden |

  Env_Volcano, Env_Final (cave) and Env_Belt keep their data. Belt has
  nothing to plan; the other two are not modelled yet.
- **Only player-visible input.** The plan reads the board (terrain, pods,
  buildings, chasms) and this turn's marks. Random picks come from a
  splitmix stream seeded by the sample's AI seed (`lookahead_seed(options,
  k)`). Each sample is one board the game could produce, sampled the same
  way as the AI seeds. Hidden instance state is sampled rather than read:
  - the seismic path past the marks;
  - the ice storm's remaining centres.

  The bridge's `mission_state.instance` dump is never read.
- **Reply.** The next turn's enemy phase runs with the planned data
  (`next_turn_mission` → `TurnContext::mission`). So idle, greedy, the
  Search reply and real solves all see the hazards resolve. The greedy
  reply also adds the hazard tiles to its relevant tiles: a unit standing on
  one gets move-off turns, and a Vek next to one can be pushed onto it.
- **Results are unchanged with the option off:**
  - `itb_tests` passes (656 cases, 4 new: the planners against the Lua
    rules, the seismic continuation, seeding, and lookahead integration);
  - `itb_inspect --corpus/--moves/--weapons/--replay/--replay --turns` on
    `recordings/` are byte-identical to the pre-change build;
  - the allowed Python modules pass (267 tests).
- **`two_turn_oracle.py` changes:**
  - `--judge-env`: sampled next turns are judged with the planned hazards;
  - `--kind env|other|all`;
  - arms `ws8e`, `ws8e_s2`, `ws16e`;
  - `validate` / `validate-report`: the actual next turn against the
    re-judged range;
  - `arms-turns`: two-turn arms on any turn start, no first-pass file
    needed.

## (a) Does the actual next turn now fall inside the sampled range?

Setup: the live plan's 8 first-pass seeds (seed 101), each next-turn board
solved as in the first pass (4-16 s, min_tiers 3). The game's actual next
turn is the first pass's own solve of it.

| set | turns | first pass below / inside / above | with planned hazards | re-judged without (noise check) |
|---|---|---|---|---|
| env missions (Airstrike 3, Crack 4, Lightning 2, Tides 2, Wind 2) | 13 | 0 / 8 / 5 | **0 / 10 / 3** | 0 / 8 / 5, identical samples |
| other missions | 57 | 0 / 53 / 4 | unchanged (the option does not touch them) | |
| all 70 | 70 | 0 / 61 / 9 | **0 / 63 / 7** | |

- **Moved inside:** Lightning t1 of run 55 (actual 1; samples were all 0,
  now one sample loses 1) and Airstrike t3 of run 57 (actual 1; samples were
  all 0, now 4 of 8 lose 1).
- **The planned tiles match the game's:**
  - Tides: the exact row on every sample;
  - Crack: the actual three tiles on 3 or 4 of 8 samples;
  - Wind: the actual columns on 4 of 8 samples;
  - Airstrike: the actual centre on 2 of 8 samples.
- **Still above range**, and the cause is not the environment:
  - Crack t1, run 55, and Airstrike t1, run 57: the Vek's actual plan
    (positions and targets) is not among the 8 AI seeds;
  - Tides t2, run 53: two hidden spawns emerged as Moth2, while the samples
    give them a visible type (Firefly).
- **Not environment hazards:** Mines (the mines are items placed at the
  start), ForestFire (no planning step) and Tanks.
- **The estimate is less optimistic:** the sum of the env turns' mean
  sampled next-turn loss went from 3.25 to 3.62 grid (actual: 12).

## Ranking: does planning the hazards pick better plans?

Two sets, judged over two turns on 16 fresh seeds (confirmation seed 301).
The next turn is judged with the planned hazards, which is the better
ground truth for these missions. The same judge applies to every arm and
to the recorded live plan. Turn-T solves ran at the first pass's budget
(1 thread, 8-24 s, min_tiers 5). Arms:
- `ws8`: the current live default, worst_sum fold, 8 samples, greedy reply;
- `ws8e`: `ws8` with `lookahead_env`;
- `_s2`: the solver's lookahead seed 11 instead of 7.

The numbers are sums of the two-turn grid loss, worst / mean.

**The 8 confirmed loss turns on env missions** (of the 53 confirmed turns;
the option does nothing on the other 45):

| arm | worst / mean | mechs | lookahead samples |
|---|---|---|---|
| recorded live plan | 21 / 14.31 | 2 | |
| ws8 | 21 / 14.69 | 2 | 7645 |
| ws8e | 19 / 14.75 | 1 | 7092 |
| ws8_s2 | 25 / 16.56 | 3 | |
| ws8e_s2 | 26 / 17.38 | 2 | |

**All 40 env-mission turn starts of runs 55-59.** These are a broader
sample than the loss turns: Airstrike 8, Wind 8, Terratide 9, Crack 4,
Lightning 4, SnowStorm 4, Tides 3.

| arm | worst / mean | mechs | better / worse than the recorded plan | lookahead samples |
|---|---|---|---|---|
| recorded live plan | 35 / 18.25 | 5 | | |
| ws8 | 37 / 17.88 | 4 | 6 / 4 | 4997 |
| **ws8e** | **30 / 17.38** | 4 | 9 / 5 | 3724 |
| ws8_s2 | 34 / 19.00 | 6 | 9 / 6 | 5714 |
| ws8e_s2 | 37 / 18.81 | 5 | 9 / 7 | 5281 |

Head to head on the 40 turn starts:
- seed 7: `ws8e` against `ws8` is better on 7 turns and worse on 3, mean
  -0.50. The same end board on 27 of 40.
- seed 11: `ws8e_s2` against `ws8_s2` is better on 3 and worse on 5, mean
  -0.19.
- noise: switching only the solver's seed moves the mean by +1.12 (`ws8`)
  and +1.44 (`ws8e`).

**Reading.** The mean is slightly lower with planned hazards at both seeds
(0.5 and 0.2 grid over 40 turns). The worst case goes opposite ways: -7 at
seed 7, +3 at seed 11. Both effects are smaller than the seed-to-seed
noise, so the gain cannot be told apart from noise. Two reasons:
- these missions rarely lose grid to the environment;
- its hazards strike units, not buildings, so mechs are what they
  threaten.

The cost is in the samples. At the same budget the env arm scored 25%
fewer samples at seed 7 and 8% fewer at seed 11, because every greedy
reply now resolves a real environment step.

## (b) This turn's tiers: regression check

Turn starts of runs 55-59, solved by `ws8` and `ws8e` (1 thread, 8-24 s,
min_tiers 7, as live):

| set | turns | same plan | grid survival / grid / buildings / mechs / stars worse | better |
|---|---|---|---|---|
| env missions (all) | 40 | 28 | **0** | 0 |
| other missions (every 8th) | 28 | 28 | 0 | 0 |

Nothing above NextTurn got worse, and that holds by construction too: the
option only changes the NextTurn estimate, and the base search's seeding
keeps the tiers above it. On the control set the plans are identical, so
the option is a no-op off env missions.

## Recommendation

- **Keep `--lookahead-env` opt-in.** Don't make it the live_play default
  yet:
  - the estimate is better calibrated (the actual next turn is inside the
    sampled range on 10 of 13 env turns, against 8, and on 63 of 70
    overall, against 61);
  - it never hurts this turn's tiers;
  - but on 48 env-mission turns its effect on the plans is within the
    seed-to-seed noise: the mean is -0.5 and -0.2 grid at two seeds, and
    the worst case moves both ways;
  - it costs 8-25% of the lookahead samples.

  The data does not clearly support a change. It is a cheap thing to turn
  on for an Airstrike / Lightning / Crack / Wind mission, where a mech
  standing in next turn's strike is the risk.
- **Next, by expected value:**
  1. AI-seed variance and hidden spawn types explain the remaining
     misses: 3 of the env misses plus Mines, ForestFire and the Tanks
     turns. The fold study's next trial (16 samples) targets the first.
  2. A better hidden-spawn model (the island's spawn pool instead of the
     visible types) would fix Tides t2 of run 53.
  3. Env_Volcano / Env_Final planning is still missing (2-3 turns per run
     at most).
- The engine default stays off. `live_play.py --lookahead-env` and itb_live
  `lookahead_env` are there for trials.

## Skipped (2-thread budget)

- The 45 non-env confirmed turns. The option is a no-op there: 28 of 28
  control turn starts gave identical plans, and the validation re-judge
  without env reproduced the first pass exactly.
- The 3x-budget arms and `ws16e` (defined, not run).
- 204 of the 232 non-env turn starts of runs 55-59 (every 8th was run as
  the control).
- `itb_inspect --lookahead-study` and `--two-turn` do not take the flag
  (itb_live's `two_turn` does).

## Reproduce

```bash
O=/path/to/recordings/oracle; L=/path/to/recordings/live; W="--out-root $O --root $L --workers 2"
python3 scripts/two_turn_oracle.py validate --tag lost --tag control --tag optional --kind env --judge-env \
    --validate-dir validate_env $W
python3 scripts/two_turn_oracle.py validate-report --tag lost --tag control --tag optional --validate-dir validate_env --out-root $O
python3 scripts/two_turn_oracle.py arms --tag lost --tag optional --kind env --judge-env --arms-dir arms_env \
    --arms ws8,ws8e,ws8_s2,ws8e_s2 $W
R="--run 20261018_121230 --run 20261018_130738 --run 20261018_144126 --run 20261018_151438 --run 20261018_185614"
python3 scripts/two_turn_oracle.py arms-turns --all-turns $R --kind env --tag regress_env --judge-env \
    --arms-dir arms_env --arms ws8,ws8e,ws8_s2,ws8e_s2 $W
python3 scripts/two_turn_oracle.py arms-report --tag regress_env --arms-dir arms_env --out-root $O
python3 scripts/two_turn_oracle.py tiers --all-turns $R --kind env --tag regress_env --min-tiers 7 --arms ws8,ws8e $W
python3 scripts/two_turn_oracle.py tiers --all-turns $R --kind other --every 8 --tag regress_env_ctl --min-tiers 7 \
    --arms ws8,ws8e $W
python3 scripts/two_turn_oracle.py tiers-report --tag regress_env --base-arm ws8 --out-root $O
```

Wall time on 2 single-thread `itb_live serve` processes (`nice -n 5`; live
play was running on the other 4 threads):

| run | wall time (about) |
|---|---|
| validation (both passes) | 10 min |
| arms on the 8 confirmed turns | 30 min |
| tiers (68 turn starts) | 25 min |
| arms-turns (40 turn starts) | 95 min |

Raw data, git-ignored, under `recordings/oracle/`:
- `{lost,control,optional}/validate_env`, `validate_plain`;
- `{lost,optional}/arms_env`;
- `regress_env/{tiers,arms_env}`;
- `regress_env_ctl/tiers`.
