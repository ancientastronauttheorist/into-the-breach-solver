"""Adapter between the C++ perfect-turn solver (module ``itb_engine``) and the bot.

The engine (``engine/``, Python bindings in ``engine/python``) takes the
bridge state as JSON and returns a plan of sub-actions plus the board after
every sub-action, after the player's turn and after the enemy phase. This
module turns that into what ``cmd_solve`` / ``cmd_auto_turn`` already consume:

  - ``Solution`` / ``MechAction`` objects (one per engine plan entry), with the
    old solver's conventions: ``move_to`` is the tile the unit acts from (its
    current tile when it does not move), a move-only entry has weapon
    ``"None"`` and target ``(255, 255)``, a repair has weapon ``"_REPAIR"``
    and targets its own tile;
  - an ``enriched`` dict shaped like ``replay_solution()``'s output
    (``action_results``, ``predicted_states`` with ``post_move`` /
    ``post_attack`` snapshots built by ``verify.snapshot_after_*`` so
    ``verify.diff_states`` compares them exactly like the Rust predictions,
    ``post_player_board``, ``final_board``, ``predicted_outcome``,
    ``score_breakdown``);
  - a ``meta`` dict for the solve recording (``solver: "cpp"``, engine
    version, worst-case score tiers, proof status, upper bound, stats).

Execution model. The engine may interleave units (A moves, B acts, A fires).
The bridge supports that: ``MOVE`` leaves the mech active, ``ATTACK`` /
``REPAIR`` deactivate it. So:
  - an interleaved plan is kept only when regrouping it into per-unit blocks
    changes its worst-case score (checked with the engine); otherwise the
    regrouped plan is used, which is exactly as good and simpler to execute;
  - sub-actions of a unit after its attack or repair (Shifty / Post_Move /
    Double_Shot pilots) cannot be executed by the bridge and are dropped
    (the plan without them is re-evaluated and its score recorded);
  - ``cmd_auto_turn`` skips the SKIP after a move-only entry when the same
    unit acts later in the plan.

Selection: ``ITB_SOLVER=cpp|rust`` (default ``cpp``); any error, timeout
without a plan, or unsupported board makes ``solve`` return ``ok=False`` with
a reason, and the caller falls back to the Rust solver.
"""

from __future__ import annotations

import json
import os
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from src.solver.solver import MechAction, Solution

REPO_ROOT = Path(__file__).resolve().parents[2]
NO_TARGET = (255, 255)
MOVE_ONLY_WEAPON = "None"
REPAIR_WEAPON = "_REPAIR"
# The engine's score tiers in rank order (engine score.hpp ScoreKey). "threat"
# (enemies able to hit a building next turn) is only scored, and only present
# in the engine's JSON, with the experimental threat tier
# (ITB_THREAT_TIER=1, options "threat_tier").
SCORE_TIERS = (
    "grid", "building_hp", "mechs", "objectives_failed", "threat", "mech_hp",
    "objectives", "kills", "vek_hp", "position",
)
# Tier weights for the single float ``Solution.score`` the bot prints and
# stores; the lexicographic tiers themselves are in ``meta["worst_case"]``.
_SCORE_WEIGHTS = (1e8, 1e7, 1e6, 1e5, 2e4, 1e4, 1.0, 1e2, 1e1, 1.0)
DEFAULT_THREADS = 8
# Achievement overlays the engine's fixed tiers already cover.
_OVERLAYS_COVERED_BY_ENGINE = {"final_turn_pod_collection"}



def _tier_options() -> dict:
    """Engine options that change how plans are ranked: the experimental
    next-turn threat tier when ITB_THREAT_TIER is 1/true, its variants with
    ITB_THREAT_KILLS / ITB_OBJECTIVES_FIRST (default off)."""
    def on(name: str) -> bool:
        return os.environ.get(name, "").strip().lower() in ("1", "true", "yes", "on")
    opts = {}
    for env, key in (("ITB_THREAT_TIER", "threat_tier"), ("ITB_THREAT_KILLS", "threat_kills"),
                     ("ITB_OBJECTIVES_FIRST", "objectives_first"), ("ITB_MECH_GRID_TURNS", "mech_grid_turns")):
        if on(env):
            opts[key] = True
    # ITB_MECH_GRID=N (N >= 0): a mech is worth up to N grid (SolveOptions::mech_grid).
    try:
        mech_grid = int(os.environ.get("ITB_MECH_GRID", "-1"))
    except ValueError:
        mech_grid = -1
    if mech_grid >= 0:
        opts["mech_grid"] = mech_grid
    else:
        opts.pop("mech_grid_turns", None)
    return opts


_engine_lock = threading.Lock()
_engine_cache: dict[tuple[str, int], Any] = {}


# ── Selection ───────────────────────────────────────────────────────────────


def requested_solver(explicit: str | None = None) -> str:
    """``explicit`` (``--solver``) > ``ITB_SOLVER`` > ``cpp``."""
    value = (explicit or os.environ.get("ITB_SOLVER") or "cpp").strip().lower()
    return "rust" if value == "rust" else "cpp"


