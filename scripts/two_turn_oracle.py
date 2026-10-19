#!/usr/bin/env python3
"""Two-turn oracle: was a recorded turn's plan the best over two turns?

    python3 scripts/two_turn_oracle.py select --run 20261018_121230
    python3 scripts/two_turn_oracle.py run --run 20261018_121230 --workers 2 --tag lost
    python3 scripts/two_turn_oracle.py run --run 20261018_151438 --mission 4 --tag control
    python3 scripts/two_turn_oracle.py confirm --tag lost --workers 2
    python3 scripts/two_turn_oracle.py report --tag lost --tag control
    python3 scripts/two_turn_oracle.py arms --tag lost --arms ws8,wssr8 --workers 6
    python3 scripts/two_turn_oracle.py arms-report --tag lost
    python3 scripts/two_turn_oracle.py tiers --all-turns --run 20261018_121230 --tag regress --arms live,ws8
    python3 scripts/two_turn_oracle.py tiers-report --tag regress

Findings of the first study: docs/agent/two-turn-oracle-2026-10-18.md;
`arms` (new ranking arms judged on the confirmation seeds) and `tiers`
(this turn's tiers under each arm, the regression check):
docs/agent/next-turn-fold-2026-10-19.md.

The live solver ranks each turn by its own score (grid, buildings, mechs,
stars, a sampled next-turn estimate, ...; README "Turn score"). This tool
asks, for the turn starts of recorded runs where grid was lost (on that
turn or the next), whether another plan would have lost less grid over
that turn and the next one, as far as the engine can tell:

1. Candidates for turn T: the plan the live solve chose (the run's
   `_solve.json`), the plans of other ranking arms (ARMS: the live flags
   re-solved here, without the lookahead, with 32 samples, the sum fold,
   the idle reply, plain tiers), the best plans of the K best distinct
   outcomes under the live ranking (itb_live `exclude_outcomes`: each solve
   excludes the outcomes found so far; the mechs' tiles are not part of an
   outcome), and end states sampled from the live arm's search (itb_live
   `end_states`), one or two per outcome.
2. Every candidate is played through its enemy phase (default chance
   outcome: Grid Defense never resists). Its "now" loss is its plain worst
   case over chance (evaluate_plan).
3. Screen: the next turn's AI plans with --screen-seeds sampled seeds
   (lookahead.hpp next_turn_start; hidden spawns as visible types), and the
   lookahead's greedy reply plays each next turn. The candidates whose
   screened two-turn grid loss is lowest go on (--finalists).
4. Full: the live plan, the arms, the K best outcomes and the finalists get
   --seeds other AI seeds; each next-turn board is solved for real (the
   live ranking without its lookahead, which ranks below grid, buildings,
   mechs and stars and so cannot change them; --next-time / --next-max-time
   with min_tiers 3: grid, buildings and mechs proven or the budget spent)
   and its plan's plain worst case is the next turn's loss. The greedy reply
   and the lookahead's Search reply (--search-nodes) run on the same boards
   (how well the cheap estimates track a real solve).
5. Two-turn value of a candidate: grid lost on T + grid lost on T+1,
   worst over the seeds (and the mean). On the mission's last turn there
   is no T+1, and the live plan's own proof (grid tiers) settles the turn;
   so does a proven live plan whose next turn loses nothing in any seed.
   The live plan is evaluated first, and nothing else runs on such turns.
6. Confirmation (`confirm`): the best alternative was picked as the best of
   several on the first seeds, so its value there is biased low (the
   winner's curse). `confirm` judges the live plan, that pick and the arms'
   and K-best plans again on --confirm-seeds fresh seeds and stores them in
   the turn's file; `report` gives the confirmed verdicts and how each
   source of plans fares there.
7. Validation: the game's actual turn T+1 (the next solve input) is
   judged the same way, to see where it falls among the live plan's
   sampled next turns.

`run` writes one JSON per turn under recordings/oracle/<tag>/ (git-ignored)
and skips turns already there; `report` reads them and prints the tables
(markdown): per turn the live plan's two-turn worst / mean against the best
alternative's, the verdict (2-turn optimal in the candidate set, forced or
avoidable), the tier by which the live ranking preferred the live plan,
and the aggregates (grid saveable, greedy accuracy).

Read-only on the recordings; never touches the game or the bridge. The
engine runs as `nice -n 5 itb_live serve --threads 1`, --workers processes
(CPU budget: one thread each).
"""

from __future__ import annotations

import argparse
import concurrent.futures as cf
import json
import queue
import re
import statistics
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
ITB_LIVE = REPO / "engine" / "build" / "itb_live"
LIVE_ROOT = REPO / "recordings" / "live"
OUT_ROOT = REPO / "recordings" / "oracle"

# The live driver's ranking (live_play.py mission flags of runs 50-59).
RANK = dict(threat_tier=True, objectives_first=True, mech_grid=2, mech_grid_turns=True, mech_grid_pilot=True,
            lookahead=True, grid_survival=True, survival_cap=True, survival_cap_share=0.3)
NO_LOOKAHEAD = {k: v for k, v in RANK.items() if k != "lookahead"}
# Candidate arms: ranking options of a turn-T solve.
ARMS = {
    "live": RANK,
    "base": NO_LOOKAHEAD,
    "la32": dict(RANK, lookahead_samples=32),
    "sum8": dict(RANK, lookahead_fold="sum"),
    "idle8": dict(RANK, lookahead_reply="idle"),
    "plain": {},
    # Second study (docs/agent/next-turn-fold-2026-10-19.md): the worst
    # sample with ties broken by the sum, and the Search reply.
    "ws8": dict(RANK, lookahead_fold="worst_sum"),
    "srch8": dict(RANK, lookahead_reply="search"),
    "wssr8": dict(RANK, lookahead_fold="worst_sum", lookahead_reply="search"),
    "sumsr8": dict(RANK, lookahead_fold="sum", lookahead_reply="search"),
    "ws16": dict(RANK, lookahead_fold="worst_sum", lookahead_samples=16),
    "sum16": dict(RANK, lookahead_fold="sum", lookahead_samples=16),
    # The same arms with other lookahead sample seeds (the estimate's own
    # sampling noise; the solver's default seed is 7).
    "live_s2": dict(RANK, lookahead_seed=11),
    "sum8_s2": dict(RANK, lookahead_fold="sum", lookahead_seed=11),
    "ws8_s2": dict(RANK, lookahead_fold="worst_sum", lookahead_seed=11),
}
# `arms` re-solves these under their own name: the first pass's arms again,
# to measure the budget noise between two solves of one arm.
ARM_ALIASES = {"live_r": "live", "sum8_r": "sum8"}
# The ranking tiers in order (itb_live score names), for "which tier
# decided".
RANKED = ("grid_survival", "grid", "building_hp", "mechs", "objectives_failed", "next_turn", "threat", "mech_hp",
          "objectives", "kills", "vek_hp", "position")


class OracleError(RuntimeError):
    pass


# ---------------------------------------------------------------- engine

