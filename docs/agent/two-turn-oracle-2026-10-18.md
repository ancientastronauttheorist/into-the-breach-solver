# Two-turn oracle on recorded losses (2026-10-18)

**Question.** The Unfair runs 55-57 were lost although every turn was proven
optimal under the live per-turn score and every enemy phase matched the
game. Were those losses forced, or does the per-turn objective (this turn's
grid, buildings, mechs and stars first, then a sampled next-turn estimate)
pick plans that a two-turn view would reject?

**Answer, in short.** Mostly forced. On 49 loss turns of the three lost runs,
42 (86%) were two-turn optimal among the candidates the oracle found, and 7
could be improved. Those 7 save 3.2 grid in expectation (11 in the worst
case) over all 49 turns. They all come from run 56 and run 57, none from
run 55. Run 56 (lost at the Factory, turn 3, on its last grid point) might
have survived with them; runs 55 and 57 would not have. The improvable turns
show one pattern: the next-turn tier is coarse. On 12 of the 15 improvable
turns across all sets, the live estimate rated the better plan equal or
worse, often because of a tie that a lower tier then broke. Only 3 were real
horizon trades, where giving up 1 grid or a star now saves more on the next
turn.

## Method

`scripts/two_turn_oracle.py` (committed; `select`, `run`, `confirm`, `report`;
raw outputs in `recordings/oracle/<tag>/`, git-ignored). For every turn start
T where the live plan lost grid on T or on T+1 (recorded, or in the live
solve's worst case):

1. **Candidates for turn T:**
   - the live plan (the run's `_solve.json`, the plan solved at turn start);
   - six ranking arms re-solved here: the live flags (`live`), no lookahead
     (`base`), 32 samples (`la32`), sum fold (`sum8`), idle reply (`idle8`)
     and plain tiers (`plain`);
   - the best plans of the 3 next-best distinct outcomes under the live
     ranking (new `exclude_outcomes`; the mechs' tiles are not part of an
     outcome);
   - up to 1500 end states sampled from the live arm's search (new
     `end_states`).
2. **Turn T:** each candidate's plain worst case over chance
   (`evaluate_plan`; Grid Defense never resists). The next turn starts from
   its default outcome.
3. **Screen:** the sampled end states (at most 2 per outcome, at most 80)
   and the named candidates, with the next turn judged by the greedy reply
   on 8 screen seeds. The 3 best sampled end states that screen no worse
   than the live plan go on.
4. **Full evaluation:** 8 AI seeds per candidate (`next_turn_start`:
   `Engine::ai_phase` with a sampled seed, hidden spawns as visible types).
   Each next-turn board is solved for real with the live ranking minus its
   lookahead. The lookahead ranks below grid, buildings, mechs and stars, so
   dropping it cannot change those tiers; it would only use budget. The
   adaptive budget runs until grid, buildings and mechs are proven: 94% of
   the 3152 next-turn solves proved them. The two-turn value is grid lost on
   T plus grid lost on T+1, taken as the worst over the seeds, plus the
   mean. A candidate counts only if it loses no more mechs over the two
   turns.
5. **Settled by proof:** if the live solve was proven optimal and its plan
   loses nothing on T+1 in every seed (or T is the mission's last turn),
   no plan beats it (T's grid tier is proven). The turn is settled without
   alternatives: 33 of the 86 turns.
6. **Confirmation (`confirm`):** the alternative was picked as the best of
   about 7 candidates on 8 seeds, so its value there is biased low (the
   winner's curse). The live plan, the pick and every arm or K-best plan
   are judged again on 16 fresh seeds. All verdicts below are confirmed
   verdicts: *avoidable* means a lower two-turn worst case, *better_mean*
   means the same worst case and a mean lower by at least 0.5. Of the 22
   first-pass improvements, 14 held up; one turn the first pass called
   forced turned out improvable.
7. **Validation:** the game's actual T+1 board (the next solve input) is
   solved the same way.

Budgets (6 threads, `nice -n 5`, 6 single-thread `itb_live serve`
processes; live play was paused): turn-T arms 8 s base, up to 24 s until the
first 5 tiers are proven, one thread each. Next-turn solves 4 s base, up to
16 s, min_tiers 3, one thread (mean 3.5-8.8 s). Search reply 200 nodes. The
first pass took about 3 h of wall time for 86 turns, the confirmation about
1.5 h. The arms ran with far less budget than the live solves (6 threads,
30-120 s), so `arm:live` is the live ranking on a small budget.

## Results

| set | turns | settled by proof | two-turn optimal (no loss / forced / last turn) | improvable (confirmed) | grid saved, worst / mean (sum over turns) |
|---|---|---|---|---|---|
| lost runs 55-57 | 49 | 23 | 42 (4 / 27 / 11) | 7 (6 avoidable, 1 better_mean) | 11 / 3.25 |
| control: winning runs 58-59 | 11 | 3 | 9 (0 / 9 / 0) | 2 | 5 / 1.25 |
| optional: runs 52-54 | 26 | 7 | 20 (2 / 14 / 4) | 6 (5 + 1) | 8 / 1.75 |
| all | 86 | 33 | 71 (83%) | 15 (17%) | 24 / 6.25 |

Control runs 58 (`20261018_151438`) m00, m06, m20 and 59 (`20261018_185614`)
m08, m36. Optional runs 52 (`20261018_061335`) and 53 (`20261018_072756`),
all missions; run 54 (`20261018_083852`) m38, m40, m41. The winning runs show
the same picture as the lost ones: the per-turn objective is not worse on
the turns where a run was lost.

Per lost run (confirmed, improvements on overlapping turns not counted twice):

| run | lost at | grid saveable, worst / mean | could it have survived? |
|---|---|---|---|
| 55 (`20261018_121230`) | R.S.T. Volatile t2, grid 1 | 0 / 0 | no: Force, Crack, Lightning and Volatile were forced over two turns (Crack t2: 3 grid in the worst case for every candidate) |
| 56 (`20261018_130738`) | Pinnacle Factory t3, grid 1 (odds 0.11) | 9 / 2.3 | plausibly: BotDefense t1 (2/0.25 → 0/0), BotDefense t2 (5/2.25 → 2/2.0), Factory t1 (3/1.44 → 1/0.25), Volatile t1, JellyBoss t2, Tides t2. One to two grid more at the Factory would most likely have carried the mission. |
| 57 (`20261018_144126`) | Archive ScorpionBoss t4, grid 1 | 0 / 0.69 | no: the only improvement (Train t2) costs a star for 0.69 grid in expectation; the boss mission was entered with 1 grid and its losses were forced |

The improvable turns (confirmed live → alternative, two-turn worst/mean on 16
seeds), with the tier that made the live ranking prefer the live plan:

| turn | live → alt | alt from | live preferred it by | what happened |
|---|---|---|---|---|
| 56 Tides t2/3 | 2/0.62 → 0/0 | la32, sink | mech_hp | estimate tie: both rated 0 next-turn loss |
| 56 JellyBoss t2/4 | 2/0.88 → 1/0.75 | sink | next_turn | estimate misranked (-1025 vs -2114) |
| 56 Volatile t1/4 | 1/0.12 → 0/0 | sink | next_turn | estimate misranked (0 vs -1025) |
| 56 BotDefense t1/4 | 2/0.25 → 0/0 | arm:live, sum8, kbest | threat | estimate tie (0 vs 0) |
| 56 BotDefense t2/4 | 5/2.25 → 2/2.0 | sink | grid | horizon: 1 more grid now, 0 next turn in all 16 seeds |
| 56 Factory t1/4 | 3/1.44 → 1/0.25 | sink | threat | estimate tie (-2050 vs -2050) |
| 57 Train t2/3 | 3/1.75 → 3/1.06 | sink | objectives_failed | horizon: the alternative fails a star |
| 59 BouncerBoss t2/4 | 5/2.75 → 2/2.0 | kbest:2 | grid | horizon: 1 more grid now |
| 59 Civilians t2/4 | 2/0.50 → 0/0 | arm:live, base, sum8, ... | vek_hp | estimate tie |
| 52 Tanks t1/4 | 3/1.25 → 1/1.0 | la32, sum8 | next_turn | estimate misranked |
| 52 Tanks t3/4 | 2/0.31 → 1/0.12 | sum8, idle8, sink | (same score) | estimate tie |
| 53 Satellite t3/4 | 2/1.12 → 1/1.0 | idle8, sink | threat | estimate tie |
| 54 SpiderBoss t3/4 | 2/1.19 → 2/0.62 | la32, sum8 | next_turn | building-HP noise in the estimate |
| 54 Final t1/4 | 4/2.5 → 2/2.0 | arm:live, la32, sum8, idle8 | next_turn | estimate misranked (0 vs -2050) |
| 54 Final t3/4 | 2/0.12 → 0/0 | arm:live, kbest, sink | mech_hp | estimate tie |

So 7 of the 15 are ties in the next-turn tier, where the tier said "equal"
and threat, mech HP or Vek HP decided. Another 5 are misranks, where the
estimate rated the better plan worse. Only 3 are horizon trades above the
tier: two give up 1 grid now (BotDefense t2, BouncerBoss t2) and one gives
up a star (Train t2). In those 3 the live estimate did see the coming loss
(-3075, i.e. 3 grid next turn), but the strict order puts this turn's grid
first. Their worst-case gain is 3 grid each, their mean gain only 0.25-0.75.

**Ranking arms on the confirmation seeds** (53 confirmed turns, sums of the
two-turn worst / mean):

| arm | worst | mean |
|---|---|---|
| recorded live plan (6 threads, 30-120 s) | 120 | 69.9 |
| `sum8` (lookahead, sum of 8 samples), small budget | 120 | 69.9 |
| `live` flags, small budget | 120 | 74.7 |
| `la32` (worst of 32) | 126 | 75.3 |
| `idle8` (idle reply) | 119 | 75.8 |
| `base` (no lookahead) | 139 | 82.6 |
| `plain` tiers | 137 | 82.4 |

At the same small budget, the lookahead is worth about 8 grid of mean over
53 turns (`live` against `base`). The sum fold adds another 4.7 (`sum8`
against `live`) and matches the big-budget live plan. More samples (worst of
32) do not help.

**How accurate the cheap next-turn estimates are**, on the same next-turn
boards as the real solve (3152 boards):

- The greedy reply equals the solve's grid loss on 78% of boards. It
  overestimates on 19% and underestimates on 3%, and its total is 2589
  against 1949 grid (+33%). Taking the worst over a candidate's 8 seeds,
  it equals the solve on 66%.
- The lookahead's Search reply (200 nodes) equals the solve on 86%, with a
  total of +11%.
- The live solve's own estimate (worst of its 8 greedy samples) against the
  oracle's worst next turn on 8 other seeds, solved: equal on 41 of 71
  turns, lower on 23, higher on 7 (sums 55 against 77 grid). Sample
  variance outweighs the greedy reply's pessimism: 8 seeds miss bad AI
  plans.
- The game's actual next turn fell inside the live plan's sampled range on
  61 of 70 turns and above it on 9. Seven of those 9 are missions whose
  environment adds next-turn hazards (Airstrike twice, Lightning, Crack,
  Tides, ForestFire, Mines); the other two are Tanks. The sampled next turn
  plans only the Vek (`next_turn_start` runs `Engine::ai_phase`), not the
  environment's next-turn hazards.

## Conclusions

- **Forced, mostly.** 83% of the loss turns, and 86% in the lost runs, were
  already two-turn optimal. Deepening the horizon would buy at most
  0.07 grid per loss turn in expectation, plus some worst-case insurance:
  the 3 horizon trades gain 3 grid each in the worst case but at most 0.75
  in the mean. Two of the three lost runs were not saveable with a
  two-turn view.
- **The cheap win is the next-turn tier itself, not a deeper horizon.**
  Twelve of 15 improvements are ties or misranks of the 8-sample worst-case
  greedy estimate.

## Recommended next step

1. **Make the next-turn tier discriminative.** Fold the samples as worst,
   then sum: lexicographic, so ties in the worst case break by the mean
   (`sum8` already matched the big-budget live plan at a fraction of the
   budget). Use the Search reply, or a better greedy that reaches its 86%
   agreement, instead of the greedy reply's 78%. These are cheap and target
   12 of 15 improvable turns. Then re-measure with `two_turn_oracle.py`, for
   example with an arm for the new fold.
2. **Model the environment in `next_turn_start`.** Plan the mission's
   next-turn hazards (airstrikes, lightning, cracks, tides) as the game
   does, so the estimate stops seeing those turns as safe.
3. **Leave the grid-before-next-turn order alone for now.** The 3 horizon
   trades are worth 0.25-0.75 grid in the mean. If anything, try a bounded
   exchange analogous to `mech_grid`: accept 1 grid now when the next-turn
   estimate's worst case saves at least 2.
4. **Otherwise, effort goes to search speed and the strategy layer**
   (deployment, squad and mission choice, run-level grid economy). Most loss
   turns had no better plan within two turns: the boards were lost before.

## Reproduce

```bash
python3 scripts/two_turn_oracle.py select --run 20261018_121230
python3 scripts/two_turn_oracle.py run --run 20261018_121230 --run 20261018_130738 --run 20261018_144126 --tag lost --workers 5
python3 scripts/two_turn_oracle.py confirm --tag lost --workers 6
python3 scripts/two_turn_oracle.py report --tag lost --tag control --tag optional
```

(`--root` points at the live recordings when they are not in this checkout;
`ITB_GAME_DIR` at the game scripts.) Caveats:

- The candidate set is not exhaustive. "Forced" means no plan the oracle
  found does better, and the search's own sample (`end_states`) is biased
  toward plans near its incumbent.
- Next-turn solves are budget-limited (94% proved grid, buildings and
  mechs).
- Hidden spawns are sampled as visible types, and the environment's next
  turn is not planned.
- In the first pass, outcome keys crossed `itb_live` processes before
  `hash_board_stable` existed, so the K-best chain sometimes returned an
  outcome already found. That only reduced diversity, and the confirmation
  pass used the fixed keys.

**Follow-up (2026-10-19):** recommendation 1 was measured in
`next-turn-fold-2026-10-19.md`. On the same 53 confirmed turns the
worst-then-sum fold lowers the two-turn mean by 3.2-4.6 grid against the
worst fold, at both budgets tried, and never changed this turn's tiers on
272 turn starts. It is now live_play's default (`--lookahead-fold
worst_sum`). The Search reply stays opt-in: per board it is more accurate,
but each sample costs 4x as much.

**Follow-up (2026-10-19):** recommendation 2 is in
`next-turn-env-2026-10-19.md`. With `lookahead_env`, the actual next turn
falls inside the sampled range on 63 of 70 turns (10 of the 13
environment-mission turns), and the misses left are AI-seed and
hidden-spawn sampling. Its effect on the ranking is within noise, so it
stays opt-in.
