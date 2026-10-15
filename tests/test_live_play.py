"""scripts/live_play.py against a fake bridge (a temp dir with a state file
and a thread that answers commands) and a fake engine. Nothing here touches
the game or the desktop: clicks go to a recorder.

    python3 -m unittest tests.test_live_play
"""

from __future__ import annotations

import contextlib
import copy
import importlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
_BRIDGE_DIR = tempfile.mkdtemp(prefix="itb_fake_bridge_")
os.environ["ITB_BRIDGE_DIR"] = _BRIDGE_DIR
sys.path.insert(0, str(REPO / "scripts"))
for _m in ("live_validate", "live_play"):
    if _m in sys.modules:
        importlib.reload(sys.modules[_m])
import live_play as lp  # noqa: E402
import live_validate as lv  # noqa: E402

lp.SETTLE_INTERVAL = 0.01
lp.POLL = 0.02
BRIDGE = Path(_BRIDGE_DIR)


def base_state() -> dict:
    tiles = []
    for x in range(8):
        for y in range(8):
            t = {"x": x, "y": y, "terrain": "ground", "terrain_id": 0}
            if (x, y) == (2, 2):
                t.update(terrain="building", terrain_id=1, building_hp=1)
            tiles.append(t)
    return {
        "phase": "combat_player", "turn": 1, "total_turns": 5, "mission_id": "Mission_Test",
        "in_active_mission": True, "grid_power": 7, "grid_power_max": 7, "bridge_ext_version": 1,
        "bridge_errors": [], "spawning_tiles": [[5, 5]], "tiles": tiles,
        "units": [
            {"uid": 0, "type": "PunchMech", "x": 6, "y": 6, "hp": 3, "max_hp": 3, "team": 1, "mech": True,
             "active": True, "moved": False, "weapons": ["Prime_Punchmech"],
             "weapons_exact": ["Prime_Punchmech_B", "Prime_Lasermech_A"],
             "weapon_slots": [{"slot": 0, "id": "Prime_Punchmech_B"}, {"slot": 1, "id": "Prime_Lasermech_A"}]},
            {"uid": 1, "type": "TankMech", "x": 6, "y": 1, "hp": 3, "max_hp": 3, "team": 1, "mech": True,
             "active": True, "moved": False, "weapons": ["Brute_Tankmech"], "weapons_exact": ["Brute_Tankmech"],
             "weapon_slots": [{"slot": 0, "id": "Brute_Tankmech"}]},
            {"uid": 100, "type": "Scorpion1", "x": 3, "y": 3, "hp": 3, "max_hp": 3, "team": 6, "mech": False,
             "active": False, "has_queued_attack": True, "queued_target": [2, 3]},
        ],
    }


def compact(state: dict) -> dict:
    """What itb_live's live_board_json gives for these simple boards."""
    us, under = [], []
    for u in state["units"]:
        if u.get("is_extra_tile") or u.get("hp", 0) <= 0:
            continue
        if u.get("underground"):
            under.append({"uid": u["uid"], "type": u["type"], "x": u["x"], "y": u["y"], "hp": u["hp"],
                          "max_hp": u.get("max_hp"), "team": u["team"], "underground": True})
            continue
        us.append({"uid": u["uid"], "type": u["type"], "x": u["x"], "y": u["y"], "hp": u["hp"],
                   "team": u["team"], "mech": u.get("mech", False),
                   **{f: bool(u.get(f)) for f in ("fire", "acid", "frozen", "shield", "web")}})
    us.sort(key=lambda u: u["uid"])
    bs = [{"x": t["x"], "y": t["y"], "hp": t["building_hp"]} for t in state["tiles"]
          if t["terrain"] == "building"]
    out = {"grid_power": state["grid_power"], "buildings": bs, "units": us}
    if under:
        out["underground"] = sorted(under, key=lambda u: u["uid"])
    return out


def unit(state, uid):
    return next(u for u in state["units"] if u["uid"] == uid)


def tile(state, x, y):
    return next(t for t in state["tiles"] if (t["x"], t["y"]) == (x, y))


# A deploy weapon's tank (weapons_deploy.lua): a player unit, not a mech,
# active at once, its weapon from the type's SkillList (the bridge has no
# save record of it until the next turn: no weapons_exact / weapon_slots).
def deployed_tank(uid: int, x: int, y: int) -> dict:
    return {"uid": uid, "type": "Deploy_Tank", "x": x, "y": y, "hp": 1, "max_hp": 1, "team": 1, "mech": False,
            "active": True, "moved": False, "weapons": ["Deploy_TankShot"]}


def apply(state: dict, step: dict, damage: int = 1, spawn_uid: int | None = None) -> None:
    """The fake game's rules (and the fake engine's, with damage=1). A
    DeploySkill_* drops a Deploy_Tank on its target, numbered `spawn_uid`
    (default: one more than the largest uid, as the engine numbers it)."""
    u = unit(state, step["uid"])
    if step["sub"] == "move":
        u["x"], u["y"] = step["to"]
        u["moved"] = True
    elif step["sub"] == "weapon" and str(step.get("weapon", "")).startswith("DeploySkill_"):
        if spawn_uid is None:
            spawn_uid = max(v["uid"] for v in state["units"]) + 1
        state["units"].append(deployed_tank(spawn_uid, *step["target"]))
        u["active"] = False
    elif step["sub"] == "weapon":
        tx, ty = step["target"]
        for v in state["units"]:
            if (v["x"], v["y"]) == (tx, ty) and not v.get("underground"):
                v["hp"] -= damage
                # A hurt Burrower dives: underground (the bridge omits it).
                if v["type"] == "Burrower1" and v["hp"] > 0:
                    v["underground"] = True
        u["active"] = False
    else:
        u["hp"] = min(u["max_hp"], u["hp"] + 1)
        u["active"] = False


PLAN = [
    {"sub": "move", "uid": 0, "to": [3, 4], "action": 0},
    {"sub": "weapon", "uid": 0, "weapon": "Prime_Lasermech_A", "target": [3, 3], "target2": None, "action": 0},
    {"sub": "weapon", "uid": 1, "weapon": "Brute_Tankmech", "target": [3, 3], "target2": None, "action": 1},
]


class FakeEngine:
    def __init__(self, plan=PLAN, after_enemy=None, enemy_phase=None, clears=None, shields=None,
                 enemy_shields=()):
        self.plan = plan
        # (k, tiles): from plan step k on, the boards' "shield_tiles" (the
        # structures the plan shielded); after the enemy phase, enemy_shields.
        # None: boards without "shield_tiles" (an itb_live from before it).
        self.shields = shields
        self.enemy_shields = [list(t) for t in enemy_shields]
        self.after_enemy = after_enemy
        self.enemy_phase = enemy_phase or {"mission_ended": False}
        # {uid: plan step index}: the engine clears that unit's queued shot
        # in that step (smoke, say), which the bridge's save copy never shows.
        # None: boards without "queued" (an itb_live from before it).
        self.clears = clears
        self.solves = []
        # Deployment search: None = keep the first `always` placement;
        # {uid: (x, y)} = these tiles; an exception = the engine fails.
        self.deploy_tiles = None
        self.deploys = []  # (state, time_limit, always) per deploy request
        self.deploy_rankings = []  # the ranking options of each deploy request
        self.budgets = []  # (time_limit, max_time, min_tiers) per solve
        self.tiers = []  # ranking options (threat_tier, mech_grid, ...) per solve
        self.predicts = []
        self.predict_states = []

    @staticmethod
    def _state(path) -> dict:
        d = json.loads(Path(path).read_text())
        return d["data"]["bridge_state"] if "data" in d else d

    def _board(self, b: dict) -> dict:
        c = compact(b)
        if self.clears is not None:
            for u in c["units"]:
                src = unit(b, u["uid"])
                u["queued"] = ({"weapon": 0, "origin": src.get("queued_origin"), "target": src["queued_target"]}
                               if src.get("has_queued_attack") and src.get("queued_target") else None)
        return c

    def _simulate(self, st: dict, steps: list) -> dict:
        b = copy.deepcopy(st)
        out = []
        for i, s in enumerate(steps):
            apply(b, s)
            for uid, k in (self.clears or {}).items():
                if k == self.plan.index(s) and any(u["uid"] == uid for u in b["units"]):
                    unit(b, uid)["has_queued_attack"] = False
            out.append(dict(s, index=i, status="ok", board=self._board(b)))
            if self.shields is not None:
                k, tiles = self.shields
                out[-1]["board"]["shield_tiles"] = [list(t) for t in tiles] if self.plan.index(s) >= k else []
        after = compact(b)
        enemy = copy.deepcopy(self.after_enemy(b)) if self.after_enemy else compact(b)
        if self.shields is not None:
            enemy["shield_tiles"] = copy.deepcopy(self.enemy_shields)
        plan = [{"uid": s["uid"], "description": f"#{s['uid']} {s['sub']}"} for s in steps]
        return {"ok": True, "plan": plan, "steps": out, "refused": -1, "start": compact(st),
                "after_player": after, "after_enemy": enemy, "enemy_phase": copy.deepcopy(self.enemy_phase),
                "warnings": []}

    def solve(self, path, time_limit, max_time=0, min_tiers=0, **tiers):
        st = self._state(path)
        self.solves.append(st)
        self.budgets.append((time_limit, max_time, min_tiers))
        self.tiers.append(tiers)
        active = {u["uid"] for u in st["units"] if u.get("team") == 1 and u.get("active")}
        moved = {u["uid"] for u in st["units"] if u.get("moved")}
        # Steps of units the plan creates (not on the board yet) stay.
        present = {u["uid"] for u in st["units"]}
        steps = [s for s in self.plan if (s["uid"] in active or s["uid"] not in present)
                 and not (s["sub"] == "move" and s["uid"] in moved)]
        out = self._simulate(st, steps)
        out.update(searched=True, proven_optimal=True, worst_case={"grid": 0}, stats={"time_s": 0.1})
        return out

    def predict(self, path, plan):
        self.predicts.append(plan)
        self.predict_states.append(self._state(path))
        # A given plan (`turn --plan`) plays out as this engine's own plan.
        return self._simulate(self._state(path), self.plan if plan else [])

    def board(self, path):
        return compact(self._state(path))

    def deploy(self, path, time_limit, always=None, ranking=None):
        st = self._state(path)
        self.deploys.append((st, time_limit, always))
        self.deploy_rankings.append(ranking)
        if isinstance(self.deploy_tiles, Exception):
            raise self.deploy_tiles
        mechs = sorted(u["uid"] for u in st["units"] if u.get("team") == 1 and u.get("mech"))
        if self.deploy_tiles is None:
            tiles = [list(p) for p in always[0]]
        else:
            tiles = [list(self.deploy_tiles[uid]) for uid in mechs]
        value = {"tier_min": {"grid": 0, "building_hp": 0, "mechs": 0}, "mean": {"grid": 0.0, "mechs": 0.0},
                 "outcomes": 3}
        return {"ok": True, "cmd": "deploy", "mechs": mechs, "warnings": [],
                "best": {"tiles": tiles, "visual": [lp.visual(*p) for p in tiles], "value": value}}

    def close(self):
        pass


class FakeBridge:
    """Answers /tmp-style commands from a thread, like the Lua bridge."""

    def __init__(self, state: dict):
        self.state = state
        self.commands = []
        self.damage = 1
        self.refuse_native = False
        self.exec_mode = True  # False: a bridge from before EXEC_MODE
        self.end_turn_ack = "NEEDS_MCP_CLICK END_TURN method=SetActive"
        self.refused_deploy = set()
        self.export_moved = True
        self.next_turn = None  # function(state) -> next player-turn state
        self.silent = False  # the game left the mission: no more heartbeats
        # The game's own uid counter for units created mid-turn (it also
        # counts spawns the engine never sees, so it runs ahead).
        self.next_spawn_uid = 7510
        self.stop = threading.Event()
        self.write_state()
        self.thread = threading.Thread(target=self.loop, daemon=True)
        self.thread.start()

    def write_state(self, path=None):
        path = path or lv.STATE
        tmp = Path(str(path) + ".tmp")
        st = copy.deepcopy(self.state)
        st["units"] = [u for u in st["units"] if not u.get("underground")]
        if not self.export_moved:
            for u in st["units"]:
                u.pop("moved", None)
        tmp.write_text(json.dumps(st))
        os.replace(tmp, path)

    def advance(self):
        """The game after End Turn: the enemy phase, then the next turn."""
        self.state["phase"] = "combat_enemy"
        self.write_state()
        time.sleep(0.05)
        self.state = self.next_turn(self.state) if self.next_turn else self.default_next(self.state)
        self.write_state()

    @staticmethod
    def default_next(st):
        st = copy.deepcopy(st)
        st["phase"] = "combat_player"
        st["turn"] += 1
        for u in st["units"]:
            if u["team"] == 1:
                u["active"], u["moved"] = True, False
        return st

    def handle(self, cmd: str) -> str:
        parts = cmd.split()
        name, args = parts[0], parts[1:]
        if name == "SNAPSHOT":
            path = BRIDGE / f"itb_snapshot_{args[0] if args else 'x'}.json"
            self.write_state(path)
            return f"OK SNAPSHOT {path}"
        if name == "LUA":
            return "OK LUA: refresh"
        if name == "EXEC_MODE":
            if not self.exec_mode:
                return "ERROR: unknown command: EXEC_MODE"
            return f"OK EXEC_MODE {args[0]}"
        if name in ("MOVE_NATIVE", "MOVE"):
            if name == "MOVE_NATIVE" and self.refuse_native:
                return "ERROR: MOVE_NATIVE FireWeapon[0] returned 0: nothing fired"
            uid, x, y = (int(v) for v in args)
            apply(self.state, {"sub": "move", "uid": uid, "to": [x, y]})
            return f"OK {name} {uid} to {x},{y} [FireWeapon[0]] at {x},{y}"
        if name == "ATTACK":
            uid, slot, x, y = (int(v) for v in args)
            u = unit(self.state, uid)
            listed = u.get("weapons_exact") or u.get("weapons") or []
            weapon = listed[slot] if slot < len(listed) else ""
            self.next_spawn_uid += 1
            apply(self.state, {"sub": "weapon", "uid": uid, "weapon": weapon, "target": [x, y]}, self.damage,
                  spawn_uid=self.next_spawn_uid)
            return f"OK ATTACK {uid} slot={slot} at {x},{y} [FireWeapon]"
        if name == "REPAIR":
            apply(self.state, {"sub": "repair", "uid": int(args[0])})
            return f"OK REPAIR {args[0]}"
        if name == "END_TURN":
            for u in self.state["units"]:
                if u["team"] == 1:
                    u["active"] = False
            if self.end_turn_ack.startswith("OK"):
                threading.Thread(target=self.advance, daemon=True).start()
            return self.end_turn_ack
        if name == "DEPLOY":
            uid, x, y = (int(v) for v in args)
            if (x, y) in self.refused_deploy:
                return f"ERROR: DEPLOY refused: {x},{y} reserved tile (pylons)"
            other = [v for v in self.state["units"] if (v["x"], v["y"]) == (x, y) and v["uid"] != uid]
            if other:
                return f"ERROR: DEPLOY refused: {x},{y} occupied by pawn {other[0]['uid']}"
            u = unit(self.state, uid)
            u["x"], u["y"] = x, y
            return f"OK DEPLOY {uid} at {x},{y}"
        return f"ERROR: unknown command: {name}"

    def loop(self):
        while not self.stop.is_set():
            if not self.silent:
                (BRIDGE / "itb_bridge_heartbeat").write_text("1")
            if lv.CMD.exists():
                text = lv.CMD.read_text().strip()
                lv.CMD.unlink()
                seq, _, cmd = text.partition(" ")
                self.commands.append(cmd)
                ack = self.handle(cmd)
                tmp = BRIDGE / "itb_ack.txt.tmp"
                tmp.write_text(f"{seq} {ack}")
                os.replace(tmp, lv.ACK)
                self.write_state()
            time.sleep(0.005)

    def close(self):
        self.stop.set()
        self.thread.join(timeout=2)


class LivePlayTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="itb_live_play_"))
        self._roots = (lp.LIVE_ROOT, lp.CURRENT, lp.PROFILE, lp.RESTORE_BACKUPS)
        lp.LIVE_ROOT = self.tmp / "live"
        lp.CURRENT = self.tmp / "current.json"
        lp.PROFILE = self.tmp / "profile_Alpha"
        lp.RESTORE_BACKUPS = self.tmp / "restore_backups"
        self.clicks = []
        self.fb = None

    def tearDown(self):
        if self.fb:
            self.fb.close()
        lp.LIVE_ROOT, lp.CURRENT, lp.PROFILE, lp.RESTORE_BACKUPS = self._roots
        shutil.rmtree(self.tmp, ignore_errors=True)

    def bridge(self, state=None) -> FakeBridge:
        self.fb = FakeBridge(state or base_state())
        return self.fb

    def click(self, xy):
        self.clicks.append(xy)
        threading.Thread(target=self.fb.advance, daemon=True).start()
        return True

    def run_cmd(self, argv, engine=None) -> int:
        return lp.main(argv + ["--new-run"] if "--run" not in argv else argv, engine=engine or FakeEngine(),
                       click=self.click)

    def run_dir(self) -> Path:
        return Path(json.loads(lp.CURRENT.read_text())["run"])

    def acting(self):
        return [c for c in self.fb.commands if c.split()[0] not in ("SNAPSHOT", "LUA", "EXEC_MODE")]

    # ---------------------------------------------------------------- safety

    def test_refuses_a_stale_heartbeat(self):
        self.bridge()
        self.fb.close()
        old = time.time() - 120
        os.utime(BRIDGE / "itb_bridge_heartbeat", (old, old))
        self.assertEqual(self.run_cmd(["turn"]), 2)
        self.assertEqual(self.fb.commands, [])

    def test_refuses_when_the_extension_is_disabled(self):
        st = base_state()
        st["bridge_ext_disabled"] = "budget: dump took 1.2s"
        self.bridge(st)
        self.assertEqual(self.run_cmd(["turn"]), 2)
        self.assertEqual(self.acting(), [])

    def test_refuses_without_exact_weapons_unless_given_a_loadout(self):
        st = base_state()
        for u in st["units"]:
            u.pop("weapons_exact", None)
            u.pop("weapon_slots", None)
        self.bridge(st)
        self.assertEqual(self.run_cmd(["turn"]), 2)
        loadout = self.tmp / "loadout.json"
        loadout.write_text(json.dumps({"PunchMech": ["Prime_Punchmech_B", "Prime_Lasermech_A"],
                                       "TankMech": ["Brute_Tankmech"]}))
        eng = FakeEngine()
        self.assertEqual(self.run_cmd(["turn", "--dry-run", "--loadout", str(loadout)], eng), 0)
        solved = eng.solves[0]
        self.assertEqual(unit(solved, 0)["weapons_exact"], ["Prime_Punchmech_B", "Prime_Lasermech_A"])
        # The live state is archived as it was, beside the patched input.
        raws = list(self.run_dir().glob("*_start_raw_*.json"))
        self.assertEqual(len(raws), 1)
        self.assertNotIn("weapons_exact", unit(json.loads(raws[0].read_text())["data"]["bridge_state"], 0))
        # A slot from the loadout order (secondary = 1).
        self.assertEqual(lp.weapon_slot(st, 0, "Prime_Lasermech_A", lp.load_loadout(str(loadout))), 1)

    def test_dry_run_sends_no_action(self):
        self.bridge()
        eng = FakeEngine()
        self.assertEqual(self.run_cmd(["turn", "--dry-run"], eng), 0)
        self.assertEqual(self.acting(), [])
        self.assertEqual(len(eng.solves), 1)
        self.assertEqual(self.run_cmd(["deploy", "--dry-run"]), 2)  # turn 1: not a deployment
        self.assertEqual(self.acting(), [])

    # ---------------------------------------------------------------- turn

    def test_turn_executes_and_matches(self):
        self.bridge()
        eng = FakeEngine()
        self.assertEqual(self.run_cmd(["turn"], eng), 0)
        self.assertEqual(self.acting(), ["MOVE_NATIVE 0 3 4", "ATTACK 0 1 3 3", "ATTACK 1 0 3 3"])
        self.assertEqual(len(eng.solves), 1)
        run = self.run_dir()
        names = sorted(p.name for p in run.iterdir())
        self.assertIn("m00_turn_01_solve_input.json", names)
        self.assertIn("m00_turn_01_solve_input_prediction.json", names)
        self.assertIn("m00_turn_01_step1_01.json", names)
        self.assertIn("m00_turn_01_step3_01.json", names)
        self.assertIn("events.jsonl", names)
        solve = json.loads((run / "m00_turn_01_solve.json").read_text())
        self.assertEqual(solve["data"]["actions"][0],
                         {"mech_uid": 0, "move_to": [3, 4], "weapon_id": "Prime_Lasermech_A", "target": [3, 3]})
        self.assertNotIn("partial_re_solve", solve["data"])
        wrapper = json.loads((run / "m00_turn_01_solve_input.json").read_text())
        self.assertEqual(wrapper["data"]["bridge_state"]["mission_id"], "Mission_Test")
        self.assertEqual(wrapper["run_id"], run.name)

    def test_turn_plays_a_given_plan_without_solving(self):
        self.bridge()
        eng = FakeEngine()
        plan = [{"uid": 0, "kind": "weapon", "move": [3, 4], "weapon": "Prime_Lasermech_A", "target": [3, 3]}]
        path = self.tmp / "plan.json"
        path.write_text(json.dumps({"plan": plan}))
        self.assertEqual(self.run_cmd(["turn", "--plan", str(path)], eng), 0)
        self.assertEqual(eng.solves, [])
        self.assertEqual(eng.predicts, [plan])
        self.assertEqual(self.acting(), ["MOVE_NATIVE 0 3 4", "ATTACK 0 1 3 3", "ATTACK 1 0 3 3"])

    def test_turn_refuses_an_empty_plan(self):
        self.bridge()
        path = self.tmp / "plan.json"
        path.write_text("[]")
        self.assertEqual(self.run_cmd(["turn", "--plan", str(path)]), 2)
        self.assertEqual(self.acting(), [])

    def test_turn_checkpoints_the_save_files_once_per_turn(self):
        self.bridge()
        lp.PROFILE.mkdir(parents=True)
        (lp.PROFILE / "saveData.lua").write_text('["iCurrentTurn"] = 1')
        (lp.PROFILE / "undoSave.lua").write_text("undo 1")
        self.assertEqual(self.run_cmd(["turn"]), 0)
        run = self.run_dir()
        self.assertEqual((run / "m00_turn_01_saveData.lua").read_text(), '["iCurrentTurn"] = 1')
        self.assertEqual((run / "m00_turn_01_undoSave.lua").read_text(), "undo 1")
        # A re-run of `turn` later in the same turn keeps the turn-start copy.
        (lp.PROFILE / "saveData.lua").write_text("later")
        self.fb.state = base_state()
        self.fb.write_state()
        self.assertEqual(self.run_cmd(["turn", "--run", str(run)]), 0)
        self.assertEqual((run / "m00_turn_01_saveData.lua").read_text(), '["iCurrentTurn"] = 1')
        events = [json.loads(line) for line in (run / "events.jsonl").read_text().splitlines()]
        self.assertEqual([e["detail"]["save_turns"] for e in events if e["kind"] == "checkpoint"], [[1]])

    def test_dry_run_takes_no_checkpoint(self):
        self.bridge()
        lp.PROFILE.mkdir(parents=True)
        (lp.PROFILE / "saveData.lua").write_text("save")
        self.assertEqual(self.run_cmd(["turn", "--dry-run"]), 0)
        self.assertEqual(list(self.run_dir().glob("*.lua")), [])

    def test_restore_puts_the_checkpoint_back_and_continues(self):
        st = base_state()
        self.bridge(st)
        run = self.tmp / "live" / "old"
        run.mkdir(parents=True)
        (run / "m03_turn_02_saveData.lua").write_text("checkpoint")
        (run / "m03_turn_02_solve_input.json").write_text(json.dumps({"data": {"bridge_state": st}}))
        lp.PROFILE.mkdir(parents=True)
        (lp.PROFILE / "saveData.lua").write_text("current")
        (lp.PROFILE / "undoSave.lua").write_text("current undo")
        calls = []
        saved = (lp.game_running, lp.quit_game, lp.launch_game, lp.CONTINUE_DELAY)
        lp.game_running = lambda: True
        lp.quit_game = lambda: calls.append("quit")
        lp.launch_game = lambda: calls.append("launch")
        # The bridge is silent on the title screen.
        self.fb.close()
        lp.CONTINUE_DELAY = 0.0
        try:
            # Continue loads the save: the bridge starts and writes the turn's board.
            def click(xy):
                self.clicks.append(xy)
                self.fb = FakeBridge(copy.deepcopy(st))
                return True
            rc = lp.main(["restore", "--run", str(run), "--mission", "3", "--turn", "2", "--timeout", "10"],
                         engine=FakeEngine(), click=click)
        finally:
            lp.game_running, lp.quit_game, lp.launch_game, lp.CONTINUE_DELAY = saved
        self.assertEqual(rc, 0)
        self.assertEqual(calls, ["quit", "launch"])
        self.assertEqual(self.clicks, [lp.TITLE_CONTINUE])
        self.assertEqual((lp.PROFILE / "saveData.lua").read_text(), "checkpoint")
        # No undoSave in the checkpoint: the current one (another moment) is removed.
        self.assertFalse((lp.PROFILE / "undoSave.lua").exists())
        backups = list(lp.RESTORE_BACKUPS.iterdir())
        self.assertEqual((backups[0] / "saveData.lua").read_text(), "current")
        self.assertEqual((backups[0] / "undoSave.lua").read_text(), "current undo")
        # The current run is not switched to the restored one.
        self.assertFalse(lp.CURRENT.exists())

    def test_new_run_stays_current_while_an_older_command_writes(self):
        # Live 2026-10-14: `status --new-run` at 08:17 while a `mission` begun
        # at 08:16 still ran on the old run; that process archived states into
        # its own run for minutes after, and every manifest save pointed
        # current.json back at it, so later commands used the old run.
        old = lp.Run.open("20261014_045942", False)
        self.assertEqual(self.run_dir(), old.path)
        new = lp.Run.open(None, True).path  # what `status --new-run` does
        self.assertEqual(self.run_dir(), new)
        self.assertNotEqual(new, old.path)
        old.save_state(base_state(), "start")  # the older command goes on
        old.save_meta()
        self.assertTrue(any(old.path.glob("m00_turn_01_solve_input.json")))
        self.assertEqual(self.run_dir(), new)
        self.assertEqual(lp.Run.open(None, False).path, new)
        # Naming a run explicitly still makes it the current one.
        self.assertEqual(lp.Run.open(old.path.name, False).path, old.path)
        self.assertEqual(self.run_dir(), old.path)

    def test_checkpoint_diffs(self):
        a, b = base_state(), base_state()
        self.assertEqual(lp.checkpoint_diffs(a, b), [])
        unit(b, 100)["hp"] = 2
        b["turn"] = 2
        b["units"].append({"uid": 101, "type": "Firefly1", "x": 4, "y": 4, "hp": 3, "team": 6})
        self.assertEqual(lp.checkpoint_diffs(a, b), ["turn 1 -> 2", "Scorpion1#100: hp 3 -> 2",
                                                     "Firefly1#101 only in the game"])

    def test_restore_refuses_a_missing_checkpoint(self):
        self.bridge()
        run = self.tmp / "live" / "old"
        run.mkdir(parents=True)
        self.assertEqual(lp.main(["restore", "--run", str(run), "--mission", "0", "--turn", "1"]), 2)

    def test_a_deployed_tank_is_commanded_by_its_live_uid(self):
        # A DeploySkill_Tank drop: the engine numbers the new tank 101, the
        # game 7511. Once the drop step matches (by type, tile and HP), the
        # tank's move and shot go out under the game's uid; its weapon slot
        # comes from the type's SkillList (no save record mid-turn).
        st = base_state()
        mech = unit(st, 1)
        mech["weapons_exact"] = ["Brute_Tankmech", "DeploySkill_Tank"]
        mech["weapon_slots"] = [{"slot": 0, "id": "Brute_Tankmech"},
                                {"slot": 1, "id": "DeploySkill_Tank", "limited": 1, "uses": 1, "uses_saved": 1}]
        self.bridge(st)
        plan = [
            {"sub": "weapon", "uid": 1, "weapon": "DeploySkill_Tank", "target": [6, 3], "target2": None, "action": 0},
            {"sub": "move", "uid": 101, "to": [4, 3], "action": 1},
            {"sub": "weapon", "uid": 101, "weapon": "Deploy_TankShot", "target": [3, 3], "target2": None,
             "action": 1},
        ]
        eng = FakeEngine(plan=plan)
        self.assertEqual(self.run_cmd(["turn"], eng), 0)
        self.assertEqual(self.acting(), ["ATTACK 1 1 6 3", "MOVE_NATIVE 7511 4 3", "ATTACK 7511 0 3 3"])
        self.assertEqual(len(eng.solves), 1)  # every step matched: no re-solve
        tank = unit(self.fb.state, 7511)
        self.assertEqual((tank["x"], tank["y"], tank["active"]), (4, 3, False))
        self.assertEqual(unit(self.fb.state, 100)["hp"], 2)
        # The turn's action history (UI limits, pilot bonuses) is kept under
        # the live uid.
        manifest = json.loads((self.run_dir() / "manifest.json").read_text())
        self.assertIn('"7511": "MA"', json.dumps(manifest.get("unit_actions")))

    def test_a_deployed_tank_is_a_known_unit_next_turn(self):
        # The next turn the tank is on the board under the game's uid like
        # any unit: solved, commanded and compared by uid.
        st = base_state()
        st["units"].append(dict(deployed_tank(7511, 4, 3), weapons_exact=["Deploy_TankShot"],
                                weapon_slots=[{"slot": 0, "id": "Deploy_TankShot", "limited": 0}]))
        self.bridge(st)
        plan = PLAN + [{"sub": "weapon", "uid": 7511, "weapon": "Deploy_TankShot", "target": [3, 3],
                        "target2": None, "action": 2}]
        eng = FakeEngine(plan=plan)
        self.assertEqual(self.run_cmd(["turn"], eng), 0)
        self.assertEqual(self.acting()[-1], "ATTACK 7511 0 3 3")
        self.assertEqual(len(eng.solves), 1)

    def test_a_mismatch_re_solves_from_the_live_board(self):
        self.bridge()
        self.fb.damage = 2  # the game hits harder than the engine predicts
        eng = FakeEngine()
        self.assertEqual(self.run_cmd(["turn"], eng), 1)
        self.assertEqual(len(eng.solves), 2)
        # The re-solve saw the live board: mech 0 has moved and acted.
        self.assertFalse(unit(eng.solves[1], 0)["active"])
        self.assertEqual(unit(eng.solves[1], 100)["hp"], 1)
        self.assertEqual(self.acting(), ["MOVE_NATIVE 0 3 4", "ATTACK 0 1 3 3", "ATTACK 1 0 3 3"])
        solve = json.loads((self.run_dir() / "m00_turn_01_solve.json").read_text())
        self.assertTrue(solve["data"]["partial_re_solve"])

    def test_solves_use_the_adaptive_budget_by_default(self):
        self.bridge()
        eng = FakeEngine()
        self.assertEqual(self.run_cmd(["turn"], eng), 0)
        self.assertEqual(eng.budgets, [(10.0, 120.0, lp.MIN_TIERS)])
        self.assertEqual(lp.MIN_TIERS, 5)

    def test_re_solves_keep_the_budget_settings(self):
        self.bridge()
        self.fb.damage = 2  # a mismatch: re-solve
        eng = FakeEngine()
        self.assertEqual(self.run_cmd(["turn", "--time", "30", "--max-time", "90", "--min-tiers", "3"], eng), 1)
        self.assertEqual(eng.budgets, [(30.0, 90.0, 3), (30.0, 90.0, 3)])

    def test_ranking_options_off_by_default(self):
        self.bridge()
        eng = FakeEngine()
        self.assertEqual(self.run_cmd(["turn"], eng), 0)
        self.assertEqual(eng.tiers, [{}])

    def test_mech_grid_is_passed_to_every_solve(self):
        self.bridge()
        self.fb.damage = 2  # a mismatch: re-solve
        eng = FakeEngine()
        self.run_cmd(["turn", "--mech-grid", "1", "--mech-grid-turns"], eng)
        self.assertEqual(eng.tiers, [{"mech_grid": 1, "mech_grid_turns": True}] * 2)

    def test_mech_grid_request(self):
        sent = []
        eng = lp.Engine.__new__(lp.Engine)
        eng.threads = 4
        eng.request = lambda req: sent.append(req) or {"ok": True}
        eng.solve(Path("s.json"), 10)
        eng.solve(Path("s.json"), 10, mech_grid=2)
        eng.solve(Path("s.json"), 10, mech_grid=0, mech_grid_turns=True)
        self.assertNotIn("mech_grid", sent[0])
        self.assertEqual((sent[1]["mech_grid"], "mech_grid_turns" in sent[1]), (2, False))
        self.assertEqual((sent[2]["mech_grid"], sent[2]["mech_grid_turns"]), (0, True))
        self.assertIn("mech exchange: grid slot = 2 x grid + 3 x mechs",
                      lp.plan_summary({"worst_case": {"grid": -2}, "mech_grid_rate": 1, "stats": {}}))
        self.assertNotIn("mech exchange", lp.plan_summary({"worst_case": {"grid": -2}, "stats": {}}))

    def test_lookahead_is_passed_to_every_solve(self):
        self.bridge()
        self.fb.damage = 2  # a mismatch: re-solve
        eng = FakeEngine()
        self.run_cmd(["turn", "--lookahead", "--lookahead-samples", "4", "--lookahead-reply", "idle",
                      "--mech-grid", "2", "--mech-grid-turns", "--mech-grid-pilot"], eng)
        want = {"mech_grid": 2, "mech_grid_turns": True, "mech_grid_pilot": True, "lookahead": True,
                "lookahead_samples": 4, "lookahead_reply": "idle"}
        self.assertEqual(eng.tiers, [want] * 2)

    def test_lookahead_defaults_send_only_the_switch(self):
        self.bridge()
        eng = FakeEngine()
        self.run_cmd(["turn", "--lookahead"], eng)
        self.assertEqual(eng.tiers, [{"lookahead": True}])

    def test_grid_survival_is_passed_with_the_saves_overflow(self):
        """--grid-survival goes to every solve; a bridge without grid_overflow
        gets it from the profile's save (GameData.overflow), as a patch."""
        self.bridge()
        lp.PROFILE.mkdir(parents=True)
        (lp.PROFILE / "saveData.lua").write_text(
            'GameData = {["network"] = 1, ["networkMax"] = 7, ["overflow"] = 25, ["difficulty"] = 3, }\n')
        eng = FakeEngine()
        self.run_cmd(["turn", "--grid-survival"], eng)
        self.assertEqual(eng.tiers, [{"grid_survival": True}])
        self.assertEqual(eng.solves[0]["grid_overflow"], 25)

    def test_no_grid_overflow_patch_without_grid_survival(self):
        self.bridge()
        lp.PROFILE.mkdir(parents=True)
        (lp.PROFILE / "saveData.lua").write_text('GameData = {["network"] = 1, ["overflow"] = 25, }\n')
        eng = FakeEngine()
        self.run_cmd(["turn"], eng)
        self.assertEqual(eng.tiers, [{}])
        self.assertNotIn("grid_overflow", eng.solves[0])

    def test_grid_survival_request_and_summary(self):
        sent = []
        eng = lp.Engine.__new__(lp.Engine)
        eng.threads = 4
        eng.request = lambda req: sent.append(req) or {"ok": True}
        eng.solve(Path("s.json"), 10)
        eng.solve(Path("s.json"), 10, grid_survival=True)
        self.assertNotIn("grid_survival", sent[0])
        self.assertTrue(sent[1]["grid_survival"])
        summary = lp.plan_summary({"worst_case": {"grid_survival": 0.25, "grid": -1}, "stats": {},
                                   "grid_defense": {"percent": 25, "source": "state"}})
        self.assertIn("grid_survival=0.2500 grid=-1", summary)
        self.assertIn("Grid Defense 25%, state", summary)
        self.assertEqual(lp.save_grid_overflow(Path("/nonexistent/saveData.lua")), None)

    def test_pilot_floor_needs_the_exchange(self):
        self.bridge()
        eng = FakeEngine()
        self.run_cmd(["turn", "--mech-grid-pilot"], eng)
        self.assertEqual(eng.tiers, [{}])

    def test_lookahead_request(self):
        sent = []
        eng = lp.Engine.__new__(lp.Engine)
        eng.threads = 4
        eng.request = lambda req: sent.append(req) or {"ok": True}
        eng.solve(Path("s.json"), 10)
        eng.solve(Path("s.json"), 10, lookahead=True)
        eng.solve(Path("s.json"), 10, lookahead=True, lookahead_samples=3, lookahead_reply="greedy",
                  lookahead_fold="sum")
        eng.solve(Path("s.json"), 10, mech_grid=2, mech_grid_turns=True, mech_grid_pilot=True)
        eng.solve(Path("s.json"), 10, mech_grid_pilot=True)
        for key in ("lookahead", "lookahead_samples", "lookahead_reply", "mech_grid_pilot"):
            self.assertNotIn(key, sent[0])
        self.assertEqual(sent[1]["lookahead"], True)
        self.assertNotIn("lookahead_samples", sent[1])
        self.assertEqual((sent[2]["lookahead_samples"], sent[2]["lookahead_reply"], sent[2]["lookahead_fold"]),
                         (3, "greedy", "sum"))
        self.assertTrue(sent[3]["mech_grid_pilot"])
        self.assertNotIn("mech_grid_pilot", sent[4])  # only with the exchange
        summary = lp.plan_summary({"worst_case": {"grid": 0, "next_turn": -1025}, "stats": {},
                                   "next_turn": {"grid": 1, "mechs": 0, "building_hp": 1, "mission_over": False,
                                                 "samples": [{}, {}]}})
        self.assertIn("next_turn=-1025", summary)
        self.assertIn("next turn, worst of 2 AI samples: grid -1", summary)
        self.assertIn("per mech #0 1, #1 3", lp.plan_summary(
            {"worst_case": {"grid": -2}, "mech_grid_rate": 0, "mech_rates": {"0": 0, "1": 1}, "stats": {}}))
        self.assertEqual(lp.SCORE_TIERS.index("next_turn"), lp.SCORE_TIERS.index("objectives_failed") + 1)

    def test_native_only_execution(self):
        self.bridge()
        self.assertEqual(self.run_cmd(["turn"]), 0)
        self.assertEqual(self.fb.commands.count("EXEC_MODE native"), 1)
        first_action = next(i for i, c in enumerate(self.fb.commands) if c.startswith("MOVE_NATIVE"))
        self.assertLess(self.fb.commands.index("EXEC_MODE native"), first_action)

    def test_a_refused_move_stops_without_a_fallback(self):
        self.bridge()
        self.fb.refuse_native = True
        self.assertEqual(self.run_cmd(["turn"]), 1)
        # One retry after selecting the mech in the UI (stale Move target
        # area), then the refusal stands.
        self.assertEqual(self.acting(), ["MOVE_NATIVE 0 3 4", "MOVE_NATIVE 0 3 4"])

    def test_refuses_a_bridge_without_native_execution(self):
        self.bridge()
        self.fb.exec_mode = False
        self.assertEqual(self.run_cmd(["turn"]), 2)
        self.assertEqual(self.acting(), [])

    # ---------------------------------------------------------------- UI action limits
    # Live 2026-10-12 (run 20261012_172449): the engine took a Shifty
    # mech's re-activation for a fresh action and planned it firing and
    # repairing dozens of times; native FireWeapon accepted every one.

    def test_a_plan_over_the_ui_limits_is_refused_before_acting(self):
        st = base_state()
        unit(st, 0)["pilot_id"] = "Pilot_Leader"  # Shifty
        self.bridge(st)
        plan = [
            {"sub": "weapon", "uid": 0, "weapon": "Prime_Punchmech_B", "target": [5, 6], "target2": None,
             "action": 0},
            {"sub": "move", "uid": 0, "to": [6, 5], "action": 1},
            {"sub": "weapon", "uid": 0, "weapon": "Prime_Punchmech_B", "target": [5, 5], "target2": None,
             "action": 2},
        ]
        code, out = self.quiet(["turn"], FakeEngine(plan=plan))
        self.assertEqual(code, 1)
        self.assertEqual(self.acting(), [])
        self.assertIn("per-turn action limits", out)
        self.assertIn("PunchMech#0 (Shifty) would act / move / act", out)
        # The dry run reports it and still acts on nothing.
        code, out = self.quiet(["turn", "--dry-run"], FakeEngine(plan=plan))
        self.assertEqual(code, 0)
        self.assertIn("REFUSED PLAN", out)
        self.assertEqual(self.acting(), [])

    def test_an_attack_from_smoke_is_refused(self):
        st = base_state()
        next(t for t in st["tiles"] if (t["x"], t["y"]) == (3, 4))["smoke"] = True
        self.bridge(st)
        code, out = self.quiet(["turn"], FakeEngine())
        self.assertEqual(code, 1)
        # The move into the smoke is played, the shot from it is not.
        self.assertEqual(self.acting(), ["MOVE_NATIVE 0 3 4"])
        self.assertIn("PunchMech#0 Prime_Lasermech_A at E5: the unit is on smoke", out)

    # Pilot_Soldier (Disable_Immunity): Pawn::IsSmoked is false for her, so
    # the UI arms her weapons on smoke and the guard lets the shot go.
    def camila_from_smoke(self, pilot: dict):
        st = base_state()
        tile(st, 3, 4)["smoke"] = True
        unit(st, 0).update(pilot)
        self.bridge(st)
        self.assertEqual(self.run_cmd(["turn"], FakeEngine()), 0)
        self.assertEqual(self.acting(), ["MOVE_NATIVE 0 3 4", "ATTACK 0 1 3 3", "ATTACK 1 0 3 3"])

    def test_camila_vera_attacks_from_smoke(self):
        self.camila_from_smoke({"pilot_id": "Pilot_Soldier"})

    def test_camila_vera_attacks_from_smoke_pilot_record(self):
        # The bridge extension's pilot record alone (no pilot_id).
        self.camila_from_smoke({"pilot": {"id": "Pilot_Soldier", "level": 0}})

    def test_a_shifty_step_after_attacking_is_played(self):
        st = base_state()
        unit(st, 0)["pilot_id"] = "Pilot_Leader"
        self.bridge(st)
        plan = [
            {"sub": "weapon", "uid": 0, "weapon": "Prime_Punchmech_B", "target": [5, 6], "target2": None,
             "action": 0},
            {"sub": "move", "uid": 0, "to": [6, 5], "action": 1},
        ]
        self.assertEqual(self.run_cmd(["turn"], FakeEngine(plan=plan)), 0)
        self.assertEqual(self.acting(), ["ATTACK 0 0 5 6", "MOVE_NATIVE 0 6 5"])
        meta = json.loads((self.run_dir() / "manifest.json").read_text())
        self.assertEqual(meta["unit_actions"]["units"], {"0": "AM"})

    def test_old_bridge_moved_flags_are_tracked(self):
        self.bridge()
        self.fb.damage = 2
        self.fb.export_moved = False
        eng = FakeEngine()
        self.run_cmd(["turn"], eng)
        self.assertNotIn("moved", unit(eng.solves[0], 0))
        self.assertTrue(unit(eng.solves[1], 0).get("moved"))

    # ---------------------------------------------------------------- queued shots
    # Live 2026-10-10 m36 turn 2: Aerial Bombs smoked Beetle2#943 (its shot
    # cleared), a Gravwell pulled it out; the bridge, reading the save, still
    # showed the shot, and the end-of-turn prediction charged the Beetle.

    def quiet(self, argv, engine):
        out = []
        orig = lp.say
        lp.say = lambda m="": out.append(m)
        try:
            code = self.run_cmd(argv, engine)
        finally:
            lp.say = orig
        return code, "\n".join(out)

    def test_end_turn_drops_a_queued_shot_the_engine_cleared(self):
        self.bridge()
        code, _ = self.quiet(["turn"], FakeEngine(clears={100: 0}))
        self.assertEqual(code, 0)
        self.assertTrue(unit(self.fb.state, 100)["has_queued_attack"])  # the bridge's stale copy
        eng = FakeEngine(clears={100: 0})
        code, text = self.quiet(["end-turn", "--run", str(self.run_dir())], eng)
        self.assertEqual(code, 0, text)
        scorpion = unit(eng.predict_states[0], 100)
        self.assertFalse(scorpion["has_queued_attack"])
        self.assertNotIn("queued_target", scorpion)
        self.assertIn("note: Scorpion1#100 queued attack cleared during the turn (engine)", text)
        # The live state is archived as it was, beside the patched input.
        raw = next(self.run_dir().glob("m00_turn_01_end_turn_raw_*.json"))
        self.assertTrue(unit(json.loads(raw.read_text())["data"]["bridge_state"], 100)["has_queued_attack"])
        # The tracked shots belong to that turn only.
        manifest = json.loads((self.run_dir() / "manifest.json").read_text())
        self.assertEqual((manifest["engine_queued"]["mission"], manifest["engine_queued"]["turn"]), (0, 1))
        self.assertIsNone(manifest["engine_queued"]["units"]["100"])
        run = lp.Run.open(str(self.run_dir()), False)
        self.assertEqual(run.engine_queued(self.fb.state), {})  # turn 2 now

    def test_a_re_solve_does_not_resurrect_a_cleared_shot(self):
        self.bridge()
        self.fb.damage = 2  # step 2 mismatches: re-solve
        eng = FakeEngine(clears={100: 0})
        code, text = self.quiet(["turn"], eng)
        self.assertEqual(code, 1)
        self.assertEqual(len(eng.solves), 2)
        self.assertTrue(unit(eng.solves[0], 100)["has_queued_attack"])
        self.assertFalse(unit(eng.solves[1], 100)["has_queued_attack"])
        self.assertIn("Scorpion1#100 queued attack cleared", text)

    def test_a_clear_in_a_step_that_went_differently_for_the_unit_is_not_taken(self):
        self.bridge()
        self.fb.damage = 2  # the Scorpion's HP differs after step 2
        eng = FakeEngine(clears={100: 1})
        self.assertEqual(self.quiet(["turn"], eng)[0], 1)
        self.assertTrue(unit(eng.solves[1], 100)["has_queued_attack"])

    def test_an_engine_without_queued_boards_changes_nothing(self):
        self.bridge()
        self.assertEqual(self.quiet(["turn"], FakeEngine())[0], 0)
        eng = FakeEngine()
        self.assertEqual(self.quiet(["end-turn", "--run", str(self.run_dir())], eng)[0], 0)
        self.assertTrue(unit(eng.predict_states[0], 100)["has_queued_attack"])
        self.assertFalse(list(self.run_dir().glob("*_end_turn_raw_*.json")))

    def test_override_queued(self):
        st = base_state()
        scorpion = unit(st, 100)
        scorpion.update(queued_origin=[3, 3], queued_target=[2, 3])
        st["units"].append({"uid": 7, "type": "Train_Pawn", "x": 5, "y": 1, "hp": 1, "team": 1,
                            "queued_any": {"skill": 1, "target": [5, 0], "origin": [5, 1], "source": "save"}})
        # Same shot: nothing to change.
        same = {100: {"weapon": 0, "origin": [3, 3], "target": [2, 3]}}
        self.assertEqual(lp.override_queued(copy.deepcopy(st), same), {})
        # Only the origin (the bridge does not shift it): changed, no note.
        s = copy.deepcopy(st)
        found = lp.override_queued(s, {100: {"weapon": 0, "origin": [3, 4], "target": [2, 3]}})
        self.assertEqual(found[100][0], "")
        self.assertEqual(unit(s, 100)["queued_origin"], [3, 4])
        # Retargeted (DIR_FLIP): the engine's target, with a note.
        s = copy.deepcopy(st)
        found = lp.override_queued(s, {100: {"weapon": 0, "origin": [3, 3], "target": [4, 3]}})
        self.assertIn("retargeted", found[100][0])
        self.assertEqual(unit(s, 100)["queued_target"], [4, 3])
        # Cleared, for a non-enemy unit's queued_any too.
        s = copy.deepcopy(st)
        found = lp.override_queued(s, {100: None, 7: None})
        self.assertEqual(sorted(found), [7, 100])
        self.assertFalse(unit(s, 100)["has_queued_attack"])
        self.assertNotIn("queued_any", unit(s, 7))
        # A shot the bridge does not have is left alone; untracked units too.
        s = copy.deepcopy(st)
        unit(s, 100)["has_queued_attack"] = False
        self.assertEqual(lp.override_queued(s, {100: {"weapon": 0, "origin": None, "target": [2, 3]}}), {})
        self.assertEqual(lp.override_queued(copy.deepcopy(st), {}), {})

    def test_track_queued_needs_the_live_unit_to_match(self):
        pred = {"units": [dict(compact(base_state())["units"][2], queued=None)]}
        live = copy.deepcopy(pred)
        tracked = {}
        lp.track_queued(tracked, pred, live, {100})
        self.assertEqual(tracked, {100: None})
        tracked = {100: {"weapon": 0, "origin": None, "target": [2, 3]}}
        live["units"][0]["hp"] = 1
        lp.track_queued(tracked, pred, live, {100})
        self.assertEqual(tracked[100]["target"], [2, 3])  # the last confirmed shot stands
        lp.track_queued(tracked, pred, copy.deepcopy(pred), set())  # not a turn-start unit
        self.assertEqual(tracked[100]["target"], [2, 3])

    # A structure's shield (Science_Shield on a building) is invisible to the
    # bridge on vanilla; live 20261014 m14 (Mission_Survive turns 1 and 2): the
    # end-of-turn prediction read the shielded F4 and G4 bare and lost them.

    def test_shields_the_plan_placed_reach_the_end_of_turn_prediction(self):
        self.bridge()
        code, text = self.quiet(["turn"], FakeEngine(shields=(1, [[2, 2]])))
        self.assertEqual(code, 0, text)
        manifest = json.loads((self.run_dir() / "manifest.json").read_text())
        self.assertEqual(manifest["engine_shields"], {"mission": 0, "turn": 1, "tiles": [[2, 2]]})
        self.assertFalse(tile(self.fb.state, 2, 2).get("shield"))  # the bridge cannot see it
        eng = FakeEngine(shields=(1, [[2, 2]]), enemy_shields=[[2, 2]])
        code, text = self.quiet(["end-turn", "--run", str(self.run_dir())], eng)
        self.assertEqual(code, 0, text)
        self.assertTrue(tile(eng.predict_states[0], 2, 2)["shield"])
        self.assertIn("note: shielded (engine; the bridge cannot read a structure's shield): F6", text)
        saved = json.loads((self.run_dir() / "m00_turn_01_end_turn_01.json").read_text())
        self.assertEqual(saved["data"]["patches"]["tile_shields"], [[2, 2]])
        raw = next(self.run_dir().glob("m00_turn_01_end_turn_raw_*.json"))
        self.assertFalse(tile(json.loads(raw.read_text())["data"]["bridge_state"], 2, 2).get("shield"))
        # The enemy phase left it standing and the game matched: next turn too.
        manifest = json.loads((self.run_dir() / "manifest.json").read_text())
        self.assertEqual(manifest["engine_shields"], {"mission": 0, "turn": 2, "tiles": [[2, 2]]})
        eng = FakeEngine()
        self.assertEqual(self.quiet(["turn", "--dry-run", "--run", str(self.run_dir())], eng)[0], 0)
        self.assertTrue(tile(eng.solves[0], 2, 2)["shield"])

    def test_a_shield_is_not_carried_past_an_enemy_phase_that_differed(self):
        self.bridge()
        self.assertEqual(self.quiet(["turn"], FakeEngine(shields=(1, [[2, 2]])))[0], 0)
        self.fb.next_turn = lambda st: dict(FakeBridge.default_next(st), grid_power=5)
        eng = FakeEngine(shields=(1, [[2, 2]]), enemy_shields=[[2, 2]])
        code, text = self.quiet(["end-turn", "--run", str(self.run_dir())], eng)
        self.assertEqual(code, 1, text)
        self.assertIn("grid: engine 7, game 5", text)
        manifest = json.loads((self.run_dir() / "manifest.json").read_text())
        self.assertEqual(manifest["engine_shields"], {"mission": 0, "turn": 2, "tiles": []})

    def test_an_engine_without_shield_boards_patches_no_shield(self):
        self.bridge()
        self.assertEqual(self.quiet(["turn"], FakeEngine())[0], 0)
        eng = FakeEngine()
        self.assertEqual(self.quiet(["end-turn", "--run", str(self.run_dir())], eng)[0], 0)
        self.assertNotIn("shield", tile(eng.predict_states[0], 2, 2))

    def test_confirmed_shields(self):
        pred = {"buildings": [{"x": 2, "y": 2, "hp": 2}, {"x": 4, "y": 1, "hp": 1}],
                "shield_tiles": [[4, 1], [2, 2], [0, 0]]}  # (0, 0): a mountain
        live = {"buildings": [{"x": 2, "y": 2, "hp": 2}, {"x": 4, "y": 1, "hp": 2}]}
        # (4, 1) went differently in the game: its shield may be gone.
        self.assertEqual(lp.confirmed_shields(pred, live), [[0, 0], [2, 2]])
        self.assertEqual(lp.confirmed_shields({"buildings": pred["buildings"]}, live), [])

    def test_apply_shields(self):
        st = base_state()
        tile(st, 0, 0).update(terrain="mountain", terrain_id=4, building_hp=2)
        tile(st, 5, 5).update(terrain="building", terrain_id=1, building_hp=0)  # destroyed
        s = copy.deepcopy(st)
        self.assertEqual(lp.apply_shields(s, [[2, 2], [0, 0], [5, 5], [3, 3]]), [[0, 0], [2, 2]])
        self.assertTrue(tile(s, 2, 2)["shield"])
        self.assertTrue(tile(s, 0, 0)["shield"])
        self.assertNotIn("shield", tile(s, 5, 5))
        self.assertNotIn("shield", tile(s, 3, 3))  # ground: a pawn's shield is its own
        self.assertEqual(lp.apply_shields(s, [[2, 2]]), [])  # already shielded
        # A bridge that reads structure shields itself is the authority.
        s = copy.deepcopy(st)
        s["tile_shields_live"] = True
        self.assertEqual(lp.apply_shields(s, [[2, 2]]), [])
        self.assertNotIn("shield", tile(s, 2, 2))

    # ---------------------------------------------------------------- end turn

    def test_end_turn_clicks_when_the_bridge_cannot_and_compares(self):
        self.bridge()
        eng = FakeEngine()
        self.assertEqual(self.run_cmd(["end-turn"], eng), 0)
        self.assertEqual(self.acting(), ["END_TURN"])
        self.assertEqual(self.clicks, [lv.END_TURN_SCREEN])
        self.assertEqual(eng.predicts, [[]])
        self.assertEqual(self.fb.state["turn"], 2)
        self.assertTrue(list(self.run_dir().glob("m00_turn_02_after_enemy_*.json")))

    def test_end_turn_waits_for_a_late_click_instead_of_stopping(self):
        self.bridge()
        old = lp.CLICK_WAIT
        lp.CLICK_WAIT = 0.2
        self.addCleanup(setattr, lp, "CLICK_WAIT", old)

        def late_click(xy):
            # Only the third click registers, after its own wait has run out.
            self.clicks.append(xy)
            if len(self.clicks) == 3:
                def later():
                    time.sleep(0.6)
                    self.fb.advance()
                threading.Thread(target=later, daemon=True).start()
            return True

        rc = lp.main(["end-turn", "--new-run"], engine=FakeEngine(), click=late_click)
        self.assertEqual(rc, 0)
        self.assertEqual(len(self.clicks), 3)
        self.assertEqual(self.fb.state["turn"], 2)

    def go_silent(self, xy):
        """An End Turn click after which the game leaves the mission."""
        self.clicks.append(xy)
        self.fb.silent = True
        old = time.time() - 60
        os.utime(BRIDGE / "itb_bridge_heartbeat", (old, old))
        return True

    def test_end_turn_on_the_final_mission_ends_the_run_when_the_bridge_goes_silent(self):
        st = base_state()
        st["mission_id"] = "Mission_Final_Cave"
        self.bridge(st)
        old = lp.SILENT_AFTER
        lp.SILENT_AFTER = 0.3
        self.addCleanup(setattr, lp, "SILENT_AFTER", old)
        rc = lp.main(["mission", "--new-run"], engine=FakeEngine(), click=self.go_silent)
        self.assertEqual(rc, 0)
        self.assertEqual(len(self.clicks), 1)
        events = (self.run_dir() / "events.jsonl").read_text()
        self.assertIn('"run_over"', events)

    def test_end_turn_stops_when_the_bridge_goes_silent_elsewhere(self):
        self.bridge()
        old = lp.SILENT_AFTER
        lp.SILENT_AFTER = 0.3
        self.addCleanup(setattr, lp, "SILENT_AFTER", old)
        rc = lp.main(["end-turn", "--new-run"], engine=FakeEngine(), click=self.go_silent)
        self.assertEqual(rc, 1)
        self.assertEqual(len(self.clicks), 1)

    def test_end_turn_follows_the_final_mission_into_the_cave(self):
        # Mission_Final's last turn goes straight on to the Cave's turn 1.
        st = base_state()
        st["mission_id"], st["turn"] = "Mission_Final", 4
        self.bridge(st)

        def cave(s):
            s = FakeBridge.default_next(s)
            s["mission_id"], s["turn"] = "Mission_Final_Cave", 1
            return s

        self.fb.next_turn = cave
        self.assertEqual(self.run_cmd(["mission"]), 0)
        self.assertEqual(self.acting().count("END_TURN"), 1)
        self.assertEqual(self.fb.state["mission_id"], "Mission_Final_Cave")

    def test_end_turn_without_a_click_when_the_bridge_ends_it(self):
        self.bridge()
        self.fb.end_turn_ack = "OK END_TURN phase=combat_player method=EndTurn"
        self.assertEqual(self.run_cmd(["end-turn"]), 0)
        self.assertEqual(self.clicks, [])

    def test_end_turn_reports_enemy_phase_differences_and_spawns(self):
        self.bridge()

        def next_turn(st):
            st = FakeBridge.default_next(st)
            unit(st, 1)["hp"] = 1  # the engine did not see this coming
            unit(st, 100)["x"] = 4  # Vek move while the AI plans: not a difference
            st["units"].append({"uid": 101, "type": "Firefly1", "x": 5, "y": 5, "hp": 3, "team": 6})
            return st

        self.fb.next_turn = next_turn
        out = []
        orig = lp.say
        lp.say = lambda m="": out.append(m)
        try:
            code = self.run_cmd(["end-turn"])
        finally:
            lp.say = orig
        self.assertEqual(code, 1)
        text = "\n".join(out)
        self.assertIn("TankMech#1 hp: engine 3, game 1", text)
        self.assertIn("Firefly1#101", text)
        self.assertNotIn("Scorpion1#100: engine at", text)

    def test_a_dived_burrower_is_carried_over_and_resurfaces_by_uid(self):
        # Live 2026-10-10 m20 (Mission_Reactivation) turn 2: the Laser hurt
        # Burrower1#1683, which dove; the bridge no longer listed it, the
        # end-of-turn prediction did not have it and its resurfacing in the
        # AI's move was reported as a spawn.
        st = base_state()
        unit(st, 100).update(type="Burrower1", has_queued_attack=False)
        unit(st, 100).pop("queued_target")
        self.bridge(st)

        def next_turn(s):
            s = FakeBridge.default_next(s)
            b = unit(s, 100)
            b.pop("underground")
            b["x"], b["y"] = 4, 4  # the AI's move brings it up elsewhere
            return s

        self.fb.next_turn = next_turn
        code, text = self.quiet(["turn"], FakeEngine())
        self.assertEqual(code, 0, text)
        self.assertNotIn(100, [u["uid"] for u in lv.read_json(lv.STATE)["units"]])  # the bridge omits it
        manifest = json.loads((self.run_dir() / "manifest.json").read_text())
        self.assertEqual(manifest["engine_underground"]["units"]["100"]["hp"], 2)

        eng = FakeEngine()
        code, text = self.quiet(["end-turn", "--run", str(self.run_dir())], eng)
        self.assertEqual(code, 0, text)
        carried = unit(eng.predict_states[0], 100)
        self.assertTrue(carried["underground"])
        self.assertEqual((carried["x"], carried["y"], carried["hp"]), (3, 3, 2))
        self.assertIn("note: Burrower1#100 underground (dove at E5, hp 2)", text)
        self.assertIn("note: Burrower1#100 resurfaced at D4 hp 2", text)
        self.assertNotIn("new units", text)
        manifest = json.loads((self.run_dir() / "manifest.json").read_text())
        self.assertEqual(manifest["engine_underground"]["units"], {})

    # ---------------------------------------------------------------- Mission_Reactivation
    # Live 2026-10-14 m12: the mission thawed Snowtank1#13089 at turn 0, the
    # Ice Mech re-froze it, and the game kept it frozen. The engine is given
    # the list a player can know (frozen at the start, minus seen thaws), never
    # the mission's own Enemies list.

    @staticmethod
    def reactivation_state():
        st = base_state()
        st["mission_id"] = "Mission_Reactivation"
        for uid, x in ((110, 7), (111, 5), (112, 1)):
            st["units"].append({"uid": uid, "type": "Scorpion1", "x": x, "y": 0, "hp": 3, "max_hp": 3, "team": 6,
                                "mech": False, "active": False, "frozen": True})
        # The mission's hidden list, deliberately different: never used.
        st["mission_state"] = {"instance": {"Enemies": [111, 999]}}
        return st

    def test_reactivation_visible_list_follows_the_seen_thaws(self):
        self.bridge(self.reactivation_state())

        def thaw(s, uids):
            for uid in uids:
                unit(s, uid)["frozen"] = False
            return s

        # The engine's default branch thaws 110 and 111; the game thaws 110 and 112.
        phase = {"mission_ended": False, "events": [
            {"type": "thawed", "uid": uid, "amount": 3, "detail": "candidates 110,111,112"} for uid in (110, 111)]}
        self.fb.next_turn = lambda s: thaw(FakeBridge.default_next(s), (110, 112))
        eng = FakeEngine(after_enemy=lambda b: compact(thaw(copy.deepcopy(b), (110, 111))), enemy_phase=phase)
        code, text = self.quiet(["turn"], eng)
        self.assertEqual(code, 0, text)
        self.assertEqual(eng.solves[0]["reactivation_visible_list"], [110, 111, 112])
        # The hidden list is compared for diagnostics only.
        self.assertIn("note: Mission_Reactivation visible list [110, 111, 112]; [110, 112] already drawn", text)
        code, text = self.quiet(["end-turn", "--run", str(self.run_dir())], eng)
        self.assertEqual(code, 0, text)
        self.assertEqual(eng.predict_states[0]["reactivation_visible_list"], [110, 111, 112])
        self.assertIn("random thaw: the game thawed", text)
        manifest = json.loads((self.run_dir() / "manifest.json").read_text())
        self.assertEqual(manifest["reactivation"]["list"], [111])
        self.assertEqual(manifest["reactivation"]["turn"], 2)

        # Turn 2: the player re-freezes 110 (thawed by the mission: stays off
        # the list); a later turn keeps the list.
        unit(self.fb.state, 110)["frozen"] = True
        self.fb.write_state()
        eng2 = FakeEngine()
        code, text = self.quiet(["turn", "--run", str(self.run_dir())], eng2)
        self.assertEqual(code, 0, text)
        self.assertEqual(eng2.solves[0]["reactivation_visible_list"], [111])

    def test_reactivation_list_unknown_when_joining_the_mission_late(self):
        st = self.reactivation_state()
        st["turn"] = 3
        self.bridge(st)
        eng = FakeEngine()
        code, text = self.quiet(["turn"], eng)
        self.assertEqual(code, 0, text)
        self.assertNotIn("reactivation_visible_list", eng.solves[0])  # the engine falls back

    def test_reactivation_list_starts_at_deployment(self):
        st = self.deploy_state()
        st["mission_id"] = "Mission_Reactivation"
        unit(st, 100)["frozen"] = True
        st["units"].append({"uid": 110, "type": "Scorpion1", "x": 7, "y": 0, "hp": 3, "max_hp": 3, "team": 6,
                            "mech": False, "active": False, "frozen": True})
        self.bridge(st)
        eng = FakeEngine()
        self.assertEqual(self.run_cmd(["deploy"], engine=eng), 0)
        self.assertEqual(eng.deploys[0][0]["reactivation_visible_list"], [100, 110])
        # Turn 0's enemy phase: the mission thawed 100 (only it thaws anything then).
        s = self.fb.state
        s.update(turn=1, deploying=False)
        unit(s, 100)["frozen"] = False
        for u in s["units"]:
            if u["team"] == 1:
                u["active"], u["moved"] = True, False
        self.fb.write_state()
        eng = FakeEngine()
        code, text = self.quiet(["turn", "--run", str(self.run_dir())], eng)
        self.assertEqual(code, 0, text)
        self.assertEqual(eng.solves[0]["reactivation_visible_list"], [110])

    @staticmethod
    def building_not_resisted(st):
        """The engine's worst case: the building at (2, 2) (F6) does not resist."""
        b = compact(st)
        b["buildings"] = [dict(x, hp=0) if (x["x"], x["y"]) == (2, 2) else x for x in b["buildings"]]
        b["grid_power"] -= 1
        return b

    def test_end_turn_grid_defense_resist_is_a_note(self):
        self.bridge()
        phase = {"mission_ended": False, "chance_nodes": 1, "grid_defense": [{"point": [2, 2], "amount": 1}]}
        out = []
        orig = lp.say
        lp.say = lambda m="": out.append(m)
        try:
            code = self.run_cmd(["end-turn"], FakeEngine(after_enemy=self.building_not_resisted,
                                                         enemy_phase=phase))
        finally:
            lp.say = orig
        text = "\n".join(out)
        self.assertEqual(code, 0, text)
        self.assertIn("Grid Defense resisted at F6", text)
        self.assertIn("enemy phase: MATCH", text)

    def test_end_turn_unexplained_building_hp_still_stops(self):
        self.bridge()
        # No chance node in the prediction: the same difference is an anomaly.
        self.assertEqual(self.run_cmd(["end-turn"], FakeEngine(after_enemy=self.building_not_resisted)), 1)

    def test_end_turn_refuses_outside_a_player_turn(self):
        st = base_state()
        st["phase"] = "combat_enemy"
        self.bridge(st)
        self.assertEqual(self.run_cmd(["end-turn"]), 2)
        self.assertEqual(self.acting(), [])

    def test_mission_stops_at_the_first_anomaly_before_ending_the_turn(self):
        self.bridge()
        self.fb.damage = 2
        self.assertEqual(self.run_cmd(["mission"]), 1)
        self.assertNotIn("END_TURN", self.acting())

    def test_mission_plays_until_the_mission_ends(self):
        self.bridge()

        def next_turn(st):
            st = FakeBridge.default_next(st)
            if st["turn"] > 2:
                st["phase"], st["in_active_mission"] = "unknown", False
            return st

        self.fb.next_turn = next_turn
        self.assertEqual(self.run_cmd(["mission"]), 0)
        self.assertEqual(self.acting().count("END_TURN"), 2)

    # ---------------------------------------------------------------- deploy

    def deploy_state(self):
        st = base_state()
        st["turn"] = 0
        st["deploying"] = True
        st["drop_zone"] = [[x, y] for x in range(1, 4) for y in range(1, 7)]
        st["drop_zone_source"] = "zone"
        for u in st["units"]:
            if u["team"] == 1:
                u["x"], u["y"] = -1, -1
        return st

    def test_deploy_uses_the_drop_zone_and_skips_refused_tiles(self):
        self.bridge(self.deploy_state())
        ranked = lp.choose_deploy_tiles(self.fb.state, [tuple(p) for p in self.fb.state["drop_zone"]], 2)
        self.fb.refused_deploy = {ranked[0]}
        self.assertEqual(self.run_cmd(["deploy"]), 0)
        deploys = [c for c in self.acting() if c.startswith("DEPLOY")]
        self.assertEqual(len(deploys), 3)  # one refused, retried elsewhere
        placed = {(u["x"], u["y"]) for u in self.fb.state["units"] if u["team"] == 1}
        self.assertNotIn(ranked[0], placed)
        self.assertTrue(placed <= {tuple(p) for p in self.fb.state["drop_zone"]})

    def placed_tiles(self):
        return {u["uid"]: (u["x"], u["y"]) for u in self.fb.state["units"] if u["team"] == 1}

    def test_deploy_engine_search_moves_the_mechs(self):
        self.bridge(self.deploy_state())
        zone = [tuple(p) for p in self.fb.state["drop_zone"]]
        h = lp.choose_deploy_tiles(self.fb.state, zone, 2)[:2]
        free = next(p for p in zone if p not in h)
        engine = FakeEngine()
        # Mech 0 takes mech 1's heuristic tile: mech 1 has to move first.
        engine.deploy_tiles = {0: h[1], 1: free}
        self.assertEqual(self.run_cmd(["deploy"], engine=engine), 0)
        self.assertEqual(self.placed_tiles(), {0: h[1], 1: free})
        state, budget, always = engine.deploys[0]
        self.assertEqual(always, [[h[0], h[1]]])  # the heuristic is a candidate
        self.assertEqual(engine.deploy_rankings, [{}])  # no ranking options: the request is as before
        self.assertEqual(sorted(u["uid"] for u in state["units"] if u.get("mech")), [0, 1])  # mechs on the board
        self.assertEqual(budget, 45.0)
        deploys = [c for c in self.acting() if c.startswith("DEPLOY")]
        self.assertEqual(deploys[2:], [f"DEPLOY 1 {free[0]} {free[1]}", f"DEPLOY 0 {h[1][0]} {h[1][1]}"])
        names = [p.name for p in self.run_dir().iterdir()]
        self.assertIn("m00_turn_00_deployed_02.json", names)

    def test_deploy_passes_the_ranking_options(self):
        self.bridge(self.deploy_state())
        engine = FakeEngine()
        args = ["deploy", "--threat-tier", "--objectives-first", "--mech-grid", "2", "--mech-grid-turns",
                "--mech-grid-pilot", "--lookahead"]
        self.assertEqual(self.run_cmd(args, engine=engine), 0)
        self.assertEqual(engine.deploy_rankings, [{"threat_tier": True, "objectives_first": True, "mech_grid": 2,
                                                   "mech_grid_turns": True, "mech_grid_pilot": True,
                                                   "lookahead": True}])

    def test_deploy_engine_search_swaps_through_a_free_tile(self):
        self.bridge(self.deploy_state())
        zone = [tuple(p) for p in self.fb.state["drop_zone"]]
        h = lp.choose_deploy_tiles(self.fb.state, zone, 2)[:2]
        engine = FakeEngine()
        engine.deploy_tiles = {0: h[1], 1: h[0]}
        self.assertEqual(self.run_cmd(["deploy"], engine=engine), 0)
        self.assertEqual(self.placed_tiles(), {0: h[1], 1: h[0]})

    def test_deploy_keeps_the_heuristic_when_the_engine_cannot_search(self):
        self.bridge(self.deploy_state())
        engine = FakeEngine()
        engine.deploy_tiles = lp.Anomaly("itb_live deploy: no mech on the board")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(self.run_cmd(["deploy"], engine=engine), 0)
        self.assertIn("keeping the heuristic placement", out.getvalue())
        self.assertEqual(len([c for c in self.acting() if c.startswith("DEPLOY")]), 2)
        h = lp.choose_deploy_tiles(self.deploy_state(), [tuple(p) for p in self.fb.state["drop_zone"]], 2)[:2]
        self.assertEqual(self.placed_tiles(), {0: h[0], 1: h[1]})

    def test_deploy_heuristic_only(self):
        self.bridge(self.deploy_state())
        engine = FakeEngine()
        self.assertEqual(self.run_cmd(["deploy", "--heuristic"], engine=engine), 0)
        self.assertEqual(engine.deploys, [])
        self.assertEqual(len([c for c in self.acting() if c.startswith("DEPLOY")]), 2)

    def test_deploy_tiles_override(self):
        self.bridge(self.deploy_state())
        self.assertEqual(self.run_cmd(["deploy", "--tiles", "A1,C5"]), 2)  # A1 is outside the zone
        self.assertEqual(self.acting(), [])
        self.assertEqual(self.run_cmd(["deploy", "--tiles", "C5,E6"]), 0)
        self.assertEqual(self.acting(), ["DEPLOY 0 3 5", "DEPLOY 1 2 3"])
        self.assertEqual(lp.parse_tiles("3,5;2,2"), [(3, 5), (2, 2)])

    # ---------------------------------------------------------------- comparing

    def test_diff_boards(self):
        a = compact(base_state())
        b = copy.deepcopy(a)
        self.assertEqual(lp.diff_boards(a, b, {0, 1, 100}), ([], []))
        b["units"][2]["x"] = 4
        b["buildings"] = []
        diffs, _ = lp.diff_boards(a, b, {0, 1, 100})
        self.assertEqual(diffs, ["Scorpion1#100: engine at E5, game at E4", "building F6 hp: engine 1, game gone"])
        diffs, _ = lp.diff_boards(a, b, {0, 1, 100}, enemy_phase=True)
        self.assertEqual(diffs, ["building F6 hp: engine 1, game gone"])
        # Units the engine creates have engine uids: matched by type and tile.
        a["units"].append({"uid": 500, "type": "Spiderling1", "x": 1, "y": 1, "hp": 1, "team": 6, "mech": False})
        b = copy.deepcopy(a)
        b["units"][-1]["uid"] = 120
        self.assertEqual(lp.diff_boards(a, b, {0, 1, 100})[0], [])

    def test_xp_split_level_up_is_a_note(self):
        a = compact(base_state())
        b = copy.deepcopy(a)
        mech = next(u for u in b["units"] if u.get("mech"))
        mech["hp"] += 2
        # Without an XP split in the prediction it is a difference...
        diffs, notes = lp.diff_boards(a, b, {0, 1, 100}, enemy_phase=True)
        self.assertEqual(len(diffs), 1)
        # ...with one, the remainder's level-up (+2 HP) is a note.
        diffs, notes = lp.diff_boards(a, b, {0, 1, 100}, enemy_phase=True, xp_split=True)
        self.assertEqual(diffs, [])
        self.assertEqual(len(notes), 1)
        self.assertIn("XP split", notes[0])
        # Only after the enemy phase, and only +2.
        self.assertEqual(len(lp.diff_boards(a, b, {0, 1, 100}, xp_split=True)[0]), 1)
        mech["hp"] += 1
        self.assertEqual(len(lp.diff_boards(a, b, {0, 1, 100}, enemy_phase=True, xp_split=True)[0]), 1)