class Serve:
    """One `itb_live serve --threads 1` process."""

    def __init__(self, itb_live: Path, game: str | None, nice: int, log):
        cmd = (["nice", "-n", str(nice)] if nice else []) + [str(itb_live)] + \
              (["--game", game] if game else []) + ["serve", "--threads", "1"]
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, text=True,
                                     bufsize=1)
        ready = self._read()
        if not ready.get("ready"):
            raise OracleError(f"itb_live did not start: {ready}")

    def _read(self) -> dict:
        line = self.proc.stdout.readline()
        if not line:
            raise OracleError("itb_live exited")
        return json.loads(line)

    def request(self, req: dict) -> dict:
        self.proc.stdin.write(json.dumps(req) + "\n")
        self.proc.stdin.flush()
        reply = self._read()
        if not reply.get("ok"):
            raise OracleError(f"itb_live {req.get('cmd')}: {reply.get('error')}")
        return reply

    def close(self) -> None:
        try:
            self.proc.stdin.write(json.dumps({"cmd": "quit"}) + "\n")
            self.proc.stdin.flush()
            self.proc.wait(timeout=10)
        except Exception:  # noqa: BLE001
            self.proc.kill()


class Pool:
    """`workers` engine processes; map() runs requests on them in parallel."""

    def __init__(self, workers: int, itb_live: Path, game: str | None, nice: int, log_path: Path):
        self.log = open(log_path, "a")
        self.free: queue.Queue = queue.Queue()
        self.all = [Serve(itb_live, game, nice, self.log) for _ in range(workers)]
        for s in self.all:
            self.free.put(s)
        self.exe = cf.ThreadPoolExecutor(max_workers=workers)

    def _one(self, req: dict) -> dict:
        s = self.free.get()
        try:
            return s.request(req)
        finally:
            self.free.put(s)

    def map(self, reqs: list) -> list:
        return list(self.exe.map(self._one, reqs))

    def one(self, req: dict) -> dict:
        return self._one(req)

    def close(self) -> None:
        for s in self.all:
            s.close()
        self.exe.shutdown()
        self.log.close()


# ---------------------------------------------------------------- recordings

def bridge_state(path: Path) -> dict:
    d = json.loads(path.read_text())
    d = d.get("data", d)
    return d.get("bridge_state", d)


