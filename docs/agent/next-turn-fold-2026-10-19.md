# Next-turn tier: worst-then-sum fold and the Search reply (2026-10-19)

Follow-up to `two-turn-oracle-2026-10-18.md`, whose recommendation #1 was to
make the NextTurn tier more discriminative. That study found 12 of 15
improvable loss turns to be ties or misranks of the tier (the worst of 8
greedy-reply samples).

## What changed

- **New fold `worst_sum`** (`LookaheadOptions::Fold::WorstSum`; itb_live and
  Python `"lookahead_fold": "worst_sum"`, `itb_inspect --solve
  --lookahead-fold worst_sum`, `live_play.py --lookahead-fold worst_sum`,
  two-turn arm `wsN`). The worst sample ranks first, and ties are broken by
  the sum over the samples. It is still one int32 tier,
  `-(2^17 x W + min(S, 2^17 - 1))`. Why one value and not a new tier:
  - It never overflows. W is at most 16383, and 16383 x 2^17 + 2^17 - 1 =
    2^31 - 1.
  - The order is exactly lexicographic for up to 8 samples. With more
    samples, a very bad sum saturates (ties among those sums), but the
    worst sample still ranks first.
  - The solver's reasoning needs only a fold that starts at 0 and that each
    sample can only lower, and this fold is one. So the cap stays 0, which
    keeps the lazy evaluation and `TierBounds` sound. `lookahead_floor`
    stays a true floor. The per-sample cut stays sound: once the value so
    far is <= alpha, the remaining samples can only lower it. The sample
    cache also stays sound, because the fold depends only on the set of
    sample values.
  - `--min-tiers` counts do not change. A separate tier would have shifted
    them.

  The tiny-board test against the naive enumeration now covers all three
  folds and the Search reply. A new unit test checks that the fold is
  monotone, independent of sample order, bounded by its floor and
  lexicographic.
- **Search reply exposed:** `lookahead_reply: "search"` plus
  `lookahead_search_nodes` (default 200) in itb_live, Python, itb_inspect
  and live_play.
- **Results are unchanged with the options off.** `itb_tests` passes (652
  cases). `itb_inspect --corpus/--moves/--weapons/--replay/--replay
  --turns` on `recordings/` gives byte-identical output to the pre-change
  build. The allowed Python test modules pass.
- **`two_turn_oracle.py` gains new commands:**
  - `arms` (`--arms-dir`): new ranking arms on the confirmed turns. Each arm
    solves turn T, and its plan is judged on the same 16 confirmation seeds.
    A plan whose end board was already judged reuses that result.
  - `arms-report`: the table for those arms.
  - `tiers` / `tiers-report`: the regression check on this turn's tiers.

## Ranking arms on the 53 confirmed turns

Same turns, same 16 confirmation seeds and same next-turn solves as the
first study's "ranking arms" table. The numbers are sums over the turns of
the two-turn grid loss: worst over the seeds / mean. Mechs is the worst-case
number of mechs lost over the two turns. "vs live" counts turns better /
worse than the recorded live plan. `_s2` means the solver's lookahead seed
11 instead of 7. Turn-T solves ran at the first study's budget (1 thread,
8 s base, up to 24 s, min_tiers 5) and at 3x that budget (24-72 s).