@unittest.skipUnless(lp.ITB_LIVE.exists(), "engine/build/itb_live not built")
class ItbLiveTest(unittest.TestCase):
    """The real engine on an archived state (needs the game's scripts)."""

    def test_board_of_an_archived_live_state(self):
        state = next((REPO / "recordings" / "live" / "2026-10-09").glob("Mission_Final_Cave_t2_start_*.json"))
        res = subprocess.run([str(lp.ITB_LIVE), "board", str(state)], capture_output=True, text=True, timeout=120)
        out = json.loads(res.stdout)
        if not out.get("ok") and "scripts" in out.get("error", ""):
            self.skipTest("no game install (set ITB_GAME_DIR)")
        self.assertTrue(out["ok"], out)
        self.assertEqual(out["turn"], 2)
        self.assertEqual(len([u for u in out["board"]["units"] if u["mech"]]), 3)

    def itb_live(self, *args) -> dict:
        res = subprocess.run([str(lp.ITB_LIVE), *args], capture_output=True, text=True, timeout=300)
        out = json.loads(res.stdout)
        if not out.get("ok") and "scripts" in out.get("error", ""):
            self.skipTest("no game install (set ITB_GAME_DIR)")
        self.assertTrue(out["ok"], out)
        return out

    def test_solve_with_the_lookahead(self):
        """itb_live solve --lookahead on an archived live state: the
        next_turn tier, its estimate of the plan's end board and the
        lookahead's counters; off, none of them."""
        state = REPO / "engine" / "tests" / "fixtures" / "live_filler_t2_solve_input.json"
        off = self.itb_live("solve", str(state), "--time", "1", "--threads", "1")
        self.assertNotIn("next_turn", off["worst_case"])
        self.assertNotIn("next_turn", off)
        self.assertNotIn("lookahead_evals", off["stats"])
        on = self.itb_live("solve", str(state), "--time", "2", "--threads", "1", "--lookahead",
                           "--lookahead-samples", "2")
        self.assertIn("next_turn", on["worst_case"])
        self.assertLessEqual(on["worst_case"]["next_turn"], 0)
        self.assertIn("lookahead_evals", on["stats"])
        self.assertEqual(len(on["next_turn"]["samples"]), 2)
        self.assertIn("next_turn", on["predicted_score"])

    def test_end_of_turn_prediction_uses_the_engines_queued_shots(self):
        """Live 2026-10-10 m36 turn 2 (Mission_Filler), end to end: the
        plan's predicted boards clear the smoked Firefly2's and Beetle2's
        shots; the bridge's End Turn snapshot still has them; patched with
        the tracked shots, the prediction is the game's (the Beetle dies
        blocking the E3 spawn, the Filler_Pawn lives)."""
        fixtures = REPO / "engine" / "tests" / "fixtures"
        tmp = Path(tempfile.mkdtemp(prefix="itb_live_queued_"))
        self.addCleanup(shutil.rmtree, tmp, True)
        plan = [
            {"uid": 1, "move": [1, 4], "kind": "weapon", "weapon": "Ranged_Defensestrike_A", "target": [4, 4]},
            {"uid": 0, "move": [5, 3], "kind": "weapon", "weapon": "Brute_Jetmech_A", "target": [5, 1]},
            {"uid": 2, "move": [5, 4], "kind": "weapon", "weapon": "Science_Gravwell", "target": [5, 2]},
        ]
        pred = self.itb_live("predict", str(fixtures / "live_filler_t2_solve_input.json"), "--plan", json.dumps(plan))
        start = json.loads((fixtures / "live_filler_t2_solve_input.json").read_text())["data"]["bridge_state"]
        known = {u["uid"] for u in lp.units(start)}
        tracked = {}
        for step in pred["steps"]:  # every step matched live
            lp.track_queued(tracked, step["board"], step["board"], known)
        self.assertIsNone(tracked[943])
        self.assertIsNone(tracked[941])
        self.assertEqual(tracked[942]["target"], [4, 3])

        stale = json.loads((fixtures / "live_filler_t2_end_turn.json").read_text())["data"]["bridge_state"]
        self.assertTrue(unit(stale, 943)["has_queued_attack"])
        notes = []
        fixed, patches = lp.patch_state(stale, {}, set(), tracked, notes)
        self.assertIsNone(patches["943.queued"])
        self.assertTrue(any(n.startswith("Beetle2#943 queued attack cleared") for n in notes), notes)
        (tmp / "stale.json").write_text(json.dumps(stale))
        (tmp / "fixed.json").write_text(json.dumps(fixed))
        before = self.itb_live("predict", str(tmp / "stale.json"), "--plan", "[]")["after_enemy"]
        after = self.itb_live("predict", str(tmp / "fixed.json"), "--plan", "[]")["after_enemy"]
        alive = lambda board: {u["uid"] for u in board["units"]}  # noqa: E731
        self.assertIn(943, alive(before))  # the bug: the Beetle charged and lived
        self.assertNotIn(938, alive(before))
        self.assertNotIn(943, alive(after))
        self.assertIn(938, alive(after))

    def test_end_of_turn_prediction_keeps_the_plans_building_shields(self):
        """Live 20261014_175032 m14 turn 1 (Mission_Survive, Unfair), end to
        end: Science_Shield from D4 at F4 shields F4 and G4; the bridge's End
        Turn snapshot has them bare. Read so, the engine lost G4 and the grid
        (0); with the shields the driver carries over, it is the game's (grid
        2, F4 and G4 at 2 HP: the Scarab's and the Digger's hits popped the
        shields)."""
        fixtures = REPO / "engine" / "tests" / "fixtures"
        tmp = Path(tempfile.mkdtemp(prefix="itb_live_shields_"))
        self.addCleanup(shutil.rmtree, tmp, True)
        plan = [
            {"uid": 2, "move": [4, 4], "kind": "weapon", "weapon": "Science_Shield", "target": [4, 2]},
            {"uid": 1, "move": [3, 1], "kind": "weapon", "weapon": "Brute_PierceShot", "target": [3, 2]},
            {"uid": 0, "move": [3, 6], "kind": "weapon", "weapon": "Prime_Lasermech", "target": [3, 7]},
        ]
        pred = self.itb_live("predict", str(fixtures / "live_survive_t1_solve_input.json"),
                             "--plan", json.dumps(plan))
        self.assertEqual(pred["after_enemy"]["grid_power"], 2)
        shields = []
        for step in pred["steps"]:  # every step matched live
            shields = lp.confirmed_shields(step["board"], step["board"])
        self.assertEqual(shields, [[4, 1], [4, 2]])

        bare = json.loads((fixtures / "live_survive_t1_end_turn.json").read_text())["data"]["bridge_state"]
        notes = []
        fixed, patches = lp.patch_state(bare, {}, set(), notes=notes, shields=shields)
        self.assertEqual(patches["tile_shields"], [[4, 1], [4, 2]])
        self.assertIn("shielded (engine; the bridge cannot read a structure's shield): G4, F4", notes)
        (tmp / "bare.json").write_text(json.dumps(bare))
        (tmp / "fixed.json").write_text(json.dumps(fixed))
        before = self.itb_live("predict", str(tmp / "bare.json"), "--plan", "[]")["after_enemy"]
        after = self.itb_live("predict", str(tmp / "fixed.json"), "--plan", "[]")["after_enemy"]
        hp = lambda board, x, y: next((b["hp"] for b in board["buildings"]  # noqa: E731
                                       if (b["x"], b["y"]) == (x, y)), 0)
        self.assertEqual(before["grid_power"], 0)  # the bug
        self.assertEqual(hp(before, 4, 1), 0)
        self.assertEqual(after["grid_power"], 2)
        self.assertEqual((hp(after, 4, 1), hp(after, 4, 2)), (2, 2))


