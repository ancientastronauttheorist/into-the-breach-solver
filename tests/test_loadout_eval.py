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
    carries Prime_Lasermech_A, mech_hp = the mechs' HP, kills = mech 0's
    move (the save's mechs 1 and 2 have their HP core, mech 1 its move core). Records every board it was asked."""

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
                 "kills": mechs.get(0, {}).get("move", 0), "next_turn": -5}
        return {"ok": True, "worst_case": worst, "proven_optimal": self.proven, "plan": []}

    def close(self):
        pass


POPULAR, INVULNERABLE = tl.lo.LEVEL_SKILLS.index("Popular"), tl.lo.LEVEL_SKILLS.index("Invulnerable")


def grid_state(overflow=0, difficulty=2, base=15) -> dict:
    """squad_state with Grid Defense as the bridge exports it (overpower
    bonus, base) and the save's pilots at level 1 with skills that do
    nothing on a board (Popular, Invulnerable): Grid Defense 15 + overflow."""
    st = squad_state()
    st.update(grid_overflow=overflow, grid_defense_base=base, grid_base_difficulty=difficulty, difficulty=difficulty)
    for u, pid in zip((u for u in st["units"] if u.get("mech")), ("Pilot_Miner", "Pilot_Detritus", "Pilot_Archive")):
        u["pilot"] = {"id": pid, "level": 1, "skill1": POPULAR, "skill2": INVULNERABLE, "xp": 30}
        u.update(pilot_id=pid, pilot_level=1, pilot_skills=[f"skill1={POPULAR}", f"skill2={INVULNERABLE}"])
    return st


