"""scripts/loadout_eval.py against a fake engine, the fake game scripts and
the recorded save fragment of tests/test_loadout.py. Never touches the game.

    python3 -m unittest tests.test_loadout_eval
"""

from __future__ import annotations

import contextlib
import copy
import io
import json
import shutil
import tempfile
import unittest
from pathlib import Path

from tests import test_live_play as tlp  # sets ITB_BRIDGE_DIR to a temp dir first
from tests import test_loadout as tl

import loadout_eval as le  # noqa: E402  (scripts/ is on sys.path via test_live_play)

lp = tlp.lp


def squad_state() -> dict:
    """test_live_play's base board with the save fragment's squad on it
    (mechs 0-2 as that save holds them), so a candidate's rewrite changes
    only what the candidate changes."""
    st = tlp.base_state()
    template = next(u for u in st["units"] if u.get("mech"))
    others = [u for u in st["units"] if not (u.get("mech") and u.get("team") == 1)]
    mechs = []
    for m, (x, y) in zip(tl.lo.squad(tl.lo.parse_save(tl.SAVE.read_text())), ((6, 6), (6, 1), (5, 5))):
        u = copy.deepcopy(template)
        weapons = [s.base for s in m.slots if s]
        u.update(uid=m.index, type=m.type, x=x, y=y, hp=m.health, max_hp=m.health, move=3, base_move=3,
                 weapons=weapons, weapons_exact=weapons)
        mechs.append(u)
    st["units"] = mechs + others
    return st


class ScoringEngine:
    """solve(): a worst case computed from the board: grid -1 unless mech 0
    carries Prime_Lasermech_A, mech_hp = the mechs' HP, kills = mech 1's
    move. Records every board it was asked."""

    def __init__(self, proven=True):
        self.boards = []
        self.proven = proven

    def solve(self, path, time_limit, max_time=0, min_tiers=0, **ranking):
        d = json.loads(Path(path).read_text())
        st = d["data"]["bridge_state"]
        self.boards.append(st)
        mechs = {u["uid"]: u for u in st["units"] if u.get("mech") and u.get("team") == 1}
        grid = 0 if "Prime_Lasermech_A" in (mechs.get(0, {}).get("weapons_exact") or []) else -1
        worst = {"grid_survival": 1.0, "grid": grid, "building_hp": 0, "mechs": 0, "objectives_failed": 0,
                 "mech_hp": sum(u.get("hp") or 0 for u in mechs.values()), "objectives": 0,
                 "kills": mechs.get(1, {}).get("move", 0), "next_turn": -5}
        return {"ok": True, "worst_case": worst, "proven_optimal": self.proven, "plan": []}

    def close(self):
        pass


class LoadoutEvalTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="itb_loadout_eval_"))
        self._roots = (lp.LIVE_ROOT, lp.CURRENT)
        lp.LIVE_ROOT = self.tmp / "live"
        lp.CURRENT = self.tmp / "current.json"
        self.game = tl.write_tree(self.tmp / "game" / "scripts", tl.SCRIPTS).parent

    def tearDown(self):
        lp.LIVE_ROOT, lp.CURRENT = self._roots
        shutil.rmtree(self.tmp, ignore_errors=True)

    def record(self, run="20261018_000000", mission=0, turn=1, state=None) -> Path:
        d = lp.LIVE_ROOT / run
        d.mkdir(parents=True, exist_ok=True)
        st = copy.deepcopy(state or squad_state())
        st["turn"] = turn
        p = d / f"m{mission:02d}_turn_{turn:02d}_solve_input.json"
        p.write_text(json.dumps({"run_id": run, "label": "solve_input", "data": {"bridge_state": st}}))
        text = tl.SAVE.read_text()
        (d / f"m{mission:02d}_turn_{turn:02d}_saveData.lua").write_text(text)
        (d / f"m{mission:02d}_turn_{turn:02d}_undoSave.lua").write_text(text)
        lp.CURRENT.write_text(json.dumps({"run": str(d)}))
        return p

    def run_eval(self, extra, engine=None):
        engine = engine or ScoringEngine()
        out = self.tmp / "eval"
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = le.main(["--game", str(self.game), "--out", str(out), "--time", "1", *extra], engine=engine)
        report = json.loads((out / "report.json").read_text()) if (out / "report.json").exists() else None
        return rc, report, buf.getvalue(), engine

    # ------------------------------------------------------------ candidates

    def test_candidates_parse(self):
        c = le.parse_candidate("1:*,Brute_Railgun+hp:2+move:0")
        self.assertEqual([(s.index, s.weapons) for s in c.specs], [(1, ["*", "Brute_Railgun"])])
        self.assertEqual((c.hp, c.move, c.cores(), c.mechs()), ([2], [0], 2, {0, 1, 2}))
        self.assertEqual(le.parse_candidate("hp:1").specs, [])
        for bad in ("", "hp:7", "0:A,B,C"):
            with self.assertRaises(le.CandidateError, msg=bad):
                le.parse_candidate(bad)

    def test_hp_and_move_patch_the_board(self):
        self.record()
        ts = le.find_turn_starts(lp.LIVE_ROOT, need_save=True)[0]
        items = tl.lo.Items(self.game / "scripts")
        before = le.read_state(ts.path)[0]
        _, info, st = le.apply_candidate(items, ts, le.parse_candidate("hp:0+move:1"))
        u0, u1 = le.mech_unit(st, 0), le.mech_unit(st, 1)
        b0, b1 = le.mech_unit(before, 0), le.mech_unit(before, 1)
        self.assertEqual((u0["hp"], u0["max_hp"]), (b0["hp"] + 2, b0["max_hp"] + 2))
        self.assertEqual(u1["move"], b1["move"] + 1)
        self.assertEqual(info["cores"], 2)
        st2 = copy.deepcopy(before)
        st2["units"] = [u for u in st2["units"] if u.get("uid") != 2]
        ts.path.write_text(json.dumps({"data": {"bridge_state": st2}}))
        with self.assertRaises(le.CandidateError):
            le.apply_candidate(items, ts, le.parse_candidate("hp:2"))  # no mech 2 on this board

    def test_tiers_compare_in_order(self):
        base = {"grid": -1, "mech_hp": 5}
        self.assertEqual(le.compare(base, {"grid": 0, "mech_hp": 1}), (1, {"grid": 1.0, "mech_hp": -4.0}))
        self.assertEqual(le.compare(base, {"grid": -1, "mech_hp": 4})[0], -1)
        # The solver's own next-turn tiers decide the verdict; the sums keep only concrete outcomes.
        self.assertEqual(le.compare(base, dict(base, next_turn=9)), (1, {}))
        self.assertEqual(le.compare(dict(base, threat=-1), {"grid": -1, "mech_hp": 3, "threat": 5}),
                         (1, {"mech_hp": -2.0}))

    # ------------------------------------------------------------ end to end

    def test_candidates_are_scored_against_each_turns_baseline_and_ranked(self):
        for t in (1, 2, 3):
            self.record(turn=t)
        rc, report, out, eng = self.run_eval(["--candidate", "hp:1", "--candidate", "0:Prime_Lasermech_A,*",
                                              "--candidate", "move:1", "--candidate", "0:Brute_Jetmech"])
        self.assertEqual(rc, 0, out)
        res = {r["candidate"]: r for r in report["results"]}
        laser = res["0:Prime_Lasermech_A,*"]
        self.assertEqual((laser["turns"], laser["better"], laser["worse"]), (3, 3, 0))
        self.assertEqual(laser["sums"]["grid"], 3.0)
        self.assertEqual(res["hp:1"]["sums"], {"mech_hp": 6.0})
        self.assertEqual(laser["sums"], {"grid": 3.0})  # nothing else moved
        self.assertEqual(res["move:1"]["sums"], {"kills": 3.0})
        self.assertEqual(res["move:1"]["cores"], 1)
        # Illegal on every turn (the squad holds that weapon already): skipped, not scored.
        self.assertEqual((res["0:Brute_Jetmech"]["turns"], res["0:Brute_Jetmech"]["skipped"]), (0, 3))
        # One baseline per turn, one solve per legal candidate and the control.
        self.assertEqual(len(eng.boards), 3 * 5)
        control = res[le.CONTROL]
        self.assertEqual((control["turns"], control["better"], control["worse"]), (3, 0, 0))
        self.assertIn("noise floor: the same board with 1.5x the budget differed on 0 of 3 turns", out)
        # Ranked by tier sums: grid first.
        table = [l for l in out.splitlines() if l[:1] in ("0", "h", "m")]
        self.assertTrue(table[0].startswith("0:Prime_Lasermech_A"), table)
        self.assertEqual(report["turns"], ["20261018_000000/m00/t01", "20261018_000000/m00/t02",
                                           "20261018_000000/m00/t03"])

    def test_a_weapon_in_storage_can_be_equipped(self):
        self.record()
        rc, report, out, eng = self.run_eval(["--candidate", "2:*,Passive_Leech"])  # the save's storage holds it
        self.assertEqual(rc, 0, out)
        self.assertEqual((report["results"][0]["turns"], report["results"][0]["skipped"]), (1, 0))
        self.assertIn("Passive_Leech", le.mech_unit(eng.boards[1], 2)["weapons_exact"])  # baseline, candidate, control

    def test_last_and_other_squads(self):
        for t in (1, 2, 3):
            self.record(turn=t)
        other = squad_state()
        tlp.unit(other, 1)["type"] = "ChargeMech"
        self.record(run="20261017_000000", turn=1, state=other)
        self.record(turn=4)  # current run again
        rc, report, out, _ = self.run_eval(["--candidate", "hp:0", "--last", "2",
                                            "--run", "20261018_000000", "--run", "20261017_000000"])
        self.assertEqual(rc, 0, out)
        self.assertEqual(report["turns"], ["20261018_000000/m00/t03", "20261018_000000/m00/t04"])

    def test_a_safer_next_turn_outranks_no_change(self):
        """Run 58's shop: Frost Beam cost 2 mech HP but the solver ranked
        6 proven turns better (its next-turn tiers); "no change" candidates
        must not rank above it."""
        def result(text, rows):
            r = le.Result(le.parse_candidate(text))
            r.rows = rows
            return r
        safer = result("hp:0", [{"verdict": 1, "delta": {"mech_hp": -2.0}, "proven": True, "cores": 1}] * 6)
        same = result("hp:1", [{"verdict": 0, "delta": {}, "proven": True, "cores": 1}] * 6)
        grid = result("hp:2", [{"verdict": 1, "delta": {"grid": 1.0}, "proven": True, "cores": 1}])
        ranked = sorted([same, safer, grid], key=lambda r: r.rank_key(), reverse=True)
        self.assertEqual([r.cand.text for r in ranked], ["hp:2", "hp:0", "hp:1"])

    def test_unproven_turns_are_counted_apart(self):
        self.record()
        rc, report, out, _ = self.run_eval(["--candidate", "hp:0", "--no-control"], engine=ScoringEngine(proven=False))
        self.assertEqual(len(report["results"]), 1)
        r = report["results"][0]
        self.assertEqual((r["better"], r["proven_turns"], r["proven_better"]), (1, 0, 0))
        self.assertIn("(unproven)", out)

    def test_refusals(self):
        rc, report, out, _ = self.run_eval(["--candidate", "hp:0"])  # no run recorded
        self.assertEqual(rc, 2)
        self.assertIn("REFUSED", out)
        self.record()
        rc, _, out, _ = self.run_eval(["--candidate", "0:A,B,C"])
        self.assertEqual(rc, 2)


if __name__ == "__main__":
    unittest.main()