def tearDownModule():
    shutil.rmtree(_BRIDGE_DIR, ignore_errors=True)


class GridBaseWarningTest(unittest.TestCase):
    def test_restart_bug_is_flagged_only_on_unfair(self):
        self.assertIsNotNone(lp.grid_base_warning({"grid_defense_base": 15, "difficulty": 3}))
        self.assertIsNotNone(lp.grid_base_warning({"grid_defense_base": 15,
                                                   "mission_state": {"difficulty": 3}}))
        self.assertIsNone(lp.grid_base_warning({"grid_defense_base": 0, "difficulty": 3}))
        self.assertIsNone(lp.grid_base_warning({"grid_defense_base": 15, "difficulty": 2}))
        self.assertIsNone(lp.grid_base_warning({"difficulty": 3}))  # older bridge: unknown


if __name__ == "__main__":
    unittest.main()


class PatchStateGridTest(unittest.TestCase):
    def test_live_grid_estimate_replaces_the_stale_save_grid(self):
        # Live 2026-10-10 (BeetleBoss turn 2): a building lost 1 HP during the
        # player's turn; the save still said 7, the bridge estimated 6.
        state = {"grid_power": 7, "grid_power_estimate": 6, "units": []}
        fixed, patches = lp.patch_state(state, {}, set())
        self.assertEqual(fixed["grid_power"], 6)
        self.assertEqual(patches["grid_power"], 6)
        self.assertEqual(state["grid_power"], 7)

    def test_no_estimate_or_no_loss_leaves_the_grid(self):
        for state in ({"grid_power": 7, "units": []}, {"grid_power": 7, "grid_power_estimate": 7, "units": []}):
            fixed, patches = lp.patch_state(state, {}, set())
            self.assertEqual(fixed["grid_power"], 7)
            self.assertNotIn("grid_power", patches)