class GridEngine:
    """solve(): the enemy phase hits two populated buildings for 1 each
    (two Grid Defense rolls; one with Prime_Lasermech_A on mech 0: its
    worst case loses 1 grid, not 2), at the Grid Defense the engine reads
    from the state (loadout_eval.grid_defense mirrors its loader). Proven."""

    def __init__(self):
        self.boards = []

    def solve(self, path, time_limit, max_time=0, min_tiers=0, **ranking):
        st = json.loads(Path(path).read_text())["data"]["bridge_state"]
        self.boards.append(st)
        mechs = {u["uid"]: u for u in st["units"] if u.get("mech") and u.get("team") == 1}
        hits = 1 if "Prime_Lasermech_A" in (mechs.get(0, {}).get("weapons_exact") or []) else 2
        pct = le.grid_defense(st)
        g0 = st["grid_power"]
        worst = {"grid_survival": 1.0, "grid": -hits, "building_hp": -hits, "mechs": 0, "objectives_failed": 0,
                 "mech_hp": sum(u.get("hp") or 0 for u in mechs.values()), "objectives": 0, "kills": 0,
                 "next_turn": -5}
        return {"ok": True, "worst_case": worst, "proven_optimal": True, "plan": [],
                "grid_defense": {"percent": 15 if pct is None else pct, "source": "state" if pct is not None else "default"},
                "start": {"grid_power": g0}, "after_enemy": {"grid_power": g0 - hits},
                "steps": [{"sub": "weapon", "grid_defense": []}],
                "enemy_phase": {"grid_defense": [{"point": [2, 2], "amount": 1}] * hits}}

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

    def record(self, run="20261018_000000", mission=0, turn=1, state=None, save_text=None) -> Path:
        d = lp.LIVE_ROOT / run
        d.mkdir(parents=True, exist_ok=True)
        st = copy.deepcopy(state or squad_state())
        st["turn"] = turn
        p = d / f"m{mission:02d}_turn_{turn:02d}_solve_input.json"
        p.write_text(json.dumps({"run_id": run, "label": "solve_input", "data": {"bridge_state": st}}))
        text = save_text if save_text is not None else tl.SAVE.read_text()
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
        rc, report, out, eng = self.run_eval(["--candidate", "hp:0", "--candidate", "0:Prime_Lasermech_A,*",
                                              "--candidate", "move:0", "--candidate", "0:Brute_Jetmech"])
        self.assertEqual(rc, 0, out)
        res = {r["candidate"]: r for r in report["results"]}
        laser = res["0:Prime_Lasermech_A,*"]
        self.assertEqual((laser["turns"], laser["better"], laser["worse"]), (3, 3, 0))
        self.assertEqual(laser["sums"]["grid"], 3.0)
        self.assertEqual(res["hp:0"]["sums"], {"mech_hp": 6.0})
        self.assertEqual(laser["sums"], {"grid": 3.0})  # nothing else moved
        self.assertEqual(res["move:0"]["sums"], {"kills": 3.0})
        self.assertEqual(res["move:0"]["cores"], 1)
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

    def test_a_core_upgrade_the_mech_has_is_no_candidate(self):
        """Run 60: Ice already had its +2 HP from an earlier shop and hp:1
        was still offered. The newest save's healthPower / movePower rule it
        out; other candidates still run."""
        text = tl.SAVE.read_text().replace('["healthPower"] = {0, },', '["healthPower"] = {1, },', 1)
        self.assertEqual([m.health_power for m in tl.lo.squad(tl.lo.parse_save(text))], [[1], [1], [1]])
        owner = 0
        self.record(turn=1)
        self.record(turn=2, save_text=text)
        rc, report, out, _ = self.run_eval(["--candidate", f"hp:{owner}", "--candidate", f"move:{owner}", "--no-control"])
        self.assertEqual(rc, 0, out)
        self.assertIn(f"skipped hp:{owner}: the mech already has hp:{owner}", out)
        self.assertEqual([r["candidate"] for r in report["results"]], [f"move:{owner}"])
        rc, _, out, _ = self.run_eval(["--candidate", f"hp:{owner}", "--no-control"])
        self.assertEqual(rc, 2)
        self.assertIn("no candidate left", out)

    def test_unproven_turns_are_counted_apart(self):
        self.record()
        rc, report, out, _ = self.run_eval(["--candidate", "hp:0", "--no-control"], engine=ScoringEngine(proven=False))
        self.assertEqual(len(report["results"]), 1)
        r = report["results"][0]
        self.assertEqual((r["better"], r["proven_turns"], r["proven_better"]), (1, 0, 0))
        self.assertIn("(unproven)", out)

    # ------------------------------------------------------------ Grid Defense

    def test_grid_defense_candidates_parse(self):
        c = le.parse_candidate("skills:1=grid,Health@2+def:3+power:1")
        self.assertEqual((c.skills, c.defense, c.power, c.mechs(), c.cores()),
                         ([(1, 2, 0, 2)], 3, 1, {1}, 0))
        self.assertEqual(le.parse_candidate("skills:0=Move,Grid").skills, [(0, 1, 2, None)])
        for bad in ("def:0", "def:26", "power:0", "skills:1=Grid,Grid", "skills:1=Grid,Foo", "skills:3=Grid,Move",
                    "skills:1=Grid,Move@3", "skills:1=Grid", "skills:1=Grid,Move+skills:1=Move,Health"):
            with self.assertRaises(le.CandidateError, msg=bad):
                le.parse_candidate(bad)

    def test_grid_defense_as_the_engine_reads_it(self):
        st = grid_state(overflow=4)
        self.assertEqual(le.grid_defense(st), 19)  # base 15 + overflow 4, nobody has Grid learned
        u = le.mech_unit(st, 1)
        u["pilot"]["skill2"] = le.GRID_SKILL  # learned at level 2 only
        self.assertEqual(le.grid_defense(st), 19)
        u["pilot"]["level"] = 2
        self.assertEqual(le.grid_defense(st), 22)
        # The bridge's pilot record wins over pilot_skills, as in the engine's loader.
        u["pilot_skills"] = []
        self.assertEqual(le.grid_defense(st), 22)
        del u["pilot"]
        u.update(pilot_level=1, pilot_skills=[f"skill1={le.GRID_SKILL}"])
        self.assertEqual(le.grid_defense(st), 22)
        st.pop("grid_defense_base")
        self.assertEqual(le.grid_defense(dict(st, difficulty=3)), 7)  # no base on Unfair
        self.assertEqual(le.grid_defense(dict(st, grid_defense=40)), 40)  # given in percent
        self.assertIsNone(le.grid_defense({"units": []}))

    def test_overpower_bonus_purchases_and_limits(self):
        self.assertEqual([le.overpower(0, n) for n in (1, 5, 6, 20, 30)], [2, 10, 11, 25, 25])
        self.assertEqual(le.overpower(9, 1), 11)
        c = le.parse_candidate("power:2")
        le.resolve_defense(c, {"grid_overflow": 4, "difficulty": 2})
        self.assertEqual(c.pp, 4)  # 4 -> 6 -> 8
        le.resolve_defense(c, {"grid_overflow": 4, "difficulty": 3})
        self.assertEqual(c.pp, 7)  # Unfair: 2 points a purchase, 4 -> 6 -> 8 -> 10 -> 11
        with self.assertRaises(le.CandidateError):
            le.resolve_defense(le.parse_candidate("power:1"), {"grid_overflow": 25, "difficulty": 2})
        c = le.parse_candidate("def:3")
        le.resolve_defense(c, {"grid_overflow": 22})
        self.assertEqual(c.pp, 3)
        with self.assertRaises(le.CandidateError):
            le.resolve_defense(le.parse_candidate("def:4"), {"grid_overflow": 22})
        with self.assertRaises(le.CandidateError):
            le.resolve_defense(le.parse_candidate("def:1"), {})  # no overpower bonus known

    def test_grid_defense_candidates_patch_the_board(self):
        self.record(state=grid_state(overflow=4))
        ts = le.find_turn_starts(lp.LIVE_ROOT, need_save=True)[0]
        items = tl.lo.Items(self.game / "scripts")
        before = le.read_state(ts.path)[0]
        _, info, st = le.apply_candidate(items, ts, le.parse_candidate("skills:0=Grid,Health@2"))
        u, b = le.mech_unit(st, 0), le.mech_unit(before, 0)
        self.assertEqual((u["pilot"]["level"], u["pilot"]["skill1"], u["pilot"]["skill2"], u["pilot"]["id"]),
                         (2, le.GRID_SKILL, 0, "Pilot_Miner"))
        self.assertEqual((u["pilot_level"], u["pilot_skills"]), (2, [f"skill1={le.GRID_SKILL}"]))
        self.assertEqual((u["hp"], u["max_hp"]), (b["hp"] + 2, b["max_hp"] + 2))  # Health learned
        self.assertEqual(info["gd"], [19, 22])
        self.assertEqual(st["grid_overflow"], 4)  # the engine adds the pilot itself
        cand = le.parse_candidate("def:3")
        le.resolve_defense(cand, before)
        _, info, st = le.apply_candidate(items, ts, cand)
        self.assertEqual((st["grid_overflow"], info["gd"]), (7, [19, 22]))
        # A state that gives the chance in percent: it changes instead.
        given = dict(grid_state(overflow=4), grid_defense=30)
        ts.path.write_text(json.dumps({"data": {"bridge_state": given}}))
        _, info, st = le.apply_candidate(items, ts, le.parse_candidate("skills:2=Grid,Move"))
        self.assertEqual((st["grid_defense"], info["gd"]), (33, [30, 33]))
        # A skill the pilot's Blacklist forbids.
        zoltan = grid_state()
        le.mech_unit(zoltan, 2)["pilot"]["id"] = "Pilot_Zoltan"
        ts.path.write_text(json.dumps({"data": {"bridge_state": zoltan}}))
        with self.assertRaises(le.CandidateError):
            le.apply_candidate(items, ts, le.parse_candidate("skills:2=Grid,Health"))

    def test_the_overpower_bonus_comes_from_the_save_when_the_state_lacks_it(self):
        st = squad_state()
        save = self.tmp / "saveData.lua"
        save.write_text('GameData = {["network"] = 7, ["overflow"] = 6, }\n')
        ts = le.TurnStart("r", 0, 1, self.tmp / "x.json", save)
        self.assertTrue(le.fill_grid_overflow(st, ts))
        self.assertEqual(st["grid_overflow"], 6)
        self.assertFalse(le.fill_grid_overflow(st, ts))  # it has one now

    def test_expected_grid_loss(self):
        # Grid 3, two rolls of 2 (the second would take only the last 1), 14%.
        self.assertAlmostEqual(le.expected_lost(3, [2, 2], 3, 0.14), 3 * 0.86 ** 2 + 2 * 2 * 0.14 * 0.86)
        # A loss without a roll stays.
        self.assertAlmostEqual(le.expected_lost(5, [1], 3, 0.25), 2 + 0.75)
        self.assertEqual(le.expected_lost(4, [], 0, 0.5), 0)
        reply = {"start": {"grid_power": 4}, "after_enemy": {"grid_power": 1},
                 "steps": [{"grid_defense": [{"amount": 1}]}, {}], "enemy_phase": {"grid_defense": [{"amount": 2}]},
                 "grid_defense": {"percent": 20}}
        e = le.expected_grid(reply, {})
        self.assertEqual((e["gd"], e["lost"], e["rolls"]), (20, 3, [1, 2]))
        self.assertAlmostEqual(e["expected"], 3 * 0.8)
        self.assertIsNone(le.expected_grid({"start": {"grid_power": 4}}, {}))  # the engine refused the plan

    def test_grid_defense_is_priced_by_expected_grid_and_ranked_below_proven_gains(self):
        for t in (1, 2):
            self.record(turn=t, state=grid_state(overflow=4))
        rc, report, out, eng = self.run_eval(
            ["--no-control", "--candidate", "move:0", "--candidate", "skills:1=Grid,Invulnerable",
             "--candidate", "skills:2=Grid,Invulnerable@0", "--candidate", "def:5",
             "--candidate", "0:Prime_Lasermech_A,*"], engine=GridEngine())
        self.assertEqual(rc, 0, out)
        res = {r["candidate"]: r for r in report["results"]}
        grid_pilot, defense, laser = res["skills:1=Grid,Invulnerable"], res["def:5"], res["0:Prime_Lasermech_A,*"]
        # The worst case cannot see it: no tier moved, the solver ranks it the same.
        self.assertEqual((grid_pilot["sums"], grid_pilot["better"], grid_pilot["worse"]), ({}, 0, 0))
        # Two resistible hits a turn at 19% -> 22%: 2 x 0.03 grid a turn saved.
        self.assertEqual(grid_pilot["gd"], [19, 22])
        self.assertAlmostEqual(grid_pilot["proven_expected"]["grid_saved"], 0.12)
        self.assertAlmostEqual(defense["proven_expected"]["grid_saved"], 0.2)
        self.assertEqual(defense["gd"], [19, 24])
        # Grid not learned yet (level 0): nothing, as "no change".
        self.assertEqual(res["skills:2=Grid,Invulnerable@0"]["expected"]["grid_saved"], 0)
        self.assertEqual(res["move:0"]["expected"]["grid_saved"], 0)
        # One hit fewer: the proven grid gain, and its expected value alike (0.81 a turn).
        self.assertEqual(laser["proven_sums"]["grid"], 2.0)
        self.assertAlmostEqual(laser["proven_expected"]["grid_saved"], 1.62)
        lines = out.splitlines()
        head = next(i for i, l in enumerate(lines) if l.startswith("candidate "))
        table = [l.split()[0] for l in lines[head + 1:] if l.strip()]
        self.assertIn("def:5: overpower bonus 4 -> 9 (+5% Grid Defense)", out)
        self.assertEqual(table[:3], ["0:Prime_Lasermech_A,*", "def:5", "skills:1=Grid,Invulnerable"])
        self.assertEqual(set(table[3:]), {"move:0", "skills:2=Grid,Invulnerable@0"})  # no change: a tie
        self.assertIn("19->22%", out)

    def test_a_grid_defense_purchase_past_the_cap_is_no_candidate(self):
        self.record(turn=1, state=grid_state(overflow=10))
        self.record(turn=2, state=grid_state(overflow=24))
        rc, report, out, _ = self.run_eval(["--no-control", "--candidate", "def:2", "--candidate", "power:1",
                                            "--candidate", "def:1"], engine=GridEngine())
        self.assertEqual(rc, 0, out)
        self.assertIn("skipped def:2: the overpower bonus caps at 25: 24 now, so def: at most 1", out)
        res = {r["candidate"]: r for r in report["results"]}
        self.assertEqual(set(res), {"power:1", "def:1"})
        self.assertEqual(res["power:1"]["gd"], [39, 40])  # 24 -> 25: one point left
        self.assertEqual(res["def:1"]["rows"][0]["gd"], [25, 26])  # the same +1 on the older turn

    def test_expected_values_rank_between_proven_gains_and_the_verdict(self):
        """A pure Grid Defense item (expected grid only) ranks below a proven
        grid gain and above a safer next turn alone and "no change"; a
        grid survival change counts as certain only when one side surely
        keeps the grid."""
        def result(text, rows):
            r = le.Result(le.parse_candidate(text))
            r.rows = rows
            return r
        row = dict(verdict=0, delta={}, proven=True, cores=0)
        gd = result("def:3", [dict(row, exp_saved=0.09)] * 4)
        grid = result("hp:2", [dict(row, verdict=1, delta={"grid": 1.0}, exp_saved=0.85)])
        safer = result("hp:0", [dict(row, verdict=1, delta={"mech_hp": -2.0})] * 6)
        same = result("hp:1", [dict(row)] * 6)
        odds = result("move:1", [dict(row, verdict=1, delta={"grid_survival": 0.04}, sure=0, odds=0.04)])
        sure = result("move:2", [dict(row, verdict=1, delta={"grid_survival": 0.74}, sure=1, odds=0.0)])
        ranked = sorted([same, safer, gd, grid, odds, sure], key=lambda r: r.rank_key(), reverse=True)
        self.assertEqual([r.cand.text for r in ranked], ["move:2", "hp:2", "move:1", "def:3", "hp:0", "hp:1"])
        self.assertEqual(le.survival_split({"grid_survival": 0.26}, {"grid_survival": 0.3}), (0, 0.04))
        self.assertEqual(le.survival_split({"grid_survival": 0.26}, {"grid_survival": 1.0}), (1, 0.0))
        self.assertEqual(le.survival_split({"grid_survival": 1.0}, {}), (0, 0.0))

    def test_refusals(self):
        rc, report, out, _ = self.run_eval(["--candidate", "hp:0"])  # no run recorded
        self.assertEqual(rc, 2)
        self.assertIn("REFUSED", out)
        self.record()
        rc, _, out, _ = self.run_eval(["--candidate", "0:A,B,C"])
        self.assertEqual(rc, 2)


if __name__ == "__main__":
    unittest.main()