def overlay_blocks_cpp(overlays: list[str] | None) -> list[str]:
    """Achievement weight overlays the engine cannot honour (it has fixed tiers).

    ``ITB_CPP_IGNORE_OVERLAYS=1`` uses the engine anyway.
    """
    if os.environ.get("ITB_CPP_IGNORE_OVERLAYS") == "1":
        return []
    return [o for o in (overlays or []) if o not in _OVERLAYS_COVERED_BY_ENGINE]


# ── Module and engine ───────────────────────────────────────────────────────


def _module_dirs() -> list[Path]:
    dirs = []
    env = os.environ.get("ITB_ENGINE_MODULE_DIR")
    if env:
        dirs.append(Path(env).expanduser())
    dirs.append(REPO_ROOT / "engine" / "build" / "python")
    return dirs


def import_engine_module():
    """Import ``itb_engine``: installed, or from ``engine/build/python``."""
    try:
        import itb_engine  # type: ignore
        return itb_engine
    except ImportError:
        pass
    for d in _module_dirs():
        if d.is_dir() and any(d.glob("itb_engine*.so")):
            if str(d) not in sys.path:
                sys.path.insert(0, str(d))
            import itb_engine  # type: ignore
            return itb_engine
    raise ImportError(
        "itb_engine not built: cmake -S engine -B engine/build -G Ninja "
        "-DITB_BUILD_PYTHON=ON -DPython_EXECUTABLE=$(command -v python3) && "
        "cmake --build engine/build"
    )


def module_staleness(module) -> str | None:
    """A warning when engine sources are newer than the built module."""
    try:
        so = Path(module.__file__)
        built = so.stat().st_mtime
        newest = 0.0
        newest_path = None
        for sub in ("src", "include", "python"):
            for p in (REPO_ROOT / "engine" / sub).rglob("*"):
                if p.suffix in {".cpp", ".hpp", ".h"}:
                    m = p.stat().st_mtime
                    if m > newest:
                        newest, newest_path = m, p
        if newest > built + 5:
            return (f"itb_engine module older than {newest_path.relative_to(REPO_ROOT)}; "
                    "rebuild: cmake --build engine/build")
    except Exception:
        return None
    return None


def resolve_game_root() -> str:
    """Directory holding the game's ``scripts/scripts.lua``.

    ``ITB_GAME_DIR``, else the local pristine copy of the game
    (``.local_decompile/builds/{mac,linux}_21601364``, searched upward from
    the repo so worktrees find the main checkout's copy), else the Steam
    install. The Steam copy is last because the bridge's modloader lives in
    its scripts directory.
    """
    env = os.environ.get("ITB_GAME_DIR")
    if env:
        return env
    rel = (
        Path(".local_decompile/builds/mac_21601364/Into the Breach.app/Contents/Resources"),
        Path(".local_decompile/builds/linux_21601364"),
    )
    for base in (REPO_ROOT, *REPO_ROOT.parents):
        for r in rel:
            cand = base / r
            if (cand / "scripts" / "scripts.lua").is_file():
                return str(cand)
    steam = (Path.home() / "Library/Application Support/Steam/steamapps/common/"
             "Into the Breach/Into the Breach.app/Contents/Resources")
    if (steam / "scripts" / "scripts.lua").is_file():
        return str(steam)
    return ""


def default_threads() -> int:
    try:
        return max(1, int(os.environ.get("ITB_CPP_THREADS", "")))
    except ValueError:
        return max(1, min(DEFAULT_THREADS, (os.cpu_count() or 2) - 2))


def get_engine(threads: int | None = None, difficulty: int = 0):
    """A process-wide engine per (game root, difficulty), grown to ``threads``."""
    module = import_engine_module()
    threads = threads or default_threads()
    root = resolve_game_root()
    key = (root, int(difficulty))
    with _engine_lock:
        engine = _engine_cache.get(key)
        if engine is None:
            engine = module.Engine(root, threads, int(difficulty))
            _engine_cache[key] = engine
        elif engine.threads < threads:
            engine.ensure_threads(threads)
        return engine


def prewarm(threads: int | None = None, difficulty: int = 0,
            solver: str | None = None) -> threading.Thread | None:
    """Load the engine in the background (the scripts take ~1-3 s to load).

    ``cmd_auto_turn`` calls this before it waits for the player's turn; the
    solve then finds the engine ready (``get_engine`` serializes on a lock).
    """
    if requested_solver(solver) != "cpp":
        return None

    def _load():
        try:
            get_engine(threads, difficulty)
        except Exception:
            pass  # the solve reports the error and falls back

    t = threading.Thread(target=_load, name="itb-engine-prewarm", daemon=True)
    t.start()
    return t


# ── Coordinates and descriptions ────────────────────────────────────────────


def visual(p) -> str:
    """Bridge (x, y) -> A1-H8 (row = 8 - x, column = 'H' - y)."""
    return f"{chr(72 - int(p[1]))}{8 - int(p[0])}"


def _pt(value) -> tuple[int, int] | None:
    if isinstance(value, (list, tuple)) and len(value) >= 2 and value[0] is not None:
        return int(value[0]), int(value[1])
    return None


