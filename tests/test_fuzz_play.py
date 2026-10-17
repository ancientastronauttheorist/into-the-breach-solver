"""scripts/fuzz_play.py against the fake bridge and engine of
tests/test_live_play.py. Nothing here touches the game or the desktop:
clicks and Esc go to a recorder (FakeGame), the game process functions
(running, quit, launch) are FakeGame's or refuse (test_live_play._never).

    python3 -m unittest tests.test_fuzz_play
"""

from __future__ import annotations

import contextlib
import copy
import io
import json
import random
import shutil
import tempfile
import threading
import time
import unittest
from pathlib import Path

from tests import test_live_play as tlp  # sets ITB_BRIDGE_DIR to a temp dir first
from tests import test_loadout as tl  # the save fragment and fake game scripts

import fuzz_play as fz  # noqa: E402  (scripts/ is on sys.path via test_live_play)

lp, lv = tlp.lp, tlp.lv
lp.CONTINUE_RETRY = 0.5
fz.END_CONTINUE_DELAY = 0.0
fz.END_WAIT = 0.5
fz.END_SETTLE = 0.0
fz.MAP_CLICK_WAIT = 0.05


def to_step(a: dict, i: int) -> dict:
    """A plan action (one sub-action) as the step itb_live predicts for it."""
    if a["kind"] == "none":
        return {"sub": "move", "uid": a["uid"], "to": a["move"], "action": i}
    return {"sub": a["kind"], "uid": a["uid"], "weapon": a.get("weapon"), "target": a.get("target"),
            "target2": a.get("target2"), "action": i}


class FuzzEngine(tlp.FakeEngine):
    """The fake engine with itb_live's "actions": every candidate of every
    team-1 unit, whatever it already did (the fuzzer's UI filter must sort
    them out), with an effect the fuzzer weights. `predict` plays the plan
    it is given. `chance` marks steps with a chance node; `grid_loss` makes
    the named weapon cost that much grid."""

    def __init__(self, candidates, chance=(), grid_loss=None, **kw):
        super().__init__(**kw)
        self.candidates = candidates
        self.chance = set(chance)
        self.grid_loss = grid_loss or {}
        self.action_requests = []

    def _play(self, st: dict, plan: list) -> dict:
        b = copy.deepcopy(st)
        for i, a in enumerate(plan):
            tlp.apply(b, to_step(a, i))
            b["grid_power"] -= self.grid_loss.get(a.get("weapon"), 0)
        return b

    def actions(self, path, plan, effects=True):
        self.action_requests.append(list(plan))
        b = self._play(self._state(path), plan)
        out = []
        for c in self.candidates:
            u = next((v for v in b["units"] if v["uid"] == c["uid"]), None)
            if u is None or (c["sub"] == "move" and list(c["target"]) == [u["x"], u["y"]]):
                continue
            a = dict(c, slot=0 if c["sub"] == "weapon" else -1, target2=None, end=c["target"])
            a["effect"] = {"hp": {"player": 0, "enemy": 1 if c["sub"] == "weapon" else 0, "neutral": 0},
                           "kills": {}, "grid": self.grid_loss.get(c.get("weapon"), 0)}
            out.append(a)
        return {"ok": True, "refused": -1, "board": self._board(b), "actions": out, "rejected": {"no effect": 1}}

    def predict(self, path, plan):
        self.predicts.append(plan)
        st = self._state(path)
        self.predict_states.append(st)
        steps = [to_step(a, i) for i, a in enumerate(plan)]
        out = self._simulate(st, steps)
        b = self._play(st, plan)
        for s, a in zip(out["steps"], plan):
            if a.get("weapon") in self.chance:
                s["chance_nodes"] = 1
        out["after_enemy"]["grid_power"] = b["grid_power"]
        for k, s in enumerate(out["steps"]):
            s["board"]["grid_power"] = self._play(st, plan[:k + 1])["grid_power"]
        return out


CANDIDATES = [
    {"sub": "move", "uid": 0, "weapon": "Move", "target": [3, 4], "target_kind": "road"},
    {"sub": "weapon", "uid": 0, "weapon": "Prime_Lasermech_A", "target": [3, 3], "target_kind": "unit_enemy"},
    {"sub": "repair", "uid": 0, "weapon": "Skill_Repair", "target": [6, 6], "target_kind": "unit_player"},
    {"sub": "weapon", "uid": 1, "weapon": "Brute_Tankmech", "target": [3, 3], "target_kind": "unit_enemy"},
]


def args_for(extra=()) -> object:
    return fz.build_parser().parse_args(["--seed", "3", "--stop-prob", "0", *extra])


