#!/usr/bin/env python3
"""What the live recordings and live fuzz sessions have exercised, against
everything the game has.

    python3 scripts/coverage_report.py                  # recordings/live/* and recordings/fuzz/*
    python3 scripts/coverage_report.py --unseen         # only what was never seen
    python3 scripts/coverage_report.py --json out.json

Seen (live): from every turn start live play recorded (recordings/live/<run>/
m*_turn_*_solve_input.json and the 2026-10-09 session's states): the mechs'
exact weapons (upgrades included) and passives, pilots and their level-up
skills, mission types and enemy types; "fired" also counts the weapons the
executed plans used (m*_turn_*_solve.json). Seen (fuzz): the same from the
turn starts of live fuzz sessions (recordings/fuzz/<session>/s<NNN>/, dry
runs left out; scripts/fuzz_play.py, loadouts it injected included), fired
from each sample's plan up to its last executed step. The full sets come
from the game's scripts (ITB_GAME_DIR, else the engine's local copy):
weapon tables of the player's weapon files, CreatePilot ids, mission
tables, and the enemy-team pawns as the engine loads them (itb_inspect
--pawns); the weapons a run can hold (shop and pod lists, squads' starting
weapons, upgrades the weapon has: scripts/loadout.py) apart from the rest.
Only names are printed, never script content. Use it to pick squads,
pilots and shop items (or fuzz_play.py --coverage loadouts) that exercise
what the engine has never been checked on.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
LIVE_ROOT = REPO / "recordings" / "live"
FUZZ_ROOT = REPO / "recordings" / "fuzz"
DEFAULT_GAME = REPO / ".local_decompile" / "builds" / "linux_21601364"
INSPECT = REPO / "engine" / "build" / "itb_inspect"

# The player's weapon files (shop, squads, AE) and the name prefixes of
# player weapons in them (TechnoVek squad weapons are Vek_*).
WEAPON_FILES = ("weapons_prime.lua", "weapons_brute.lua", "weapons_ranged.lua", "weapons_science.lua",
                "weapons_support.lua", "weapons_passive.lua", "weapons_deploy.lua", "weapons_technovek.lua",
                "advanced/ae_weapons.lua")
PLAYER_PREFIXES = ("Prime_", "Brute_", "Ranged_", "Science_", "Support_", "Passive_", "DeploySkill_", "Vek_")
UPGRADE = re.compile(r"_(A|B|AB)$")
# Tables that are not weapons a player carries (tooltip demos) or missions
# a run plays (test, tutorial).
NOT_PLAYED = re.compile(r"(_Tip|Tooltip)(_[AB]+)?$|^Mission_(Test|Tutorial)$")
TABLE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([A-Za-z_][A-Za-z0-9_]*)\s*:\s*new\b", re.M)
PILOT = re.compile(r"CreatePilot\s*\{[^}]*?\bId\s*=\s*\"([^\"]+)\"", re.S)
# Pilot level-up skills by id (engine board.hpp LevelSkill).
LEVEL_SKILLS = ("Health", "Move", "Grid", "Reactor", "Opener", "Closer", "Popular", "Thick", "Skilled",
                "Invulnerable", "Adrenaline", "Pain", "Regen", "Conservative")
KINDS = ("weapons", "fired", "pilots", "skills", "missions", "enemies", "mechs")
STEP_RE = re.compile(r"m\d+_turn_(\d+)_step(\d+)_\d+\.json$")


def game_scripts(arg: str | None) -> Path:
    root = Path(arg or os.environ.get("ITB_GAME_DIR") or DEFAULT_GAME)
    return root / "scripts" if (root / "scripts").is_dir() else root


def tables(path: Path) -> list:
    try:
        text = path.read_text(errors="replace")
    except OSError:
        return []
    return TABLE.findall(text)


def all_weapons(scripts: Path) -> dict:
    """{weapon table: file} for the player's weapons, upgrades included."""
    out = {}
    for name in WEAPON_FILES:
        for table, _parent in tables(scripts / name):
            if table.startswith(PLAYER_PREFIXES) and not NOT_PLAYED.search(table):
                out.setdefault(table, name)
    return out


def all_pilots(scripts: Path) -> list:
    ids = []
    for name in ("pilots.lua", "advanced/ae_pilots.lua"):
        try:
            ids += PILOT.findall((scripts / name).read_text(errors="replace"))
        except OSError:
            pass
    return sorted(set(ids))


def all_missions(scripts: Path) -> tuple[list, set]:
    """(mission tables, those other missions derive from)."""
    names, parents = set(), set()
    for d in (scripts / "missions", scripts / "advanced" / "missions"):
        for p in sorted(d.rglob("*.lua")) if d.is_dir() else []:
            for table, parent in tables(p):
                if table.startswith("Mission_") and not NOT_PLAYED.search(table):
                    names.add(table)
                    parents.add(parent)
    return sorted(names), parents & names


