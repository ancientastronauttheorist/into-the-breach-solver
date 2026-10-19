"""scripts/two_turn_oracle.py without an engine: turn selection from a fake
run directory, the two-turn values, verdicts and the report.

    python3 -m unittest tests.test_two_turn_oracle
"""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import two_turn_oracle as oracle  # noqa: E402


def write_state(d: Path, mm: int, tt: int, grid: int, total: int = 4, plan=None, worst=None) -> None:
    st = {"grid_power": grid, "total_turns": total, "turn": tt, "mission_id": "Mission_Test"}
    (d / f"m{mm:02d}_turn_{tt:02d}_solve_input.json").write_text(json.dumps({"data": {"bridge_state": st}}))
    data = {"engine_plan": plan if plan is not None else [{"uid": 0, "kind": "none", "description": "x"}],
            "worst_case": worst or {"grid": 0, "mechs": 0}, "proven_optimal": True}
    (d / f"m{mm:02d}_turn_{tt:02d}_solve.json").write_text(json.dumps({"data": data}))


def sample(grid: int, mechs: int = 0, greedy: int | None = None) -> dict:
    g = grid if greedy is None else greedy
    return {"solve": {"plain": {"grid": grid, "mechs": mechs}}, "greedy": {"grid": g, "mechs": mechs}}


def cand(src: str, now: int, nexts: list, mechs_now: int = 0, mechs_next: int = 0) -> dict:
    c = {"sources": [src], "plan": [{"description": src}], "now": {"grid": now, "mechs": mechs_now},
         "samples": [sample(g, mechs_next) for g in nexts]}
    c["two"] = oracle.two(now, c["samples"])
    return c


class SelectTest(unittest.TestCase):
    def test_loss_turns_and_the_turn_before(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp) / "run"
            d.mkdir()
            # Grid 7, 7, 6, 6: turn 2 loses one (recorded); turn 1 is the turn before.
            write_state(d, 0, 1, 7)
            write_state(d, 0, 2, 7)
            write_state(d, 0, 3, 6)
            write_state(d, 0, 4, 6, worst={"grid": -2, "mechs": 0})  # the last turn's worst case: 1 grid
            # Another mission, nothing lost.
            write_state(d, 2, 1, 6)
            write_state(d, 2, 2, 6)
            starts = oracle.turn_starts(Path(tmp), "run")
            self.assertEqual(len(starts), 6)
            sel = oracle.select_turns(starts)
            self.assertEqual([(t.mission, t.turn) for t in sel], [(0, 1), (0, 2), (0, 3), (0, 4)])
            self.assertEqual(sel[1].recorded_loss(), 1)
            self.assertTrue(sel[3].last)
            self.assertEqual(sel[3].predicted_grid_loss(), 1)
            self.assertEqual(oracle.select_turns(starts, missions={2}), [])

    def test_predicted_loss_unfolds_the_mech_slot(self):
        t = oracle.TurnStart("r", 0, 1, 4, "M", Path("x"), 5, {"worst_case": {"grid": -9, "mechs": -1}}, None, None,
                             None)
        self.assertEqual(t.predicted_grid_loss(), 2)  # 2 x 2 + 5 x 1
        t.solve = {"worst_case": {"grid": -(1 << 20) - 2, "mechs": 0}}
        self.assertEqual(t.predicted_grid_loss(), 1)