class FuzzTest(unittest.TestCase):
    def setUp(self):
        # test_live_play's tearDownModule removes the fake bridge directory
        # when both modules run in one process.
        lv.BRIDGE.mkdir(parents=True, exist_ok=True)
        self.tmp = Path(tempfile.mkdtemp(prefix="itb_fuzz_play_"))
        self._roots = (lp.LIVE_ROOT, lp.CURRENT, lp.PROFILE, lp.RESTORE_BACKUPS, lp.game_running)
        lp.LIVE_ROOT = self.tmp / "live"
        lp.CURRENT = self.tmp / "current.json"
        lp.PROFILE = self.tmp / "profile_Alpha"
        lp.RESTORE_BACKUPS = self.tmp / "restore_backups"
        lp.game_running = lambda: True
        self.fb = None
        self.game = None
        self.undo = None
        self.clicks = []

    def tearDown(self):
        if self.fb:
            self.fb.close()
        if self.game:
            self.game.close()
        if self.undo:
            self.undo()
        lp.LIVE_ROOT, lp.CURRENT, lp.PROFILE, lp.RESTORE_BACKUPS, lp.game_running = self._roots
        shutil.rmtree(self.tmp, ignore_errors=True)

    # ------------------------------------------------------------ recordings

    def record(self, run="20261016_000000", mission=0, turn=1, state=None, save=True) -> Path:
        d = lp.LIVE_ROOT / run
        d.mkdir(parents=True, exist_ok=True)
        st = state or tlp.base_state()
        st["turn"] = turn
        p = d / f"m{mission:02d}_turn_{turn:02d}_solve_input.json"
        p.write_text(json.dumps({"run_id": run, "label": "solve_input", "data": {"bridge_state": st}}))
        if save:
            (d / f"m{mission:02d}_turn_{turn:02d}_saveData.lua").write_text(f"-- checkpoint {mission} {turn}")
            (d / f"m{mission:02d}_turn_{turn:02d}_undoSave.lua").write_text(f"-- undo {mission} {turn}")
        return p

    def test_finds_recorded_turn_starts_and_their_checkpoints(self):
        self.record(turn=1)
        self.record(turn=2, save=False)
        self.record(run="20261016_111111", mission=3, turn=2)
        starts = fz.find_turn_starts(lp.LIVE_ROOT)
        self.assertEqual([s.key for s in starts], ["20261016_000000/m00/t01", "20261016_000000/m00/t02",
                                                   "20261016_111111/m03/t02"])
        self.assertIsNone(starts[1].save)
        with_saves = fz.find_turn_starts(lp.LIVE_ROOT, need_save=True)
        self.assertEqual(len(with_saves), 2)
        self.assertEqual(len(fz.find_turn_starts(lp.LIVE_ROOT, runs=["20261016_111111"])), 1)

    def test_last_turns_are_left_out_unless_asked(self):
        st = tlp.base_state()
        st["turn"], st["total_turns"] = 5, 5
        self.assertTrue(fz.playable(st))
        self.assertFalse(fz.playable(st, last_turns=False))
        st["turn"] = 4
        self.assertTrue(fz.playable(st, last_turns=False))

    def test_recorded_carry(self):
        st = tlp.base_state()
        st["units"].append({"uid": 300, "type": "Burrower1", "x": 1, "y": 1, "hp": 2, "max_hp": 3, "team": 6,
                            "underground": True})
        st["reactivation_visible_list"] = [100]
        carry = fz.recorded_carry(st, {"patches": {"tile_shields": [[2, 2]]}})
        self.assertEqual(sorted(carry.underground), [300])
        self.assertEqual(carry.shields, [[2, 2]])
        self.assertEqual(carry.reactivation, [100])

    # ------------------------------------------------------------ choosing

    def test_ui_limits_filter_the_candidates(self):
        a_move = {"uid": 0, "sub": "move"}
        a_shot = {"uid": 0, "sub": "weapon"}
        self.assertTrue(fz.ui_allows(a_move, {}, {0: None}))
        self.assertTrue(fz.ui_allows(a_shot, {0: "M"}, {0: None}))
        self.assertFalse(fz.ui_allows(a_move, {0: "MA"}, {0: None}))
        self.assertFalse(fz.ui_allows(a_shot, {0: "A"}, {0: None}))
        self.assertTrue(fz.ui_allows(a_move, {0: "MA"}, {0: "Shifty"}))  # Sidestep: a move after acting
        self.assertFalse(fz.ui_allows(a_shot, {0: "MA"}, {0: "Shifty"}))
        self.assertTrue(fz.ui_allows(a_shot, {0: "A"}, {0: "Double_Shot"}))  # two shots without moving
        self.assertFalse(fz.ui_allows(a_shot, {0: "MA"}, {0: "Double_Shot"}))
        self.assertFalse(fz.ui_allows(a_move, {0: "AA"}, {0: "Double_Shot"}))

    def test_an_unpowered_pilot_skill_gives_no_extra_action(self):
        st = tlp.base_state()
        tlp.unit(st, 0)["pilot"] = {"id": "Pilot_Miner", "power": [0, 0]}
        self.assertIsNone(fz.unit_skills(st)[0])
        tlp.unit(st, 0)["pilot"] = {"id": "Pilot_Miner", "power": [1, 1]}
        self.assertEqual(fz.unit_skills(st)[0], "Double_Shot")

    def test_odd_actions_weigh_more(self):
        plain = {"uid": 0, "sub": "weapon", "weapon": "W", "target_kind": "road",
                 "effect": {"hp": {"enemy": 1}, "kills": {}}}
        friendly = dict(plain, target_kind="unit_player", effect={"hp": {"player": 2}, "kills": {}, "bumps": 1})
        lua = dict(plain, effect={"hp": {"enemy": 1}, "kills": {}, "unapplied_lua_writes": 1})
        nothing = dict(plain, effect={"hp": {}, "kills": {}})
        self.assertGreater(fz.interest(friendly), fz.interest(plain))
        self.assertGreater(fz.interest(lua), fz.interest(plain))
        self.assertLess(fz.interest(nothing), fz.interest(plain))
        self.assertIn("friendly_damage", fz.effect_tags(friendly))
        chooser = fz.Chooser(random.Random(1))
        for _ in range(50):
            self.assertIn(chooser.choose([plain, friendly, lua, nothing]), [plain, friendly, lua, nothing])
        self.assertTrue(all(chooser.weight(a) > 0 for a in (plain, friendly, lua, nothing)))

    def test_generate_plays_only_what_the_ui_allows(self):
        path = self.record()
        eng = FuzzEngine(CANDIDATES)
        for seed in range(20):
            rng = random.Random(seed)
            seq = fz.generate(eng, path, fz.Chooser(rng), rng, {}, {0: None, 1: None}, max_steps=10, stop_prob=0)
            seqs = {}
            for a in seq.plan:
                seqs[a["uid"]] = seqs.get(a["uid"], "") + ("M" if a["kind"] == "none" else "A")
            for uid, s in seqs.items():
                self.assertTrue(lp.ui_sequence_ok(s, None), (seed, uid, s))
            self.assertEqual(len(seq.boards), len(seq.plan) + 1)
            # Every unit has done all it may: nothing is left to choose.
            self.assertEqual(set(seqs), {0, 1})

    def test_generate_keeps_the_grid_floor(self):
        path = self.record()
        eng = FuzzEngine(CANDIDATES, grid_loss={"Brute_Tankmech": 6})
        for seed in range(10):
            rng = random.Random(seed)
            seq = fz.generate(eng, path, fz.Chooser(rng), rng, {}, {}, 10, 0, grid_floor=2)
            self.assertNotIn("Brute_Tankmech", [a.get("weapon") for a in seq.plan])

    def test_grid_safe_needs_a_living_mech(self):
        """Fuzz 20261016_230406 sample 10: a plan that leaves no mech after
        the enemy phase never gets a player turn back."""
        mech = {"uid": 0, "mech": True, "hp": 1}
        vek = {"uid": 9, "mech": False, "hp": 3}
        pred = {"refused": -1, "steps": [{"board": {"grid_power": 5, "units": [mech, vek]}}],
                "after_enemy": {"grid_power": 5, "units": [mech, vek]}}
        self.assertTrue(fz.grid_safe(pred, 2))
        pred["after_enemy"]["units"] = [vek]  # the engine's boards omit the dead
        self.assertFalse(fz.grid_safe(pred, 2))
        pred["after_enemy"]["units"] = [mech | {"hp": 0}, vek]
        self.assertFalse(fz.grid_safe(pred, 2))
        pred["after_enemy"]["units"] = [mech]
        pred["steps"][0]["board"]["units"] = [vek]  # all dead in the player's turn
        self.assertFalse(fz.grid_safe(pred, 2))
        pred["steps"][0]["board"] = {"grid_power": 5}  # a board without units: no say
        self.assertTrue(fz.grid_safe(pred, 2))
        pred["after_enemy"]["grid_power"] = 1
        self.assertFalse(fz.grid_safe(pred, 2))

    def test_engine_checks_catch_a_board_the_prediction_does_not_reach(self):
        path = self.record()
        eng = FuzzEngine(CANDIDATES)
        rng = random.Random(0)
        seq = fz.generate(eng, path, fz.Chooser(rng), rng, {}, {}, 4, 0)
        pred = eng.predict(path, seq.plan)
        self.assertEqual(fz.engine_checks(seq, pred), [])
        seq.boards[1] = copy.deepcopy(seq.boards[1])
        seq.boards[1]["units"][0]["hp"] += 1
        self.assertTrue(fz.engine_checks(seq, pred))
        pred["refused"] = 0
        self.assertTrue(any("refused" in p for p in fz.engine_checks(seq, pred)))

    def test_dry_run_writes_samples_and_never_acts(self):
        self.record()
        self.record(turn=2)
        eng = FuzzEngine(CANDIDATES)
        out = self.tmp / "fuzz"
        rc = fz.main(["--dry-run", "--recordings", str(lp.LIVE_ROOT), "--samples", "4", "--seed", "1",
                      "--out", str(out)], engine=eng)
        self.assertEqual(rc, 0)
        sess = next(out.iterdir())
        samples = [json.loads(line) for line in (sess / "samples.jsonl").read_text().splitlines()]
        self.assertEqual(len(samples), 4)
        self.assertTrue(all(s["problems"] == [] for s in samples))
        meta = json.loads((sess / "session.json").read_text())
        self.assertTrue(meta["dry_run"])
        self.assertEqual(meta["samples"], 4)
        self.assertFalse((sess / "findings.jsonl").exists())
        self.assertFalse(lv.CMD.exists())

    def test_unplayable_says_why(self):
        st = tlp.base_state()
        st["turn"], st["total_turns"] = 5, 5
        self.assertEqual(fz.unplayable(st, last_turns=False)[0], "last_turn")
        self.assertIsNone(fz.unplayable(st))
        st["phase"] = "combat_enemy"
        self.assertEqual(fz.unplayable(st)[0], "not_player_turn")
        st = tlp.base_state()
        for u in st["units"]:
            if u.get("mech"):
                u["active"] = False
        self.assertEqual(fz.unplayable(st)[0], "no_active_mech")

    def test_starts_are_drawn_without_replacement(self):
        starts = [fz.TurnStart("r", 0, t, Path(f"m00_turn_{t:02d}_solve_input.json")) for t in range(1, 6)]
        pool = fz.StartPool(starts, random.Random(4))
        first = [pool.draw().key for _ in range(5)]
        self.assertEqual(sorted(first), sorted(ts.key for ts in starts))
        self.assertEqual(pool.passes, 1)
        pool.reject(starts[0], "no")
        second = [pool.draw().key for _ in range(4)]
        self.assertEqual(sorted(second), sorted(ts.key for ts in starts[1:]))
        self.assertEqual(pool.passes, 2)
        for ts in starts[1:]:
            pool.reject(ts, "no")
        self.assertIsNone(pool.draw())
        # The one named: every time, until rejected.
        named = fz.StartPool(starts[:1], random.Random(0), starts[0])
        self.assertEqual([named.draw().key for _ in range(3)], [starts[0].key] * 3)
        named.reject(starts[0], "the mission's last turn")
        self.assertIsNone(named.draw())
        self.assertIn("last turn", named.exhausted())

    def test_dry_run_draws_each_start_once_per_pass(self):
        self.record()
        self.record(turn=2)
        st = tlp.base_state()
        st["total_turns"] = 3
        self.record(turn=3, state=st)  # the mission's last turn: never drawn as a sample
        out = self.tmp / "fuzz"
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = fz.main(["--dry-run", "--recordings", str(lp.LIVE_ROOT), "--samples", "4", "--seed", "1",
                          "--out", str(out)], engine=FuzzEngine(CANDIDATES))
        self.assertEqual(rc, 0)
        sess = next(out.iterdir())
        sources = [json.loads(line)["source"] for line in (sess / "samples.jsonl").read_text().splitlines()]
        self.assertEqual(len(sources), 4)
        self.assertEqual(len(set(sources[:2])), 2)
        self.assertEqual(len(set(sources[2:])), 2)
        self.assertNotIn("20261016_000000/m00/t03", sources)
        self.assertIn("drawing them again", buf.getvalue())
        meta = json.loads((sess / "session.json").read_text())
        self.assertEqual(meta["outcomes"]["skipped_last_turn"], 1)
        self.assertIn("20261016_000000/m00/t03", meta["rejected_starts"])

    # ------------------------------------------------------------ safety

    def test_other_drivers_are_found_but_not_ourselves_or_dry_runs(self):
        lines = ["  1     0 /sbin/launchd",
                 " 50     1 /bin/zsh -c python3 scripts/fuzz_play.py --samples 3",
                 " 51    50 python3 scripts/fuzz_play.py --samples 3",
                 " 60     1 python3 scripts/live_play.py mission --max-turns 8",
                 " 61     1 python3 scripts/live_play.py turn --dry-run",
                 " 62     1 /bin/bash /tmp/x/scratchpad/drive2.sh run40",
                 " 63     1 python3 scripts/live_play.py status"]
        found = fz.match_drivers(lines, 51)
        self.assertEqual([f.split()[0] for f in found], ["60", "62"])

    def test_the_driver_lock_keeps_live_play_out(self):
        self.fb = tlp.FakeBridge(tlp.base_state())
        lock = lp.DriverLock("fuzz test").acquire()
        try:
            with self.assertRaises(lp.Refused):
                lp.DriverLock("other").acquire()
            rc = lp.main(["turn", "--new-run"], engine=tlp.FakeEngine(), click=lambda xy: True)
            self.assertEqual(rc, 2)
            self.assertEqual([c for c in self.fb.commands if c.split()[0] not in ("SNAPSHOT", "LUA")], [])
            # A dry run only reads: it does not need the lock.
            self.assertEqual(lp.main(["turn", "--dry-run", "--new-run"], engine=tlp.FakeEngine()), 0)
        finally:
            lock.release()
        lp.DriverLock("after").acquire().release()

    def test_live_refuses_while_another_driver_runs(self):
        self.record()
        orig = fz.other_drivers
        fz.other_drivers = lambda own_pid=None: ["60 python3 scripts/live_play.py mission"]
        try:
            rc = fz.main(["--recordings", str(lp.LIVE_ROOT), "--out", str(self.tmp / "fuzz")], engine=FuzzEngine([]))
        finally:
            fz.other_drivers = orig
        self.assertEqual(rc, 2)
        self.assertFalse((self.tmp / "fuzz").exists())

    # ------------------------------------------------------------ restore by relaunch

    def fake_game(self, own_board="mission", quirk_sticks=False, own_save=True) -> tlp.FakeGame:
        """The player's own run in the profile (in a mission, or on the
        island map: own_board "island"), a checkpoint recorded, the game
        running that run."""
        self.record()
        lp.PROFILE.mkdir(parents=True)
        if own_save:
            (lp.PROFILE / "saveData.lua").write_text("-- the player's own run")
        (lp.PROFILE / "undoSave.lua").write_text("-- the player's own undo")
        own = copy.deepcopy(tlp.base_state())
        own["mission_id"], own["turn"] = "Mission_Own", 3
        boards = {"-- checkpoint 0 1": dict(tlp.base_state(), turn=1),
                  "-- the player's own run": own if own_board == "mission" else None}
        game = tlp.FakeGame(boards, "-- the player's own run", quirk_sticks=quirk_sticks)
        self.game = game
        self.undo = game.install()
        return game

    def ctx_for(self, game, extra=()):
        args = args_for(list(extra))
        sess = fz.Session(args, self.tmp / "fuzz")
        ctx = lp.Context(args, bridge=None, engine=None, run=None, click=game.click, escape=game.escape)
        return args, sess, ctx

    def test_restore_relaunches_with_the_checkpoint_and_loads_it_twice(self):
        # Live 2026-10-16: Continue after Main Menu resumed the run in
        # memory, not the swapped save; the game reads the saves at launch.
        game = self.fake_game()
        ts = fz.find_turn_starts(lp.LIVE_ROOT)[0]
        _, sess, ctx = self.ctx_for(game)
        state = fz.restore_by_relaunch(ctx, sess, ts)
        self.assertEqual((state["mission_id"], state["turn"]), ("Mission_Test", 1))
        self.assertEqual(game.events, ["quit", "launch", lp.TITLE_CONTINUE, lp.PAUSE_GEAR, lp.PAUSE_MAIN_MENU,
                                       lp.TITLE_CONTINUE])
        self.assertEqual(game.misclicks(), [])
        self.assertEqual((state["grid_defense_base"], state["grid_base_difficulty"]), (0, 3))
        self.assertEqual((lp.PROFILE / "saveData.lua").read_text(), "-- checkpoint 0 1")
        self.assertEqual((lp.PROFILE / "undoSave.lua").read_text(), "-- undo 0 1")
        self.assertEqual((sess.backup / "saveData.lua").read_text(), "-- the player's own run")
        # A second sample keeps the first backup.
        fz.restore_by_relaunch(ctx, sess, ts)
        self.assertEqual((sess.backup / "saveData.lua").read_text(), "-- the player's own run")
        # The end: the player's own run back, resumed in its mission, loaded twice.
        game.events.clear()
        self.assertEqual(fz.put_back(ctx, sess), [])
        self.assertEqual(game.events, ["quit", "launch", lp.TITLE_CONTINUE, lp.PAUSE_GEAR, lp.PAUSE_MAIN_MENU,
                                       lp.TITLE_CONTINUE])
        self.assertEqual((lp.PROFILE / "saveData.lua").read_text(), "-- the player's own run")
        self.assertEqual((lp.PROFILE / "undoSave.lua").read_text(), "-- the player's own undo")
        st = lp.Bridge().raw_state()
        self.assertEqual((st["mission_id"], st["grid_defense_base"]), ("Mission_Own", 0))

    def test_the_second_load_falls_back_to_esc(self):
        # The live 2026-10-16 case the other way round: when the gear does
        # not open the pause menu, Esc does.
        game = self.fake_game()
        game.gear_works = False
        ts = fz.find_turn_starts(lp.LIVE_ROOT)[0]
        _, sess, ctx = self.ctx_for(game)
        fz.restore_by_relaunch(ctx, sess, ts)
        self.assertEqual(game.events, ["quit", "launch", lp.TITLE_CONTINUE, lp.PAUSE_GEAR, "esc",
                                       lp.PAUSE_MAIN_MENU, lp.TITLE_CONTINUE])

    def test_the_second_load_stops_when_the_pause_menu_does_not_open(self):
        game = self.fake_game()
        game.gear_works = game.esc_works = False
        ts = fz.find_turn_starts(lp.LIVE_ROOT)[0]
        _, sess, ctx = self.ctx_for(game)
        with self.assertRaisesRegex(lp.Anomaly, "neither the gear nor Esc"):
            fz.restore_by_relaunch(ctx, sess, ts)
        self.assertNotIn(lp.PAUSE_MAIN_MENU, game.events)

    def test_restore_stops_on_a_grid_defense_base_still_wrong(self):
        game = self.fake_game(quirk_sticks=True)
        ts = fz.find_turn_starts(lp.LIVE_ROOT)[0]
        _, sess, ctx = self.ctx_for(game)
        with self.assertRaisesRegex(lp.Anomaly, "Grid Defense base 15"):
            fz.restore_by_relaunch(ctx, sess, ts)

    def test_restore_refuses_a_board_that_differs_from_the_checkpoint(self):
        game = self.fake_game()
        other = dict(tlp.base_state(), turn=1)
        other["units"] = copy.deepcopy(other["units"])
        tlp.unit(other, 100)["hp"] = 1
        game.boards["-- checkpoint 0 1"] = other
        ts = fz.find_turn_starts(lp.LIVE_ROOT)[0]
        _, sess, ctx = self.ctx_for(game)
        with self.assertRaisesRegex(lp.Anomaly, "differs"):
            fz.restore_by_relaunch(ctx, sess, ts)

    def test_put_back_on_the_island_map_loads_once_more_by_the_map_gear(self):
        # The own run resumes on the island map (the bridge is silent there,
        # and Esc does nothing): one Continue, then the second load blind,
        # by the map's gear, MAIN MENU and Continue.
        game = self.fake_game(own_board="island")
        ts = fz.find_turn_starts(lp.LIVE_ROOT)[0]
        _, sess, ctx = self.ctx_for(game)
        fz.restore_by_relaunch(ctx, sess, ts)
        game.events.clear()
        notes = fz.put_back(ctx, sess)
        self.assertEqual(game.events, ["quit", "launch", lp.TITLE_CONTINUE, lp.MAP_GEAR, lp.PAUSE_MAIN_MENU,
                                       lp.TITLE_CONTINUE])
        self.assertEqual((game.screen, game.loads), ("island", 2))
        self.assertTrue(notes and "island map" in notes[0] and "blind" in notes[0])
        self.assertEqual(game.misclicks(), [])

    def test_put_back_on_the_island_map_with_no_click_asks(self):
        game = self.fake_game(own_board="island")
        ts = fz.find_turn_starts(lp.LIVE_ROOT)[0]
        _, sess, ctx = self.ctx_for(game)
        fz.restore_by_relaunch(ctx, sess, ts)
        game.events.clear()
        ctx.args.no_click = True
        notes = fz.put_back(ctx, sess)
        self.assertEqual(game.events, ["quit", "launch"])
        self.assertTrue(notes and "by hand" in notes[0])

    def test_put_back_without_a_run_leaves_the_title_screen(self):
        # The previous run was lost: the profile has only undoSave.lua.
        game = self.fake_game(own_save=False)
        ts = fz.find_turn_starts(lp.LIVE_ROOT)[0]
        _, sess, ctx = self.ctx_for(game)
        fz.restore_by_relaunch(ctx, sess, ts)
        game.events.clear()
        fz.put_back(ctx, sess)
        self.assertEqual(game.events, ["quit", "launch"])
        self.assertFalse((lp.PROFILE / "saveData.lua").exists())
        self.assertEqual((lp.PROFILE / "undoSave.lua").read_text(), "-- the player's own undo")

    def test_put_back_cli_takes_a_backup_without_savedata(self):
        game = self.fake_game()
        session = self.tmp / "fuzz" / "old"
        (session / "profile_backup").mkdir(parents=True)
        (session / "profile_backup" / "undoSave.lua").write_text("-- undo only")
        rc = fz.main(["--put-back", str(session), "--dry-run"], click=game.click, escape=game.escape)
        self.assertEqual((rc, game.events), (0, []))
        self.assertEqual((lp.PROFILE / "saveData.lua").read_text(), "-- the player's own run")
        orig = fz.other_drivers
        fz.other_drivers = lambda own_pid=None: []  # whatever runs on this machine
        try:
            rc = fz.main(["--put-back", str(session)], click=game.click, escape=game.escape)
        finally:
            fz.other_drivers = orig
        self.assertEqual(rc, 0)
        self.assertEqual(game.events, ["quit", "launch"])
        self.assertFalse((lp.PROFILE / "saveData.lua").exists())
        self.assertEqual((lp.PROFILE / "undoSave.lua").read_text(), "-- undo only")
        self.assertEqual(fz.main(["--put-back", str(self.tmp / "nothing")]), 2)

    def test_a_live_session_restores_plays_and_puts_back(self):
        game = self.fake_game()
        args = args_for(["--samples", "1", "--recordings", str(lp.LIVE_ROOT)])
        sess = fz.Session(args, self.tmp / "fuzz")
        starts = fz.find_turn_starts(lp.LIVE_ROOT, need_save=True)
        rc = fz.live(args, FuzzEngine(CANDIDATES), sess, starts, click=game.click, escape=game.escape)
        self.assertEqual(rc, 0)
        self.assertEqual(sess.outcomes["matched"], 1)
        self.assertEqual(game.misclicks(), [])
        self.assertEqual(game.events.count("quit"), 2)  # the sample, then the put-back
        self.assertIn("END_TURN", game.fb.commands)
        self.assertEqual((lp.PROFILE / "saveData.lua").read_text(), "-- the player's own run")
        self.assertEqual(lp.Bridge().raw_state()["mission_id"], "Mission_Own")
        lp.DriverLock("after").acquire().release()

    def test_unplayable_starts_are_never_restored(self):
        # Live 2026-10-16: a mission's last turn cost a relaunch, then was
        # drawn again. Now it is skipped from the recording, before any
        # restore, and the playable start is not drawn twice in one pass.
        game = self.fake_game()
        st = tlp.base_state()
        st["total_turns"] = 2
        self.record(turn=2, state=st)  # the mission's last turn
        low = tlp.base_state()
        low["grid_power"] = 1
        self.record(mission=1, turn=1, state=low)  # no sequence keeps --min-grid 2
        args = args_for(["--samples", "2", "--recordings", str(lp.LIVE_ROOT)])
        sess = fz.Session(args, self.tmp / "fuzz")
        starts = fz.find_turn_starts(lp.LIVE_ROOT, need_save=True)
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = fz.live(args, FuzzEngine(CANDIDATES), sess, starts, click=game.click, escape=game.escape)
        self.assertEqual(rc, 0)
        self.assertEqual((sess.outcomes["skipped_last_turn"], sess.outcomes["skipped_grid_unsafe"]), (1, 1))
        self.assertEqual(sess.outcomes["matched"], 2)
        # Two samples of the one playable start (a second pass, said so), then the put-back.
        self.assertEqual(game.events.count("launch"), 3)
        self.assertIn("drawing them again", buf.getvalue())
        self.assertIn("not restored: the mission's last turn", buf.getvalue())
        self.assertEqual(game.misclicks(), [])

    def test_a_start_unplayable_after_the_restore_is_not_restored_again(self):
        game = self.fake_game()
        game.boards["-- checkpoint 0 1"] = dict(tlp.base_state(), turn=1, phase="combat_enemy")
        args = args_for(["--samples", "3", "--recordings", str(lp.LIVE_ROOT)])
        sess = fz.Session(args, self.tmp / "fuzz")
        starts = fz.find_turn_starts(lp.LIVE_ROOT, need_save=True)
        orig = lp.fresh_player_turn
        lp.fresh_player_turn = lambda bridge, since: lp.fresh_mission(bridge, since)
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                rc = fz.live(args, FuzzEngine(CANDIDATES), sess, starts, click=game.click, escape=game.escape)
        finally:
            lp.fresh_player_turn = orig
        self.assertEqual(rc, 0)
        self.assertEqual(sess.outcomes["not_playable"], 1)
        self.assertEqual(game.events.count("launch"), 2)  # one sample, then the put-back
        self.assertIn("after the restore", sess.pool.bad["20261016_000000/m00/t01"])

    def test_a_failed_restore_stops_and_still_puts_back(self):
        game = self.fake_game(quirk_sticks=True)
        args = args_for(["--samples", "3", "--recordings", str(lp.LIVE_ROOT)])
        sess = fz.Session(args, self.tmp / "fuzz")
        starts = fz.find_turn_starts(lp.LIVE_ROOT, need_save=True)
        rc = fz.live(args, FuzzEngine(CANDIDATES), sess, starts, click=game.click, escape=game.escape)
        self.assertEqual(rc, 1)
        self.assertEqual(sess.outcomes["restore_failed"], 1)
        self.assertEqual(game.events.count("launch"), 2)  # one sample, then the put-back
        self.assertEqual((lp.PROFILE / "saveData.lua").read_text(), "-- the player's own run")

    # ------------------------------------------------------------ live turns (fake bridge)

    def live_session(self, eng, extra=(), next_turn=None, state=None, checkpoint=None):
        self.record(state=copy.deepcopy(state) if state else None)
        if checkpoint is not None:
            (lp.LIVE_ROOT / "20261016_000000" / "m00_turn_01_saveData.lua").write_text(checkpoint)
        self.fb = tlp.FakeBridge(copy.deepcopy(state) if state else tlp.base_state())
        if next_turn:
            self.fb.next_turn = next_turn
        args = args_for(["--samples", "1", *extra])
        sess = fz.Session(args, self.tmp / "fuzz")
        starts = fz.find_turn_starts(lp.LIVE_ROOT, need_save=True)

        def click(xy):
            self.clicks.append(xy)
            if xy == lv.END_TURN_SCREEN:
                threading.Thread(target=self.fb.advance, daemon=True).start()
            return True

        orig = fz.restore_by_relaunch
        fz.restore_by_relaunch = lambda ctx, s, ts, ls=None: ctx.bridge.settled_state()
        try:
            rc = fz.live(args, eng, sess, starts, click=click)
        finally:
            fz.restore_by_relaunch = orig
        findings = []
        if (sess.dir / "findings.jsonl").exists():
            findings = [json.loads(line) for line in (sess.dir / "findings.jsonl").read_text().splitlines()]
        return rc, sess, findings

    def acting(self):
        return [c for c in self.fb.commands if c.split()[0] not in ("SNAPSHOT", "LUA", "EXEC_MODE")]

    def test_a_live_turn_that_matches_ends_the_turn_and_compares(self):
        eng = FuzzEngine(CANDIDATES)
        rc, sess, findings = self.live_session(eng)
        self.assertEqual(rc, 0)
        self.assertEqual(findings, [])
        acts = self.acting()
        self.assertEqual(acts[-1], "END_TURN")
        self.assertTrue(any(c.startswith("ATTACK") for c in acts))
        self.assertIn("EXEC_MODE native", self.fb.commands)
        self.assertEqual(sess.outcomes["matched"], 1)
        sample = sess.dir / "s000"
        self.assertTrue((sample / "plan.json").exists())
        self.assertTrue(list(sample.glob("m00_turn_02_after_enemy_*.json")))
        # The lock is free again.
        lp.DriverLock("after").acquire().release()

    def test_a_step_difference_is_a_finding_and_the_turn_is_not_ended(self):
        eng = FuzzEngine([c for c in CANDIDATES if c["uid"] == 1])
        orig_init = tlp.FakeBridge.__init__

        def init(fb, state):
            orig_init(fb, state)
            fb.damage = 2  # the game hits harder than the engine predicts

        tlp.FakeBridge.__init__ = init
        try:
            rc, sess, findings = self.live_session(eng)
        finally:
            tlp.FakeBridge.__init__ = orig_init
        self.assertEqual([f["kind"] for f in findings], ["step"])
        f = findings[0]
        self.assertTrue(any("hp: engine 2, game 1" in d for d in f["diffs"]))
        self.assertEqual(f["plan"][-1]["weapon"], "Brute_Tankmech")
        self.assertTrue(Path(f["files"]["plan"]).exists())
        self.assertIn("itb_live predict", f["replay"])
        self.assertNotIn("END_TURN", self.acting())
        self.assertEqual(sess.outcomes["diverged"], 1)

    def test_a_difference_after_a_chance_node_resyncs_and_goes_on(self):
        # Two shots at the Scorpion (3 HP), each with a chance node in the
        # prediction; the game hits for 2, the engine for 1. The first shot
        # differs (a chance finding); re-predicted from the live board (1 HP
        # left), the second kills it in both, and the turn ends.
        shots = [c for c in CANDIDATES if c["sub"] == "weapon"]
        eng = FuzzEngine(shots, chance={"Prime_Lasermech_A", "Brute_Tankmech"})
        orig_init = tlp.FakeBridge.__init__

        def init(fb, state):
            orig_init(fb, state)
            fb.damage = 2

        tlp.FakeBridge.__init__ = init
        try:
            rc, sess, findings = self.live_session(eng)
        finally:
            tlp.FakeBridge.__init__ = orig_init
        self.assertEqual([f["kind"] for f in findings], ["chance"])
        self.assertEqual(len(findings[0]["plan"]), 1)
        # The check before the restore, the plan, the rest from the live board, End Turn.
        self.assertEqual(len(eng.predicts), 4)
        self.assertEqual(len(eng.predicts[2]), 1)
        self.assertEqual(self.acting()[-1], "END_TURN")
        self.assertEqual(sess.outcomes["resynced"], 1)

    def test_a_grid_defense_resist_on_a_chance_step_is_no_finding(self):
        # Fuzz 20261016_191716 s001: the engine (worst case) destroyed the
        # building the shot drew a Grid Defense roll on, the game resisted.
        # Benign; the rest is re-predicted from the live board, and the turn
        # ends and compares as usual.
        shot = {"sub": "weapon", "uid": 1, "weapon": "Brute_Tankmech", "target": [2, 2], "target_kind": "building"}
        eng = FuzzEngine([shot], chance={"Brute_Tankmech"})
        orig_init = tlp.FakeBridge.__init__

        def init(fb, state):
            orig_init(fb, state)
            fb.building_damage = 0

        tlp.FakeBridge.__init__ = init
        try:
            rc, sess, findings = self.live_session(eng)
        finally:
            tlp.FakeBridge.__init__ = orig_init
        self.assertEqual(findings, [])
        self.assertEqual(tlp.tile(self.fb.state, 2, 2)["building_hp"], 1)
        self.assertEqual(sess.outcomes["step_resisted"], 1)
        self.assertEqual(self.acting()[-1], "END_TURN")
        events = (sess.dir / "s000" / "events.jsonl").read_text()
        self.assertIn("Grid Defense resisted at F6", events)

    def test_grid_lost_in_the_restored_turn_reaches_the_end_of_turn_prediction(self):
        # Fuzz 20261016_191716 s006 / s014: the restored game's bridge has no
        # turn-start snapshot, so the save's turn-start grid stood through a
        # turn whose shots destroyed a grid building, and the enemy phase
        # "lost more grid than predicted". The turn start stands in.
        shot = {"sub": "weapon", "uid": 1, "weapon": "Brute_Tankmech", "target": [2, 2], "target_kind": "building"}
        eng = FuzzEngine([shot])
        rc, sess, findings = self.live_session(
            eng, next_turn=lambda st: dict(tlp.FakeBridge.default_next(st), grid_power=6))
        self.assertEqual(findings, [])
        self.assertEqual(self.fb.state["grid_power"], 6)
        self.assertEqual(eng.predict_states[-1]["grid_power"], 6)  # the end-of-turn prediction's input
        self.assertEqual(sess.outcomes["matched"], 1)

    def test_teleporter_pads_come_from_the_checkpoint_the_game_loaded(self):
        # Fuzz 20261017_053501 s007 (run 20261016_130634 m36 t03): the restored
        # game's bridge had no pads, the profile's saveData.lua gave none,
        # and the bridge log's latest capture was another mission's; the
        # Sidestep move onto the real pad E6 was predicted to stay there.
        # Every solver input of the sample (each step, End Turn) takes the
        # checkpoint's pads.
        st = dict(tlp.base_state(), mission_id="Mission_Teleporter")
        save = ('["map_data"] = {["teleports"] = {Point( 1, 6 ), Point( 6, 1 ), Point( 0, 7 ), Point( 7, 0 ), },\n'
                '["tele_history"] = {-1, -1, -1, -1, },\n')
        lp.PROFILE.mkdir(parents=True, exist_ok=True)
        (lp.PROFILE / "saveData.lua").write_text("-- the game's own rewrite, no pads")
        orig = lp.logged_teleporter_pairs
        lp.logged_teleporter_pairs = lambda path=None: [[3, 6, 5, 4]]
        try:
            eng = FuzzEngine([c for c in CANDIDATES if c["sub"] == "move"])
            rc, sess, findings = self.live_session(eng, state=st, checkpoint=save)
        finally:
            lp.logged_teleporter_pairs = orig
        self.assertEqual(findings, [])
        self.assertEqual(sess.outcomes["matched"], 1)
        sample = sess.dir / "s000"
        # The solver's inputs: the turn start and End Turn's (the step files are the bridge's own dumps).
        inputs = [sample / "m00_turn_01_solve_input.json", *sorted(sample.glob("m00_turn_01_end_turn_[0-9][0-9].json"))]
        self.assertEqual(len(inputs), 2)
        for p in inputs:
            self.assertEqual(json.loads(p.read_text())["data"]["bridge_state"].get("teleporter_pairs"),
                             [[1, 6, 6, 1], [0, 7, 7, 0]], p.name)

    def test_an_enemy_phase_difference_is_a_finding(self):
        def next_turn(st):
            st = tlp.FakeBridge.default_next(st)
            tlp.unit(st, 0)["hp"] -= 1  # the game hurt a mech the engine left alone
            return st

        eng = FuzzEngine([c for c in CANDIDATES if c["sub"] == "move"])
        rc, sess, findings = self.live_session(eng, next_turn=next_turn)
        self.assertEqual([f["kind"] for f in findings], ["enemy_phase"])
        self.assertTrue(any("PunchMech#0 hp" in d for d in findings[0]["diffs"]))

    # ------------------------------------------------------------ loadout injection

    SPEC = "0:Prime_Lasermech_A,Passive_FlameImmune;pilot=Pilot_Rock"

    def loadout_setup(self) -> Path:
        """A recorded start whose checkpoint is the recorded save fragment
        of tests/fixtures; fake game scripts (test_loadout). Returns the
        --game root."""
        root = tl.write_tree(self.tmp / "game" / "scripts", tl.SCRIPTS).parent
        self.record()
        d = lp.LIVE_ROOT / "20261016_000000"
        text = tl.SAVE.read_text()
        (d / "m00_turn_01_saveData.lua").write_text(text)
        (d / "m00_turn_01_undoSave.lua").write_text(text)
        return root

    def dry(self, game: Path, extra: list, eng=None) -> tuple:
        eng = eng or FuzzEngine(CANDIDATES)
        out = self.tmp / "fuzz"
        rc = fz.main(["--dry-run", "--recordings", str(lp.LIVE_ROOT), "--seed", "1", "--out", str(out),
                      "--game", str(game), *extra], engine=eng)
        sess = next(out.iterdir())
        samples = [json.loads(line) for line in (sess / "samples.jsonl").read_text().splitlines()]
        return rc, sess, samples, json.loads((sess / "session.json").read_text()), eng

    def test_dry_run_injects_a_loadout_and_the_engine_sees_it(self):
        game = self.loadout_setup()
        rc, sess, samples, meta, eng = self.dry(game, ["--samples", "2", "--loadout", self.SPEC])
        self.assertEqual(rc, 0)
        self.assertEqual(len(samples), 2)
        self.assertEqual(samples[0]["loadout"]["spec"], self.SPEC)
        self.assertEqual({k: v["status"] for k, v in samples[0]["acceptance"].items()},
                         {"weapon:Prime_Lasermech_A": "supported", "passive:Passive_FlameImmune": "supported",
                          "pilot:Pilot_Rock": "supported"})
        # The engine generated and predicted from the start with the loadout.
        u = tlp.unit(eng.predict_states[-1], 0)
        self.assertEqual((u["weapons_exact"], u["pilot_id"]), (["Prime_Lasermech_A", "Passive_FlameImmune"], "Pilot_Rock"))
        s0 = sess / "s000"
        self.assertIn('["primary"] = "Prime_Lasermech"', (s0 / "loadout_saveData.lua").read_text())
        self.assertEqual((s0 / "loadout_undoSave.lua").read_text(), (s0 / "loadout_saveData.lua").read_text())
        self.assertEqual(json.loads((s0 / "loadout.json").read_text())["mechs"][0]["pilot"], "Pilot_Rock")
        self.assertEqual(meta["loadout_items"]["pilot:Pilot_Rock"]["samples"], [0, 1])
        self.assertEqual(meta["outcomes"], {"consistent": 2})
        self.assertFalse(lv.CMD.exists())

    def test_an_item_the_engine_does_not_model_is_not_played(self):
        game = self.loadout_setup()
        rc, sess, samples, meta, eng = self.dry(game, ["--samples", "1", "--loadout", "2:*,Passive_Strange"])
        self.assertEqual(rc, 0)
        self.assertEqual(meta["outcomes"], {"unsupported": 1})
        self.assertEqual(eng.predicts, [])
        self.assertEqual(samples[0]["outcome"], "unsupported")
        self.assertEqual(meta["loadout_items"]["passive:Passive_Strange"]["status"], "unsupported")

    def test_an_illegal_loadout_is_a_finding_and_a_bad_spec_is_refused(self):
        game = self.loadout_setup()
        rc, sess, samples, meta, eng = self.dry(game, ["--samples", "1", "--loadout", "0:Brute_Jetmech"])
        self.assertEqual(meta["outcomes"], {"loadout_illegal": 1})
        finding = json.loads((sess / "findings.jsonl").read_text().splitlines()[0])
        self.assertIn("twice in the squad", finding["problem"])
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(fz.main(["--dry-run", "--recordings", str(lp.LIVE_ROOT), "--loadout", "0:A,B,C"]), 2)
            self.assertEqual(fz.main(["--dry-run", "--recordings", str(lp.LIVE_ROOT), "--loadout", "0:A",
                                      "--coverage"]), 2)

    def test_coverage_hands_out_unseen_items_once(self):
        game = self.loadout_setup()
        rc, sess, samples, meta, eng = self.dry(game, ["--samples", "3", "--coverage"])
        self.assertEqual(rc, 0)
        given = [it for s in samples if s.get("loadout") for m in s["loadout"]["mechs"] for it in m["weapons"]]
        self.assertTrue(given)
        status = meta["loadout_items"]
        # Known unsupported up front: never injected.
        self.assertEqual(status["passive:Passive_Strange"]["status"], "unsupported")
        self.assertEqual(status["pilot:Pilot_Odd"]["samples"], [])
        self.assertNotIn("Passive_Strange", given)
        # Seen in the recordings (the start carries it): not handed out.
        self.assertNotIn("Prime_Lasermech_A", given)
        self.assertNotIn("Passive_Leech", given)  # in the squad's storage
        for s in samples:
            for m in (s.get("loadout") or {}).get("mechs", []):
                self.assertNotIn(m["pilot"], ("Pilot_BeetleMech",))
        # An item supported once is used up: not handed out again.
        supported = [k.split(":", 1)[1] for k, v in status.items() if v["status"] == "supported"]
        for wid in supported:
            if not wid.startswith("Pilot_"):
                self.assertEqual(given.count(wid), 1, wid)
        self.assertIn("coverage_left", meta)

    def loadout_game(self, loaded_matches: bool) -> tuple:
        """The game for a live sample with SPEC: the profile holds the
        player's run; the rewritten checkpoint resumes at the recorded start
        with the loadout (as the bridge reads it from that save), or, with
        loaded_matches False, at the recorded start without it."""
        game_root = self.loadout_setup()
        lp.PROFILE.mkdir(parents=True)
        (lp.PROFILE / "saveData.lua").write_text("-- the player's own run")
        (lp.PROFILE / "undoSave.lua").write_text("-- the player's own undo")
        items = tl.lo.Items(game_root / "scripts")
        text = tl.SAVE.read_text()
        inj = tl.lo.inject(items, {"saveData.lua": text, "undoSave.lua": text}, tl.lo.parse_spec(self.SPEC))
        start = dict(tlp.base_state(), turn=1)
        own = dict(tlp.base_state(), mission_id="Mission_Own", turn=3)
        boards = {inj.texts["saveData.lua"]: tl.lo.patch_state(start, inj, items) if loaded_matches else start,
                  "-- the player's own run": own}
        game = tlp.FakeGame(boards, "-- the player's own run")
        self.game = game
        self.undo = game.install()
        args = args_for(["--samples", "1", "--recordings", str(lp.LIVE_ROOT), "--game", str(game_root),
                         "--loadout", self.SPEC])
        sess = fz.Session(args, self.tmp / "fuzz")
        sess.loadouts = fz.Loadouts(args, random.Random(1))
        return game, args, sess, inj

    def test_a_live_sample_restores_the_rewritten_checkpoint_and_checks_the_loadout(self):
        game, args, sess, inj = self.loadout_game(loaded_matches=True)
        starts = fz.find_turn_starts(lp.LIVE_ROOT, need_save=True)
        loaded = []
        launch = game.launch

        def spy():
            loaded.append((lp.PROFILE / "saveData.lua").read_text())
            launch()
        lp.launch_game = spy
        rc = fz.live(args, FuzzEngine(CANDIDATES), sess, starts, click=game.click, escape=game.escape)
        self.assertEqual(rc, 0)
        self.assertEqual(sess.outcomes["matched"], 1)
        self.assertEqual(loaded[0], inj.texts["saveData.lua"])  # the sample's load
        self.assertIn("END_TURN", game.fb.commands)
        self.assertEqual((lp.PROFILE / "saveData.lua").read_text(), "-- the player's own run")
        samples = [json.loads(line) for line in (sess.dir / "samples.jsonl").read_text().splitlines()]
        self.assertEqual((samples[0]["outcome"], samples[0]["loadout"]["spec"]), ("matched", self.SPEC))
        self.assertEqual(game.misclicks(), [])

    def test_a_loadout_the_game_does_not_show_is_rejected(self):
        game, args, sess, inj = self.loadout_game(loaded_matches=False)
        starts = fz.find_turn_starts(lp.LIVE_ROOT, need_save=True)
        rc = fz.live(args, FuzzEngine(CANDIDATES), sess, starts, click=game.click, escape=game.escape)
        self.assertEqual(rc, 0)
        self.assertEqual(sess.outcomes["loadout_rejected"], 1)
        self.assertNotIn("END_TURN", game.fb.commands)
        finding = json.loads((sess.dir / "findings.jsonl").read_text().splitlines()[0])
        self.assertEqual(finding["kind"], "loadout_rejected")
        self.assertIn("weapons_exact", finding["problem"])
        self.assertEqual(finding["loadout"]["spec"], self.SPEC)
        self.assertEqual((lp.PROFILE / "saveData.lua").read_text(), "-- the player's own run")


if __name__ == "__main__":
    unittest.main()