class UiLimitTest(unittest.TestCase):
    STATE = {"units": [{"uid": 0, "pilot_id": "Pilot_Leader"}, {"uid": 1, "pilot": {"id": "Pilot_Miner"}},
                       {"uid": 2, "pilot_id": "Pilot_Aquatic"}, {"uid": 3, "pilot_id": "Pilot_Rust"}]}

    @staticmethod
    def steps(uid, seq):
        return [{"uid": uid, "sub": "move" if c == "M" else "weapon"} for c in seq]

    def errors(self, uid, seq, history=None):
        return lp.ui_limit_errors(self.steps(uid, seq), history or {}, self.STATE)

    def test_sequences(self):
        ok = {0: ("", "M", "A", "MA", "AM", "MAM"), 1: ("MA", "A", "AA", "M"), 2: ("MAM", "AM"),
              3: ("MA", "A", "M")}
        bad = {0: ("AA", "AMA", "MAA", "MM"), 1: ("MAA", "AAA", "AM", "AAM"), 2: ("AA", "AMA"),
               3: ("AA", "AM", "MM", "AMA")}
        for uid, seqs in ok.items():
            for seq in seqs:
                self.assertEqual(self.errors(uid, seq), [], f"{uid} {seq}")
        for uid, seqs in bad.items():
            for seq in seqs:
                self.assertEqual(len(self.errors(uid, seq)), 1, f"{uid} {seq}")

    def test_history_counts(self):
        # One action already done: the plan may not add another.
        self.assertEqual(len(self.errors(0, "A", {0: "MA"})), 1)
        self.assertEqual(self.errors(0, "M", {0: "MA"}), [])
        self.assertIn("1 already done", self.errors(3, "A", {3: "A"})[0])
        # A unit the plan leaves alone is not judged.
        self.assertEqual(lp.ui_limit_errors([], {0: "AAA"}, self.STATE), [])

    def test_ui_blocked_on_smoke_and_in_water(self):
        def state(tile, **unit_fields):
            u = {"uid": 0, "x": 1, "y": 1, "flying": False, "traits": {}}
            u.update(unit_fields)
            return {"units": [u], "tiles": [dict({"x": 1, "y": 1, "terrain": "ground", "terrain_id": 0}, **tile)]}
        self.assertIsNone(lp.ui_blocked(state({}), 0))
        self.assertEqual(lp.ui_blocked(state({"smoke": True}), 0), "on smoke")
        self.assertIsNone(lp.ui_blocked(state({"smoke": True}, traits={"ignore_smoke": True}), 0))
        self.assertIsNone(lp.ui_blocked(state({"smoke": True}, pilot_id="Pilot_Soldier"), 0))
        self.assertIsNone(lp.ui_blocked(state({"smoke": True}, pilot={"id": "Pilot_Soldier"}), 0))
        # Her immunity is to smoke and webs only: water still stops her weapons.
        self.assertEqual(lp.ui_blocked(state({"terrain": "water", "terrain_id": 3}, pilot_id="Pilot_Soldier"), 0),
                         "submerged")
        self.assertEqual(lp.ui_blocked(state({"terrain": "water", "terrain_id": 3}), 0), "submerged")
        self.assertIsNone(lp.ui_blocked(state({"terrain": "water", "terrain_id": 3}, flying=True), 0))
        self.assertIsNone(lp.ui_blocked(state({"terrain": "lava", "terrain_id": 5}), 0))  # old name for ice
        self.assertIsNone(lp.ui_blocked(state({}), 7))

    def test_patch_state_restores_the_pilot_bonus(self):
        state = {"units": [
            {"uid": 0, "team": 1, "active": True, "moved": False, "pilot_id": "Pilot_Leader"},
            {"uid": 1, "team": 1, "active": True, "moved": False, "pilot_id": "Pilot_Miner"},
            {"uid": 2, "team": 1, "active": True, "moved": True, "base_move": 4, "move": 4,
             "pilot_id": "Pilot_Aquatic"},
            {"uid": 3, "team": 1, "active": True, "moved": False, "pilot_id": "Pilot_Rust"},
            {"uid": 4, "team": 1, "active": True, "moved": False, "pilot_id": "Pilot_Leader"},
        ]}
        history = {0: "A", 1: "A", 2: "MA", 3: "A"}
        fixed, patches = lp.patch_state(state, {}, {2}, history=history)
        u = {x["uid"]: x for x in fixed["units"]}
        self.assertEqual(u[0]["bonus_shift"], 1)
        self.assertTrue(u[0]["active"])
        self.assertTrue(u[1]["moved"])  # Double_Shot: one more action, no move
        self.assertEqual(u[2]["bonus_shift"], 4)
        self.assertFalse(u[3]["active"])  # no pilot bonus: done
        self.assertNotIn("bonus_shift", u[4])  # has not acted
        self.assertEqual(patches["0.bonus_shift"], 1)
        self.assertEqual(patches["3.active"], False)
        self.assertNotIn("bonus_shift", state["units"][0])