def _describe(mech_type: str, pos_before, move, kind: str, weapon: str, target, target2) -> str:
    parts = [mech_type]
    if move is not None and pos_before is not None and tuple(move) != tuple(pos_before):
        parts.append(f"move {visual(pos_before)}→{visual(move)}")
    if kind == "weapon":
        try:
            from src.model.weapons import get_weapon_name
            name = get_weapon_name(weapon)
        except Exception:
            name = weapon
        text = f"fire {name} at {visual(target)}"
        if target2 is not None:
            text += f" then {visual(target2)}"
        parts.append(text)
    elif kind == "repair":
        parts.append("repair")
    elif len(parts) == 1:
        parts.append("skip")
    return ", ".join(parts)


def score_scalar(worst_case: dict) -> float:
    total = 0.0
    for name, w in zip(SCORE_TIERS, _SCORE_WEIGHTS):
        v = worst_case.get(name)
        if isinstance(v, (int, float)):
            total += float(v) * w
    return total


# ── Plan shape: execution constraints ───────────────────────────────────────


def plan_entries_after_unit_action(plan: list[dict]) -> list[int]:
    """Entries the bridge cannot run: a unit's sub-action after its attack/repair.

    The bridge's ATTACK / TWO_CLICK_ATTACK / REPAIR call SetActive(false).
    """
    acted: set[int] = set()
    bad = []
    for i, a in enumerate(plan):
        uid = int(a["uid"])
        if uid in acted:
            bad.append(i)
            continue
        if a.get("kind") in ("weapon", "repair"):
            acted.add(uid)
    return bad


def is_interleaved(plan: list[dict]) -> bool:
    """True when some unit's entries are not contiguous."""
    seen: list[int] = []
    for a in plan:
        uid = int(a["uid"])
        if seen and seen[-1] == uid:
            continue
        if uid in seen:
            return True
        seen.append(uid)
    return False


def regroup_by_unit(plan: list[dict]) -> list[dict]:
    """Stable regrouping into per-unit blocks (units in order of first appearance),
    merging a unit's move-only entry with the action that follows it."""
    order: list[int] = []
    by_uid: dict[int, list[dict]] = {}
    for a in plan:
        uid = int(a["uid"])
        if uid not in by_uid:
            order.append(uid)
            by_uid[uid] = []
        by_uid[uid].append(dict(a))
    out: list[dict] = []
    for uid in order:
        block = by_uid[uid]
        merged: list[dict] = []
        for a in block:
            if (merged and merged[-1].get("kind") == "none" and merged[-1].get("move")
                    and not a.get("move") and a.get("kind") in ("weapon", "repair")):
                last = merged[-1]
                last["kind"] = a["kind"]
                last["weapon"] = a.get("weapon", "")
                last["target"] = a.get("target")
                last["target2"] = a.get("target2")
                continue
            merged.append(a)
        out.extend(merged)
    return out


def _plan_payload(plan: list[dict]) -> str:
    keys = ("uid", "move", "kind", "weapon", "target", "target2")
    return json.dumps([{k: a.get(k) for k in keys} for a in plan])


# ── Boards: engine output -> bridge-shaped dicts ────────────────────────────

_ENGINE_ONLY_UNIT_KEYS = ("off_board", "fallen", "moved", "neutral")
_UNIT_ENGINE_FIELDS = (
    "type", "x", "y", "hp", "max_hp", "team", "active", "fire", "acid",
    "frozen", "shield", "boosted", "web", "infected", "has_queued_attack",
)
_TILE_ENGINE_FIELDS = (
    "terrain", "terrain_id", "fire", "smoke", "acid", "shield", "cracked", "pod",
)
_STALE_TOP_LEVEL = {
    "tiles", "units", "attack_order", "targeted_tiles", "spawning_tiles",
    "environment_danger", "environment_danger_v2", "environment_freeze",
    "mission_final_volcano", "mission_final_cave", "timestamp", "eval_weights",
    "disabled_actions", "island_map_debug", "island_map_game_seen",
    "island_map_probe", "pilot_calibration_requests", "cached_bridge_read",
}


def _merge_unit(raw: dict | None, eng: dict) -> dict:
    out = dict(raw) if isinstance(raw, dict) else {}
    if not out:
        out.update({k: v for k, v in eng.items() if k not in _ENGINE_ONLY_UNIT_KEYS})
    for k in _UNIT_ENGINE_FIELDS:
        if k in eng:
            out[k] = eng[k]
    if eng.get("has_queued_attack"):
        out["queued_target"] = eng.get("queued_target")
        if eng.get("queued_origin") is not None:
            out["queued_origin"] = eng.get("queued_origin")
    else:
        for k in ("queued_target", "queued_origin", "queued_target_raw",
                  "queued_target_normalized"):
            out.pop(k, None)
    if eng.get("web") and "web_source_uid" in eng:
        out["web_source_uid"] = eng["web_source_uid"]
    elif not eng.get("web"):
        out.pop("web_source_uid", None)
    out["max_hp"] = max(int(out.get("max_hp", 0) or 0), int(out.get("hp", 0) or 0), 1)
    return out