| arm | 1x worst / mean | 1x mechs | 1x vs live | 3x worst / mean | 3x mechs | 3x vs live |
|---|---|---|---|---|---|---|
| recorded live plan (6 threads, 30-120 s, worst fold) | 120 / 69.88 | 15 | - | | | |
| worst fold (`live_r`, first pass 120 / 74.66) | 121 / 75.28 | 14 | 9 / 12 | 120 / 73.91 | 16 | 7 / 9 |
| worst fold, seed 11 | 126 / 77.00 | 13 | 19 / 23 | | | |
| **worst_sum, 8 samples (`ws8`)** | 119 / 72.06 | 16 | 14 / 17 | 116 / 69.31 | 14 | 15 / 12 |
| worst_sum, seed 11 | 122 / 72.57 | 13 | 18 / 21 | | | |
| worst_sum, 16 samples | 120 / 71.00 | 12 | 22 / 17 | 112 / 67.44 | 14 | 22 / 13 |
| sum, 8 samples (first pass 120 / 69.94) | 120 / 70.81 | 14 | 17 / 18 | 113 / 68.38 | 12 | 19 / 14 |
| sum, seed 11 | 121 / 71.82 | 10 | 17 / 21 | | | |
| sum, 16 samples | 120 / 70.51 | 14 | 23 / 16 | | | |
| worst, Search reply (`srch8`) | 126 / 75.78 | 16 | 13 / 19 | 120 / 71.60 | 16 | 12 / 16 |
| worst_sum, Search reply (`wssr8`) | 129 / 73.41 | 16 | 15 / 21 | 118 / 67.85 | 14 | 21 / 13 |
| sum, Search reply | 130 / 73.22 | 16 | 17 / 21 | | | |
| no lookahead (`base`, first pass) | 139 / 82.62 | 19 | 11 / 26 | | | |

Head to head, `ws8` against the worst fold:
- 1x: better on 12 turns, worse on 7, mean -3.2.
- 3x: better on 14, worse on 5, mean -4.6.

How far one arm moves between two solver seeds, or between two solves:
- the worst fold: 75.3 → 77.0;
- `ws8`: 72.1 → 72.6;
- `sum8`: 70.8 → 71.8, and 69.9 in the first pass.

The fold effect is larger than that noise at both budgets and both seeds:
the worst fold scores 73.9-77.0, the sum folds 68.4-72.6. Sum and worst_sum
are within noise of each other: head to head, `sum8` against `ws8` is 7 / 7
at 1x and 7 / 5 at 3x.

**On the first study's 15 improvable turns**, the number of turns where each
arm beats the recorded live plan, at 1x / 3x:

| arm | 1x | 3x |
|---|---|---|
| worst fold | 5 | 5 |
| `ws8` | 8 | 9 |
| `sum8` | 10 | 11 |
| `ws16` | 11 | 13 |
| `wssr8` | 7 | 10 |

The three horizon trades (BotDefense t2, BouncerBoss t2, Train t2) stay
unfixed by any fold, as expected: they rank above the tier.

## The Search reply

- **Accuracy on 800 of the oracle's solved next-turn boards** (same boards,
  reproduced exactly), by the share of boards whose grid loss equals the
  real solve's:

  | reply | equal | time per board |
  |---|---|---|
  | greedy | 80.9% | 36 ms |
  | Search, 50 nodes | 85.4% | 99 ms |
  | Search, 100 nodes | 85.6% | 111 ms |
  | Search, 200 nodes | 85.9% | 132 ms |
  | Search, 400 nodes | 86.2% | 172 ms |

  The node budget barely matters. Most of the cost is the solver's own
  enemy phases (child ordering, the beam): even at 1 node it costs about
  2.6x the greedy reply on a profiled board.
- **Cost inside a solve:** 78 ms per sample against 18 ms for greedy
  (4.2x). At equal budget the search scores about a third as many samples
  (1x: 4.3-5.2k against 13-16k over the 53 turns).
- **Effect:** at 1x the Search reply is worse than greedy under every fold
  (fewer end boards get an estimate). At 3x it starts to pay: `wssr8` 67.85
  against `ws8` 69.31. Doubling the greedy samples (`ws16`, 67.44) does as
  well at the same cost, with a better worst case (112 against 118). The
  live budget (6 threads, 30-120 s) is about 20-30x the 1x budget, so the
  Search reply might pay more there. That is not measured; `ws16` is the
  cheaper bet on the same trend.

## Regression: this turn's tiers