@dataclass
class TurnStart:
    run: str
    mission: int
    turn: int
    total: int
    mission_id: str
    state: Path
    grid: int
    solve: dict | None          # the run's _solve.json data
    next_state: Path | None     # turn T+1's solve input (same mission)
    next_grid: int | None       # grid at T+1's start, or after the last turn's enemy phase
    next_solve: dict | None

    @property
    def key(self) -> str:
        return f"{self.run}_m{self.mission:02d}_t{self.turn:02d}"

    @property
    def last(self) -> bool:
        return self.turn >= self.total

    def recorded_loss(self) -> int | None:
        return None if self.next_grid is None else self.grid - self.next_grid

    def predicted_grid_loss(self) -> int | None:
        """The live solve's worst case, plain grid (mech_grid folds 2 x grid +
        5 x mechs into the slot; the run-lost penalty 2^20)."""
        if not self.solve or not self.solve.get("worst_case"):
            return None
        wc = self.solve["worst_case"]
        slot = -int(wc.get("grid") or 0)
        if slot >= 1 << 20:
            slot -= 1 << 20
        mechs = -int(wc.get("mechs") or 0)
        return max(0, (slot - 5 * mechs) // 2) if "mechs" in wc else slot


def turn_starts(root: Path, run: str) -> list:
    d = root / run
    by = {}
    for f in d.glob("m*_turn_*_solve_input.json"):
        m = re.match(r"m(\d+)_turn_(\d+)_solve_input\.json$", f.name)
        if m:
            by[(int(m.group(1)), int(m.group(2)))] = f
    out = []
    for (mm, tt), f in sorted(by.items()):
        st = bridge_state(f)
        nxt = by.get((mm, tt + 1))
        next_grid = None
        if nxt is not None:
            next_grid = bridge_state(nxt).get("grid_power")
        else:
            after = sorted(d.glob(f"m{mm + 1:02d}_turn_{tt:02d}_after_enemy_*.json")) + \
                    sorted(d.glob(f"m{mm:02d}_turn_{tt + 1:02d}_after_enemy_*.json"))
            if after:
                next_grid = bridge_state(after[0]).get("grid_power")
        sf = d / f"m{mm:02d}_turn_{tt:02d}_solve.json"
        nsf = d / f"m{mm:02d}_turn_{tt + 1:02d}_solve.json"
        out.append(TurnStart(run, mm, tt, int(st.get("total_turns") or 0), str(st.get("mission_id")), f,
                             int(st.get("grid_power") or 0),
                             json.loads(sf.read_text())["data"] if sf.exists() else None, nxt, next_grid,
                             json.loads(nsf.read_text())["data"] if nsf.exists() else None))
    return out


def select_turns(starts: list, missions: set | None = None) -> list:
    """Turn starts where the live plan's turn lost grid, or the next turn
    did (recorded, or in the live solve's worst case)."""
    by = {(t.mission, t.turn): t for t in starts}
    out = []
    for t in starts:
        if missions is not None and t.mission not in missions:
            continue
        if not t.solve or not t.solve.get("engine_plan"):
            continue
        nxt = by.get((t.mission, t.turn + 1))

        def lost(x):
            return x is not None and (((x.recorded_loss() or 0) > 0) or ((x.predicted_grid_loss() or 0) > 0))
        if lost(t) or (not t.last and lost(nxt)):
            out.append(t)
    return out


# ---------------------------------------------------------------- the oracle

def plan_text(plan: list) -> str:
    return "; ".join(a.get("description", "?") for a in plan)


def two(now_grid: int, samples: list, field: str = "solve") -> dict:
    """Two-turn grid value of a candidate from its next-turn samples."""
    g, m = [], []
    for s in samples:
        loss = s[field]["plain"] if field == "solve" else s[field]
        if loss is None:
            continue
        g.append(loss["grid"])
        m.append(loss["mechs"])
    if not g:
        return {"worst": now_grid, "mean": float(now_grid), "next_worst": 0, "next_mean": 0.0, "mechs_worst": 0,
                "n": 0}
    return {"worst": now_grid + max(g), "mean": now_grid + statistics.mean(g), "next_worst": max(g),
            "next_mean": statistics.mean(g), "mechs_worst": max(m), "n": len(g)}


def first_diff(a: dict, b: dict) -> str | None:
    """The first ranked tier where scores a and b differ."""
    for k in RANKED:
        if k in a and k in b and a[k] != b[k]:
            return k
    return None


def next_request(args) -> dict:
    """A next-turn solve: the live ranking without its lookahead (it ranks
    below grid, buildings, mechs and stars, so it cannot change them; it
    would only take budget), the adaptive budget proving grid, buildings
    and mechs (min_tiers 3)."""
    return dict(time_limit=args.next_time, max_time=args.next_max_time, min_tiers=3, **NO_LOOKAHEAD)


def oracle_turn(pool: Pool, ts: TurnStart, args) -> dict:
    t0 = time.time()
    state = str(ts.state)
    out = {"key": ts.key, "run": ts.run, "mission": ts.mission, "turn": ts.turn, "total_turns": ts.total,
           "mission_id": ts.mission_id, "grid": ts.grid, "recorded_loss": ts.recorded_loss(),
           "live_worst_case": ts.solve.get("worst_case"), "live_proven": ts.solve.get("proven_optimal"),
           "partial_re_solve": bool(ts.solve.get("partial_re_solve")), "budget": vars_budget(args)}
    cands = []
    uniq = {}

    def evaluate(new: list) -> list:
        """Turn T of each new candidate (no samples), merged into `uniq` by
        end board. Returns the candidates that were new boards."""
        evals = []
        for i in range(0, len(new), 200):
            part = new[i:i + 200]
            evals += pool.one(dict(cmd="two_turn", state=state, plans=[c["plan"] for c in part], samples=0,
                                   greedy=False))["results"]
        fresh = []
        for c, e in zip(new, evals):
            if not e.get("ok"):
                continue
            h = e["end_hash"]
            if h in uniq:
                for src in c["sources"]:
                    if src not in uniq[h]["sources"]:
                        uniq[h]["sources"].append(src)
                continue
            c.update(end_hash=h, outcome_key=e["outcome_key"], now=e["now"], mission_over=e["mission_over"])
            uniq[h] = c
            cands.append(c)
            fresh.append(c)
        return fresh

    def full(cs: list) -> None:
        """The next turn on args.seeds AI seeds, each board solved."""
        jobs = [(c, k) for c in cs for k in range(args.seeds)]
        reqs = [dict(cmd="two_turn", state=state, plan=c["plan"], samples=[k], seed=args.seed, greedy=True,
                     search_nodes=args.search_nodes, next=next_request(args)) for c, k in jobs]
        for (c, k), rep in zip(jobs, pool.map(reqs)):
            c.setdefault("samples", []).extend(rep["results"][0]["samples"])
        for c in cs:
            c["samples"].sort(key=lambda x: x["k"])
            c["two"] = two(c["now"]["grid"], c["samples"])
            c["two_greedy"] = two(c["now"]["grid"], c["samples"], field="greedy")

    # The live plan first.
    live = {"sources": ["live"], "plan": ts.solve["engine_plan"]}
    if not evaluate([live]):
        raise OracleError("the live plan does not replay")
    no_next = ts.last or live["mission_over"]
    if no_next:
        live["two"] = two(live["now"]["grid"], [])
    else:
        full([live])
    # Settled by the live solve's own proof: its turn-T grid tier is optimal,
    # so with no next turn, or no next-turn loss in any seed, nothing beats it.
    if ts.solve.get("proven_optimal") and live["two"]["worst"] == live["now"]["grid"]:
        out["verdict"] = "last_turn" if no_next else ("optimal" if live["two"]["worst"] == 0 else "forced")
        out["settled_by_proof"] = True
        out["live"] = {"now": live["now"], "two": live["two"]}
        out["candidates"] = [{k: v for k, v in live.items() if k != "plan"} | {"plan": plan_text(live["plan"])}]
        add_actual(pool, ts, args, out)
        out["time_s"] = time.time() - t0
        return out
    # 1. Arms (the live arm also samples end states) and, alongside, the K
    # best outcomes under the live ranking (each solve excludes the live
    # plan's outcome and those found so far).
    budget = dict(time_limit=args.time, max_time=args.max_time, min_tiers=args.min_tiers, threads=1)
    kbest = []
    chain_cands = []

    def chain() -> None:
        excluded = [live["outcome_key"]]
        for k in range(args.kbest):
            rep = pool.one(dict(cmd="solve", state=state, exclude_outcomes=excluded, **budget, **RANK))
            if rep.get("outcome_key") in excluded or not rep.get("plan"):
                break
            excluded.append(rep["outcome_key"])
            kbest.append({"worst_case": rep.get("worst_case"), "proven_optimal": rep.get("proven_optimal"),
                          "time_s": rep["stats"]["time_s"]})
            chain_cands.append({"sources": [f"kbest:{k + 1}"], "plan": rep["plan"]})

    th = threading.Thread(target=chain)
    th.start()
    reqs = []
    for arm in args.arms:
        r = dict(cmd="solve", state=state, **budget, **ARMS[arm])
        if arm == "live" and args.end_states > 0:
            r["end_states"] = args.end_states
        reqs.append(r)
    replies = pool.map(reqs)
    th.join()
    arm_info, new, sink = {}, [], []
    for arm, rep in zip(args.arms, replies):
        arm_info[arm] = {"worst_case": rep.get("worst_case"), "proven_optimal": rep.get("proven_optimal"),
                         "proven_components": rep.get("proven_components"), "time_s": rep["stats"]["time_s"],
                         "outcome_key": rep.get("outcome_key")}
        new.append({"sources": ["arm:" + arm], "plan": rep["plan"]})
        for i, e in enumerate(rep.get("end_states", [])):
            sink.append({"sources": ["sink"], "plan": e["plan"], "sink_rank": i})
    out["arms"] = arm_info
    out["kbest"] = kbest
    evaluate(new + chain_cands)
    sink = evaluate(sink)
    out["candidates_total"] = len(cands)
    # 2. Screen: at most per_outcome sampled end states per outcome, by the
    # greedy reply on screen seeds (not the full evaluation's).
    named = [c for c in cands if c is not live and any(s != "sink" for s in c["sources"])]
    per = {}
    pool_s = []
    for c in sorted(sink, key=lambda c: c.get("sink_rank", 0)):
        n = per.get(c["outcome_key"], 0)
        if n < args.per_outcome and len(pool_s) < args.screen_max:
            per[c["outcome_key"]] = n + 1
            pool_s.append(c)
    screened = [live] + named + pool_s
    step = max(1, -(-len(screened) // (2 * len(pool.all))))
    reqs = [dict(cmd="two_turn", state=state, plans=[c["plan"] for c in screened[i:i + step]],
                 samples=args.screen_seeds, seed=args.screen_seed, greedy=True)
            for i in range(0, len(screened), step)]
    res = [r for rep in pool.map(reqs) for r in rep["results"]]
    for c, r in zip(screened, res):
        c["screen"] = two(c["now"]["grid"], r["samples"], field="greedy")
    # 3. Full evaluation of the named candidates and the best screened end
    # states, except those whose turn T alone loses at least the live plan's
    # two-turn worst (they cannot do better in the worst case nor the mean).
    # Sampled end states go on only if the screen rates them no worse than
    # the live plan (the same greedy reply on the same seeds).
    ls = live["screen"]
    top = sorted((c for c in pool_s if (c["screen"]["worst"], c["screen"]["mean"]) <= (ls["worst"], ls["mean"])),
                 key=lambda c: (c["screen"]["worst"], c["screen"]["mean"], c["now"]["mechs"]))
    rest = named + top[:args.finalists]
    limit = live["two"]["worst"]
    pruned = [c for c in rest if c["now"]["grid"] >= limit]
    rest = [c for c in rest if c["now"]["grid"] < limit]
    full(rest)
    out["pruned"] = len(pruned)
    # 4. The live ranking's scores of the fully judged candidates.
    judged = [live] + rest
    rep = pool.one(dict(cmd="two_turn", state=state, plans=[c["plan"] for c in judged], samples=0, greedy=False,
                        score=True, **RANK))
    for c, r in zip(judged, rep["results"]):
        c["score"] = r.get("score")
    add_actual(pool, ts, args, out)
    out["candidates"] = [{k: v for k, v in c.items() if k != "plan"} | {"plan": plan_text(c["plan"])} |
                         ({"actions": c["plan"]} if "two" in c or "sink" not in c["sources"] else {})
                         for c in cands if "two" in c or "screen" in c]
    out.update(verdict_of(live, rest))
    out["time_s"] = time.time() - t0
    return out


def add_actual(pool: Pool, ts: TurnStart, args, out: dict) -> None:
    """The game's actual next turn, judged as the sampled ones are."""
    if ts.next_state is None or ts.last:
        return
    rep = pool.one(dict(cmd="two_turn", state=str(ts.next_state), start_only=True, greedy=True,
                        search_nodes=args.search_nodes, next=next_request(args)))
    out["actual_next"] = rep["start"]
    out["actual_next_grid_start"] = ts.next_grid


# A lower two-turn mean (same worst case) counts as better only from this
# much grid: with 8 seeds a mean moves by 1/8 per seed.
MEAN_MARGIN = 0.5


def summary(c: dict) -> dict:
    return {"sources": c["sources"], "now": c["now"], "two": c["two"], "plan": plan_text(c["plan"]),
            "score": c.get("score")}


def verdict_of(live: dict, rest: list) -> dict:
    """Live plan against the best alternative over two turns (grid, worst
    over the seeds, then the mean). An alternative counts only if it loses
    no more mechs (now + worst next) than the live plan ("best_any": the
    best one regardless of mechs).

    avoidable: an alternative's two-turn worst is lower; better_mean: the
    same worst, a mean lower by MEAN_MARGIN or more; otherwise the live plan
    is two-turn optimal within the candidates: "optimal" (no grid lost in
    two turns) or "forced"."""
    def mechs(c):
        return c["now"]["mechs"] + c["two"]["mechs_worst"]

    def key(c):
        return (c["two"]["worst"], c["two"]["mean"])

    lv = live["two"]
    judged = [c for c in rest if "two" in c]
    ok = [c for c in judged if mechs(c) <= mechs(live)]
    best = min(ok, key=key, default=None)
    best_any = min(judged, key=key, default=None)
    out = {"live": {"now": live["now"], "two": lv, "score": live.get("score"), "mechs_two": mechs(live)},
           "best": None, "best_alt": summary(best) if best else None,
           "best_any": summary(best_any) | {"mechs_two": mechs(best_any)} if best_any else None}
    if best is not None and best["two"]["worst"] < lv["worst"]:
        out["verdict"] = "avoidable"
    elif best is not None and best["two"]["worst"] == lv["worst"] and lv["mean"] - best["two"]["mean"] >= MEAN_MARGIN:
        out["verdict"] = "better_mean"
    else:
        out["verdict"] = "optimal" if lv["worst"] == 0 else "forced"
        return out
    out["best"] = summary(best)
    if live.get("score") and best.get("score"):
        out["decided_by"] = first_diff(live["score"], best["score"])
    return out


def vars_budget(args) -> dict:
    return {k: getattr(args, k) for k in ("time", "max_time", "min_tiers", "kbest", "end_states", "screen_seeds",
                                          "per_outcome", "screen_max", "finalists", "seeds", "next_time",
                                          "next_max_time", "search_nodes")}


def parse_plan_text(text: str, units: dict) -> list:
    """A plan back from its description ("JudoMech#0 move G6, fire
    Prime_Shift at G5; ..."; solver.cpp describe_action). `units`: uid ->
    (x, y) at the turn start, for a repair's default target (where the mech
    stands)."""
    def tile(name: str) -> list:
        return [8 - int(name[1]), ord("H") - ord(name[0])]

    plan, pos = [], dict(units)
    for part in [p.strip() for p in text.split(";") if p.strip()]:
        m = re.match(r"\S+#(\d+)(?: move ([A-H][1-8]))?(?:,? (.*))?$", part)
        if not m:
            raise OracleError(f"cannot parse plan step '{part}'")
        uid = int(m.group(1))
        a = {"uid": uid, "move": tile(m.group(2)) if m.group(2) else None, "kind": "none", "weapon": "",
             "target": None, "target2": None}
        if a["move"]:
            pos[uid] = a["move"]
        rest = (m.group(3) or "").strip()
        w = re.match(r"fire (\S+) at ([A-H][1-8])(?: then ([A-H][1-8]))?$", rest)
        r = re.match(r"repair(?: \((\S+)\))?(?: at ([A-H][1-8]))?$", rest)
        if w:
            a.update(kind="weapon", weapon=w.group(1), target=tile(w.group(2)),
                     target2=tile(w.group(3)) if w.group(3) else None)
        elif r:
            a.update(kind="repair", weapon=r.group(1) or "",
                     target=tile(r.group(2)) if r.group(2) else list(pos[uid]))
        elif rest not in ("", "(no action)"):
            raise OracleError(f"cannot parse plan step '{part}'")
        plan.append(a)
    return plan


def confirm_picks(r: dict, root: Path) -> tuple:
    """The turn's state file and the plans `confirm` judges, in order: the
    live plan, the arms' and K-best plans and the best alternatives, as
    (candidate, actions)."""
    state = root / r["run"] / f"m{r['mission']:02d}_turn_{r['turn']:02d}_solve_input.json"
    units = {u["uid"]: [u["x"], u["y"]] for u in bridge_state(state).get("units", [])}
    solve = json.loads((root / r["run"] / f"m{r['mission']:02d}_turn_{r['turn']:02d}_solve.json").read_text())["data"]
    picks = []
    for c in r["candidates"]:
        named = [s for s in c["sources"] if s == "live" or s.startswith("arm:") or s.startswith("kbest:")]
        is_best = any(r.get(k) and r[k]["plan"] == c["plan"] for k in ("best", "best_alt", "best_any"))
        if named or is_best:
            plan = solve["engine_plan"] if "live" in c["sources"] else (c.get("actions") or parse_plan_text(c["plan"], units))
            picks.append((c, plan))
    return state, picks


def confirm_turn(pool: Pool, path: Path, root: Path, args) -> dict | None:
    """Re-judges a turn's live plan, its best alternative and the arms' and
    K-best plans on --confirm-seeds fresh AI seeds (the full evaluation's
    budget): the alternative was picked as the best of several on the first
    seeds, so its value there is biased low (the winner's curse)."""
    r = json.loads(path.read_text())
    if r.get("settled_by_proof") or r["verdict"] == "last_turn" or "confirm" in r:
        return None
    state, picks = confirm_picks(r, root)
    reqs = [dict(cmd="two_turn", state=str(state), plans=[p for _, p in picks], samples=0, greedy=False,
                 describe=True)]
    check = pool.map(reqs)[0]["results"]
    def norm(text: str) -> str:
        # A repair's " at <tile>" depends on the board it was described on
        # (the turn start, or the board after the mech's move).
        # A unit created during the turn has no type on the turn-start board.
        text = re.sub(r"(repair(?: \(\S+\))?) at [A-H][1-8]", r"\1", text)
        return re.sub(r"\S+#(\d+)", r"#\1", text)

    for (c, _), e in zip(picks, check):
        if norm("; ".join(e.get("plan", []))) != norm(c["plan"]) or e.get("now") != c["now"]:
            raise OracleError(f"{path.name}: the parsed plan '{c['plan']}' does not replay as recorded")
    ks = list(range(args.confirm_seeds))
    jobs = [(c, p, k) for c, p in picks for k in ks]
    out = {"seeds": args.confirm_seeds, "seed": args.confirm_seed, "plans": []}
    reps = pool.map([dict(cmd="two_turn", state=str(state), plan=p, samples=[k], seed=args.confirm_seed, greedy=True,
                          next=next_request(args)) for _, p, k in jobs])
    per = {}
    for (c, _, _), rep in zip(jobs, reps):
        per.setdefault(c["end_hash"], []).extend(rep["results"][0]["samples"])
    for c, _ in picks:
        out["plans"].append({"sources": c["sources"], "plan": c["plan"], "end_hash": c["end_hash"],
                             "now": c["now"], "two": two(c["now"]["grid"], per[c["end_hash"]]),
                             "next": [s["solve"]["plain"]["grid"] if (s.get("solve") or {}).get("plain") else None
                                      for s in sorted(per[c["end_hash"]], key=lambda s: s["k"])]})
    r["confirm"] = out
    path.write_text(json.dumps(r, indent=1))
    return r


def confirmed_verdict(r: dict) -> str | None:
    """The verdict on the confirmation seeds: the turn's best alternative
    (as picked on the first seeds) against the live plan."""
    cf_ = r.get("confirm")
    if not cf_:
        return None
    pick = r.get("best") or r.get("best_alt")
    live = next(p for p in cf_["plans"] if "live" in p["sources"])
    alt = next((p for p in cf_["plans"] if pick and p["plan"] == pick["plan"] and p is not live), None)
    if alt is None:
        return "forced" if live["two"]["worst"] > 0 else "optimal"
    lw, aw = live["two"], alt["two"]
    mechs = live["now"]["mechs"] + lw["mechs_worst"] < alt["now"]["mechs"] + aw["mechs_worst"]
    if not mechs and aw["worst"] < lw["worst"]:
        return "avoidable"
    if not mechs and aw["worst"] == lw["worst"] and lw["mean"] - aw["mean"] >= MEAN_MARGIN:
        return "better_mean"
    return "forced" if lw["worst"] > 0 else "optimal"


# ---------------------------------------------------------------- arms

def arm_options(name: str) -> dict:
    return ARMS[ARM_ALIASES.get(name, name)]


def stable_hashes(pool: Pool, state: Path, plans: list) -> list:
    """Each plan's end board hash (hash_board_stable) and turn-T loss, from
    this build (the first pass's hashes were not stable across processes)."""
    out = []
    for i in range(0, len(plans), 200):
        out += pool.one(dict(cmd="two_turn", state=str(state), plans=plans[i:i + 200], samples=0,
                             greedy=False))["results"]
    return out


def arms_turn(pool: Pool, path: Path, root: Path, out_dir: Path, args) -> dict | None:
    """New ranking arms on a confirmed turn: each arm solves turn T with the
    first pass's budget, and its plan is judged on the confirmation seeds
    (reusing the confirmation of a plan with the same end board, and the
    judgement of an earlier arm's). Writes out_dir/<key>.json."""
    r = json.loads(path.read_text())
    if not r.get("confirm"):
        return None
    f = out_dir / path.name
    res = json.loads(f.read_text()) if f.exists() else {"key": r["key"], "arms": {}, "judged": {}}
    todo = [a for a in args.arms if a not in res["arms"]]
    if not todo:
        return res
    state, picks = confirm_picks(r, root)
    conf = r["confirm"]["plans"]
    if len(conf) != len(picks):
        raise OracleError(f"{path.name}: the confirmation's plans do not match the candidates")
    judged = res["judged"]  # end hash -> {"now", "two", "next", "from"}
    if not res.get("confirm_hashes"):
        hs = stable_hashes(pool, state, [p for _, p in picks])
        res["confirm_hashes"] = []
        for (c, _), cp, e in zip(picks, conf, hs):
            if not e.get("ok") or e["now"] != cp["now"]:
                raise OracleError(f"{path.name}: the confirmed plan '{cp['plan']}' does not replay as recorded")
            res["confirm_hashes"].append(e["end_hash"])
            judged.setdefault(e["end_hash"], {"now": cp["now"], "two": cp["two"], "next": cp.get("next"),
                                              "from": "confirm:" + ",".join(cp["sources"])})
    budget = dict(time_limit=args.time, max_time=args.max_time, min_tiers=args.min_tiers, threads=1)
    replies = pool.map([dict(cmd="solve", state=str(state), **budget, **arm_options(a)) for a in todo])
    plans = [rep.get("plan") or [] for rep in replies]
    evals = stable_hashes(pool, state, plans)
    fresh = {}
    for a, rep, plan, e in zip(todo, replies, plans, evals):
        if not e.get("ok"):
            raise OracleError(f"{path.name}: arm {a}'s plan does not replay")
        res["arms"][a] = {"end_hash": e["end_hash"], "now": e["now"], "worst_case": rep.get("worst_case"),
                          "proven_optimal": rep.get("proven_optimal"),
                          "proven_components": rep.get("proven_components"), "time_s": rep["stats"]["time_s"],
                          "lookahead_evals": rep["stats"].get("lookahead_evals"),
                          "lookahead_s": rep["stats"].get("lookahead_s"), "plan": plan_text(plan)}
        if e["end_hash"] not in judged and not e["mission_over"] and not (r["turn"] >= r["total_turns"]):
            fresh.setdefault(e["end_hash"], (plan, e["now"]))
        elif e["end_hash"] not in judged:
            judged[e["end_hash"]] = {"now": e["now"], "two": two(e["now"]["grid"], []), "next": [], "from": "no_next"}
    jobs = [(h, plan, k) for h, (plan, _) in fresh.items() for k in range(r["confirm"]["seeds"])]
    reps = pool.map([dict(cmd="two_turn", state=str(state), plan=plan, samples=[k], seed=r["confirm"]["seed"],
                          greedy=True, next=next_request(args)) for _, plan, k in jobs])
    per = {}
    for (h, _, _), rep in zip(jobs, reps):
        per.setdefault(h, []).extend(rep["results"][0]["samples"])
    for h, (_, now) in fresh.items():
        ss = sorted(per[h], key=lambda s: s["k"])
        judged[h] = {"now": now, "two": two(now["grid"], ss), "from": "arms",
                     "next": [s["solve"]["plain"]["grid"] if (s.get("solve") or {}).get("plain") else None for s in ss]}
    res["judged"] = judged
    f.write_text(json.dumps(res, indent=1))
    return res


def arms_report(tags: list, out_root: Path, arms_dir: str = "arms") -> str:
    """Per arm, over the turns every listed arm was judged on: the sums of
    the two-turn worst / mean on the confirmation seeds (as the first
    study's "ranking arms" table), mechs lost over the two turns (worst),
    turns better / worse than the recorded live plan, and the search cost."""
    rows = []
    for tag in tags:
        for f in sorted((out_root / tag / arms_dir).glob("*.json")):
            a = json.loads(f.read_text())
            r = json.loads((out_root / tag / f.name).read_text())
            rows.append((tag, r, a))
    names = sorted({n for _, _, a in rows for n in a["arms"]})
    first = ["live", "base", "la32", "sum8", "idle8", "plain"]
    agg = {}

    def add(name, two_, now, live_two):
        x = agg.setdefault(name, {"worst": 0, "mean": 0.0, "mechs": 0, "n": 0, "better": 0, "worse": 0})
        x["worst"] += two_["worst"]
        x["mean"] += two_["mean"]
        x["mechs"] += now["mechs"] + two_["mechs_worst"]
        x["n"] += 1
        k, lk = (two_["worst"], round(two_["mean"], 3)), (live_two["worst"], round(live_two["mean"], 3))
        x["better"] += k < lk
        x["worse"] += k > lk

    cost = {}
    for tag, r, a in rows:
        conf = r["confirm"]["plans"]
        live = next(p for p in conf if "live" in p["sources"])
        add("recorded live", live["two"], live["now"], live["two"])
        for arm in first:
            p = next((p for p in conf if "arm:" + arm in p["sources"]), None)
            if p:
                add("arm:" + arm + " (first pass)", p["two"], p["now"], live["two"])
        for name in names:
            info = a["arms"].get(name)
            if not info:
                continue
            j = a["judged"][info["end_hash"]]
            add(name, j["two"], j["now"], live["two"])
            c = cost.setdefault(name, [0.0, 0, 0])
            c[0] += info["time_s"]
            c[1] += info.get("lookahead_evals") or 0
            c[2] += int(bool(info.get("proven_optimal")))
    lines = ["| arm | turns | two-turn worst | two-turn mean | mechs (worst) | better / worse than live | "
             "solve s (sum) | lookahead samples | proven |", "|---|---|---|---|---|---|---|---|---|"]
    for name, x in sorted(agg.items(), key=lambda kv: (kv[1]["mean"], kv[0])):
        c = cost.get(name)
        lines.append(f"| {name} | {x['n']} | {x['worst']} | {x['mean']:.2f} | {x['mechs']} | {x['better']} / "
                     f"{x['worse']} | " + (f"{c[0]:.0f} | {c[1]} | {c[2]} |" if c else "- | - | - |"))
    return "\n".join(lines)


# The tiers above NextTurn (itb_live score names): a ranking change below
# them must never make them worse.
TOP_TIERS = ("grid_survival", "grid", "building_hp", "mechs", "objectives_failed")


def tiers_turn(pool: Pool, ts: TurnStart, arms: list, args) -> dict:
    """Turn T solved by each arm (the regression check: this turn's grid,
    buildings, mechs and stars under each ranking)."""
    budget = dict(time_limit=args.time, max_time=args.max_time, min_tiers=args.min_tiers, threads=1)
    reps = pool.map([dict(cmd="solve", state=str(ts.state), **budget, **arm_options(a)) for a in arms])
    out = {"key": ts.key, "mission_id": ts.mission_id, "turn": ts.turn, "total_turns": ts.total, "arms": {}}
    for a, rep in zip(arms, reps):
        out["arms"][a] = {"worst_case": rep.get("worst_case"), "proven_optimal": rep.get("proven_optimal"),
                          "proven_components": rep.get("proven_components"), "time_s": rep["stats"]["time_s"],
                          "base_proven_components": rep["stats"].get("base_proven_components"),
                          "plan": plan_text(rep.get("plan") or [])}
    return out


def tiers_report(tag: str, out_root: Path, base: str) -> str:
    """Per arm against `base`: turns where a tier above NextTurn is worse /
    better (lexicographically over TOP_TIERS), the first such tier, and
    whether both solves proved those tiers."""
    rows = [json.loads(f.read_text()) for f in sorted((out_root / tag / "tiers").glob("*.json"))]
    names = sorted({n for r in rows for n in r["arms"]} - {base})
    lines = [f"{len(rows)} turn starts; against `{base}`:", "",
             "| arm | turns | same plan | top tiers worse | better | worse with both proven | next_turn worse | "
             "next_turn better |", "|---|---|---|---|---|---|---|---|"]
    detail = []
    for n in names:
        same = worse = better = proven_worse = nt_w = nt_b = cnt = 0
        for r in rows:
            b, x = r["arms"].get(base), r["arms"].get(n)
            if not b or not x or not b.get("worst_case") or not x.get("worst_case"):
                continue
            cnt += 1
            same += b["plan"] == x["plan"]
            kb = tuple(b["worst_case"].get(t, 0) for t in TOP_TIERS)
            kx = tuple(x["worst_case"].get(t, 0) for t in TOP_TIERS)
            if kx < kb:
                worse += 1
                both = (b.get("proven_components") or 0) >= 5 and (x.get("proven_components") or 0) >= 5
                proven_worse += both
                tier = next(t for t, u, v in zip(TOP_TIERS, kb, kx) if u != v)
                detail.append(f"- {n} worse on {r['key']} {r['mission_id']} t{r['turn']}: {tier} {kb} -> {kx} "
                              f"(proven {b.get('proven_components')} / {x.get('proven_components')})")
            elif kx > kb:
                better += 1
                tier = next(t for t, u, v in zip(TOP_TIERS, kb, kx) if u != v)
                detail.append(f"- {n} better on {r['key']} {r['mission_id']} t{r['turn']}: {tier} {kb} -> {kx} "
                              f"(proven {b.get('proven_components')} / {x.get('proven_components')})")
            elif "next_turn" in b["worst_case"] and "next_turn" in x["worst_case"]:
                nb = (b["worst_case"]["next_turn"], x["worst_case"]["next_turn"])
                # Different folds have different scales: compare the worst sample (lookahead_fold_worst).
                wb, wx = worst_sample(nb[0], arm_options(base)), worst_sample(nb[1], arm_options(n))
                nt_w += wx < wb
                nt_b += wx > wb
        lines.append(f"| {n} | {cnt} | {same} | {worse} | {better} | {proven_worse} | {nt_w} | {nt_b} |")
    return "\n".join(lines + [""] + detail)


def worst_sample(value: int, opts: dict) -> int:
    """The worst sample's next_turn_value inside a folded NextTurn value
    (lookahead.hpp lookahead_fold_worst; the sum fold has none: the mean)."""
    fold = opts.get("lookahead_fold", "worst")
    if fold == "worst_sum":
        return -((-value) // (1 << 17))
    if fold == "sum":
        return value // max(1, opts.get("lookahead_samples", 8))
    return value


# ---------------------------------------------------------------- report

def load_results(tags: list, out_root: Path = OUT_ROOT) -> list:
    rows = []
    for tag in tags:
        for f in sorted((out_root / tag).glob("*.json")):
            r = json.loads(f.read_text())
            r["tag"] = tag
            rows.append(r)
    return rows


def fmt(x, nd=2):
    return "-" if x is None else (f"{x:.{nd}f}" if isinstance(x, float) else str(x))


def confirm_text(r: dict) -> str:
    cf_ = r.get("confirm")
    if not cf_:
        return "-"
    pick = r.get("best") or r.get("best_alt")
    live = next(p for p in cf_["plans"] if "live" in p["sources"])
    alt = next((p for p in cf_["plans"] if pick and p["plan"] == pick["plan"] and p is not live), None)
    t = f"{live['two']['worst']}/{fmt(live['two']['mean'])}"
    return t + (f" / {alt['two']['worst']}/{fmt(alt['two']['mean'])}" if alt else " / -")


def confirmed_gain(r: dict) -> tuple:
    """Grid the turn's alternative saves on the confirmation seeds (worst
    case, mean), when the confirmed verdict says it is better; else 0."""
    v = confirmed_verdict(r)
    if v not in ("avoidable", "better_mean"):
        return 0, 0.0
    pick = r.get("best") or r.get("best_alt")
    live = next(p for p in r["confirm"]["plans"] if "live" in p["sources"])
    alt = next(p for p in r["confirm"]["plans"] if p["plan"] == pick["plan"] and p is not live)
    return live["two"]["worst"] - alt["two"]["worst"], live["two"]["mean"] - alt["two"]["mean"]


def saved_by_mission(rows: list) -> dict:
    """Per run: the grid the confirmed alternatives save (worst case, then
    mean), over turns that do not overlap (choosing turn T's alternative
    covers T and T+1, so T+1's own alternative cannot be added on top), per
    mission by a small dynamic program on the worst case."""
    by = {}
    for r in rows:
        by.setdefault((r["run"], r["mission"]), {})[r["turn"]] = confirmed_gain(r)
    out = {}
    for (run, _), turns in by.items():
        best = {}  # turn -> best (worst gain, mean gain) using turns up to it
        for t in sorted(turns):
            before = max((v for u, v in best.items() if u < t - 1), default=(0, 0.0))
            take = (turns[t][0] + before[0], round(turns[t][1] + before[1], 3))
            best[t] = max(take, max(best.values(), default=(0, 0.0)))
        w, m = max(best.values(), default=(0, 0.0))
        o = out.setdefault(run, [0, 0.0])
        o[0] += w
        o[1] = round(o[1] + m, 3)
    return out


def report(rows: list) -> str:
    lines = []
    lines.append("| tag | turn | mission | grid | rec. loss T | live now | live T+1 worst/mean | live 2-turn worst/mean "
                 "| best alt 2-turn worst/mean (source) | verdict | decided by | actual T+1 loss | confirmed "
                 "live / alt 2-turn worst/mean | confirmed verdict |")
    lines.append("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    agg = {}
    sources, decided, arms = {}, {}, {}
    greedy_pairs, search_pairs, cand_pairs, la, actual = [], [], [], [], []
    for r in rows:
        a = agg.setdefault(r["tag"], {"turns": 0, "optimal": 0, "forced": 0, "avoidable": 0, "better_mean": 0,
                                      "last_turn": 0, "saved_worst": 0, "saved_mean": 0.0})
        a["turns"] += 1
        a[r["verdict"]] += 1
        lv = r["live"]
        b = r.get("best")
        if b:
            a["saved_worst"] += lv["two"]["worst"] - b["two"]["worst"]
            a["saved_mean"] = round(a["saved_mean"] + lv["two"]["mean"] - b["two"]["mean"], 3)
            for s in b["sources"]:
                sources[s] = sources.get(s, 0) + 1
            decided[r.get("decided_by") or "-"] = decided.get(r.get("decided_by") or "-", 0) + 1
        an = ((r.get("actual_next") or {}).get("solve") or {}).get("plain")
        lines.append("| {tag} | {run} m{m:02d} t{t}/{tt} | {mid} | {g} | {rl} | {now} | {nw}/{nm} | {w}/{mn} | {bb} "
                     "| {v} | {d} | {an} |".format(
                         tag=r["tag"], run=r["run"], m=r["mission"], t=r["turn"], tt=r["total_turns"],
                         mid=r["mission_id"].replace("Mission_", ""), g=r["grid"], rl=fmt(r.get("recorded_loss")),
                         now=lv["now"]["grid"], nw=lv["two"]["next_worst"], nm=fmt(lv["two"]["next_mean"]),
                         w=lv["two"]["worst"], mn=fmt(lv["two"]["mean"]),
                         bb=(f"{b['two']['worst']}/{fmt(b['two']['mean'])} ({','.join(b['sources'])})" if b else "-"),
                         v=r["verdict"], d=r.get("decided_by") or "-", an=an["grid"] if an else "-")
                     + " {cf} | {cv} |".format(cf=confirm_text(r), cv=confirmed_verdict(r) or "-"))
        cv = confirmed_verdict(r) or (r["verdict"] if r.get("settled_by_proof") or r["verdict"] == "last_turn" else None)
        if cv:
            a.setdefault("confirmed", {})
            a["confirmed"][cv] = a["confirmed"].get(cv, 0) + 1
        if r.get("confirm"):
            gw, gm = confirmed_gain(r)
            a["confirmed_saved_worst"] = a.get("confirmed_saved_worst", 0) + gw
            a["confirmed_saved_mean"] = round(a.get("confirmed_saved_mean", 0.0) + gm, 3)
        if r.get("confirm"):
            for p in r["confirm"]["plans"]:
                for s in p["sources"]:
                    if s == "live" or s.startswith("arm:"):
                        arm = arms.setdefault(s, [0, 0.0, 0])
                        arm[0] += p["two"]["worst"]
                        arm[1] += p["two"]["mean"]
                        arm[2] += 1
        for c in r.get("candidates", []):
            ss = [s for s in c.get("samples", []) if (s.get("solve") or {}).get("plain")]
            for s in ss:
                g = s["solve"]["plain"]["grid"]
                greedy_pairs.append((s["greedy"]["grid"], g))
                if "search" in s:
                    search_pairs.append((s["search"]["grid"], g))
            if ss:
                cand_pairs.append((max(s["greedy"]["grid"] for s in ss), max(s["solve"]["plain"]["grid"] for s in ss)))
            if "live" in c["sources"] and ss:
                nt = (r.get("live_worst_case") or {}).get("next_turn")
                if nt is not None:
                    la.append((-int(nt) // 1024, c["two"]["next_worst"], c["two"]["next_mean"]))
                if an is not None:
                    gs = [s["solve"]["plain"]["grid"] for s in ss]
                    actual.append((an["grid"], min(gs), max(gs), r.get("recorded_next_loss")))
    lines.append("")
    lines.append("Verdicts and grid saveable (worst case / mean, summed over turns):")
    for tag, a in agg.items():
        lines.append(f"- {tag}: " + json.dumps(a))
    lines.append(f"- saveable per run on the confirmation seeds, turns not overlapping ([worst case, mean]): "
                 f"{json.dumps(saved_by_mission(rows))}")
    lines.append(f"- best alternative's sources: {json.dumps(sources)}; the live ranking preferred the live plan "
                 f"by: {json.dumps(decided)}")
    if arms:
        lines.append("- plans by source on the confirmation seeds (sum over the confirmed turns of the two-turn worst / "
                     "mean, turns): " + ", ".join(f"{k} {v[0]} / {v[1]:.2f} ({v[2]})" for k, v in sorted(arms.items())))

    def acc(pairs, name):
        if not pairs:
            return f"- {name}: no samples"
        eq = sum(1 for x, y in pairs if x == y)
        hi = sum(1 for x, y in pairs if x > y)
        lo = sum(1 for x, y in pairs if x < y)
        return (f"- {name}: {len(pairs)} pairs, equal grid loss {eq} ({100 * eq / len(pairs):.0f}%), estimate higher "
                f"{hi}, lower {lo}; sums {sum(x for x, _ in pairs)} vs {sum(y for _, y in pairs)}")
    lines.append("")
    lines.append("Cheap next-turn estimates against a real solve of the same next-turn board:")
    lines.append(acc(greedy_pairs, "greedy reply, per board"))
    lines.append(acc(search_pairs, "Search reply (node budget), per board"))
    lines.append(acc(cand_pairs, "greedy reply, worst over a candidate's seeds"))
    if la:
        hi = sum(1 for e, w, _ in la if e > w)
        lo = sum(1 for e, w, _ in la if e < w)
        lines.append(f"- live solve's own next-turn estimate (worst of its 8 greedy samples) vs the oracle's worst "
                     f"next turn (8 other seeds, solved): {len(la)} turns, estimate higher on {hi}, lower on {lo}, "
                     f"equal on {len(la) - hi - lo}; sums {sum(e for e, _, _ in la)} vs {sum(w for _, w, _ in la)} "
                     f"(oracle mean {sum(m for _, _, m in la):.2f})")
    if actual:
        inside = sum(1 for g, a0, a1, _ in actual if a0 <= g <= a1)
        lines.append(f"- the game's actual next turn, solved the same way: inside the live plan's sampled range on "
                     f"{inside} of {len(actual)} turns (below {sum(1 for g, a0, _, _ in actual if g < a0)}, above "
                     f"{sum(1 for g, _, a1, _ in actual if g > a1)})")
    return "\n".join(lines)


# ---------------------------------------------------------------- main

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("cmd", choices=["select", "run", "confirm", "report", "arms", "arms-report", "tiers",
                                    "tiers-report"])
    ap.add_argument("--root", default=str(LIVE_ROOT), help="recordings/live directory")
    ap.add_argument("--run", action="append", default=[], help="run directory name (repeatable)")
    ap.add_argument("--mission", type=int, action="append", help="only these mission indices (repeatable)")
    ap.add_argument("--turn", type=int, action="append", help="only these turns (repeatable)")
    ap.add_argument("--tag", action="append", default=[], help="output subdirectory (run: the first)")
    ap.add_argument("--out-root", default=str(OUT_ROOT), help="recordings/oracle directory")
    ap.add_argument("--arms-dir", default="arms", help="arms: the subdirectory of each tag for the results "
                                                       "(e.g. one per budget)")
    ap.add_argument("--all-turns", action="store_true", help="tiers: every turn start, not only the loss turns")
    ap.add_argument("--every", type=int, default=1, help="tiers: every Nth turn start")
    ap.add_argument("--base-arm", default="live", help="tiers-report: the arm the others are compared with")
    ap.add_argument("--workers", type=int, default=2, help="itb_live processes, one thread each")
    ap.add_argument("--itb-live", default=str(ITB_LIVE))
    ap.add_argument("--game", default=None)
    ap.add_argument("--nice", type=int, default=5)
    ap.add_argument("--arms", default="live,base,la32,sum8,idle8,plain")
    ap.add_argument("--time", type=float, default=8.0, help="turn-T solves: base time (one thread)")
    ap.add_argument("--max-time", type=float, default=24.0)
    ap.add_argument("--min-tiers", type=int, default=5)
    ap.add_argument("--kbest", type=int, default=3, help="extra distinct outcomes under the live ranking")
    ap.add_argument("--end-states", type=int, default=1500, help="end states sampled from the live arm's search")
    ap.add_argument("--per-outcome", type=int, default=2)
    ap.add_argument("--screen-max", type=int, default=80)
    ap.add_argument("--screen-seeds", type=int, default=8)
    ap.add_argument("--screen-seed", type=int, default=201)
    ap.add_argument("--finalists", type=int, default=3, help="screened end states judged in full")
    ap.add_argument("--seeds", type=int, default=8, help="AI seeds per candidate, full evaluation")
    ap.add_argument("--seed", type=int, default=101)
    ap.add_argument("--next-time", type=float, default=4.0)
    ap.add_argument("--next-max-time", type=float, default=16.0)
    ap.add_argument("--search-nodes", type=int, default=200)
    ap.add_argument("--confirm-seeds", type=int, default=16, help="confirm: fresh AI seeds per plan")
    ap.add_argument("--confirm-seed", type=int, default=301)
    args = ap.parse_args(argv)
    args.arms = [a for a in args.arms.split(",") if a]
    for a in args.arms:
        if a not in ARMS and a not in ARM_ALIASES:
            ap.error(f"unknown arm {a}")
    root = Path(args.root)
    out_root = Path(args.out_root)
    if args.cmd == "report":
        print(report(load_results(args.tag or ["default"], out_root)))
        return 0
    if args.cmd == "arms-report":
        print(arms_report(args.tag or ["default"], out_root, args.arms_dir))
        return 0
    if args.cmd == "tiers-report":
        print(tiers_report((args.tag or ["default"])[0], out_root, args.base_arm))
        return 0
    if args.cmd == "arms":
        tags = args.tag or ["default"]
        pool = Pool(args.workers, Path(args.itb_live), args.game, args.nice, out_root / tags[0] / "arms.log")
        try:
            for tag in tags:
                out_dir = out_root / tag / args.arms_dir
                out_dir.mkdir(parents=True, exist_ok=True)
                for f in sorted((out_root / tag).glob("*.json")):
                    t0 = time.time()
                    try:
                        res = arms_turn(pool, f, root, out_dir, args)
                    except OracleError as e:
                        print(f"{f.name}: error: {e}", flush=True)
                        continue
                    if res:
                        print(f"{f.name}: " + ", ".join(
                            f"{a} {res['judged'][i['end_hash']]['two']['worst']}/"
                            f"{res['judged'][i['end_hash']]['two']['mean']:.2f}" for a, i in res["arms"].items())
                              + f" ({time.time() - t0:.0f}s)", flush=True)
        finally:
            pool.close()
        return 0
    if args.cmd == "confirm":
        tags = args.tag or ["default"]
        pool = Pool(args.workers, Path(args.itb_live), args.game, args.nice, out_root / tags[0] / "itb_live.log")
        try:
            for tag in tags:
                for f in sorted((out_root / tag).glob("*.json")):
                    t0 = time.time()
                    try:
                        r = confirm_turn(pool, f, root, args)
                    except OracleError as e:
                        print(f"{f.name}: error: {e}", flush=True)
                        continue
                    if r:
                        print(f"{f.name}: {r['verdict']} -> confirmed {confirmed_verdict(r)} ({time.time() - t0:.0f}s)",
                              flush=True)
        finally:
            pool.close()
        return 0
    turns = []
    for run in args.run:
        starts = turn_starts(root, run)
        if args.cmd == "tiers" and args.all_turns:
            sel = [t for t in starts if args.mission is None or t.mission in args.mission][::max(1, args.every)]
        else:
            sel = select_turns(starts, set(args.mission) if args.mission else None)
        if args.turn:
            sel = [t for t in sel if t.turn in args.turn]
        turns += sel
    if args.cmd == "select":
        for t in turns:
            print(f"{t.key} {t.mission_id} t{t.turn}/{t.total} grid {t.grid} recorded loss {t.recorded_loss()} "
                  f"predicted {t.predicted_grid_loss()}")
        print(f"{len(turns)} turns")
        return 0
    out_dir = out_root / (args.tag[0] if args.tag else "default")
    if args.cmd == "tiers":
        (out_dir / "tiers").mkdir(parents=True, exist_ok=True)
        pool = Pool(args.workers, Path(args.itb_live), args.game, args.nice, out_dir / "tiers.log")
        try:
            # Turns in parallel: each arm of a turn on its own process.
            per_batch = max(1, args.workers // max(1, len(args.arms)))
            todo = [t for t in turns if not (out_dir / "tiers" / f"{t.key}.json").exists()]
            exe = cf.ThreadPoolExecutor(max_workers=per_batch)
            for t, res in zip(todo, exe.map(lambda t: tiers_turn(pool, t, args.arms, args), todo)):
                (out_dir / "tiers" / f"{t.key}.json").write_text(json.dumps(res, indent=1))
                print(f"{t.key}: " + ", ".join(f"{a} {json.dumps({k: (x['worst_case'] or {}).get(k) for k in TOP_TIERS})}"
                                               for a, x in res["arms"].items()), flush=True)
            exe.shutdown()
        finally:
            pool.close()
        return 0
    out_dir.mkdir(parents=True, exist_ok=True)
    pool = Pool(args.workers, Path(args.itb_live), args.game, args.nice, out_dir / "itb_live.log")
    try:
        for t in turns:
            f = out_dir / f"{t.key}.json"
            if f.exists():
                continue
            print(f"== {t.key} {t.mission_id} t{t.turn}/{t.total} grid {t.grid}", flush=True)
            try:
                r = oracle_turn(pool, t, args)
            except OracleError as e:
                print(f"   error: {e}", flush=True)
                continue
            f.write_text(json.dumps(r, indent=1))
            lv = r["live"]["two"]
            b = r.get("best")
            print(f"   {r['verdict']}: live 2-turn worst {lv['worst']} mean {lv['mean']:.2f}"
                  + (f" | best {b['two']['worst']} / {b['two']['mean']:.2f} {b['sources']}" if b else "")
                  + f" ({r['time_s']:.0f}s)", flush=True)
    finally:
        pool.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