def all_enemies(game_root: Path) -> list:
    """Enemy-team pawn types as the engine loads them (itb_inspect --pawns:
    team 6); [] without a build."""
    if not INSPECT.exists():
        return []
    env = dict(os.environ, ITB_GAME_DIR=str(game_root))
    try:
        out = subprocess.run([str(INSPECT), "--pawns"], capture_output=True, text=True, env=env, timeout=300).stdout
    except (OSError, subprocess.TimeoutExpired):
        return []
    found = []
    for line in out.splitlines():
        m = re.match(r"^(\S+)\s+hp\s+-?\d+\s+move\s+-?\d+\s+team\s+(-?\d+)", line)
        if m and m.group(2) == "6":
            found.append(m.group(1))
    return sorted(set(found))


def obtainable(scripts: Path) -> set | None:
    """Exact weapon ids a run can hold (loadout.Items), None if unknown."""
    try:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import loadout
        return set(loadout.Items(scripts).all_weapon_ids())
    except Exception:  # noqa: BLE001  (a report: go on without the split)
        return None


def bridge_state(path: Path) -> dict | None:
    try:
        d = json.loads(path.read_text())
    except (OSError, ValueError):
        return None
    if isinstance(d, dict) and isinstance(d.get("data"), dict):
        d = d["data"].get("bridge_state", d["data"])
    return d if isinstance(d, dict) and "units" in d else None


def count_starts(files: list, out: dict) -> None:
    """Adds the turn starts in `files` to the Counters of `out`."""
    for p in files:
        st = bridge_state(p)
        if st is None:
            continue
        out["turn_starts"] += 1
        out["missions"][st.get("mission_id") or "?"] += 1
        for u in st.get("units") or []:
            if u.get("team") == 1 and u.get("mech"):
                out["mechs"][u.get("type") or "?"] += 1
                for w in u.get("weapons_exact") or u.get("weapons") or []:
                    out["weapons"][w] += 1
                pilot = u.get("pilot") if isinstance(u.get("pilot"), dict) else {}
                pid = u.get("pilot_id") or pilot.get("id")
                if pid:
                    out["pilots"][pid] += 1
                level = pilot.get("level", u.get("pilot_level"))
                for n, key in ((1, "skill1"), (2, "skill2")):
                    sid = pilot.get(key)
                    if isinstance(sid, int) and isinstance(level, int) and level >= n and 0 <= sid < len(LEVEL_SKILLS):
                        out["skills"][LEVEL_SKILLS[sid]] += 1
            elif u.get("team") == 6:
                out["enemies"][u.get("type") or "?"] += 1


def empty() -> dict:
    out = {k: Counter() for k in KINDS}
    out["turn_starts"] = 0
    return out


def seen(root: Path) -> dict:
    """What the live recordings hold: Counters of turn starts by name."""
    out = empty()
    count_starts(sorted(root.glob("*/m*_turn_*_solve_input.json")) + sorted((root / "2026-10-09").glob("*_start_*.json")),
                 out)
    for p in sorted(root.glob("*/m*_turn_*_solve.json")):
        try:
            d = json.loads(p.read_text())
        except (OSError, ValueError):
            continue
        for a in (d.get("data") or {}).get("actions") or []:
            w = a.get("weapon_id")
            if w and w != "_REPAIR":
                out["fired"][w] += 1
    return out


def live_sessions(root: Path) -> list:
    """The fuzz sessions under root that played the game (not dry runs)."""
    out = []
    for d in sorted(p for p in root.iterdir() if p.is_dir()) if root.is_dir() else []:
        try:
            meta = json.loads((d / "session.json").read_text())
        except (OSError, ValueError):
            continue
        if not meta.get("dry_run", True):
            out.append(d)
    return out


def seen_fuzz(root: Path) -> dict:
    """What live fuzz sessions played: their samples' turn starts, and the
    weapons of each sample's plan up to its last executed step."""
    out = empty()
    for sess in live_sessions(root):
        for sample in sorted(sess.glob("s[0-9]*")):
            if not sample.is_dir():
                continue
            count_starts(sorted(sample.glob("m*_turn_*_solve_input.json")), out)
            steps = [STEP_RE.search(p.name) for p in sample.glob("m*_turn_*_step*_*.json")]
            steps = [(int(m.group(1)), int(m.group(2))) for m in steps if m]
            try:
                plan = json.loads((sample / "plan.json").read_text())
            except (OSError, ValueError):
                continue
            if not steps or not isinstance(plan, list):
                continue
            last = max(t for t, _ in steps)
            done = max(n for t, n in steps if t == last)
            for a in plan[:done]:
                if isinstance(a, dict) and a.get("kind") == "weapon" and a.get("weapon"):
                    out["fired"][a["weapon"]] += 1
    return out