Setup: 272 turn starts, every turn start of runs 55-59 (`20261018_121230`,
`130738`, `144126`, `151438`, `185614`). Each was solved with the live flags
(worst fold) and with `ws8` and `ws16` (1 thread, 8-24 s, min_tiers 7, as
live).

Results:
- **Grid survival, grid, buildings, mechs and stars:** worse on 0 turns and
  better on 0, for both arms. So nothing above NextTurn ever got worse.
  This holds by construction too: the fold only reorders plans within the
  NextTurn tier, and the base search's seeding keeps the higher tiers.
- **Same plan:** `ws8` 238 of 272 turns, `ws16` 172.
- **NextTurn:** the worst sample is worse on 3 turns with `ws8`
  (budget). `ws16` reports a worse worst sample on 54 turns; that compares
  16 seeds against 8, so it is expected.

`recordings/oracle/regress/tiers/` (git-ignored) has the per-turn data.

## Recommendation

- **Live default: `--lookahead-fold worst_sum`** (8 samples, greedy
  reply). The data supports it clearly:
  - its mean is 3.2-4.6 grid lower than the worst fold over 53 loss turns,
    at both budgets and both solver seeds;
  - its two-turn worst case is the same or better;
  - this turn's tiers were never worse on 272 turns;
  - it costs nothing extra.

  It keeps the worst-case doctrine (the worst sample still ranks first),
  which `sum` would give up for no measurable gain. Changed in code:
  - `scripts/live_play.py`: `--lookahead-fold` now defaults to `worst_sum`,
    so every solve and the deploy ranking with `--lookahead` send it;
    `--lookahead-fold worst` restores the old ranking;
  - `scripts/loadout_eval.py`: `RANKING` mirrors it.

  The engine's own default (`LookaheadOptions::fold`) stays `worst`, so
  itb_inspect and the existing studies are unchanged.
- **Next trial, not a default: `--lookahead-samples 16`** with worst_sum. It
  was the best arm at both budgets (71.0 and 67.4; worst 112 at 3x), but its
  edge over `ws8` (1.1-1.9 of mean) is within the seed-to-seed noise, and it
  doubles the sample cost.
- **Search reply: keep it opt-in.** It is more accurate per board, 86%
  against 81%, but costs 4x per sample. It loses at 1x and only matches
  more greedy samples at 3x. Worth re-measuring at the real live budget, or
  after making it cheaper: most of its cost is the solver's own
  ordering/beam phases, not the node budget.
- The first study's other recommendations stand: plan the environment's
  next-turn hazards in `next_turn_start`, and leave the horizon trades
  alone for now.

## Reproduce

```bash
O=/path/to/recordings/oracle; L=/path/to/recordings/live
python3 scripts/two_turn_oracle.py arms --tag lost --tag control --tag optional --out-root $O --root $L \
    --workers 6 --arms live_r,sum8_r,ws8,srch8,wssr8,sumsr8,ws16,sum16,live_s2,sum8_s2,ws8_s2
python3 scripts/two_turn_oracle.py arms --arms-dir arms3x --time 24 --max-time 72 --tag lost --tag control \
    --tag optional --out-root $O --root $L --workers 6 --arms live_r,sum8_r,ws8,wssr8,ws16,srch8
python3 scripts/two_turn_oracle.py arms-report [--arms-dir arms3x] --tag lost --tag control --tag optional --out-root $O
python3 scripts/two_turn_oracle.py tiers --all-turns --run 20261018_121230 --run 20261018_130738 \
    --run 20261018_144126 --run 20261018_151438 --run 20261018_185614 --tag regress --min-tiers 7 \
    --out-root $O --root $L --workers 6 --arms live,ws8,ws16
python3 scripts/two_turn_oracle.py tiers-report --tag regress --out-root $O
```

Wall time on 6 single-thread `itb_live serve` processes (`nice -n 5`):
- the 1x arms: about 30 min per batch;
- the 3x arms: about 70 min;
- the regression: about 45 min.

The CPU budget was 6 threads throughout, with live play paused.
