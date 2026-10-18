#!/usr/bin/env python3
"""Loadout evaluator: what a shop purchase or a reactor core would have
been worth, measured by the engine on boards the run really played.

    python3 scripts/loadout_eval.py --candidate '1:*,Brute_Railgun' --candidate 'hp:2' --candidate 'move:0'
    python3 scripts/loadout_eval.py --run 20261018_083852 --last 12 --time 8 --threads 2 \\
        --candidate '2:Science_SwapOther,*' --candidate 'hp:2+move:2'

Every recorded turn start of the run (live_play's solve inputs with their
save checkpoints; --last N: the N most recent; --run repeatable, mechs
matched by type to the first run's squad) is solved twice or more with
the same budget and the live driver's ranking: once as recorded (the
baseline) and once per candidate, the candidate's loadout written into
that turn's checkpoint (scripts/loadout.py: legal loadouts, the reactor
cores they need, the mech's HP as the game would compute it) and read back
as the board the engine solves. Each candidate is scored by the change of
the solver's worst case, tier by tier, against that turn's baseline:

    grid_survival, grid, building_hp, mechs, objectives_failed, mech_hp,
    objectives, kills

(the live ranking's tiers; the lookahead's next-turn and threat estimates
are left out: they are sampled). A turn is better, worse or the same
lexicographically; the report sums each tier's change over the turns and
ranks the candidates by those sums in tier order.

Candidates (joined with '+'):
  <loadout.py spec>   '1:*,Brute_Railgun', '0:Prime_Punchmech_A,*',
                      '2:pilot=Pilot_Rock' (weapons the squad's storage holds
                      may be given: they come out of storage)
  hp:<mech>           the mech's +2 Health core (HP and max HP +2)
  move:<mech>         the mech's +1 Move core
'<mech>' is the squad position (the bridge uid of mechs 0-2).

It measures one turn at a time on boards the old loadout reached, so it
says what the change would have done on those turns, not where the run
would have gone instead; turns the solver did not prove carry search noise:
the ranking puts the proven turns' sums first, and a control (each
baseline solved again with 1.5x the budget; the solver is deterministic on
a board, so only the budget can move it; --no-control leaves it out) shows
how much an unproven answer moves with search effort alone. A turn's
verdict follows the solver's own ranking, next-turn estimates included
(a sturdier mech may let it trade mech HP for a safer next turn); the sums
are the concrete outcomes. Read-only: it never
touches the game; the engine runs as `itb_live serve` (--threads).
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import live_play as lp  # noqa: E402
import loadout as lo  # noqa: E402
from fuzz_play import TurnStart, find_turn_starts, read_state, unplayable  # noqa: E402

REPO = Path(__file__).resolve().parents[1]
OUT_ROOT = REPO / "recordings" / "loadout_eval"
TIERS = ("grid_survival", "grid", "building_hp", "mechs", "objectives_failed", "mech_hp", "objectives", "kills")
# The verdict: better or worse by the solver's own ranking (live_play's
# flags), its next-turn estimates (next_turn: the sampled lookahead, threat)
# included, so a candidate is never "worse" on a turn where the solver
# preferred its line; the sums stay on TIERS (concrete outcomes).
RANKED = ("grid_survival", "grid", "building_hp", "mechs", "objectives_failed", "next_turn", "threat", "mech_hp",
          "objectives", "kills", "vek_hp", "position")
# The live driver's ranking (scratchpad drive2.sh / live_play mission flags).
RANKING = dict(threat_tier=True, objectives_first=True, mech_grid=2, mech_grid_turns=True, mech_grid_pilot=True,
               lookahead=True, grid_survival=True, survival_cap=0.3)


class CandidateError(ValueError):
    pass


CONTROL = "(control: the baseline, 1.5x budget)"


@dataclass
class Candidate:
    text: str
    specs: list = field(default_factory=list)  # loadout.MechSpec
    hp: list = field(default_factory=list)  # mech indices getting +2 Health
    move: list = field(default_factory=list)  # mech indices getting +1 Move

    def mechs(self) -> set:
        return {s.index for s in self.specs} | set(self.hp) | set(self.move)

    @property
    def control(self) -> bool:
        return self.text == CONTROL

    def cores(self) -> int:
        """Cores of the hp/move upgrades (weapon cores come from the plan)."""
        return len(self.hp) + len(self.move)


def parse_candidate(text: str) -> Candidate:
    c = Candidate(text)
    loadout = []
    for part in (p.strip() for p in text.split("+")):
        if not part:
            continue
        m = re.fullmatch(r"(hp|move):(\d+)", part)
        if m:
            (c.hp if m.group(1) == "hp" else c.move).append(int(m.group(2)))
            continue
        loadout.append(part)
    if loadout:
        try:
            c.specs = lo.parse_spec(";".join(loadout))
        except lo.LoadoutError as e:
            raise CandidateError(str(e)) from e
    if not c.specs and not c.hp and not c.move:
        raise CandidateError(f"empty candidate {text!r}")
    for k in c.hp + c.move:
        if not 0 <= k <= 2:
            raise CandidateError(f"mech index {k} in {text!r}: 0-2")
    return c


def squad_types(state: dict) -> dict:
    return {u["uid"]: u.get("type") for u in lp.units(state) if u.get("mech") and u.get("team") == 1}


def mech_unit(state: dict, index: int) -> dict | None:
    return next((u for u in state.get("units", []) if u.get("mech") and u.get("team") == 1
                 and u.get("uid") == index), None)


def apply_candidate(items: lo.Items, ts: TurnStart, cand: Candidate) -> tuple[dict, dict, dict]:
    """The turn start's solve input with the candidate: (data with the
    patched bridge_state, notes {"cores": n, "notes": [...]}, the state).
    CandidateError when the loadout is illegal there."""
    state, data = read_state(ts.path)
    info = {"cores": cand.cores(), "notes": []}
    if cand.specs:
        if ts.save is None:
            raise CandidateError("no save checkpoint to plan the loadout on")
        texts = {"saveData.lua": ts.save.read_text(errors="replace"),
                 "undoSave.lua": ts.undo.read_text(errors="replace") if ts.undo else None}
        try:
            inj = lo.inject(items, texts, cand.specs, storage_ok=True)
        except (lo.LoadoutError, ValueError) as e:
            raise CandidateError(str(e)) from e
        state = lo.patch_state(state, inj, items)
        for p in inj.changed:
            info["cores"] += p.added_cores
            info["notes"] += [f"mech {p.index}: {n}" for n in p.notes]
    for k in cand.hp:
        u = mech_unit(state, k)
        if u is None:
            raise CandidateError(f"no mech {k} on the board")
        if (u.get("hp") or 0) > 0:  # a dead mech stays dead
            u["hp"] = u["hp"] + 2
        u["max_hp"] = (u.get("max_hp") or 0) + 2
    for k in cand.move:
        u = mech_unit(state, k)
        if u is None:
            raise CandidateError(f"no mech {k} on the board")
        keys = [key for key in ("move", "base_move") if isinstance(u.get(key), int)]
        if not keys:
            raise CandidateError(f"mech {k} has no move on the board")
        for key in keys:
            u[key] += 1
    return dict(data, bridge_state=state), info, state


def tier_vector(worst: dict, tiers: tuple = RANKED) -> tuple:
    out = []
    for t in tiers:
        v = worst.get(t)
        out.append(0.0 if v is None else float(v))
    return tuple(out)


def compare(base: dict, cand: dict) -> tuple[int, dict]:
    """(+1 better / -1 worse / 0 same by the solver's ranking (RANKED),
    {tier: delta} of the concrete TIERS)."""
    b, c = tier_vector(base), tier_vector(cand)
    tb, tc = tier_vector(base, TIERS), tier_vector(cand, TIERS)
    delta = {t: round(cv - bv, 4) for t, bv, cv in zip(TIERS, tb, tc) if cv != bv}
    return (c > b) - (c < b), delta


@dataclass
class Result:
    cand: Candidate
    rows: list = field(default_factory=list)  # per turn: {"turn", "verdict", "delta", "proven", "cores"}
    skipped: list = field(default_factory=list)  # (turn key, why)

    def summary(self) -> dict:
        sums = {t: 0.0 for t in TIERS}
        for r in self.rows:
            for t, d in r["delta"].items():
                sums[t] += d
        proven = [r for r in self.rows if r["proven"]]
        proven_sums = {t: 0.0 for t in TIERS}
        for r in proven:
            for t, d in r["delta"].items():
                proven_sums[t] += d
        return {
            "candidate": self.cand.text, "turns": len(self.rows),
            "better": sum(r["verdict"] > 0 for r in self.rows), "worse": sum(r["verdict"] < 0 for r in self.rows),
            "same": sum(r["verdict"] == 0 for r in self.rows),
            "proven_turns": len(proven), "proven_better": sum(r["verdict"] > 0 for r in proven),
            "proven_worse": sum(r["verdict"] < 0 for r in proven),
            "sums": {t: round(v, 4) for t, v in sums.items() if v},
            "proven_sums": {t: round(v, 4) for t, v in proven_sums.items() if v},
            "cores": max((r["cores"] for r in self.rows), default=self.cand.cores()),
            "skipped": len(self.skipped),
        }

    def rank_key(self) -> tuple:
        """Proven turns first (no search noise): their sums of the outcome
        tiers above mech HP (grid .. objectives failed), then how many of
        them the solver itself ranked better minus worse (that verdict
        includes the next-turn estimates the sums leave out: a candidate
        that only buys a safer next turn still counts), then the rest of
        their sums, then the same over all turns."""
        s = self.summary()
        head, tail = TIERS[:5], TIERS[5:]
        proven, every = s["proven_sums"], s["sums"]
        return (tuple(proven.get(t, 0.0) for t in head), s["proven_better"] - s["proven_worse"],
                tuple(proven.get(t, 0.0) for t in tail), tuple(every.get(t, 0.0) for t in head),
                s["better"] - s["worse"], tuple(every.get(t, 0.0) for t in tail))


def current_run() -> str:
    """The live_play run current.json points at (read only: Run.open would
    write it)."""
    try:
        return Path(json.loads(lp.CURRENT.read_text())["run"]).name
    except (OSError, ValueError, KeyError) as e:
        raise lp.Refused(f"no current live_play run ({lp.CURRENT}): give --run") from e


def pick_turns(args) -> list:
    root = Path(args.recordings)
    runs = args.run or [current_run()]
    starts = find_turn_starts(root, runs, need_save=True)
    by_run = {r: [ts for ts in starts if ts.run == r] for r in runs}
    ref = by_run.get(runs[0]) or []
    if not ref:
        raise lp.Refused(f"no recorded turn starts with save checkpoints in {root / runs[0]}")
    types = squad_types(read_state(ref[-1].path)[0])
    out = []
    for r in runs:
        for ts in by_run.get(r, []):
            st, _ = read_state(ts.path)
            if unplayable(st, last_turns=True):
                continue
            if squad_types(st) != types:
                continue  # another squad (or a mech not on the board)
            out.append(ts)
    out.sort(key=lambda ts: (ts.run, ts.mission, ts.turn))
    if args.last:
        out = out[-args.last:]
    return out


def solve(engine, path: Path, args, scale: float = 1.0) -> dict:
    t, mt = args.time * scale, args.max_time * scale
    reply = engine.solve(path, t, max_time=mt, min_tiers=args.min_tiers if mt > t else 0, **RANKING)
    return {"worst": reply.get("worst_case") or {}, "proven": bool(reply.get("proven_optimal")),
            "plan": [a.get("description") for a in reply.get("plan") or reply.get("engine_plan") or []]}


def evaluate(args, engine, cands: list, turns: list, out_dir: Path, items: lo.Items) -> list:
    results = [Result(c) for c in cands]
    for i, ts in enumerate(turns):
        st, _ = read_state(ts.path)
        lp.say(f"[{i + 1}/{len(turns)}] {ts.key} {st.get('mission_id')} turn {st.get('turn')}: baseline")
        base_path = out_dir / f"{ts.run}_m{ts.mission:02d}_t{ts.turn:02d}_base.json"
        base_path.write_text(ts.path.read_text())
        base = solve(engine, base_path, args)
        for k, res in enumerate(results):
            try:
                data, info, _ = apply_candidate(items, ts, res.cand)
            except CandidateError as e:
                res.skipped.append((ts.key, str(e)))
                lp.say(f"    {res.cand.text}: skipped ({e})")
                continue
            path = out_dir / f"{ts.run}_m{ts.mission:02d}_t{ts.turn:02d}_c{k}.json"
            path.write_text(json.dumps({"run_id": ts.run, "label": "solve_input", "data": data}))
            got = solve(engine, path, args, 1.5 if res.cand.control else 1.0)
            verdict, delta = compare(base["worst"], got["worst"])
            res.rows.append({"turn": ts.key, "verdict": verdict, "delta": delta,
                             "proven": base["proven"] and got["proven"], "cores": info["cores"],
                             "base": base["worst"], "cand": got["worst"], "plan": got["plan"]})
            mark = {1: "better", -1: "WORSE", 0: "same"}[verdict]
            lp.say(f"    {res.cand.text}: {mark}" + (f" {delta}" if delta else "")
                   + ("" if base["proven"] and got["proven"] else " (unproven)"))
    return results


def fmt_sums(sums: dict) -> str:
    return ", ".join(f"{t} {v:+g}" for t, v in sums.items()) or "no change"


def report(results: list) -> list:
    """The ranking (proven turns' tier sums first), the control apart: how
    often solving the same board again differed is the noise floor."""
    lines = ["", f"{'candidate':34} {'turns':>5} {'better':>6} {'worse':>5} {'proven b/w':>10} {'cores':>5}  "
                 "tier sums: proven turns | all turns"]
    ranked = sorted((r for r in results if not r.cand.control), key=lambda r: r.rank_key(), reverse=True)
    control = [r for r in results if r.cand.control]
    for res in ranked + control:
        s = res.summary()
        lines.append(f"{s['candidate'][:34]:34} {s['turns']:>5} {s['better']:>6} {s['worse']:>5} "
                     f"{s['proven_better']:>4}/{s['proven_worse']:<5} {s['cores']:>5}  "
                     f"{fmt_sums(s['proven_sums'])} | {fmt_sums(s['sums'])}"
                     + (f"  ({s['skipped']} skipped)" if s["skipped"] else ""))
    for res in control:
        s = res.summary()
        lines.append(f"noise floor: the same board with 1.5x the budget differed on {s['better'] + s['worse']} of "
                     f"{s['turns']} turns ({s['proven_better'] + s['proven_worse']} of {s['proven_turns']} proven)")
    return lines


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--candidate", action="append", required=True, help="a loadout change (repeatable; see above)")
    ap.add_argument("--run", action="append", help="live_play run directory name (default: the current run)")
    ap.add_argument("--last", type=int, default=0, help="only the N most recent turn starts")
    ap.add_argument("--recordings", default=str(lp.LIVE_ROOT))
    ap.add_argument("--time", type=float, default=10.0, help="solve time per board (s)")
    ap.add_argument("--max-time", type=float, default=30.0, help="extend up to this until --min-tiers are proven")
    ap.add_argument("--min-tiers", type=int, default=7)
    ap.add_argument("--threads", type=int, default=2)
    ap.add_argument("--no-control", action="store_true", help="skip the control (each baseline solved twice)")
    ap.add_argument("--game", help="game install for the engine and scripts (default: ITB_GAME_DIR / build default)")
    ap.add_argument("--out", help="output directory (default recordings/loadout_eval/<time>)")
    return ap


def main(argv=None, *, engine=None) -> int:
    args = build_parser().parse_args(argv)
    args.max_time = max(args.max_time, args.time)
    try:
        cands = [parse_candidate(c) for c in args.candidate]
        if not args.no_control:
            cands.append(Candidate(CONTROL))
    except CandidateError as e:
        lp.say(f"REFUSED: --candidate: {e}")
        return 2
    try:
        turns = pick_turns(args)
        if not turns:
            raise lp.Refused("no playable turn starts of this squad to evaluate on")
        items = lo.Items(lo.game_scripts(args.game))
        out_dir = Path(args.out) if args.out else OUT_ROOT / time.strftime("%Y%m%d_%H%M%S")
        out_dir.mkdir(parents=True, exist_ok=True)
        lp.say(f"{len(cands)} candidate(s) on {len(turns)} turn start(s) ({args.time:g}s per solve, "
               f"{args.threads} thread(s)) -> {out_dir}")
        own = engine is None
        if own:
            engine = lp.Engine(args.threads, args.game, out_dir / "itb_live.log")
        try:
            results = evaluate(args, engine, cands, turns, out_dir, items)
        finally:
            if own:
                engine.close()
    except lp.Refused as e:
        lp.say(f"REFUSED: {e}")
        return 2
    except lp.Anomaly as e:
        lp.say(f"STOPPED: {e}")
        return 1
    for line in report(results):
        lp.say(line)
    (out_dir / "report.json").write_text(json.dumps(
        {"argv": sys.argv[1:] if argv is None else argv, "turns": [ts.key for ts in turns],
         "results": [dict(r.summary(), rows=r.rows, skipped_turns=r.skipped) for r in results]}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