def _merge_tile(raw: dict | None, eng: dict, frozen_pawn: bool) -> dict:
    out = dict(raw) if isinstance(raw, dict) else {"x": eng["x"], "y": eng["y"]}
    for k in _TILE_ENGINE_FIELDS:
        out[k] = eng[k]
    out.pop("has_pod", None)
    if eng.get("lava") or "lava" in out:
        out["lava"] = bool(eng.get("lava"))
    if "building_hp" in eng:
        out["building_hp"] = int(eng["building_hp"])
    elif "building_hp" in out:
        out["building_hp"] = 0
    if eng.get("terrain") == "building" and eng.get("populated") is False:
        out["population"] = 0
    out["frozen"] = bool(eng.get("frozen")) or frozen_pawn
    if not eng.get("item"):
        for k in ("item", "freeze_mine", "old_earth_mine", "repair_platform"):
            if k in out:
                out[k] = "" if k == "item" else False
    if eng.get("objective_name") and not out.get("objective_name"):
        out["objective_name"] = eng["objective_name"]
        out["unique_building"] = True
    return out


def bridge_board(engine_board: dict, bridge_data: dict) -> dict:
    """An engine board as a bridge-state dict, keeping the input's metadata
    (pilots, loadouts, objective names, conveyors, mines) for what the engine
    leaves unchanged."""
    out = {k: v for k, v in bridge_data.items() if k not in _STALE_TOP_LEVEL}
    for k in ("grid_power", "grid_power_max", "turn", "total_turns", "spawning_tiles"):
        if k in engine_board:
            out[k] = engine_board[k]
    raw_units: dict[int, list[dict]] = {}
    for u in bridge_data.get("units") or []:
        if isinstance(u, dict) and isinstance(u.get("uid"), int):
            raw_units.setdefault(u["uid"], []).append(u)
    units = []
    frozen_tiles = set()
    for eu in engine_board.get("units") or []:
        if eu.get("off_board") or eu.get("fallen"):
            continue
        raws = raw_units.get(int(eu["uid"]), [])
        primary = next((r for r in raws if not r.get("is_extra_tile")), None)
        if primary is not None and (primary.get("type") != eu.get("type")):
            primary = None
        if int(eu.get("hp", 0)) <= 0 and not (eu.get("mech") and eu.get("team") == 1):
            continue  # dead bodies the game removes (mech wrecks stay)
        merged = _merge_unit(primary, eu)
        units.append(merged)
        if merged.get("frozen"):
            frozen_tiles.add((merged["x"], merged["y"]))
        # Multi-tile pawns: the engine keeps the main tile; keep the extra
        # tiles while the pawn stands where it stood.
        if primary is not None and (primary.get("x"), primary.get("y")) == (eu["x"], eu["y"]):
            for r in raws:
                if r.get("is_extra_tile"):
                    extra = dict(r)
                    extra["hp"] = merged["hp"]
                    units.append(extra)
    out["units"] = units
    raw_tiles = {
        (t.get("x"), t.get("y")): t for t in (bridge_data.get("tiles") or []) if isinstance(t, dict)
    }
    out["tiles"] = [
        _merge_tile(raw_tiles.get((t["x"], t["y"])), t, (t["x"], t["y"]) in frozen_tiles)
        for t in engine_board.get("tiles") or []
    ]
    return out


# ── Predicted states for verify_action ──────────────────────────────────────

_TILE_COMPARE = ("terrain", "building_hp", "fire", "acid", "smoke", "shield", "pod",
                 "repair_platform", "item")
_UNIT_COMPARE = ("x", "y", "hp", "active", "fire", "acid", "frozen", "shield", "web",
                 "boosted", "has_queued_attack", "queued_target")


def _changed_tiles(before: dict, after: dict) -> set[tuple[int, int]]:
    b = {(t["x"], t["y"]): t for t in before.get("tiles") or []}
    changed = set()
    for t in after.get("tiles") or []:
        p = (t["x"], t["y"])
        o = b.get(p, {})
        if any(o.get(k) != t.get(k) for k in _TILE_COMPARE):
            changed.add(p)
    bu = {u["uid"]: u for u in before.get("units") or [] if not u.get("is_extra_tile")}
    au = {u["uid"]: u for u in after.get("units") or [] if not u.get("is_extra_tile")}
    for uid in set(bu) | set(au):
        o, n = bu.get(uid), au.get(uid)
        if o is None or n is None or any(o.get(k) != n.get(k) for k in _UNIT_COMPARE):
            for u in (o, n):
                if u is not None and 0 <= u.get("x", -1) < 8 and 0 <= u.get("y", -1) < 8:
                    changed.add((u["x"], u["y"]))
    return changed