class VerdictTest(unittest.TestCase):
    def test_two_turn_values(self):
        v = oracle.two(1, [sample(0), sample(2), sample(1, mechs=1)])
        self.assertEqual((v["worst"], v["next_worst"], v["mechs_worst"], v["n"]), (3, 2, 1, 3))
        self.assertAlmostEqual(v["mean"], 2.0)
        self.assertEqual(oracle.two(2, [])["worst"], 2)
        g = oracle.two(0, [sample(0, greedy=2)], field="greedy")
        self.assertEqual(g["worst"], 2)

    def test_verdicts(self):
        live = cand("live", 1, [2, 2, 1])
        self.assertEqual(oracle.verdict_of(live, [cand("a", 1, [2, 2, 2])])["verdict"], "forced")
        better = cand("a", 2, [0, 0, 0])
        v = oracle.verdict_of(live, [better])
        self.assertEqual(v["verdict"], "avoidable")
        self.assertEqual(v["best"]["sources"], ["a"])
        # The same worst case: better only with a mean lower by MEAN_MARGIN.
        self.assertEqual(oracle.verdict_of(live, [cand("b", 1, [2, 1, 1])])["verdict"], "forced")
        self.assertEqual(oracle.verdict_of(live, [cand("b", 0, [3, 0, 0])])["verdict"], "better_mean")
        # An alternative that loses a mech more does not count.
        v = oracle.verdict_of(live, [cand("c", 0, [0, 0, 0], mechs_next=1)])
        self.assertEqual(v["verdict"], "forced")
        self.assertEqual(v["best_any"]["sources"], ["c"])
        self.assertEqual(oracle.verdict_of(cand("live", 0, [0, 0]), [])["verdict"], "optimal")

    def test_decided_by_the_first_differing_tier(self):
        live = cand("live", 1, [2])
        live["score"] = {"grid": -2, "building_hp": -1, "next_turn": -2048, "mech_hp": 0}
        alt = cand("a", 1, [0])
        alt["score"] = {"grid": -2, "building_hp": -1, "next_turn": -3072, "mech_hp": 0}
        self.assertEqual(oracle.verdict_of(live, [alt])["decided_by"], "next_turn")


class PlanTextTest(unittest.TestCase):
    def test_descriptions_parse_back(self):
        text = ("JudoMech#0 move G6, fire Prime_Shift at G5; NanoMech#2 repair; MirrorMech#1 move C6; "
                "LaserMech#3 fire Brute_Grapple at A1 then B2; Tank#4 (no action); "
                "NanoMech#2 move D3, repair (Skill_Repair_Power) at D4")
        plan = oracle.parse_plan_text(text, {2: [5, 5]})
        self.assertEqual(plan[0], {"uid": 0, "move": [2, 1], "kind": "weapon", "weapon": "Prime_Shift",
                                   "target": [3, 1], "target2": None})
        self.assertEqual(plan[1]["kind"], "repair")
        self.assertEqual(plan[1]["target"], [5, 5])  # where the mech stands
        self.assertEqual((plan[2]["move"], plan[2]["kind"]), ([2, 5], "none"))
        self.assertEqual((plan[3]["target"], plan[3]["target2"]), ([7, 7], [6, 6]))
        self.assertEqual((plan[4]["move"], plan[4]["kind"]), (None, "none"))
        self.assertEqual((plan[5]["weapon"], plan[5]["move"], plan[5]["target"]), ("Skill_Repair_Power", [5, 4], [4, 4]))
        with self.assertRaises(oracle.OracleError):
            oracle.parse_plan_text("JudoMech#0 dance", {})


class ReportTest(unittest.TestCase):
    def test_report_aggregates(self):
        live = cand("live", 1, [2, 1])
        alt = cand("a", 1, [0, 0])
        row = {"tag": "t", "key": "k", "run": "r", "mission": 0, "turn": 2, "total_turns": 4, "mission_id": "Mission_Test",
               "grid": 5, "recorded_loss": 1, "live_worst_case": {"next_turn": -1024}, "candidates": [live, alt]}
        row.update(oracle.verdict_of(live, [alt]))
        # The confirmation seeds agree: the alternative saves 2 in the worst case.
        row["confirm"] = {"plans": [
            {"sources": ["live"], "plan": "live", "now": live["now"], "two": oracle.two(1, [sample(2), sample(1)])},
            {"sources": ["a"], "plan": "a", "now": alt["now"], "two": oracle.two(1, [sample(0), sample(0)])}]}
        self.assertEqual(oracle.confirmed_verdict(row), "avoidable")
        self.assertEqual(oracle.confirmed_gain(row), (2, 1.5))
        text = oracle.report([row])
        self.assertIn("avoidable", text)
        self.assertIn('"saved_worst": 2', text)
        self.assertIn("greedy reply, per board: 4 pairs", text)
        self.assertIn("\"r\": [2, 1.5]", text)


if __name__ == "__main__":
    unittest.main()
