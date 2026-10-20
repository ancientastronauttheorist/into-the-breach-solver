# Into the Breach solver

A rules engine for [Into the Breach](https://subsetgames.com/itb.html),
and a solver built on it that **proves** it has found the best possible turn.

The project began as an autonomous achievement bot, which earned all 70 Steam
achievements by July 2026. It then went further: it reconstructed the game's
exact rules by reading the game itself, then built a search that returns the
optimal plan for a turn, or says how far from optimal its best plan might be.
The same solver now drives the live bot.

## Results

Measured on 2026-10-09 against the bot's recorded games (469 boards) and live
play:

| | |
|---|---|
| Recorded attacks, where the old simulator was right | **98.3%** match the game (91.9% exactly, the rest are labelled recording artefacts) |
| Recorded moves | **98.8%** match (92.6% exactly) |
| Full turns (player actions and the whole enemy phase) | **99.5%** accounted for (68% exact; the rest explained, mostly by Vek movement after their turn) |
| Steps where the old simulator **failed** | the engine reproduces the game **66.9%** of the time |
| Optimality proofs | **76.7%** of sampled boards proven optimal within 2 minutes on 8 threads (median 13 s); every 1–2-unit board |
| Against the old bot's plans | never worse wherever the search finished; strictly better on 362 of 469 boards |
| Live, two missions incl. a boss | every player action matched the engine's prediction once the loadout was known |

Since then (2026-10-20), playing whole runs on **Unfair** with Advanced
Edition, shop and reward choices priced by the solver itself
(`scripts/loadout_eval.py`):

| | |
|---|---|
| Unfair runs | **22 wins in 54**; 5 of the last 6; runs 58 and 63 finished with the grid full (7/7) |
| Coverage | every weapon, upgrade, passive, pilot and pilot skill a run can hold was played live or in live fuzz sessions; 58/58 reachable missions, 92/93 reachable enemy types |
| Recorded whole turns replayed through the enemy phase | **99.6%** of 223 match the game exactly or with a labelled recording artefact |
| Loss analysis (two-turn oracle, 86 loss turns) | 83% were already the best play over two turns; the rest led to the worst-then-sum next-turn estimate now live |

## How it works

```
game ──Lua bridge──▶ board state ──▶ engine ──▶ solver ──▶ plan ──bridge──▶ game
                                       │
             the game's own weapon Lua ┘  (run in an embedded Lua 5.1)
```

- **Rules from the game itself.** The behaviour was read from the unstripped
  Linux build (14,800 named C++ functions in Ghidra) and the shipped Lua.
  The engine reimplements it in original C++.
- **The real weapon scripts.** An embedded Lua 5.1 host runs the game's own
  `GetTargetArea` and `GetSkillEffect` for every weapon (559 ids, Advanced
  Edition included) against the engine's board. It reproduces the scripting
  library's quirks, down to a byte-exact clone of the game's random number
  generator.
- **Frame-exact resolution.** Pushes, projectiles, death animations and
  delays gate each other in real time in the game. The engine replays the
  game's frame clock with its float32 arithmetic, skips empty frames, and
  flags outcomes that depend on frame rate.
- **The whole enemy phase:** status ticks, environments (air strikes, tides,
  cataclysm, volcano and the rest), telegraphed Vek attacks re-aimed after
  pushes, and emerging spawns.
- **Strict scoring.** A turn is ranked lexicographically: grid power,
  buildings, mechs lost, mech HP, mission objectives (weighted by the game's
  own reward values), kills, position.
- **Proof-carrying search.** Every interleaving of moves and attacks is
  covered (a mech can move, let another act, then fire). A transposition
  table merges identical boards, chance (Grid Defense, Lightning order, death
  splits) is taken worst case, and pruning uses only bounds argued sound. The
  result is proven optimal when the search completes, or best-so-far with an
  honest gap. It is cross-checked against brute force on small boards.

Details, specs and open questions: [`engine/README.md`](engine/README.md).

## Quick start

Requirements: macOS (the only platform tested so far), CMake ≥ 3.24, Ninja, a C++20 compiler, and
your own copy of the game. The engine reads the game's scripts from your
install, via `ITB_GAME_DIR` or the Steam default.

```bash
cmake -S engine -B engine/build -G Ninja -DITB_BUILD_PYTHON=ON -DPython_EXECUTABLE="$(command -v python3)"
cmake --build engine/build
engine/build/itb_tests

# Solve a recorded board and show the plan, its score and proof status
engine/build/itb_inspect --solve recordings/20260713_052159_731/m07_turn_01_solve_input.json --time 30

# Check the engine against what really happened in recorded games
engine/build/itb_inspect --replay recordings --turns
```

Live play: install the bridge with `scripts/install_modloader.sh`, restart
the game, and run `python3 game_loop.py auto_turn` each turn. See
[`CLAUDE.md`](CLAUDE.md) and [`docs/agent/live-runbook.md`](docs/agent/live-runbook.md).

## Repository layout

| Path | What |
|---|---|
| `engine/` | C++ rules engine, Lua weapon host, frame-exact executor, enemy phase, scoring, solver, tools and tests |
| `engine/python/` | Python bindings used by the live bot |
| `src/`, `game_loop.py` | The live bot: Lua bridge IPC, game loop, per-action verification, strategy |
| `src/bridge/modloader.lua` | The in-game bridge |
| `rust_solver/` | The original Rust solver, now the fallback |
| `recordings/` | Boards, plans and outcomes from past runs (the validation corpus) |
| `docs/agent/` | Live-bot manuals |
| `docs/archive/` | Achievement-era documents |

## Clean room

This repository contains no game files and no decompiled code. The engine is
original code written from behavioural specs; game data (unit definitions,
weapon scripts) is read at runtime from a local install. Decompiler
workspaces stay in a git-ignored folder.

## History

- **April–July 2026: achievement bot.** A Lua bridge, an empirically tuned
  Rust simulator and agent-driven UI play earned all 70 achievements (the
  last on 2026-07-09). Its story, including the Lightning War speedrun, is in
  [`docs/archive/achievement-era/`](docs/archive/achievement-era/).
- **July–October 2026: first decompile attempt.** An earlier agent-driven
  reverse-engineering effort on the Windows build is archived on the
  `archive/codex-decompile` branch.
- **October 2026: the engine.** A clean restart on the unstripped Linux
  build, from first spec to a proven-optimal solver driving the live bot.