def _snapshot(board_dict: dict, before_dict: dict, index: int, uid: int, phase: str,
              start_dict: dict) -> dict:
    from src.model.board import Board
    from src.solver.verify import snapshot_after_action, snapshot_after_move

    board = Board.from_bridge_data(board_dict)
    touched = _changed_tiles(before_dict, board_dict)
    fn = snapshot_after_move if phase == "move" else snapshot_after_action
    snap = fn(board, index, uid, [], extra_touched=touched)
    # Units the engine removed (dead Vek, fallen pawns) stay in the snapshot
    # as dead so verify only flags them if the game still shows them alive.
    present = {u["uid"] for u in snap["units"]}
    for u in start_dict.get("units") or []:
        if u.get("is_extra_tile") or u.get("uid") in present:
            continue
        snap["units"].append({
            "uid": u["uid"], "type": u.get("type"), "pos": [u.get("x"), u.get("y")],
            "hp": 0, "max_hp": u.get("max_hp", 1), "alive": False, "active": False,
            "is_mech": bool(u.get("mech")), "team": u.get("team"), "corpse": False,
            "corpse_on_death": False, "persistent_path_corpse": False,
            "queued_target": None, "queued_origin": None, "has_queued_attack": False,
            "status": {"fire": False, "acid": False, "frozen": False, "shield": False,
                       "web": False, "boosted": False},
        })
        present.add(u["uid"])
    before_uids = {u["uid"] for u in before_dict.get("units") or []}
    snap["unstable_spawn_uids"] = sorted(
        u["uid"] for u in board_dict.get("units") or []
        if u["uid"] not in before_uids and not u.get("is_extra_tile")
    )
    snap["predicted_by"] = "cpp"
    return snap


def _action_result(before: dict, after: dict) -> dict:
    """The old per-action counters, from the boards around one entry."""
    bu = {u["uid"]: u for u in before.get("units") or [] if not u.get("is_extra_tile")}
    au = {u["uid"]: u for u in after.get("units") or [] if not u.get("is_extra_tile")}
    enemies_killed = enemy_damage = mech_damage = mechs_killed = 0
    for uid, u in bu.items():
        n = au.get(uid)
        hp_after = int(n.get("hp", 0)) if n else 0
        loss = max(0, int(u.get("hp", 0)) - hp_after)
        if u.get("team") == 6 and int(u.get("hp", 0)) > 0:
            enemy_damage += loss
            if hp_after <= 0:
                enemies_killed += 1
        if u.get("mech") and u.get("team") == 1 and int(u.get("hp", 0)) > 0:
            mech_damage += loss
            if hp_after <= 0:
                mechs_killed += 1
    bt = {(t["x"], t["y"]): t for t in before.get("tiles") or []}
    buildings_damaged = buildings_lost = pods = 0
    for t in after.get("tiles") or []:
        o = bt.get((t["x"], t["y"]), {})
        if o.get("terrain") == "building" and int(o.get("building_hp", 0) or 0) > 0:
            new_hp = int(t.get("building_hp", 0) or 0) if t.get("terrain") == "building" else 0
            if new_hp < int(o.get("building_hp", 0)):
                buildings_damaged += 1
                if new_hp <= 0:
                    buildings_lost += 1
        if o.get("pod") and not t.get("pod"):
            if any((u.get("x"), u.get("y")) == (t["x"], t["y"]) and u.get("team") == 1
                   for u in after.get("units") or []):
                pods += 1
    return {
        "buildings_damaged": buildings_damaged,
        "buildings_lost": buildings_lost,
        "enemies_killed": enemies_killed,
        "mission_kills": enemies_killed,
        "enemy_damage_dealt": enemy_damage,
        "events": [],
        "grid_damage": max(0, int(before.get("grid_power", 0)) - int(after.get("grid_power", 0))),
        "mech_damage_taken": mech_damage,
        "mechs_killed": mechs_killed,
        "pods_collected": pods,
        "repair_platforms_used": 0,
        "spawns_blocked": 0,
    }


def _outcome(board: dict, start: dict, post_player: dict) -> dict:
    def buildings(b):
        return [t for t in b.get("tiles") or []
                if t.get("terrain") == "building" and int(t.get("building_hp", 0) or 0) > 0]
    alive_enemies = [u for u in board.get("units") or []
                     if u.get("team") == 6 and int(u.get("hp", 0)) > 0 and not u.get("is_extra_tile")]
    mechs = [u for u in board.get("units") or []
             if u.get("mech") and u.get("team") == 1 and not u.get("is_extra_tile")]
    start_enemies = {u["uid"] for u in start.get("units") or []
                     if u.get("team") == 6 and int(u.get("hp", 0)) > 0 and not u.get("is_extra_tile")}
    alive_ids = {u["uid"] for u in alive_enemies}
    kills = len(start_enemies - alive_ids)
    out = {
        "building_hp_total": sum(int(t.get("building_hp", 0)) for t in buildings(board)),
        "buildings_alive": len(buildings(board)),
        "buildings_destroyed_by_enemies": max(0, len(buildings(post_player)) - len(buildings(board))),
        "enemies_alive": len(alive_enemies),
        "enemy_hp_total": sum(int(u.get("hp", 0)) for u in alive_enemies),
        "grid_power": int(board.get("grid_power", 0)),
        "mech_hp": [{"hp": int(u.get("hp", 0)), "max_hp": int(u.get("max_hp", 0)),
                     "type": u.get("type"), "uid": u["uid"]} for u in mechs],
        "mechs_alive": sum(1 for u in mechs if int(u.get("hp", 0)) > 0),
        "mission_kills_total_projected": kills,
    }
    done = start.get("mission_kills_done")
    if isinstance(done, int) and not isinstance(done, bool) and done >= 0:
        out["mission_kills_done_projected"] = done + kills
    return out