class PatchStateWebTest(unittest.TestCase):
    def test_a_grappled_pawn_the_bridge_reports_unwebbed_is_webbed(self):
        state = {"units": [{"uid": 1, "type": "IgniteMech", "team": 1, "web": False, "grappled": True},
                           {"uid": 2, "type": "TeleMech", "team": 1, "web": False, "grappled": False}]}
        fixed, patches = lp.patch_state(state, {}, set())
        self.assertTrue(fixed["units"][0]["web"])
        self.assertFalse(fixed["units"][1]["web"])
        self.assertEqual(patches.get("1.web"), True)


class TeleporterPairsTest(unittest.TestCase):
    def test_pairs_come_from_the_latest_capture_in_the_bridge_log(self):
        import tempfile
        log = (
            "x | TELEPORT PAD pair captured: (1,1) <-> (6,6)\n"
            "x | TELEPORT PAD: StartMission complete, 1 pair(s) captured\n"
            "x | something else\n"
            "x | TELEPORT PAD pair captured: (5,3) <-> (2,2)\n"
            "x | TELEPORT PAD pair captured: (2,5) <-> (5,5)\n"
            "x | TELEPORT PAD: StartMission complete, 2 pair(s) captured\n"
            "x | TELEPORT PAD: clearing stale pairs for Mission_Civilians\n")
        with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as f:
            f.write(log)
        self.assertEqual(lp.logged_teleporter_pairs(Path(f.name)), [[5, 3, 2, 2], [2, 5, 5, 5]])
        self.assertEqual(lp.logged_teleporter_pairs(Path(f.name + ".missing")), [])