def section(title: str, full: list, live: Counter, fuzz: Counter, unseen_only: bool,
            fired: tuple | None = None, holdable: set | None = None) -> list:
    """One item kind: seen live, in fuzz sessions, in either; with
    `holdable`, the totals over what a run can hold, the rest listed apart."""
    either = lambda n: live.get(n) or fuzz.get(n)  # noqa: E731
    pool = [n for n in full if holdable is None or n in holdable]
    head = (f"{title}: {sum(1 for n in pool if live.get(n))}/{len(pool)} seen live, "
            f"{sum(1 for n in pool if fuzz.get(n))} in fuzz sessions, {sum(1 for n in pool if either(n))} in either")
    if holdable is not None:
        head += f" (of those a run can hold; {len(full)} tables in all)"
    lines = [head]
    if not unseen_only:
        lines.append("    live    fuzz" + ("  (fired live/fuzz)" if fired else ""))
        for n in sorted((n for n in full if either(n)), key=lambda n: (-(live[n] + fuzz[n]), n)):
            lines.append(f"  {live[n]:6d}  {fuzz[n]:6d}  {n}" + (f"  (fired {fired[0][n]}/{fired[1][n]})" if fired else "")
                         + ("  [not holdable]" if holdable is not None and n not in holdable else ""))
    missing = [n for n in pool if not either(n)]
    if missing:
        lines.append(f"  never seen ({len(missing)}): " + ", ".join(missing))
    if holdable is not None:
        apart = [n for n in full if n not in holdable and not either(n)]
        if apart:
            lines.append(f"  not holdable in a run, never seen ({len(apart)}): " + ", ".join(apart))
    other = sorted(n for n in set(live) | set(fuzz) if n not in set(full))
    if other and not unseen_only:
        lines.append("  seen, not in the game's list: " + ", ".join(other))
    return lines


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--recordings", default=str(LIVE_ROOT), help="live_play run directories (default %(default)s)")
    ap.add_argument("--fuzz", default=str(FUZZ_ROOT), help="fuzz_play session directories (default %(default)s)")
    ap.add_argument("--game", help="game install (default: ITB_GAME_DIR, else the engine's local copy)")
    ap.add_argument("--unseen", action="store_true", help="only list what was never seen")
    ap.add_argument("--json", help="also write the report as JSON")
    args = ap.parse_args(argv)
    scripts = game_scripts(args.game)
    if not (scripts / "scripts.lua").exists():
        print(f"no game scripts at {scripts} (set ITB_GAME_DIR or --game)", file=sys.stderr)
        return 2
    s = seen(Path(args.recordings))
    f = seen_fuzz(Path(args.fuzz))
    weapons = all_weapons(scripts)
    holdable = obtainable(scripts)
    passives = sorted(w for w in weapons if w.startswith("Passive_"))
    actives = sorted(w for w in weapons if not w.startswith("Passive_"))
    bases = sorted(w for w in actives if not UPGRADE.search(w))
    upgrades = sorted(w for w in actives if UPGRADE.search(w))
    missions, parents = all_missions(scripts)
    enemies = all_enemies(scripts.parent)
    pilots = all_pilots(scripts)
    fired = (s["fired"], f["fired"])
    lines = [f"{s['turn_starts']} recorded turn starts under {args.recordings}, "
             f"{f['turn_starts']} in live fuzz sessions under {args.fuzz}", ""]
    lines += section("player weapons (base)", bases, s["weapons"], f["weapons"], args.unseen, fired, holdable) + [""]
    lines += section("weapon upgrades", upgrades, s["weapons"], f["weapons"], args.unseen, fired, holdable) + [""]
    never_fired = [w for w in actives if (s["weapons"].get(w) or f["weapons"].get(w))
                   and not (s["fired"].get(w) or f["fired"].get(w))]
    if never_fired:
        lines += [f"carried but never fired ({len(never_fired)}): " + ", ".join(never_fired), ""]
    lines += section("passives", passives, s["weapons"], f["weapons"], args.unseen, None, holdable) + [""]
    lines += section("pilots", pilots, s["pilots"], f["pilots"], args.unseen) + [""]
    lines += section("pilot level-up skills", list(LEVEL_SKILLS), s["skills"], f["skills"], args.unseen) + [""]
    lines += section("missions", [m for m in missions if m not in parents or s["missions"].get(m)
                                  or f["missions"].get(m)], s["missions"], f["missions"], args.unseen) + [""]
    if enemies:
        lines += section("enemy types", enemies, s["enemies"], f["enemies"], args.unseen)
    else:
        lines.append("enemy types: no engine build (engine/build/itb_inspect) to list them")
    print("\n".join(lines))
    if args.json:
        report = {"turn_starts": s["turn_starts"], "fuzz_turn_starts": f["turn_starts"],
                  "seen": {k: dict(v) for k, v in s.items() if isinstance(v, Counter)},
                  "seen_fuzz": {k: dict(v) for k, v in f.items() if isinstance(v, Counter)},
                  "all": {"weapons": bases, "upgrades": upgrades, "passives": passives, "pilots": pilots,
                          "skills": list(LEVEL_SKILLS), "missions": missions, "enemies": enemies,
                          "holdable": sorted(holdable) if holdable is not None else None}}
        Path(args.json).write_text(json.dumps(report, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