def build_enriched(bridge_data: dict, sim: dict, entries: list[dict], spawns,
                   current_turn: int, total_turns: int, remaining_spawns: int,
                   weights=None) -> dict:
    """``replay_solution()``-shaped data from an engine simulation."""
    from src.model.board import Board
    from src.solver.evaluate import evaluate_breakdown

    start = bridge_board(sim["start_board"], bridge_data)
    prev = start
    predicted_states = []
    action_results = []
    last_index: dict[int, int] = {}
    for i, e in enumerate(entries):
        last_index[int(e["uid"])] = i
    # Units the bot has ended with SKIP (SetActive(false)) so far; the engine
    # leaves a unit that only moved active, the live game will not.
    skipped: set[int] = set()

    def _skips(board_dict: dict) -> dict:
        for u in board_dict.get("units") or []:
            if u.get("uid") in skipped:
                u["active"] = False
        return board_dict

    for i, step in enumerate(sim["steps"]):
        uid = int(entries[i]["uid"])
        entry: dict[str, Any] = {"post_move": None}
        if step.get("after_move"):
            moved = _skips(bridge_board(step["after_move"], bridge_data))
            entry["post_move"] = _snapshot(moved, prev, i, uid, "move", start)
            before_action = moved
        else:
            before_action = prev
        if entries[i].get("kind") == "none" and last_index.get(uid) == i:
            skipped.add(uid)  # the unit's last entry, without an action: SKIP
        after = _skips(bridge_board(step["after_action"], bridge_data))
        entry["post_attack"] = _snapshot(after, before_action, i, uid, "action", start)
        predicted_states.append(entry)
        action_results.append(_action_result(prev, after))
        prev = after
    post_player = _skips(bridge_board(sim["post_player_board"], bridge_data))
    final = bridge_board(sim["final_board"], bridge_data)
    if int(final.get("turn", current_turn) or 0) <= int(current_turn or 0):
        final["turn"] = int(current_turn or 0) + 1
    predicted_outcome = _outcome(final, start, post_player)
    final_board = Board.from_bridge_data(final)
    predicted_outcome["bigbomb_alive"] = bool(getattr(final_board, "bigbomb_alive", False))
    kills = predicted_outcome["mission_kills_total_projected"]
    score_breakdown = evaluate_breakdown(
        final_board, spawns, kills=kills, mission_kills=kills,
        current_turn=current_turn, total_turns=total_turns,
        remaining_spawns=remaining_spawns, weights=weights,
    )
    return {
        "action_results": action_results,
        "predicted_states": predicted_states,
        "predicted_outcome": predicted_outcome,
        "post_player_board": post_player,
        "final_board": final,
        "score_breakdown": score_breakdown,
        "replay_annotations": [],
        "predicted_by": "cpp",
    }


# ── Plan -> bot actions ─────────────────────────────────────────────────────


def to_mech_actions(entries: list[dict], sim: dict, bridge_data: dict) -> list[MechAction]:
    types = {u.get("uid"): u.get("type", "") for u in bridge_data.get("units") or []
             if isinstance(u, dict)}
    actions = []
    for e, step in zip(entries, sim["steps"]):
        uid = int(e["uid"])
        pos_before = _pt(step.get("pos_before"))
        move = _pt(e.get("move"))
        acts_from = move if move is not None else pos_before
        kind = e.get("kind", "none")
        target = _pt(e.get("target"))
        target2 = _pt(e.get("target2"))
        mech_type = types.get(uid) or ""
        if kind == "weapon":
            weapon, tgt = e.get("weapon", ""), target
        elif kind == "repair":
            weapon, tgt, target2 = REPAIR_WEAPON, acts_from, None
        else:
            weapon, tgt, target2 = MOVE_ONLY_WEAPON, NO_TARGET, None
        actions.append(MechAction(
            mech_uid=uid,
            mech_type=mech_type,
            move_to=acts_from if acts_from is not None else NO_TARGET,
            weapon=weapon,
            target=tgt if tgt is not None else NO_TARGET,
            target2=target2,
            description=_describe(mech_type, pos_before, move, kind,
                                  e.get("weapon", ""), target, target2),
        ))
    return actions


def unsupported_reason(entries: list[dict], sim: dict) -> str | None:
    """Plans the bridge's command set cannot express."""
    for e, step in zip(entries, sim["steps"]):
        if e.get("kind") == "repair":
            here = _pt(step.get("pos_after")) or _pt(e.get("move")) or _pt(step.get("pos_before"))
            target = _pt(e.get("target"))
            if target is not None and here is not None and target != here:
                return f"repair aimed at {visual(target)}, the bridge repairs in place"
            if e.get("weapon") not in ("", "Skill_Repair", None):
                return f"repair skill {e.get('weapon')} (the bridge emulates Skill_Repair only)"
    return None


# ── Solve ───────────────────────────────────────────────────────────────────


@dataclass
class CppSolveOutcome:
    ok: bool
    reason: str = ""
    solution: Solution | None = None
    enriched: dict | None = None
    meta: dict = field(default_factory=dict)