class DiffBoardsTest(unittest.TestCase):
    """The enemy-phase comparison's handling of AI planning moves."""

    @staticmethod
    def board(units, buildings=(), grid=5):
        return {"units": list(units), "buildings": list(buildings), "grid_power": grid}

    @staticmethod
    def vek(x, y, acid=False):
        return {"uid": 97, "type": "Firefly1", "x": x, "y": y, "hp": 3, "acid": acid}

    def test_acid_pool_picked_up_by_an_ai_move_is_a_note(self):
        want = self.board([self.vek(7, 1)])
        got = self.board([self.vek(7, 3, acid=True)])
        diffs, notes = lp.diff_boards(want, got, {97}, enemy_phase=True, acid_pools=frozenset({(7, 3)}))
        self.assertEqual(diffs, [])
        self.assertTrue(any("acid pool" in n for n in notes))

    def test_acid_without_a_pool_is_still_a_difference(self):
        want = self.board([self.vek(7, 1)])
        got = self.board([self.vek(7, 3, acid=True)])
        diffs, _ = lp.diff_boards(want, got, {97}, enemy_phase=True, acid_pools=frozenset())
        self.assertEqual(len(diffs), 1)

    def test_a_unit_put_out_by_planning_smoke_is_a_note(self):
        # Live 20261014_045942 m78 (Mission_Power) turn 2 -> 3: the engine's
        # prediction (it stops before the AI plans) has the TeleMech burning;
        # a Mosquito2 then queued its whip on it, smoking G4 at once, and the
        # game shows it put out on that smoke.
        mech = {"uid": 2, "type": "TeleMech", "x": 4, "y": 1, "hp": 1, "mech": True, "acid": True}
        want = self.board([dict(mech, fire=True)])
        got = self.board([dict(mech, fire=False)])
        diffs, notes = lp.diff_boards(want, got, {2}, enemy_phase=True, smoke_new=frozenset({(4, 1)}))
        self.assertEqual(diffs, [])
        self.assertTrue(any("put out by smoke" in n for n in notes))
        # Without new smoke there it is still a difference, and so is a unit
        # the game shows burning that the engine does not.
        diffs, _ = lp.diff_boards(want, got, {2}, enemy_phase=True, smoke_new=frozenset({(4, 2)}))
        self.assertEqual(diffs, ["TeleMech#2 fire: engine True, game False"])
        diffs, _ = lp.diff_boards(got, want, {2}, enemy_phase=True, smoke_new=frozenset({(4, 1)}))
        self.assertEqual(len(diffs), 1)

    def test_a_different_random_thaw_is_a_note(self):
        # Live 2026-10-10 (Mission_Reactivation): the engine thawed Snowart2,
        # the game thawed Burrower1 instead.
        a = {"uid": 1, "type": "Snowart2", "x": 5, "y": 1, "hp": 3, "team": 6}
        b = {"uid": 2, "type": "Burrower1", "x": 3, "y": 3, "hp": 3, "team": 6}
        want = self.board([dict(a, frozen=False), dict(b, frozen=True)])
        got = self.board([dict(a, frozen=True), dict(b, frozen=False)])
        diffs, notes = lp.diff_boards(want, got, {1, 2}, enemy_phase=True, random_thaw=True)
        self.assertEqual(diffs, [])
        self.assertTrue(any("random thaw" in n for n in notes))
        diffs, _ = lp.diff_boards(want, got, {1, 2}, enemy_phase=True, random_thaw=False)
        self.assertEqual(len(diffs), 2)
        # An unbalanced difference (one more thawed than predicted) is real.
        got2 = self.board([dict(a, frozen=False), dict(b, frozen=False)])
        diffs, _ = lp.diff_boards(want, got2, {1, 2}, enemy_phase=True, random_thaw=True)
        self.assertEqual(len(diffs), 1)

    @staticmethod
    def burrower(x, y, hp=1):
        return {"uid": 1683, "type": "Burrower1", "x": x, "y": y, "hp": hp, "team": 6}

    def test_underground_burrower_matches_by_uid(self):
        # Underground on both sides: the engine lists it under "underground",
        # the bridge not at all.
        want = dict(self.board([]), underground=[dict(self.burrower(1, 4), underground=True)])
        self.assertEqual(lp.diff_boards(want, self.board([]), {1683}), ([], []))
        # After the enemy phase the AI's move brought it up elsewhere: a note
        # (not a spawn), with the HP still compared.
        got = self.board([self.burrower(1, 5)])
        diffs, notes = lp.diff_boards(want, got, {1683}, enemy_phase=True)
        self.assertEqual(diffs, [])
        self.assertEqual(len(notes), 1)
        self.assertIn("Burrower1#1683 resurfaced at C7 hp 1", notes[0])
        diffs, _ = lp.diff_boards(want, self.board([self.burrower(1, 5, hp=3)]), {1683}, enemy_phase=True)
        self.assertEqual(len(diffs), 1)
        # During the player's turn the game showing it is a difference.
        diffs, _ = lp.diff_boards(want, got, {1683})
        self.assertEqual(diffs, ["Burrower1#1683: engine underground (dove at D7), game at C7 hp 1"])

    def test_track_underground(self):
        entry = dict(self.burrower(1, 4), underground=True)
        pred = dict(self.board([]), underground=[entry])
        under = {}
        lp.track_underground(under, pred, self.board([]), {1683})
        self.assertEqual(under, {1683: entry})
        lp.track_underground(under, self.board([]), self.board([self.burrower(1, 5)]), {1683})
        self.assertEqual(under, {})  # resurfaced
        lp.track_underground(under, pred, self.board([]), set())  # not a turn-start unit
        self.assertEqual(under, {})
        state, patches = lp.patch_state({"units": []}, {}, set(), underground={1683: entry})
        self.assertEqual(state["units"], [{"uid": 1683, "type": "Burrower1", "x": 1, "y": 4, "hp": 1, "team": 6,
                                           "underground": True}])
        self.assertEqual(patches, {"1683.underground": [1, 4]})
        # Listed by the bridge again: not added twice.
        state, patches = lp.patch_state({"units": [self.burrower(1, 5)]}, {}, set(), underground={1683: entry})
        self.assertEqual(len(state["units"]), 1)
        self.assertEqual(patches, {})

    def test_vek_frozen_on_a_tripped_freeze_mine_is_a_note(self):
        want = self.board([dict(self.vek(5, 2), frozen=False)])
        got = self.board([dict(self.vek(5, 2), frozen=True)])
        diffs, notes = lp.diff_boards(want, got, {97}, enemy_phase=True, freeze_mines_gone=frozenset({(4, 2)}))
        self.assertEqual(diffs, [])
        self.assertTrue(any("freeze mine" in n for n in notes))
        diffs, _ = lp.diff_boards(want, got, {97}, enemy_phase=True)
        self.assertEqual(len(diffs), 1)

    def test_vek_frozen_on_a_freeze_mine_a_snowmine_laid_during_planning(self):
        # Live 20261014_222734 m12 (Mission_BotDefense) turn 3 -> 4: the
        # player-team Snowmine1 left C3 for B2 during AI planning, laying a
        # freeze mine on C3; the Firefly2 then moved B3 -> C3 and froze. The
        # mine shows on neither captured board.
        bot = {"uid": 3588, "type": "Snowmine1", "x": 5, "y": 5, "hp": 1, "team": 1}
        fly = {"uid": 3591, "type": "Firefly2", "x": 6, "y": 5, "hp": 5, "team": 6, "frozen": False}
        want = self.board([bot, fly])
        got = self.board([dict(bot, x=6, y=6), dict(fly, x=5, y=5, frozen=True)])
        laid = lp.freeze_mines_laid(want, got)
        self.assertEqual(laid, {(5, 5)})
        diffs, notes = lp.diff_boards(want, got, {3588, 3591}, enemy_phase=True, freeze_mines_gone=frozenset(laid))
        self.assertEqual(diffs, [])
        self.assertTrue(any("freeze mine" in n for n in notes))
        # A Snowmine that stayed put laid nothing.
        self.assertEqual(lp.freeze_mines_laid(want, self.board([bot, fly])), set())

    def test_vek_healed_on_a_tripped_repair_mine_is_a_note(self):
        # Live 2026-10-11 (Mission_Repair): a Burnbug walked onto the C3
        # repair mine while the Vek planned their next turn: healed to full,
        # acid gone.
        want = self.board([dict(self.vek(3, 6), hp=1, acid=True)])
        got = self.board([dict(self.vek(3, 3), hp=3, acid=False)])
        diffs, notes = lp.diff_boards(want, got, {97}, enemy_phase=True, repair_mines_gone=frozenset({(3, 3)}))
        self.assertEqual(diffs, [])
        self.assertTrue(any("repair mine" in n for n in notes))
        # Elsewhere, or without a tripped repair mine, it is still a difference.
        diffs, _ = lp.diff_boards(want, got, {97}, enemy_phase=True, repair_mines_gone=frozenset({(1, 1)}))
        self.assertEqual(len(diffs), 2)
        diffs, _ = lp.diff_boards(want, got, {97}, enemy_phase=True)
        self.assertEqual(len(diffs), 2)

    def test_an_egg_that_hatched_during_ai_planning_is_a_note(self):
        egg = {"uid": 97, "type": "SpiderlingEgg1", "x": 5, "y": 2, "hp": 1}
        diffs, notes = lp.diff_boards(self.board([egg]), self.board([]), {97}, enemy_phase=True)
        self.assertEqual(diffs, [])
        self.assertTrue(any("hatched" in n for n in notes))
        diffs, _ = lp.diff_boards(self.board([egg]), self.board([]), {97}, enemy_phase=False)
        self.assertEqual(len(diffs), 1)

    def test_vek_dead_on_a_tripped_mine_is_a_note(self):
        # Live 2026-10-10 (Mission_Mines): Firefly1 walked onto the F4 mine
        # while the Vek planned their next turn and died there.
        want = self.board([self.vek(5, 2)])
        got = self.board([])
        diffs, notes = lp.diff_boards(want, got, {97}, enemy_phase=True, mines_gone=frozenset({(4, 2)}))
        self.assertEqual(diffs, [])
        self.assertTrue(any("mine" in n for n in notes))

    def test_vek_death_without_a_tripped_mine_is_still_a_difference(self):
        want = self.board([self.vek(5, 2)])
        got = self.board([])
        diffs, _ = lp.diff_boards(want, got, {97}, enemy_phase=True, mines_gone=frozenset())
        self.assertEqual(len(diffs), 1)
        diffs, _ = lp.diff_boards(want, got, {97}, enemy_phase=False, mines_gone=frozenset({(4, 2)}))
        self.assertEqual(len(diffs), 1)

    # Grid Defense: the engine predicts every roll as not resisted (worst case).
    # Live 2026-10-09 m22 turn 3: G6 predicted 1 HP, game 2; grid 4, game 5.

    @staticmethod
    def building(x, y, hp):
        return {"x": x, "y": y, "hp": hp}

    def resist_case(self, game_hp=2, game_grid=5, tiles=None, before_hp=2):
        want = self.board([], [self.building(2, 1, 1), self.building(5, 5, 2)], grid=4)
        got = self.board([], [self.building(2, 1, game_hp), self.building(5, 5, 2)], grid=game_grid)
        before = self.board([], [self.building(2, 1, before_hp), self.building(5, 5, 2)], grid=5)
        return lp.diff_boards(want, got, set(), enemy_phase=True, resist={"before": before, "tiles": tiles})

    def test_grid_defense_resist_is_a_note(self):
        diffs, notes = self.resist_case()
        self.assertEqual(diffs, [])
        self.assertTrue(any("Grid Defense resisted at G6" in n for n in notes), notes)
        self.assertTrue(any("grid: engine 4, game 5" in n for n in notes), notes)

    def test_grid_defense_resist_on_a_listed_roll_tile(self):
        diffs, _ = self.resist_case(tiles={(2, 1)})
        self.assertEqual(diffs, [])
        diffs, _ = self.resist_case(tiles={(5, 5)})  # no roll on G6: not a resist
        self.assertEqual(len(diffs), 2)

    def test_resist_with_a_destroyed_building(self):
        want = self.board([], [], grid=4)
        got = self.board([], [self.building(2, 1, 1)], grid=5)
        before = self.board([], [self.building(2, 1, 1)], grid=5)
        diffs, notes = lp.diff_boards(want, got, set(), enemy_phase=True, resist={"before": before, "tiles": None})
        self.assertEqual(diffs, [])
        self.assertTrue(any("engine gone, game 1" in n for n in notes), notes)

    def test_building_differences_without_chance_nodes_stop(self):
        want = self.board([], [self.building(2, 1, 1)], grid=4)
        got = self.board([], [self.building(2, 1, 2)], grid=5)
        diffs, _ = lp.diff_boards(want, got, set(), enemy_phase=True)
        self.assertEqual(len(diffs), 2)

    def test_resist_that_does_not_explain_the_grid_stops(self):
        diffs, _ = self.resist_case(game_grid=6)  # one HP kept, two grid
        self.assertEqual(len(diffs), 2)
        diffs, _ = self.resist_case(game_grid=4)  # HP kept but grid lost anyway
        self.assertEqual(len(diffs), 1)

    def test_building_above_its_hp_before_stops(self):
        diffs, _ = self.resist_case(game_hp=2, before_hp=1)  # more HP than before the phase: not a resist
        self.assertEqual(len(diffs), 2)

    def test_building_lower_than_predicted_stops(self):
        want = self.board([], [self.building(2, 1, 2)], grid=5)
        got = self.board([], [self.building(2, 1, 1)], grid=4)
        before = self.board([], [self.building(2, 1, 2)], grid=5)
        diffs, _ = lp.diff_boards(want, got, set(), enemy_phase=True, resist={"before": before, "tiles": None})
        self.assertEqual(len(diffs), 2)

    def test_resist_info_from_a_prediction(self):
        self.assertIsNone(lp.resist_info({"enemy_phase": {"chance_nodes": 0}}))
        r = lp.resist_info({"enemy_phase": {"chance_nodes": 1}, "after_player": {"buildings": []}})
        self.assertIsNone(r["tiles"])
        r = lp.resist_info({"enemy_phase": {"chance_nodes": 2, "grid_defense": [{"point": [2, 1], "amount": 1}]},
                            "after_player": {"buildings": []}})
        self.assertEqual(r["tiles"], {(2, 1)})

    # A Soldier Psion emerging from a hidden spawn: every Vek +1 HP at once.
    # Live 2026-10-10 m24 turn 1 (Jelly_Health1 from a spawn point).

    @staticmethod
    def unit(uid, type_, hp, x, y, team=6, mech=False, **kw):
        return dict({"uid": uid, "type": type_, "hp": hp, "x": x, "y": y, "team": team, "mech": mech}, **kw)

    def psion_case(self, psion=True, bonus=1, leader=1, psion_type="Jelly_Health1"):
        known = {0, 566, 567}
        want = self.board([self.unit(0, "JetMech", 4, 2, 3, team=1, mech=True),
                           self.unit(566, "Digger2", 4, 4, 5), self.unit(567, "Leaper2", 3, 3, 1)])
        got_units = [self.unit(0, "JetMech", 4, 2, 3, team=1, mech=True),
                     self.unit(566, "Digger2", 4 + bonus, 4, 5), self.unit(567, "Leaper2", 3 + bonus, 3, 1)]
        if psion:
            extra = {"leader": leader} if leader else {}
            got_units.append(self.unit(577, psion_type, 2, 6, 1, **extra))
        return lp.diff_boards(want, self.board(got_units), known, enemy_phase=True)

    def test_soldier_psion_emerging_is_a_note(self):
        diffs, notes = self.psion_case()
        self.assertEqual(diffs, [])
        self.assertTrue(any("Jelly_Health1#577 emerged" in n and "Digger2#566" in n for n in notes), notes)

    def test_soldier_psion_buffs_a_burrower_underground_in_the_engine(self):
        # Live 2026-10-12 run 8 Mission_AcidStorm: two Burrowers dove, then
        # came up with +1 HP as a Jelly_Health1 emerged.
        known = {0, 5580}
        want = self.board([self.unit(0, "JetMech", 4, 2, 3, team=1, mech=True)])
        want["underground"] = [self.unit(5580, "Burrower2", 3, 6, 2)]
        got = self.board([self.unit(0, "JetMech", 4, 2, 3, team=1, mech=True), self.unit(5580, "Burrower2", 4, 5, 1),
                          self.unit(5582, "Jelly_Health1", 2, 6, 5, leader=1)])
        diffs, notes = lp.diff_boards(want, got, known, enemy_phase=True)
        self.assertEqual(diffs, [])
        self.assertTrue(any("Burrower2#5580" in n and "emerged" in n for n in notes), notes)
        got["units"].pop()  # no psion: +1 HP stops
        diffs, _ = lp.diff_boards(want, got, known, enemy_phase=True)
        self.assertEqual(len(diffs), 1)

    def test_psion_without_the_leader_field_by_type(self):
        diffs, _ = self.psion_case(leader=None)
        self.assertEqual(diffs, [])
        diffs, _ = self.psion_case(leader=None, psion_type="Jelly_Boss")
        self.assertEqual(diffs, [])

    def test_vek_hp_without_a_new_health_psion_stops(self):
        diffs, _ = self.psion_case(psion=False)
        self.assertEqual(len(diffs), 2)
        diffs, _ = self.psion_case(leader=4, psion_type="Jelly_Regen1")  # regen psion: no HP on arrival
        self.assertEqual(len(diffs), 2)

    def test_psion_bonus_other_than_one_stops(self):
        diffs, _ = self.psion_case(bonus=2)
        self.assertEqual(len(diffs), 2)

    def test_psion_buffs_mechs_with_the_psionic_receiver(self):
        # Live 2026-10-13 run 11 Mission_Force: a Jelly_Health1 emerged and
        # every mech gained 1 HP through Passive_Psions.
        mech = self.unit(0, "ChargeMech", 1, 2, 3, team=1, mech=True)
        mech["weapons_exact"] = ["Brute_Beetle", "Passive_Psions"]
        want = self.board([dict(mech)])
        buffed = dict(mech, hp=2)
        got = self.board([buffed, self.unit(577, "Jelly_Health1", 2, 6, 1, leader=1)])
        diffs, notes = lp.diff_boards(want, got, {0}, enemy_phase=True)
        self.assertEqual(diffs, [])
        self.assertTrue(any("ChargeMech#0" in n for n in notes), notes)

    def test_psion_does_not_explain_mech_hp(self):
        want = self.board([self.unit(0, "JetMech", 3, 2, 3, team=1, mech=True)])
        got = self.board([self.unit(0, "JetMech", 4, 2, 3, team=1, mech=True),
                          self.unit(577, "Jelly_Health1", 2, 6, 1, leader=1)])
        diffs, _ = lp.diff_boards(want, got, {0}, enemy_phase=True)
        self.assertEqual(len(diffs), 1)

    def test_psion_already_predicted_explains_nothing(self):
        want = self.board([self.unit(566, "Digger2", 4, 4, 5), self.unit(577, "Jelly_Health1", 2, 6, 1, leader=1)])
        got = self.board([self.unit(566, "Digger2", 5, 4, 5), self.unit(577, "Jelly_Health1", 2, 6, 1, leader=1)])
        diffs, _ = lp.diff_boards(want, got, {566}, enemy_phase=True)
        self.assertEqual(len(diffs), 1)

    def test_unit_uid_map_matches_created_units_by_type_and_tile(self):
        want = self.board([self.unit(1, "BomblingMech", 0, 5, 3, team=1, mech=True),
                           self.unit(2317, "DeployUnit_Bomby", 5, 5, 1, team=1)])
        got = self.board([self.unit(1, "BomblingMech", 0, 5, 3, team=1, mech=True),
                          self.unit(2320, "DeployUnit_Bomby", 5, 5, 1, team=1)])
        self.assertEqual(lp.unit_uid_map(want, got, {1}), {2317: 2320})
        # Same uid on both sides, or no unique match: nothing to map.
        self.assertEqual(lp.unit_uid_map(got, got, {1}), {})
        got["units"][1]["x"] = 4
        self.assertEqual(lp.unit_uid_map(want, got, {1}), {})


