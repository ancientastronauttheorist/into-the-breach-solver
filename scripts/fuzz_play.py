#!/usr/bin/env python3
"""Differential fuzzing: random legal turns in the real game, every step
checked against the engine.

Live play only ever executes the moves the solver picks, so an interaction
the engine mis-models in a way that makes it look bad is never played and
never caught. This driver plays random but UI-legal sub-actions instead,
biased toward the odd ones (friendly fire, hitting buildings, pushes into
units, water, chasms and mountains, deployables, Lua the engine does not
model), from turn starts recorded by scripts/live_play.py, and compares the
game with the engine after every step and after the enemy phase.

    python3 scripts/fuzz_play.py --dry-run --samples 20          # engine only, never touches the game
    python3 scripts/fuzz_play.py --dry-run --run 20261016_130634 --mission 2 --turn 2
    python3 scripts/fuzz_play.py --samples 10                     # live (see engine/README.md "Fuzzing")

Moves come from the engine's own enumeration (itb_live "actions": the
solver's Action model, each candidate run once on a copy of the board for
its effect), filtered by what the game's UI allows (live_play's per-turn
limits: move then act; Shifty / Post_Move a move after acting;
Double_Shot two actions without moving; unpowered pilot skills off), and
executed exactly as live play executes them (EXEC_MODE native:
MOVE_NATIVE, ATTACK / TWO_CLICK_ATTACK with the weapon's slot, REPAIR; no
SetSpace, no emulation; a weapon or Repair from smoke or water is never
sent). Each step's settled board is compared with live_play.diff_boards and
the same carried state (the engine's queued shots, Burrowers underground,
structure shields, Mission_Reactivation's visible list); End Turn and the
enemy phase are live_play.end_turn itself.

A difference is a finding, appended to recordings/fuzz/<session>/
findings.jsonl with the source turn, the action list up to it, the
differences and the files to reproduce it offline (the sample's start state
and plan.json: `itb_live predict <start> --plan @plan.json`). A step that
drew a chance node in the prediction (a Grid Defense roll on a building the
fuzzer hit) and then differs is a "chance" finding: the driver re-predicts
the rest from the live board and goes on. Any other step difference ends
the sample (no End Turn); the next sample restores another turn start.

Live mode restores each turn start as `live_play.py restore` does, with its
functions (the game reads its saves only at launch: Continue after the pause
menu's Main Menu resumes the run in memory): it quits the game, puts that
turn's checkpoint (m<MM>_turn_<TT>_saveData.lua / _undoSave.lua) into the
profile, relaunches the game (live_play.launch_game: `open steam://run/...`
through the offline Steam client, nothing else of Steam), clicks Continue on
the title screen until a fresh player turn shows, then loads it once more
(the pause menu by its gear at --gear-xy, Esc as a fallback; once the
heartbeat stalls, its MAIN MENU at --main-menu-xy; Continue) against the
Grid Defense restart quirk, and checks the board against the recorded one
and the Grid Defense base. Title-screen clicks go out only right after a
launch or a Main Menu, and never again once the bridge ticks. Turn starts
are drawn without replacement (a new pass, said so, once all have been
drawn), and only those the recording shows playable are restored (a
player turn with an active mech, not a mission's last turn unless
--last-turns, a sequence the engine predicts keeps --min-grid and a
mech); the check after the restore stays. The profile's own save files are
copied aside the first time (after the game has exited) and put back at the
end the same way: quit, swap, relaunch, then with a run in them one
Continue and the second load: from a mission as above, from the island map (the bridge
stays silent END_WAIT s; Esc does nothing there) blind, the map's gear at
--map-gear-xy, MAIN MENU, Continue, MAP_CLICK_WAIT s after each click; with
none (no saveData.lua: no run in progress) the game stays on the title
screen. It refuses to start while the
game is not running or another driver acts (the driver lock live_play holds,
a running live_play.py mission/turn/deploy/end-turn/restore, drive2.sh /
drive_final.sh, game_loop.py auto_turn, a bridge ack younger than
--idle-seconds) and holds the driver lock for the whole session.

--dry-run never talks to the bridge: it generates the sequences on
recorded turn starts, predicts them, and checks the engine against itself
(every enumerated sub-action must replay through `predict` to the same
board; refusals, Lua errors, unmodelled Lua writes and frame-timing flags
are counted), writing samples.jsonl and the same summary.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import random
import re
import subprocess
import sys
import time
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import live_play as lp  # noqa: E402
import live_validate as lv  # noqa: E402

FUZZ_ROOT = REPO / "recordings" / "fuzz"

# Other drivers of the game (command lines; a --dry-run one is harmless).
OTHER_DRIVERS = (r"live_play\.py\s+(mission|turn|deploy|end-turn|restore)\b", r"drive2\.sh", r"drive_final\.sh",
                 r"game_loop\.py\s+auto_turn", r"fuzz_play\.py")

# Sampling: a unit's weapons first, then moves, Repair seldom.
GROUP_WEIGHT = {"weapon": 3.0, "move": 1.5, "repair": 0.4}
STOP_PROB = 0.06
MAX_STEPS = 12
# Putting the profile's own run back at the end: Continue is clicked once,
# END_CONTINUE_DELAY s after the launch (never again: that run may resume on
# the island map, where the bridge is silent as on the title screen), then
# the bridge is given END_WAIT s to show a mission, which gets END_SETTLE s
# before the second load's pause menu (a silent bridge: the island map,
# map_reload).
END_CONTINUE_DELAY = 25.0
END_WAIT = 60.0
END_SETTLE = 3.0
# The second load from the island map, where the bridge is silent: a fixed
# wait after each click (the map's gear, MAIN MENU, Continue).
MAP_CLICK_WAIT = 5.0


def say(msg: str = "") -> None:
    print(msg, flush=True)


class LeftMission(lp.Anomaly):
    """The game left the mission (a mission or the run ended, the bridge
    went silent after End Turn) or a restore failed: stop the session (the
    profile's own save files are put back as at any end)."""


# ------------------------------------------------------------------ turn starts

@dataclass
class TurnStart:
    run: str
    mission: int
    turn: int
    path: Path  # m<MM>_turn_<TT>_solve_input.json
    save: Path | None = None  # its saveData.lua checkpoint
    undo: Path | None = None

    @property
    def key(self) -> str:
        return f"{self.run}/m{self.mission:02d}/t{self.turn:02d}"

    def raw_path(self) -> Path:
        """The bridge's own dump of that turn start (the solve input is the
        patched one when the driver patched it)."""
        raw = self.path.with_name(f"m{self.mission:02d}_turn_{self.turn:02d}_start_raw_01.json")
        return raw if raw.exists() else self.path


START_RE = re.compile(r"m(\d+)_turn_(\d+)_solve_input\.json$")


def find_turn_starts(root: Path, runs: list | None = None, need_save: bool = False) -> list:
    """Every recorded turn start under root/<run>/ (live_play's runs), with
    its save checkpoint if there is one; need_save: only those with one."""
    out = []
    if not root.is_dir():
        return out
    for run_dir in sorted(p for p in root.iterdir() if p.is_dir()):
        if runs and run_dir.name not in runs:
            continue
        for p in sorted(run_dir.glob("m*_turn_*_solve_input.json")):
            m = START_RE.search(p.name)
            if not m:
                continue
            mission, turn = int(m.group(1)), int(m.group(2))
            pre = f"m{mission:02d}_turn_{turn:02d}"
            save, undo = run_dir / f"{pre}_saveData.lua", run_dir / f"{pre}_undoSave.lua"
            ts = TurnStart(run_dir.name, mission, turn, p, save if save.exists() else None,
                           undo if undo.exists() else None)
            if need_save and ts.save is None:
                continue
            out.append(ts)
    return out


def read_state(path: Path) -> tuple[dict, dict]:
    """(bridge state, recording data) of a live_play state file."""
    d = json.loads(path.read_text())
    data = d.get("data", {}) if isinstance(d, dict) else {}
    return (data.get("bridge_state") if "bridge_state" in data else d), data


def unplayable(state: dict, last_turns: bool = True) -> tuple | None:
    """Why a state is no turn to fuzz, as (key, text), or None: it must be a
    player turn with an active mech; last_turns False: not the mission's
    last turn (its enemy phase ends the mission, and the game goes on to the
    reward screens or, after the final Cave, ends the run)."""
    turn, total = int(state.get("turn") or 0), state.get("total_turns")
    if state.get("phase") != "combat_player" or not state.get("in_active_mission", True) or turn < 1:
        return "not_player_turn", f"not a player turn (phase {state.get('phase')}, turn {turn})"
    if not last_turns and isinstance(total, int) and total > 0 and turn >= total:
        return "last_turn", f"the mission's last turn ({turn} of {total}; --last-turns to play it)"
    if not lp.active_mechs(state):
        return "no_active_mech", "no active mech"
    return None


def playable(state: dict, last_turns: bool = True) -> bool:
    return unplayable(state, last_turns) is None


@dataclass
class Carry:
    """What live play carried into a turn start besides the bridge's dump."""
    underground: dict = field(default_factory=dict)  # {uid: engine underground entry}
    shields: list = field(default_factory=list)  # [[x, y]] shielded structures
    reactivation: list | None = None  # Mission_Reactivation's visible list


def recorded_carry(state: dict, data: dict) -> Carry:
    patches = data.get("patches") or {}
    under = {}
    for u in lp.units(state):
        if u.get("underground"):
            under[int(u["uid"])] = {k: u.get(k) for k in ("uid", "type", "x", "y", "hp", "max_hp", "team")} | \
                {"underground": True}
    react = state.get("reactivation_visible_list")
    return Carry(under, [list(p) for p in patches.get("tile_shields") or []],
                 list(react) if isinstance(react, list) else None)


# ------------------------------------------------------------------ choosing sub-actions

def plan_action(a: dict) -> dict:
    """A candidate of itb_live "actions" as one plan action (one sub-action)."""
    if a["sub"] == "move":
        return {"uid": a["uid"], "move": a["target"], "kind": "none"}
    return {"uid": a["uid"], "move": None, "kind": a["sub"], "weapon": a["weapon"], "target": a["target"],
            "target2": a.get("target2")}


def effect_tags(a: dict) -> list:
    """The noteworthy parts of a candidate's effect, as names."""
    e = a.get("effect") or {}
    hp, kills = e.get("hp") or {}, e.get("kills") or {}
    tags = []
    if hp.get("player") or e.get("self_hp"):
        tags.append("friendly_damage")
    if hp.get("enemy"):
        tags.append("enemy_damage")
    if hp.get("neutral"):
        tags.append("neutral_damage")
    for k, n in sorted(kills.items()):
        if n:
            tags.append(f"kill_{k}")
    for k in ("building_hp", "grid", "bumps", "fell", "drowned", "mountain_hp", "terrain", "statuses", "pushes",
              "moved_units", "fire_tiles", "smoke_tiles", "acid_tiles", "chance_nodes", "lua_errors",
              "unapplied_lua_writes", "mission_hooks"):
        if e.get(k):
            tags.append(k)
    if e.get("created"):
        tags.append("created")
    if e.get("timing_sensitive"):
        tags.append("timing_sensitive")
    return tags


def interest(a: dict) -> float:
    """How odd a candidate is: 1 for a plain one, more for friendly fire,
    building damage, bumps, falls, kills, created units, terrain and status
    changes, and most for what the engine itself is unsure of (Lua errors,
    unmodelled writes, frame-timing flags, mission hooks)."""
    e = a.get("effect") or {}
    hp, kills = e.get("hp") or {}, e.get("kills") or {}
    w = 1.0
    w += 3 * min(hp.get("player", 0), 3) + 2 * min(e.get("self_hp", 0), 3)
    w += 1.5 * min(e.get("building_hp", 0), 3) + min(e.get("grid", 0), 2)
    w += 2 * min(e.get("bumps", 0), 3) + 3 * min(e.get("fell", 0) + e.get("drowned", 0), 2)
    w += 1.5 * min(sum(kills.values()), 3) + 3 * min(len(e.get("created") or []), 2)
    w += 0.5 * min(e.get("statuses", 0), 4) + min(e.get("terrain", 0), 3) + min(e.get("mountain_hp", 0), 2)
    w += 0.3 * min(e.get("moved_units", 0), 4) + 0.3 * min(e.get("smoke_tiles", 0) + e.get("acid_tiles", 0)
                                                        + e.get("fire_tiles", 0), 4)
    w += 4 * bool(e.get("lua_errors")) + 4 * bool(e.get("unapplied_lua_writes"))
    w += 3 * bool(e.get("timing_sensitive")) + 2 * bool(e.get("mission_hooks")) + bool(e.get("chance_nodes"))
    kind = a.get("target_kind", "")
    if a["sub"] == "weapon" and kind in ("unit_player", "building", "unit_neutral"):
        w += 1
    if kind in ("water", "chasm", "lava", "mountain", "ice", "forest"):
        w += 0.5
    if a["sub"] != "move" and not effect_tags(a):
        w *= 0.3  # a shot that changes nothing visible
    return w


def novelty_key(a: dict) -> tuple:
    return (a["sub"], a["weapon"], a.get("target_kind"), tuple(effect_tags(a)))


class Chooser:
    """Weighted choice of the next sub-action. Two levels: a (unit, kind,
    weapon) group by GROUP_WEIGHT and its best candidate's interest, then a
    target within it by interest, each damped by how often the session has
    already played the same kind of action (novelty). --uniform: every
    candidate alike."""

    def __init__(self, rng: random.Random, uniform: bool = False):
        self.rng = rng
        self.uniform = uniform
        self.seen: Counter = Counter()

    def weight(self, a: dict) -> float:
        return interest(a) / math.sqrt(1 + self.seen[novelty_key(a)])

    def choose(self, cands: list) -> dict:
        if self.uniform:
            a = self.rng.choice(cands)
        else:
            groups: dict = {}
            for c in cands:
                groups.setdefault((c["uid"], c["sub"], c["weapon"]), []).append(c)
            keys = sorted(groups)
            gw = [GROUP_WEIGHT.get(k[1], 1.0) * (1 + math.log1p(max(self.weight(c) for c in groups[k])))
                  for k in keys]
            members = groups[self.rng.choices(keys, weights=gw)[0]]
            a = self.rng.choices(members, weights=[self.weight(c) for c in members])[0]
        self.seen[novelty_key(a)] += 1
        return a


def unit_skills(state: dict) -> dict:
    """uid -> the pilot skill that changes the UI's per-turn limits (None
    for the rest, and for an unpowered one)."""
    return {u["uid"]: lp.pilot_skill(u) for u in lp.units(state)}


def ui_allows(a: dict, seqs: dict, skills: dict) -> bool:
    seq = seqs.get(a["uid"], "") + ("M" if a["sub"] == "move" else "A")
    return lp.ui_sequence_ok(seq, skills.get(a["uid"]))


@dataclass
class Sequence:
    plan: list  # plan actions, one per sub-action
    picks: list  # the chosen candidates (with their effects)
    boards: list  # the engine's board after each prefix (actions reply), boards[k] after k sub-actions
    candidates: list  # how many UI-legal candidates each choice had
    rejected: Counter = field(default_factory=Counter)


def generate(engine, path: Path, chooser: Chooser, rng: random.Random, history: dict, skills: dict,
             max_steps: int = MAX_STEPS, stop_prob: float = STOP_PROB, grid_floor: int = 0) -> Sequence:
    """A random UI-legal sequence of sub-actions from the state at `path`:
    each step asks the engine for every sub-action it accepts after the
    prefix, keeps those the UI allows (`history`: what each unit already did
    this turn) and that leave at least `grid_floor` grid power, and picks
    one. Ends with no candidate left, after max_steps, or at random
    (stop_prob per step after the first)."""
    seq = Sequence([], [], [], [])
    seqs = dict(history)
    while True:
        reply = engine.actions(path, seq.plan, effects=len(seq.plan) < max_steps)
        if reply.get("refused", -1) >= 0:
            raise lp.Anomaly(f"the engine refused its own prefix at step {reply['refused']}: "
                             f"{reply.get('refused_reason')}")
        seq.boards.append(reply["board"])
        seq.rejected.update(reply.get("rejected") or {})
        if len(seq.plan) >= max_steps:
            break
        grid = reply["board"].get("grid_power")
        cands = [a for a in reply["actions"] if ui_allows(a, seqs, skills)
                 and not (isinstance(grid, int) and grid - (a.get("effect") or {}).get("grid", 0) < grid_floor)]
        if not cands or (seq.plan and rng.random() < stop_prob):
            break
        a = chooser.choose(cands)
        seq.picks.append(a)
        seq.candidates.append(len(cands))
        seq.plan.append(plan_action(a))
        seqs[a["uid"]] = seqs.get(a["uid"], "") + ("M" if a["sub"] == "move" else "A")
    return seq


def mech_alive(board: dict) -> bool:
    """A mech with HP left on an engine board (its units omit the dead), or
    a board without units."""
    if "units" not in board:
        return True
    return any(u.get("mech") and (u.get("hp") or 0) > 0 for u in board["units"])


def grid_safe(pred: dict, min_grid: int) -> bool:
    """The prediction keeps at least min_grid grid power and a living mech
    through every step and the enemy phase (its worst case: no Grid Defense
    resist). At 0 grid the run is lost: the game leaves the mission for the
    Timeline Lost screens, which no restore here gets out of. With no mech
    left the game gives no player turn with an active mech again, so End
    Turn never comes back to one (fuzz 20261016_230406 sample 10,
    Mission_ScarabBoss turn 3: Brute_Beetle_A's charge killed the Charge Mech
    and the Napalm Mech, Firefly2 the Hornet Mech; no next player turn
    within 180 s)."""
    boards = [s.get("board") or {} for s in pred.get("steps") or []] + [pred.get("after_enemy") or {}]
    return pred.get("refused", -1) < 0 and all(
        (not isinstance(b.get("grid_power"), int) or b["grid_power"] >= min_grid) and mech_alive(b)
        for b in boards)


def safe_sequence(engine, path: Path, chooser: Chooser, rng: random.Random, history: dict, skills: dict,
                  args) -> tuple:
    """generate() until the prediction keeps --min-grid and a mech (up to
    --tries sequences). Returns (sequence, prediction, tries), the sequence None
    when none did."""
    pred: dict = {}
    for n in range(1, args.tries + 1):
        seq = generate(engine, path, chooser, rng, history, skills, args.max_steps, args.stop_prob, args.min_grid)
        pred = engine.predict(path, seq.plan)
        if grid_safe(pred, args.min_grid):
            return seq, pred, n
    return None, pred, args.tries


def describe(a: dict) -> str:
    who = f"#{a['uid']}"
    if a["sub"] == "move":
        return f"{who} move {lp.visual(*a['target'])}"
    if a["sub"] == "repair":
        return f"{who} repair"
    text = f"{who} {a['weapon']} {lp.visual(*a['target'])}"
    if a.get("target2"):
        text += f">{lp.visual(*a['target2'])}"
    return text + (f" [{','.join(effect_tags(a))}]" if effect_tags(a) else "")


# ------------------------------------------------------------------ engine self-checks (dry run)

def engine_checks(seq: Sequence, pred: dict) -> list:
    """The prediction against the enumeration it came from: every picked
    sub-action replays through `predict` (no refusal, one step each), to the
    board the enumeration's prefix gave, a move to the tile it reported."""
    out = []
    if pred.get("refused", -1) >= 0:
        out.append(f"predict refused step {pred['refused']}: {pred.get('refused_reason')}")
    steps = pred.get("steps") or []
    if len(steps) != len(seq.plan):
        out.append(f"predict has {len(steps)} steps for {len(seq.plan)} sub-actions")
    for k, s in enumerate(steps[:len(seq.plan)]):
        if k + 1 < len(seq.boards) and s.get("board") != seq.boards[k + 1]:
            diffs, _ = lp.diff_boards(seq.boards[k + 1], s["board"], {u["uid"] for u in seq.boards[0]["units"]})
            out.append(f"step {k + 1} {describe(seq.picks[k])}: predict's board differs from the enumeration's"
                       + (": " + "; ".join(diffs) if diffs else " (fields the comparison does not show)"))
        a = seq.picks[k]
        if a["sub"] == "move" and s.get("end") is not None and a.get("end") is not None and s["end"] != a["end"]:
            out.append(f"step {k + 1} {describe(a)}: ends at {s['end']} in predict, {a['end']} enumerated")
    return out


def surprises(pred: dict) -> Counter:
    """Engine-side uncertainty in a prediction, by kind."""
    c = Counter()
    for s in pred.get("steps") or []:
        if s.get("lua_errors"):
            c["step_lua_error"] += 1
        if s.get("unapplied_lua_writes"):
            c["step_unapplied_lua_writes"] += 1
        if s.get("timing_sensitive"):
            c["step_timing_sensitive"] += 1
        if s.get("chance_nodes"):
            c["step_chance_node"] += 1
        if s.get("mission_hooks"):
            c["step_mission_hook"] += 1
    ph = pred.get("enemy_phase") or {}
    if ph.get("lua_errors"):
        c["enemy_lua_error"] += 1
    if ph.get("unapplied_lua_writes"):
        c["enemy_unapplied_lua_writes"] += 1
    if ph.get("timing_sensitive"):
        c["enemy_timing_sensitive"] += 1
    if ph and not ph.get("exact", True):
        c["enemy_inexact"] += 1
    if ph and not ph.get("quiescent", True):
        c["enemy_not_quiescent"] += 1
    if ph.get("mission_ended"):
        c["mission_ended"] += 1
    return c


# ------------------------------------------------------------------ session

class Session:
    def __init__(self, args, root: Path | None = None):
        self.args = args
        base = (root or FUZZ_ROOT) / time.strftime("%Y%m%d_%H%M%S")
        self.dir, n = base, 1
        while self.dir.exists():
            self.dir, n = base.with_name(f"{base.name}-{n}"), n + 1
        self.dir.mkdir(parents=True)
        self.backup: Path | None = None  # the profile's own save files (live)
        self.findings = 0
        self.mix: Counter = Counter()  # sub-action kinds and weapons played
        self.tags: Counter = Counter()  # effect tags of the sub-actions played
        self.outcomes: Counter = Counter()
        self.surprise: Counter = Counter()
        self.rejected: Counter = Counter()
        self.samples = 0
        self.steps = 0
        self.pool: StartPool | None = None
        self.t0 = time.time()

    def write_meta(self, extra: dict | None = None) -> None:
        meta = {"tool": "scripts/fuzz_play.py", "argv": sys.argv[1:], "created": lp.now_iso(),
                "dry_run": bool(self.args.dry_run), "seed": self.args.seed, "git": git_rev()}
        meta.update(extra or {})
        (self.dir / "session.json").write_text(json.dumps(meta, indent=1))

    def finding(self, kind: str, ts: TurnStart, sample: int, detail: dict) -> None:
        self.findings += 1
        rec = {"t": lp.now_iso(), "kind": kind, "sample": sample,
               "source": {"run": ts.run, "mission": ts.mission, "turn": ts.turn, "solve_input": str(ts.path),
                          "saveData": str(ts.save) if ts.save else None}}
        rec.update(detail)
        with open(self.dir / "findings.jsonl", "a") as f:
            f.write(json.dumps(rec) + "\n")
        say(f"  FINDING ({kind}): " + "; ".join(detail.get("diffs") or [detail.get("problem", "")]))

    def sample_log(self, rec: dict) -> None:
        with open(self.dir / "samples.jsonl", "a") as f:
            f.write(json.dumps(rec) + "\n")

    def count(self, seq: Sequence, executed: int | None = None) -> None:
        picks = seq.picks if executed is None else seq.picks[:executed]
        self.steps += len(picks)
        for a in picks:
            self.mix[a["sub"]] += 1
            if a["sub"] == "weapon":
                self.mix[f"weapon:{a['weapon']}"] += 1
            self.mix[f"target:{a.get('target_kind')}"] += 1
            self.tags.update(effect_tags(a))
        self.rejected.update(seq.rejected)

    def summary(self) -> str:
        mins = (time.time() - self.t0) / 60
        lines = [f"{self.samples} sample(s), {self.steps} sub-action(s) in {mins:.1f} min; "
                 f"{self.findings} finding(s) -> {self.dir}"]
        kinds = {k: v for k, v in self.mix.items() if ":" not in k}
        lines.append("  kinds: " + ", ".join(f"{k} {v}" for k, v in sorted(kinds.items())))
        weapons = sorted(((v, k.split(":", 1)[1]) for k, v in self.mix.items() if k.startswith("weapon:")),
                         reverse=True)
        if weapons:
            lines.append("  weapons: " + ", ".join(f"{w} {v}" for v, w in weapons))
        targets = sorted(((v, k.split(":", 1)[1]) for k, v in self.mix.items() if k.startswith("target:")),
                         reverse=True)
        lines.append("  targets: " + ", ".join(f"{t} {v}" for v, t in targets))
        if self.tags:
            lines.append("  effects: " + ", ".join(f"{k} {v}" for k, v in self.tags.most_common()))
        if self.outcomes:
            lines.append("  outcomes: " + ", ".join(f"{k} {v}" for k, v in sorted(self.outcomes.items())))
        if self.surprise:
            lines.append("  engine notes: " + ", ".join(f"{k} {v}" for k, v in sorted(self.surprise.items())))
        if self.rejected:
            lines.append("  enumerated but refused by the engine: "
                         + ", ".join(f"{k} {v}" for k, v in sorted(self.rejected.items())))
        return "\n".join(lines)


def git_rev() -> str | None:
    try:
        return subprocess.run(["git", "-C", str(REPO), "rev-parse", "--short", "HEAD"], capture_output=True,
                              text=True, check=False).stdout.strip() or None
    except OSError:
        return None


class StartPool:
    """The turn starts to fuzz: drawn without replacement (a shuffled pass
    over all of them; once each has been drawn, a new pass, said so), a
    rejected one (unplayable, before or after a restore) never again. With
    --mission/--turn: that one start, every time (until rejected)."""

    def __init__(self, starts: list, rng: random.Random, fixed: TurnStart | None = None):
        self.starts = list(starts)
        self.rng = rng
        self.fixed = fixed
        self.queue: list = []
        self.passes = 0
        self.bad: dict = {}  # key -> why

    def draw(self) -> TurnStart | None:
        """The next start, None when every one has been rejected."""
        if self.fixed is not None:
            return None if self.fixed.key in self.bad else self.fixed
        while True:
            while self.queue:
                ts = self.queue.pop()
                if ts.key not in self.bad:
                    return ts
            left = [ts for ts in self.starts if ts.key not in self.bad]
            if not left:
                return None
            if self.passes:
                say(f"  every turn start has been drawn ({len(left)} playable of {len(self.starts)}): "
                    "drawing them again (repeats from here)")
            self.passes += 1
            self.queue = left
            self.rng.shuffle(self.queue)

    def reject(self, ts: TurnStart, why: str) -> None:
        self.bad[ts.key] = why

    def exhausted(self) -> str:
        if self.fixed is not None:
            return f"the turn start named is not playable: {self.bad.get(self.fixed.key)}"
        return f"no playable turn start left: all {len(self.starts)} were rejected"


def start_pool(args, starts: list, rng: random.Random) -> StartPool:
    """The pool of turn starts to fuzz: the one named, else all of them."""
    if args.mission is not None and args.turn is not None:
        chosen = [ts for ts in starts if ts.mission == args.mission and ts.turn == args.turn]
        if not chosen:
            raise lp.Refused(f"no recorded turn start m{args.mission:02d} turn {args.turn} in {args.run}")
        return StartPool(chosen, rng, chosen[0])
    return StartPool(starts, rng)


def offline_reason(engine, ts: TurnStart, args, rng: random.Random, grid: bool = True) -> tuple | None:
    """Why a recorded turn start cannot be fuzzed, from the recording alone
    (before any restore), as (key, text), or None: its states unreadable,
    unplayable(), and with grid, no sequence of --tries keeping --min-grid
    and a mech through the enemy phase from the recorded solve input
    (safe_sequence with a scratch chooser: the session's novelty counts are
    left alone) or the engine refusing it."""
    try:
        state, _ = read_state(ts.path)
        read_state(ts.raw_path())
    except (OSError, ValueError) as e:
        return "unreadable", f"unreadable recording: {e}"
    why = unplayable(state, args.last_turns)
    if why or not grid:
        return why
    scratch = random.Random(rng.random())
    try:
        seq, _, _ = safe_sequence(engine, ts.path, Chooser(scratch, args.uniform), scratch, {}, unit_skills(state),
                                  args)
    except lp.Anomaly as e:
        return "engine_error", f"the engine on the recorded start: {e}"
    if seq is None:
        return "grid_unsafe", (f"no sequence of {args.tries} the engine predicts keeps {args.min_grid} grid "
                               "and a mech")
    return None


def skip(sess, pool: StartPool, ts: TurnStart, why: tuple, when: str = "skipped") -> None:
    pool.reject(ts, why[1])
    sess.outcomes[f"skipped_{why[0]}"] += 1
    say(f"[-] {ts.key}: {when}: {why[1]}")


def budget_left(sess: Session, args) -> bool:
    return not (args.minutes and time.time() - sess.t0 > args.minutes * 60)


# ------------------------------------------------------------------ dry run

def dry_run(args, engine, sess: Session, starts: list) -> int:
    rng = random.Random(args.seed)
    chooser = Chooser(rng, uniform=args.uniform)
    pool = sess.pool = start_pool(args, starts, rng)
    i = -1
    while i + 1 < args.samples and budget_left(sess, args):
        ts = pool.draw()
        if ts is None:
            say(pool.exhausted())
            break
        why = offline_reason(engine, ts, args, rng, grid=False)
        if why:
            skip(sess, pool, ts, why)
            continue
        i += 1
        state, _ = read_state(ts.path)
        say(f"[{i + 1}] {ts.key} {state.get('mission_id')} turn {state.get('turn')}")
        try:
            seq, pred, tries = safe_sequence(engine, ts.path, chooser, rng, {}, unit_skills(state), args)
        except lp.Anomaly as e:
            sess.finding("engine_error", ts, i, {"problem": str(e)})
            sess.outcomes["engine_error"] += 1
            continue
        if seq is None:
            say(f"  no sequence of {args.tries} keeps {args.min_grid} grid and a mech: skipped")
            sess.outcomes["grid_unsafe"] += 1
            continue
        sess.samples += 1
        sess.count(seq)
        sess.surprise.update(surprises(pred))
        problems = engine_checks(seq, pred)
        say("  " + " | ".join(describe(a) for a in seq.picks) if seq.picks else "  (no legal sub-action)")
        rec = {"sample": i, "source": ts.key, "mission_id": state.get("mission_id"), "plan": seq.plan,
               "picks": [describe(a) for a in seq.picks], "candidates": seq.candidates,
               "warnings": pred.get("warnings"), "problems": problems,
               "after_enemy_grid": (pred.get("after_enemy") or {}).get("grid_power")}
        sess.sample_log(rec)
        if problems:
            sess.outcomes["engine_inconsistent"] += 1
            plan_path = sess.dir / f"s{i:03d}_plan.json"
            plan_path.write_text(json.dumps(seq.plan, indent=1))
            sess.finding("engine_inconsistent", ts, i, {"diffs": problems, "plan": seq.plan,
                                                       "files": {"start": str(ts.path), "plan": str(plan_path)},
                                                       "replay": f"itb_live predict {ts.path} --plan @{plan_path}"})
        else:
            sess.outcomes["consistent"] += 1
    say(sess.summary())
    return 0


# ------------------------------------------------------------------ live safety

def other_drivers(own_pid: int | None = None) -> list:
    """Command lines of other processes driving the game (OTHER_DRIVERS),
    --dry-run ones, this process and its ancestors (the shell that started
    it) aside."""
    try:
        out = subprocess.run(["ps", "-axo", "pid=,ppid=,command="], capture_output=True, text=True,
                             check=False).stdout
    except OSError:
        return []
    return match_drivers(out.splitlines(), own_pid or os.getpid())


def match_drivers(lines: list, own: int) -> list:
    """The `ps -axo pid=,ppid=,command=` lines of other drivers
    (OTHER_DRIVERS), not `own` or its ancestors."""
    procs = {}
    for line in lines:
        parts = line.strip().split(None, 2)
        if len(parts) == 3 and parts[0].isdigit() and parts[1].isdigit():
            procs[int(parts[0])] = (int(parts[1]), parts[2])
    skip, pid = set(), own
    while pid in procs and pid not in skip:
        skip.add(pid)
        pid = procs[pid][0]
    skip.add(pid)
    found = []
    for pid, (_, cmd) in sorted(procs.items()):
        if pid in skip or "--dry-run" in cmd or "ps -axo" in cmd:
            continue
        if any(re.search(p, cmd) for p in OTHER_DRIVERS):
            found.append(f"{pid} {cmd.strip()[:160]}")
    return found


def recent_activity(idle: float) -> list:
    """Signs another driver acted on the game within `idle` seconds: the
    bridge's last ack, a command still waiting, the current live run's log."""
    now, out = time.time(), []
    if lv.CMD.exists():
        out.append(f"a bridge command is waiting ({lv.CMD})")
    for path, what in ((lv.ACK, "the bridge acked a command"),):
        try:
            age = now - path.stat().st_mtime
        except OSError:
            continue
        if age < idle:
            out.append(f"{what} {age:.0f}s ago ({path})")
    try:
        cur = Path(json.loads(lp.CURRENT.read_text())["run"]) / "events.jsonl"
        age = now - cur.stat().st_mtime
        if age < idle:
            out.append(f"the current live run logged an event {age:.0f}s ago ({cur})")
    except (OSError, ValueError, KeyError):
        pass
    return out


def check_safe(args) -> None:
    others = other_drivers()
    if others:
        raise lp.Refused("another driver is running: " + " | ".join(others))
    if args.idle_seconds > 0:
        busy = recent_activity(args.idle_seconds)
        if busy:
            raise lp.Refused("the game was driven recently (--idle-seconds): " + "; ".join(busy))
    if not lp.game_running():
        raise lp.Refused("the game is not running (start it first; each sample then quits and relaunches it)")


# ------------------------------------------------------------------ live restore (quit, swap, relaunch, Continue twice)
#
# The game reads its save files only at launch: Continue after the pause
# menu's Main Menu resumes the run still in memory, not a save swapped in
# meanwhile. So each sample restores exactly as `live_play.py restore`
# does (its pieces: quit_game, swap_saves, relaunch, continue_to_player_turn),
# then loads once more (live_play.reload_save) against the Grid Defense
# restart quirk.

def restore_by_relaunch(ctx, sess: Session, ts: TurnStart) -> dict:
    """Loads a recorded turn start: quits the game, puts the checkpoint's
    save files in the profile (the profile's own ones copied to
    <session>/profile_backup/ the first time, after the game has exited),
    relaunches it, clicks Continue on the title screen until a fresh player
    turn shows, then loads that save once more (gear or Esc, Main Menu, Continue)
    and checks the board against the recorded turn start and the Grid
    Defense base (live_play.grid_base_wrong). Returns the live state."""
    args = ctx.args
    say(f"  restoring {ts.key}")
    if lp.game_running():
        say("  quitting the game")
        lp.quit_game()
    backup = None
    if sess.backup is None:
        backup = sess.backup = sess.dir / "profile_backup"
    lp.swap_saves({"saveData.lua": ts.save, "undoSave.lua": ts.undo}, backup)
    if backup is not None:
        say(f"  the profile's own save files are in {backup} (put back at the end)")
    t0 = lp.relaunch(args.timeout)
    first = lp.continue_to_player_turn(ctx, t0 + args.timeout, args.continue_xy, guard=True)
    say(f"  loaded {first.get('mission_id')} turn {first.get('turn')} (Grid Defense base "
        f"{first.get('grid_defense_base')}, built under difficulty {first.get('grid_base_difficulty')})")
    state = lp.reload_save(ctx, args.timeout)
    want, _ = read_state(ts.raw_path())
    diffs = lp.checkpoint_diffs(want, state)
    if diffs:
        raise lp.Anomaly(f"the loaded board differs from {ts.raw_path().name}: " + "; ".join(diffs))
    warning = lp.grid_base_wrong(state)
    if warning:
        raise lp.Anomaly(f"after the second load: {warning}")
    say(f"  loaded again: board as recorded, Grid Defense base {state.get('grid_defense_base')} "
        f"(difficulty {state.get('grid_base_difficulty')})")
    return state


def restore_profile(ctx, backup: Path) -> list:
    """The profile's own save files back from `backup`: quits the game,
    puts them in (a file the backup lacks is removed: no saveData.lua is no
    run in progress), relaunches. With a run in progress, clicks Continue
    once and gives the bridge END_WAIT s: a mission that opens is loaded
    once more (the Grid Defense restart quirk) and its base checked; a
    silent bridge (the island map) gets the second load blind (map_reload).
    Without one, the game stays on the title screen. Returns warnings."""
    args, bridge = ctx.args, ctx.bridge
    has_run = (backup / "saveData.lua").exists()
    say("putting the profile's own save files back" + ("" if has_run else " (no run in progress in them)"))
    if lp.game_running():
        say("  quitting the game")
        lp.quit_game()
    lp.swap_saves({name: backup / name for name in lp.SAVE_FILES})
    t0 = lp.relaunch(args.timeout)
    if not has_run:
        say("  relaunched: the game is on the title screen")
        return []
    time.sleep(max(0.0, t0 + END_CONTINUE_DELAY - time.time()))
    if args.no_click:
        say("  >>> click Continue on the title screen now")
    else:
        say("  clicking Continue (once)")
    clicked = time.time()
    if not args.no_click:
        ctx.click(args.continue_xy)
    deadline = clicked + END_WAIT
    while time.time() < deadline and lp.fresh_mission(bridge, clicked) is None:
        time.sleep(lp.POLL)
    if lp.fresh_mission(bridge, clicked) is None:
        return map_reload(ctx)
    time.sleep(END_SETTLE)  # the resumed mission's opening, before the pause menu
    state = lp.reload_save(ctx, args.timeout, player_turn=False)
    say(f"  resumed {state.get('mission_id')} turn {state.get('turn')} phase {state.get('phase')}")
    warning = lp.grid_base_wrong(state)
    if warning:
        say("  " + warning)
        return [warning]
    return []


def map_reload(ctx) -> list:
    """The put-back's second load when its run resumed outside a mission
    (the bridge stayed silent END_WAIT s after Continue: the island map,
    where it is always silent and Esc does nothing): blind, the map's gear
    (--map-gear-xy), the pause menu's MAIN MENU, the title's Continue,
    MAP_CLICK_WAIT s after each click, nothing to confirm them. (Were the
    game still on the title screen, the gear and MAIN MENU points hold
    nothing there, and Continue loads the run.) A mission that shows after
    all gets its Grid Defense base checked, and one more load if it is
    wrong. Returns notes."""
    args, bridge = ctx.args, ctx.bridge
    gear = getattr(args, "map_gear_xy", None) or lp.MAP_GEAR
    menu = getattr(args, "main_menu_xy", None) or lp.PAUSE_MAIN_MENU
    cont = getattr(args, "continue_xy", None) or lp.TITLE_CONTINUE
    silent = f"the bridge stayed silent {END_WAIT:.0f}s after Continue: the run resumed on the island map"
    if args.no_click:
        msg = (f"{silent}. Load it once more by hand (Grid Defense restart quirk): the gear at the top left, "
               "MAIN MENU, Continue")
        say("  >>> " + msg)
        return [msg]
    say(f"  {silent} (no heartbeat there). Loading it once more blind (Grid Defense restart quirk): the map's "
        f"gear, MAIN MENU, Continue, waiting {MAP_CLICK_WAIT:.0f}s after each click (the bridge cannot confirm them)")
    clicked = time.time()
    for what, xy in (("the gear", gear), ("MAIN MENU", menu), ("Continue", cont)):
        say(f"  clicking {what} at {xy}")
        clicked = time.time()
        ctx.click(xy)
        time.sleep(MAP_CLICK_WAIT)
    state = lp.fresh_mission(bridge, clicked)
    if state is None:
        msg = ("on the island map: loaded once more blind (gear, MAIN MENU, Continue; unconfirmed: the bridge is "
               "silent there)")
        say("  " + msg)
        return [msg]
    say(f"  a mission is open: {state.get('mission_id')} turn {state.get('turn')} phase {state.get('phase')}")
    warning = lp.grid_base_wrong(state)
    if warning:
        say("  " + warning + ": loading once more")
        time.sleep(END_SETTLE)
        state = lp.reload_save(ctx, args.timeout, player_turn=False)
        warning = lp.grid_base_wrong(state)
        if warning:
            say("  " + warning)
            return [warning]
    return []


def put_back(ctx, sess: Session) -> list:
    """At the end of a live session: restore_profile from its backup."""
    if sess.backup is None:
        return []
    return restore_profile(ctx, sess.backup)


# ------------------------------------------------------------------ live turns

def remap(plan: list, uid_map: dict) -> list:
    return [dict(a, uid=uid_map.get(a["uid"], a["uid"])) for a in plan]


def live_turn(ctx, sess: Session, ts: TurnStart, sample: int, chooser: Chooser, rng: random.Random) -> str:
    """One fuzzed player turn on the live board, then End Turn and the
    enemy phase. Returns the outcome: "matched", "diverged", "mission_over"
    or "refused"."""
    args, bridge, engine, run = ctx.args, ctx.bridge, ctx.engine, ctx.run
    state = bridge.settled_state()
    why = unplayable(state, args.last_turns)
    if why:
        say(f"  not playable: {why[1]}")
        return "not_playable"
    under = run.engine_underground(state)
    known = {u["uid"] for u in lp.units(state)} | set(under)
    names = {u["uid"]: u.get("type", "?") for u in lp.units(state)}
    names.update({uid: d.get("type", "?") for uid, d in under.items()})
    history: dict = {}
    moved: set = set()
    tracked: dict = {}
    react = run.reactivation_list(state, turn_start=True)
    shields = run.engine_shields(state)
    skills = unit_skills(state)
    # A restored game's bridge has no turn-start building snapshot, so it
    # estimates no grid lost during the turn: this turn start stands in.
    run.save_grid_start(state)
    grid_start = run.grid_start(state)

    def solver_input(st: dict, label: str) -> Path:
        notes: list = []
        inp, patches = lp.patch_state(st, ctx.loadout, moved, tracked, notes, under, history, react, shields,
                                      grid_start)
        path = run.save_state(inp, label, {"patches": patches} if patches else None)
        if patches:
            run.save_state(st, label + "_raw")
        return path

    start = solver_input(state, "start")
    say(f"  {state.get('mission_id')} turn {state.get('turn')}: generating")
    seq, pred, tries = safe_sequence(engine, start, chooser, rng, history, skills, args)
    run.save_json(lp.prediction_path(start).name, pred)
    if seq is None:
        say(f"  no sequence of {args.tries} keeps {args.min_grid} grid and a mech: not played")
        return "grid_unsafe"
    run.save_json("plan.json", seq.plan)
    sess.surprise.update(surprises(pred))
    say("  plan: " + (" | ".join(describe(a) for a in seq.picks) or "(nothing)"))
    errors = lp.ui_limit_errors(pred.get("steps") or [], history, state, names)
    problems = engine_checks(seq, pred) + errors
    if problems:
        sess.finding("engine_inconsistent", ts, sample, {"diffs": problems, "plan": seq.plan,
                                                        "files": {"start": str(start)}})
        sess.count(seq, 0)
        return "refused"
    lp.ensure_native(ctx)
    steps, i, executed, uid_map = pred["steps"], 0, 0, {}
    offset = 0  # index in seq.plan of the predicted plan's first action
    outcome = "matched"
    while i < len(steps):
        step = steps[i]
        k = offset + int(step.get("action", i))  # index in seq.plan
        if step["uid"] not in known and step["uid"] not in uid_map:
            sess.finding("uid_unmatched", ts, sample, {"problem": f"step {k + 1}: no unit created this turn matches "
                                                                  f"the engine's #{step['uid']}",
                                                       "plan": seq.plan[:k + 1]})
            outcome = "diverged"
            break
        step = dict(step, uid=uid_map.get(step["uid"], step["uid"]))
        text = lp.step_text(step, names)
        if step["sub"] != "move":
            blocked = lp.ui_blocked(state, step["uid"])
            if blocked:
                sess.finding("ui_blocked", ts, sample, {"problem": f"step {k + 1} {text}: the unit is {blocked} in "
                                                                   "the game; the engine let it act",
                                                        "plan": seq.plan[:k + 1], "files": {"start": str(start)}})
                outcome = "diverged"
                break
        try:
            state, path, live, diffs, shields = lp.play_step(ctx, step, state, known, executed + 1, tracked, under,
                                                             history, moved, text)
        except lp.Anomaly as e:
            sess.finding("game_refused", ts, sample, {"problem": f"step {k + 1} {text}: {e}",
                                                      "plan": seq.plan[:k + 1], "files": {"start": str(start)}})
            outcome = "diverged"
            break
        executed += 1
        if not diffs:
            uid_map = lp.unit_uid_map(step["board"], live, known, uid_map)
            for u in live["units"]:
                names.setdefault(u["uid"], u.get("type", "?"))
            say(f"  step {executed} {text}: matches")
            i += 1
            continue
        chance = bool(step.get("chance_nodes"))
        resisted = []
        if chance:
            # A Grid Defense roll the game won: the building kept HP the
            # engine (worst case) took. Benign, but the rest of the plan was
            # predicted without it: re-predicted from the live board.
            before = steps[i - 1]["board"] if i > 0 else pred.get("start") or {}
            diffs, resisted = lp.diff_boards(step["board"], live, known, resist={"before": before, "tiles": None})
        if chance and not diffs:
            uid_map = lp.unit_uid_map(step["board"], live, known, uid_map)
            say(f"  step {executed} {text}: " + "; ".join(resisted) + " (chance node; benign)")
            run.event("resisted", {"step": text, "state": path.name, "notes": resisted})
            sess.outcomes["step_resisted"] += 1
        else:
            plan_path = run.save_json(f"plan_to_step{executed}.json", seq.plan[:k + 1])
            say(f"  step {executed} {text}: MISMATCH" + (" (after a chance node)" if chance else ""))
            sess.finding("chance" if chance else "step", ts, sample, {
                "step": k + 1, "action": text, "diffs": diffs, "plan": seq.plan[:k + 1],
                "pick": seq.picks[k] if k < len(seq.picks) else None,
                "files": {"start": str(start), "live": str(path), "prediction": str(lp.prediction_path(start)),
                          "plan": str(plan_path), "sample_dir": str(run.path)},
                "engine_board": step["board"], "game_board": live,
                "replay": f"itb_live predict {start} --plan @{plan_path}"})
        if not (chance or args.continue_after_diff):
            outcome = "diverged"
            break
        # Re-predict the rest from the live board (created units by their
        # live uids); a rest the live board no longer allows ends there.
        rest = remap(seq.plan[k + 1:], {e: g for e, g in uid_map.items()})
        if not rest:
            break
        resync = solver_input(state, "resync")
        pred = engine.predict(resync, rest)
        run.save_json(lp.prediction_path(resync).name, pred)
        cut = pred.get("refused", -1)
        steps = pred["steps"][:cut] if cut >= 0 else pred["steps"]
        offset, i = k + 1, 0
        uid_map = {u["uid"]: u["uid"] for u in lp.units(state) if u["uid"] not in known}
        outcome = "resynced"
    sess.count(seq, executed)
    if outcome == "diverged":
        return outcome
    anomalies = lp.end_turn(ctx)
    if anomalies:
        ev = last_event(run, "enemy_phase")
        sess.finding("enemy_phase", ts, sample, {
            "diffs": (ev or {}).get("diffs") or anomalies, "notes": (ev or {}).get("notes"),
            "plan": seq.plan, "files": {"start": str(start), "sample_dir": str(run.path),
                                        "after_enemy": str(run.path / (ev or {}).get("state", ""))}})
        return "diverged"
    if ctx.mission_over:
        return "mission_over"
    return outcome


def last_event(run, kind: str) -> dict | None:
    try:
        lines = (run.path / "events.jsonl").read_text().splitlines()
    except OSError:
        return None
    for line in reversed(lines):
        try:
            ev = json.loads(line)
        except ValueError:
            continue
        if ev.get("kind") == kind:
            return ev.get("detail")
    return None


def seed_run(run, state: dict, carry: Carry) -> None:
    """A sample's run directory starts with what live play carried into the
    recorded turn start (Burrowers underground, shields, the visible list)."""
    run.mission_index(state)
    if carry.underground:
        run.save_engine_underground(state, carry.underground)
    if carry.shields:
        run.save_engine_shields(state, carry.shields)
    if carry.reactivation is not None:
        run.save_reactivation(state, carry.reactivation)


def live(args, engine, sess: Session, starts: list, bridge=None, click=None, escape=None) -> int:
    rng = random.Random(args.seed)
    chooser = Chooser(rng, uniform=args.uniform)
    ctx = lp.Context(args, bridge=bridge, engine=engine, run=None, click=click, escape=escape)
    ctx.loadout = {}
    lock = lp.DriverLock("fuzz_play.py").acquire()
    rc = 0
    try:
        pool = sess.pool = start_pool(args, starts, rng)
        i = -1
        while i + 1 < args.samples and budget_left(sess, args):
            ts = pool.draw()
            if ts is None:
                say(pool.exhausted())
                break
            # Only a start the recording shows playable costs a restore.
            why = offline_reason(engine, ts, args, rng)
            if why:
                skip(sess, pool, ts, why, "not restored")
                continue
            i += 1
            say(f"[{i + 1}] {ts.key}")
            run = lp.Run(sess.dir / f"s{i:03d}", {"tool": "scripts/fuzz_play.py", "source": ts.key})
            ctx.run, ctx.mission_over = run, False
            try:
                state = restore_by_relaunch(ctx, sess, ts)
            except lp.Anomaly as e:
                # Never fuzz on (or restore again from) a board that is not
                # the checkpoint's: stop.
                sess.outcomes["restore_failed"] += 1
                raise LeftMission(f"restore failed: {e}") from e
            ctx.native_checked = False  # EXEC_MODE native again after each load
            rec_state, rec_data = read_state(ts.path)
            seed_run(run, state, recorded_carry(rec_state, rec_data))
            sess.samples += 1
            for t in range(args.turns):
                try:
                    outcome = live_turn(ctx, sess, ts, i, chooser, rng)
                except lp.Anomaly as e:
                    sess.finding("driver", ts, i, {"problem": str(e)})
                    if "silent" in str(e):
                        raise LeftMission(str(e)) from e
                    outcome = "driver_error"
                sess.outcomes[outcome] += 1
                if t == 0 and outcome in ("not_playable", "grid_unsafe"):
                    pool.reject(ts, f"{outcome} after the restore")  # never restored again
                if outcome == "mission_over":
                    raise LeftMission("the mission ended (the game is past the mission's last enemy phase)")
                if outcome not in ("matched", "resynced"):
                    break
            sess.sample_log({"sample": i, "source": ts.key, "dir": str(run.path)})
        say(sess.summary())
    except LeftMission as e:
        say(sess.summary())
        say(f"STOPPED: {e}")
        rc = 1
    finally:
        try:
            ctx.run = None
            put_back(ctx, sess)
        except (lp.Anomaly, lp.Refused) as e:
            say(f"STOPPED while putting the profile's own save files back: {e}")
            say(f"  they are in {sess.backup}: python3 scripts/fuzz_play.py --put-back {sess.dir}")
            rc = 1
        finally:
            lock.release()
    return rc


def put_back_session(args, bridge=None, click=None, escape=None) -> int:
    """--put-back SESSION: the profile's own save files back from that
    session's profile_backup/ (restore_profile: quit, swap, relaunch,
    Continue and the second load when they hold a run). A backup without
    saveData.lua (no run was in progress) is put back as it is."""
    backup = Path(args.put_back) / "profile_backup"
    if not backup.is_dir():
        say(f"REFUSED: no {backup}")
        return 2
    held = [name for name in lp.SAVE_FILES if (backup / name).exists()]
    say(f"profile backup {backup}: " + (", ".join(held) if held else "no save files (an empty profile)"))
    if args.dry_run:
        say("dry run: would quit the game, put these in the profile (removing the others), relaunch it"
            + (", click Continue and load once more" if "saveData.lua" in held else ""))
        return 0
    others = other_drivers()
    if others:
        say("REFUSED: another driver is running: " + " | ".join(others))
        return 2
    ctx = lp.Context(args, bridge=bridge, engine=None, run=None, click=click, escape=escape)
    lock = lp.DriverLock("fuzz_play.py --put-back").acquire()
    try:
        restore_profile(ctx, backup)
        return 0
    finally:
        lock.release()


# ------------------------------------------------------------------ main

def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dry-run", action="store_true", help="engine only: never touches the game")
    ap.add_argument("--put-back", metavar="SESSION",
                    help="put a live session's profile_backup save files back (quit, swap, relaunch, Continue "
                         "and the second load, from a mission or the island map; with --dry-run: only say what it "
                         "would do)")
    ap.add_argument("--recordings", default=str(lp.LIVE_ROOT), help="live_play run directories (default %(default)s)")
    ap.add_argument("--run", action="append", help="only this run (repeatable); with --mission/--turn: that turn")
    ap.add_argument("--mission", type=int, help="mission index MM of m<MM>_turn_<TT> (with --run and --turn)")
    ap.add_argument("--turn", type=int, help="turn TT (with --run and --mission)")
    ap.add_argument("--samples", type=int, default=10, help="turn starts to fuzz (default %(default)s)")
    ap.add_argument("--minutes", type=float, default=0, help="stop after this long (0: no limit)")
    ap.add_argument("--seed", type=int, default=None, help="random seed (default: from the clock; logged)")
    ap.add_argument("--max-steps", type=int, default=MAX_STEPS, help="sub-actions per turn at most")
    ap.add_argument("--stop-prob", type=float, default=STOP_PROB, help="chance to end the turn after each step")
    ap.add_argument("--uniform", action="store_true", help="uniform choice (no bias toward odd actions)")
    ap.add_argument("--min-grid", type=int, default=2,
                    help="play only sequences the engine predicts keep this much grid (and a mech) through the "
                         "enemy phase")
    ap.add_argument("--tries", type=int, default=5, help="sequences to try for --min-grid before skipping a start")
    ap.add_argument("--last-turns", action="store_true",
                    help="also a mission's last turn (its enemy phase ends the mission: reward screens, or the "
                         "end of the run after the final Cave)")
    ap.add_argument("--turns", type=int, default=1, help="live: turns to play on from each restored start")
    ap.add_argument("--continue-after-diff", action="store_true",
                    help="live: after a step difference, re-predict from the live board and go on")
    ap.add_argument("--threads", type=int, default=1, help="engine threads (itb_live serve)")
    ap.add_argument("--game", help="game install for the engine (default: ITB_GAME_DIR / build default)")
    ap.add_argument("--itb-live", help="itb_live binary (default engine/build/itb_live)")
    ap.add_argument("--out", help="session root (default recordings/fuzz)")
    xy = lambda s: tuple(int(v) for v in s.split(","))  # noqa: E731
    ap.add_argument("--continue-xy", type=xy, default=lp.TITLE_CONTINUE,
                    help="the title screen's Continue (default: fullscreen 1360x768)")
    ap.add_argument("--gear-xy", type=xy, default=lp.PAUSE_GEAR,
                    help="the pause menu's gear icon in a mission, for the second load (Esc is the fallback; "
                         "default: fullscreen 1360x768)")
    ap.add_argument("--map-gear-xy", type=xy, default=lp.MAP_GEAR,
                    help="the gear icon on the island map, for the put-back's second load there (default: "
                         "fullscreen 1360x768)")
    ap.add_argument("--main-menu-xy", type=xy, default=lp.PAUSE_MAIN_MENU,
                    help="the pause menu's MAIN MENU, for the second load (default: fullscreen 1360x768)")
    ap.add_argument("--idle-seconds", type=float, default=300.0,
                    help="live: refuse if the bridge acked a command or the live run logged this recently")
    # live_play.end_turn's options.
    ap.add_argument("--timeout", type=float, default=180.0, help="wait for the next player turn (s)")
    ap.add_argument("--no-click", action="store_true", help="never click; ask to click End Turn / Continue")
    ap.add_argument("--end-turn-xy", type=xy, default=lv.END_TURN_SCREEN, help="End Turn screen point")
    ap.add_argument("--max-heartbeat-age", type=float, default=10.0)
    return ap


def main(argv=None, *, bridge=None, engine=None, click=None, escape=None) -> int:
    args = build_parser().parse_args(argv)
    if args.seed is None:
        args.seed = int(time.time() * 1000) % 2 ** 31
    if args.put_back:
        try:
            return put_back_session(args, bridge=bridge, click=click, escape=escape)
        except lp.Refused as e:
            say(f"REFUSED: {e}")
            return 2
        except lp.Anomaly as e:
            say(f"STOPPED: {e}")
            return 1
    if (args.mission is None) != (args.turn is None) or (args.mission is not None and not args.run):
        say("REFUSED: --mission and --turn go together, with --run")
        return 2
    if args.itb_live:
        lp.ITB_LIVE = Path(args.itb_live)
    root = Path(args.recordings)
    starts = find_turn_starts(root, args.run, need_save=not args.dry_run)
    if not starts:
        say(f"REFUSED: no recorded turn starts under {root}" + ("" if args.dry_run else " with save checkpoints"))
        return 2
    sess = None
    own_engine = engine is None
    try:
        if not args.dry_run:
            check_safe(args)
        sess = Session(args, Path(args.out) if args.out else None)
        sess.write_meta({"recordings": str(root), "turn_starts": len(starts)})
        say(f"fuzz session {sess.dir} (seed {args.seed}, {len(starts)} turn starts"
            + (", dry run)" if args.dry_run else ")"))
        if engine is None:
            engine = lp.Engine(args.threads, args.game, sess.dir / "itb_live.log")
        if args.dry_run:
            return dry_run(args, engine, sess, starts)
        if click is None:
            click = None if args.no_click else lp.click_end_turn
        return live(args, engine, sess, starts, bridge=bridge, click=click, escape=escape)
    except lp.Refused as e:
        say(f"REFUSED: {e}")
        return 2
    except lp.Anomaly as e:
        say(f"STOPPED: {e}")
        return 1
    finally:
        if own_engine and engine is not None:
            engine.close()
        if sess is not None:
            sess.write_meta({"recordings": str(root), "turn_starts": len(starts), "samples": sess.samples,
                             "steps": sess.steps, "findings": sess.findings, "outcomes": dict(sess.outcomes),
                             "mix": dict(sess.mix), "effects": dict(sess.tags), "engine_notes": dict(sess.surprise),
                             "rejected_starts": dict(sess.pool.bad) if sess.pool else {},
                             "passes": sess.pool.passes if sess.pool else 0})


if __name__ == "__main__":
    sys.exit(main())