def solve(bridge_data: dict, time_limit: float, *, spawns=None, current_turn: int = 0,
          total_turns: int = 5, remaining_spawns: int = 2**31 - 1, weights=None,
          active_mech_count: int = 0, threads: int | None = None,
          allow_interleaving: bool | None = None) -> CppSolveOutcome:
    """Solve the bridge state with the engine; ``ok=False`` means fall back.

    ``time_limit`` is the whole budget: engine loading counts against it.
    """
    t0 = time.time()
    meta: dict[str, Any] = {"solver": "cpp"}
    try:
        module = import_engine_module()
        meta["engine_version"] = getattr(module, "ENGINE_VERSION", "unknown")
        meta["engine_build"] = getattr(module, "BUILD_GIT", "unknown")
        stale = module_staleness(module)
        if stale:
            meta["module_stale"] = stale
            print(f"  WARNING: {stale}", file=sys.stderr)
        difficulty = bridge_data.get("difficulty")
        if not isinstance(difficulty, int) or isinstance(difficulty, bool) or difficulty < 0:
            difficulty = 0
        engine = get_engine(threads, difficulty)
        meta["game_root"] = engine.game_root
        meta["threads"] = engine.threads
        load_s = time.time() - t0
        meta["engine_load_s"] = round(load_s, 3)
        bridge_json = json.dumps(bridge_data)
        budget = max(1.0, float(time_limit) - load_s - 0.5)
        raw = json.loads(engine.solve(bridge_json, json.dumps({
            "time_limit": budget, "threads": engine.threads, **_tier_options(),
        })))
    except Exception as exc:  # engine missing, bad board, engine error
        meta["error"] = f"{type(exc).__name__}: {exc}"
        return CppSolveOutcome(False, f"cpp solver error: {exc}", meta=meta)

    meta.update({
        "search_time_limit": round(budget, 3),
        "worst_case": raw.get("worst_case"),
        "worst_case_vector": raw.get("worst_case_vector"),
        "upper_bound": raw.get("upper_bound"),
        "proven_optimal": bool(raw.get("proven_optimal")),
        "proven_components": raw.get("proven_components"),
        "chance_exact": raw.get("chance_exact"),
        "timed_out": raw.get("timed_out"),
        "contingent": raw.get("contingent"),
        "warnings": raw.get("warnings") or [],
        "load_warnings": raw.get("load_warnings") or [],
        "stats": raw.get("stats") or {},
        "engine_plan": raw.get("plan") or [],
    })
    if raw.get("error"):
        return CppSolveOutcome(False, f"cpp solver: {raw['error']}", meta=meta)
    plan = [dict(a) for a in raw.get("plan") or []]
    if not plan:
        return CppSolveOutcome(False, "cpp solver returned no plan", meta=meta)
    sim = raw.get("simulation") or {}

    try:
        plan, sim = _executable_plan(engine, bridge_json, plan, sim, meta, allow_interleaving)
    except Exception as exc:
        meta["error"] = f"{type(exc).__name__}: {exc}"
        return CppSolveOutcome(False, f"cpp plan post-processing failed: {exc}", meta=meta)
    if sim.get("refused", -1) < 0:
        try:
            plan, sim = _end_idle_units(engine, bridge_json, bridge_data, plan, sim, meta)
        except Exception as exc:
            meta["error"] = f"{type(exc).__name__}: {exc}"
            return CppSolveOutcome(False, f"cpp plan post-processing failed: {exc}", meta=meta)
    if sim.get("refused", -1) >= 0:
        return CppSolveOutcome(
            False, f"engine refused its own plan at entry {sim['refused']}: "
                   f"{sim.get('refused_reason')}", meta=meta)
    why = unsupported_reason(plan, sim)
    if why:
        return CppSolveOutcome(False, f"plan not executable by the bridge: {why}", meta=meta)

    meta["executed_plan"] = plan
    meta["enemy_phase"] = {k: v for k, v in (sim.get("enemy_phase") or {}).items() if k != "events"}
    meta["simulated_score"] = sim.get("score")
    actions = to_mech_actions(plan, sim, bridge_data)
    worst = meta.get("executed_worst_case") or meta.get("worst_case") or {}
    solution = Solution(
        actions=actions,
        score=score_scalar(worst),
        elapsed_seconds=time.time() - t0,
        timed_out=not meta["proven_optimal"],
        permutations_tried=int((meta["stats"] or {}).get("nodes", 0) or 0),
        total_permutations=0,
        active_mech_count=active_mech_count,
    )
    try:
        enriched = build_enriched(
            bridge_data, sim, plan, list(spawns or []), current_turn, total_turns,
            remaining_spawns, weights,
        )
    except Exception as exc:
        meta["error"] = f"{type(exc).__name__}: {exc}"
        return CppSolveOutcome(False, f"cpp predicted-state build failed: {exc}", meta=meta)
    meta["elapsed_s"] = round(time.time() - t0, 3)
    return CppSolveOutcome(True, solution=solution, enriched=enriched, meta=meta)