class ReactivationAfterTest(unittest.TestCase):
    """Mission_Reactivation's visible list across an enemy phase
    (reactivation_after): the engine's thaw events name the draw's
    candidates, the game's board shows which ones it thawed."""

    @staticmethod
    def pred(cands, thawed=None, events=()):
        detail = "candidates " + ",".join(str(u) for u in cands)
        ev = [{"type": "thawed", "uid": u, "amount": len(cands), "detail": detail}
              for u in (thawed if thawed is not None else cands[:2])]
        return {"enemy_phase": {"mission_ended": False, "events": ev + list(events)}}

    @staticmethod
    def live(frozen, unfrozen):
        return {"units": [{"uid": u, "frozen": True} for u in frozen] + [{"uid": u, "frozen": False} for u in unfrozen]}

    def test_more_than_two_candidates_the_game_picks_the_pair(self):
        listed = [10, 11, 12, 13]
        got, notes = lp.reactivation_after(listed, self.pred([10, 11, 12, 13]), self.live([11, 13], [10, 12]))
        self.assertEqual((got, notes), ([11, 13], []))

    def test_a_thawed_unit_stays_off_the_list_when_frozen_again(self):
        got, _ = lp.reactivation_after([10, 11, 12], self.pred([10, 11, 12]), self.live([12], [10, 11]))
        self.assertEqual(got, [12])
        # Next turn the player re-froze 10: the engine gets the list as is.
        state = {"mission_id": "Mission_Reactivation", "units": [{"uid": 10, "team": 6, "hp": 3, "frozen": True},
                                                                 {"uid": 12, "team": 6, "hp": 3, "frozen": True}]}
        fixed, patches = lp.patch_state(state, {}, set(), reactivation=got)
        self.assertEqual(fixed["reactivation_visible_list"], [12])
        self.assertEqual(patches["reactivation_visible_list"], [12])
        got, _ = lp.reactivation_after(got, self.pred([12]), self.live([10], [12]))
        self.assertEqual(got, [])

    def test_a_unit_unfrozen_by_damage_stays_listed(self):
        # 12 was unfrozen by an attack before the draw: not a candidate, and
        # with exactly two candidates the third entry may or may not have been
        # drawn (hidden): it stays listed.
        got, _ = lp.reactivation_after([10, 11, 12], self.pred([10, 11]), self.live([], [10, 11, 12]))
        self.assertEqual(got, [12])

    def test_fewer_than_two_candidates_empty_the_list(self):
        self.assertEqual(lp.reactivation_after([10, 11], self.pred([10]), self.live([], [10, 11]))[0], [])
        none = {"enemy_phase": {"mission_ended": False, "events": []}}
        self.assertEqual(lp.reactivation_after([10, 11], none, self.live([], [10, 11]))[0], [])

    def test_mission_end_keeps_the_list(self):
        ended = {"enemy_phase": {"mission_ended": True, "events": []}}
        self.assertEqual(lp.reactivation_after([10, 11], ended, self.live([10, 11], []))[0], [10, 11])

    def test_a_spawn_bump_after_the_draw_is_ambiguous(self):
        bump = [{"type": "spawn blocked", "uid": 12}]
        got, notes = lp.reactivation_after([10, 11, 12, 13], self.pred([10, 11, 12, 13], events=bump),
                                           self.live([13], [10, 11, 12]))
        self.assertEqual((got, notes), ([12, 13], []))
        # Still more than two: nothing leaves the list (worst case), with a note.
        got, notes = lp.reactivation_after([10, 11, 12, 13], self.pred([10, 11, 12, 13]),
                                           self.live([13], [10, 11, 12]))
        self.assertEqual(got, [10, 11, 12, 13])
        self.assertTrue(notes)