def _executable_plan(engine, bridge_json: str, plan: list[dict], sim: dict, meta: dict,
                     allow_interleaving: bool | None) -> tuple[list[dict], dict]:
    """Adapt the engine's plan to what the bot executes (see module docstring)."""
    if allow_interleaving is None:
        allow_interleaving = os.environ.get("ITB_CPP_INTERLEAVE", "auto") != "never"
    original = meta.get("worst_case_vector")
    changed = False

    bad = plan_entries_after_unit_action(plan)
    if bad:
        dropped = [plan[i] for i in bad]
        plan = [a for i, a in enumerate(plan) if i not in set(bad)]
        meta["dropped_after_unit_action"] = dropped
        changed = True

    if is_interleaved(plan):
        regrouped = regroup_by_unit(plan)
        ev = json.loads(engine.evaluate(bridge_json, _plan_payload(regrouped), json.dumps(_tier_options())))
        cur = json.loads(engine.evaluate(bridge_json, _plan_payload(plan), json.dumps(_tier_options())))
        same = ev.get("ok") and cur.get("ok") and \
            ev.get("worst_case_vector") == cur.get("worst_case_vector")
        meta["interleaved"] = True
        meta["regroup"] = {"equivalent": bool(same),
                           "regrouped_worst_case": ev.get("worst_case"),
                           "interleaved_worst_case": cur.get("worst_case")}
        if same or not allow_interleaving:
            if not same:
                meta["regroup"]["forced"] = True
            plan = regrouped
            changed = True
    else:
        meta["interleaved"] = False

    if changed:
        sim = json.loads(engine.simulate(bridge_json, _plan_payload(plan)))
        ev = json.loads(engine.evaluate(bridge_json, _plan_payload(plan), json.dumps(_tier_options())))
        meta["executed_worst_case"] = ev.get("worst_case")
        meta["executed_worst_case_vector"] = ev.get("worst_case_vector")
        if ev.get("worst_case_vector") != original:
            meta.setdefault("warnings", []).append(
                "executed plan's worst case differs from the solver's: "
                f"{ev.get('worst_case')} vs {meta.get('worst_case')}")
    # Entries carry their description from the engine; keep them aligned
    # with the simulated steps.
    for e, step in zip(plan, sim.get("steps") or []):
        e["description"] = (step.get("action") or {}).get("description", e.get("description"))
    return plan, sim


def idle_units(bridge_data: dict, plan: list[dict]) -> list[int]:
    """Active player units the plan never uses (the bot's End Turn gate counts
    them as actions left: ``_active_player_action_count``)."""
    from src.model.board import Board

    board = Board.from_bridge_data(bridge_data)
    used = {int(a["uid"]) for a in plan}
    out = []
    for u in board.mechs():
        if getattr(u, "is_extra_tile", False) or u.uid in used or u.uid in out:
            continue
        if u.active and u.hp > 0 and (u.is_mech or u.weapon or u.weapon2):
            out.append(u.uid)
    return out


def _end_idle_units(engine, bridge_json: str, bridge_data: dict, plan: list[dict],
                    sim: dict, meta: dict) -> tuple[list[dict], dict]:
    """Give every unused active unit a final no-op entry: the bot SKIPs it, so
    End Turn finds no actions left. SetActive(false) does not change the turn."""
    idle = idle_units(bridge_data, plan)
    if not idle:
        return plan, sim
    meta["idle_units_skipped"] = idle
    plan = plan + [{"uid": uid, "move": None, "kind": "none", "weapon": "",
                    "target": None, "target2": None} for uid in idle]
    sim = json.loads(engine.simulate(bridge_json, _plan_payload(plan)))
    for e, step in zip(plan, sim.get("steps") or []):
        e["description"] = (step.get("action") or {}).get("description", e.get("description"))
    return plan, sim


# ── Offline ─────────────────────────────────────────────────────────────────


def load_bridge_file(path: str | Path) -> dict:
    """A recording (``data.bridge_state``) or a bare bridge-state JSON file."""
    data = json.loads(Path(path).read_text())
    if isinstance(data, dict) and isinstance(data.get("data"), dict) \
            and isinstance(data["data"].get("bridge_state"), dict):
        return data["data"]["bridge_state"]
    return data


def solve_file_report(path: str | Path, time_limit: float = 10.0,
                      threads: int | None = None) -> dict:
    """Solve a recorded board offline and summarize (``game_loop.py solve_cpp --file``)."""
    bridge = load_bridge_file(path)
    out = solve(
        bridge, time_limit,
        spawns=[tuple(s) for s in bridge.get("spawning_tiles", [])],
        current_turn=bridge.get("turn", 0),
        total_turns=bridge.get("total_turns", 5),
        remaining_spawns=bridge.get("remaining_spawns", 2**31 - 1),
        threads=threads,
    )
    meta = out.meta
    report = {
        "ok": out.ok,
        "reason": out.reason,
        "engine_version": meta.get("engine_version"),
        "proven_optimal": meta.get("proven_optimal"),
        "worst_case": meta.get("executed_worst_case") or meta.get("worst_case"),
        "upper_bound": meta.get("upper_bound"),
        "proven_components": meta.get("proven_components"),
        "interleaved": meta.get("interleaved"),
        "warnings": meta.get("warnings"),
        "timing": {k: meta.get(k) for k in ("engine_load_s", "search_time_limit", "elapsed_s")},
    }
    if out.ok:
        report["actions"] = [a.description for a in out.solution.actions]
        report["predicted_grid_power"] = out.enriched["predicted_outcome"].get("grid_power")
    return report


if __name__ == "__main__":  # python3 -m src.solver.cpp_solver <file> [time]
    print(json.dumps(solve_file_report(
        sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 10.0), indent=2))
