#!/usr/bin/env python3
"""Loadout evaluator: what a shop purchase or a reactor core would have
been worth, measured by the engine on boards the run really played.

    python3 scripts/loadout_eval.py --candidate '1:*,Brute_Railgun' --candidate 'hp:2' --candidate 'move:0'
    python3 scripts/loadout_eval.py --run 20261018_083852 --last 12 --time 8 --threads 2 \\
        --candidate '2:Science_SwapOther,*' --candidate 'hp:2+move:2'
    python3 scripts/loadout_eval.py --last 12 --candidate 'power:1' --candidate 'skills:2=Grid,Move@1'

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
the expected grid each saves (Grid Defense, below) and ranks the
candidates by them (The ranking, below).

Candidates (joined with '+'):
  <loadout.py spec>   '1:*,Brute_Railgun', '0:Prime_Punchmech_A,*',
                      '2:pilot=Pilot_Rock' (weapons the squad's storage holds
                      may be given: they come out of storage)
  hp:<mech>           the mech's +2 Health core (HP and max HP +2)
  move:<mech>         the mech's +1 Move core
  skills:<mech>=<A>,<B>[@<level>]
                      the mech's pilot with level-up skills A (learned at
                      level 1) and B (level 2), at that level (default: its
                      own); e.g. 'skills:1=Grid,Health@1' (a pilot of the
                      same XP with Grid). HP and move follow the skills;
                      the pilot's Blacklist holds. Goes after a pilot=
                      spec: '1:pilot=Pilot_Rock+skills:1=Grid,Move@2'.
  def:<N>             +N percentage points of Grid Defense (the overpower
                      bonus; it caps at 25)
  power:<N>           N Grid Power bought with the grid full: the overpower
                      bonus the newest turn's would get (+2 per point while
                      below 10, then +1, to 25; a purchase is 2 points on
                      Unfair)
'<mech>' is the squad position (the bridge uid of mechs 0-2).

Grid Defense. A populated building hit rolls to resist (one roll per
damaging hit; resisted: no HP or grid lost) with the game's Grid Defense
chance, PowerBar::GetDefense: a base (15, or 0 when the power bar was built
under Unfair; the bridge's grid_defense_base), plus the overpower bonus
(grid power gained past a full grid; the save's GameData.overflow, the
bridge's grid_overflow, 0-25), plus 3 for each squad pilot whose learned
level-up skills include Grid (skill1 from level 1, skill2 from level 2;
Pilot::GetDefBonus, summed by SquadControl::GetDefBonus). The engine's
loader adds them (load_grid_defense); a state may give grid_defense in
percent instead. The solver takes chance at its worst (no resist), so the
chance only reaches its ranking through grid_survival, on turns where every
line can lose the grid. Candidates that change it (def:, power:, a pilot
gaining or losing Grid) are written into the board as the engine reads it
(grid_overflow, or grid_defense when the state gives that; the pilot's
level and skills), and every solve is also priced by its expected grid loss:
the plan's rolls (its own shots and bumps on buildings, then the enemy
phase's, from the solve reply's no-resist prediction) each resisted with
that chance, independently, the rest of the turn as predicted, grid floored
at 0; grid lost otherwise (no roll) stays lost. "E[grid] saved" is the
baseline's expected loss minus the candidate's, summed over the turns: it
moves with the worst case (one hit fewer saves 1 - GD) and with the chance
(+3% on 2 resistible grid saves 0.06), so every candidate is priced alike.

The ranking. The tier sums are certain outcomes, the expected values are
not; a grid_survival change counts as certain only where a line surely
keeps the grid on one side (odds 1) and not on the other, otherwise its
odds go with the expected values. Proven turns first: the certain sums
above mech HP (sure grid survival, grid, building HP, mechs, objectives
failed), then the expected values (grid survival odds, then E[grid]
saved), then how many turns the solver itself ranked better minus worse,
then the rest of the sums; then the same over all turns. So a candidate
that only raises Grid Defense never outranks one that proves a grid,
building or mech gain, and beats "no change" and a safer next turn alone:
expected grid is grid-tier value, which the solver ranks above every
next-turn estimate.

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
# The live driver's ranking (scratchpad drive2.sh / live_play mission flags;
# live_play's --lookahead-fold defaults to worst_sum since 2026-10-19).
RANKING = dict(threat_tier=True, objectives_first=True, mech_grid=2, mech_grid_turns=True, mech_grid_pilot=True,
               lookahead=True, lookahead_fold="worst_sum", grid_survival=True, survival_cap=0.3)


class CandidateError(ValueError):
    pass


CONTROL = "(control: the baseline, 1.5x budget)"


# Grid Defense (PowerBar::GetDefense; module docstring).
GRID_SKILL = lo.LEVEL_SKILLS.index("Grid")
GRID_SKILL_BONUS = 3  # Pilot_GridDesc: "Grid Defense increased by 3"
OVERPOWER_MAX = 25  # TipText_BuyPower_Grid: "to a maximum of +25"
DEFAULT_GRID_DEFENSE = 15  # the engine's Board::grid_defense when a state has no Grid Defense
UNFAIR = 3


@dataclass
class Candidate:
    text: str
    specs: list = field(default_factory=list)  # loadout.MechSpec
    hp: list = field(default_factory=list)  # mech indices getting +2 Health
    move: list = field(default_factory=list)  # mech indices getting +1 Move
    skills: list = field(default_factory=list)  # (mech, skill1 id, skill2 id, level or None)
    defense: int = 0  # def:N, percentage points of overpower bonus
    power: int = 0  # power:N, Grid Power bought with the grid full
    pp: int = 0  # the overpower bonus it adds (resolve_defense: def:N + what power:N gives)

    def mechs(self) -> set:
        return {s.index for s in self.specs} | set(self.hp) | set(self.move) | {s[0] for s in self.skills}

    @property
    def control(self) -> bool:
        return self.text == CONTROL

    @property
    def grid_bonus(self) -> bool:
        return bool(self.defense or self.power)

    def cores(self) -> int:
        """Cores of the hp/move upgrades (weapon cores come from the plan)."""
        return len(self.hp) + len(self.move)


SKILL_IDS = {n.lower(): i for i, n in enumerate(lo.LEVEL_SKILLS)}


def parse_skills(part: str, text: str, m: re.Match) -> tuple:
    k, names = int(m.group(1)), (m.group(2), m.group(3))
    ids = [SKILL_IDS.get(n.lower()) for n in names]
    bad = [n for n, i in zip(names, ids) if i is None]
    if bad:
        raise CandidateError(f"{part} in {text!r}: no level-up skill {bad[0]!r} (one of {', '.join(lo.LEVEL_SKILLS)})")
    if ids[0] == ids[1]:
        raise CandidateError(f"{part} in {text!r}: a pilot's two level-up skills differ")
    level = None if m.group(4) is None else int(m.group(4))
    if level is not None and not 0 <= level <= 2:
        raise CandidateError(f"{part} in {text!r}: level 0-2")
    return (k, ids[0], ids[1], level)


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
        m = re.fullmatch(r"(def|power):(\d+)", part)
        if m:
            n, top = int(m.group(2)), OVERPOWER_MAX if m.group(1) == "def" else 2 * OVERPOWER_MAX
            if not 1 <= n <= top:
                raise CandidateError(f"{part} in {text!r}: 1-{top}")
            if m.group(1) == "def":
                c.defense += n
            else:
                c.power += n
            continue
        m = re.fullmatch(r"skills:(\d+)=(\w+),(\w+)(?:@(\d+))?", part)
        if m:
            sk = parse_skills(part, text, m)
            if any(s[0] == sk[0] for s in c.skills):
                raise CandidateError(f"mech {sk[0]} in {text!r}: two skills: parts")
            c.skills.append(sk)
            continue
        if part.startswith(("skills:", "def:", "power:")):
            raise CandidateError(f"cannot read {part!r} in {text!r} (want skills:<mech>=<A>,<B>[@<level>], "
                                 "def:<N> or power:<N>)")
        loadout.append(part)
    if loadout:
        try:
            c.specs = lo.parse_spec(";".join(loadout))
        except lo.LoadoutError as e:
            raise CandidateError(str(e)) from e
    if not (c.specs or c.hp or c.move or c.skills or c.grid_bonus):
        raise CandidateError(f"empty candidate {text!r}")
    for k in c.hp + c.move + [s[0] for s in c.skills]:
        if not 0 <= k <= 2:
            raise CandidateError(f"mech index {k} in {text!r}: 0-2")
    return c


# ------------------------------------------------------------------ Grid Defense

def pilot_of(u: dict) -> tuple:
    """(level, skill1, skill2) of a mech's pilot as the engine's loader
    reads them: the bridge's pilot record when there is one (a skill it
    lacks is -1), else pilot_level and pilot_skills ('skill1=2')."""
    p = u.get("pilot")
    if isinstance(p, dict):
        return int(p.get("level") or 0), p.get("skill1", -1), p.get("skill2", -1)
    s = {"skill1": -1, "skill2": -1}
    for t in u.get("pilot_skills") or []:
        m = re.fullmatch(r"(skill[12])=(-?\d+)", str(t))
        if m:
            s[m.group(1)] = int(m.group(2))
    return int(u.get("pilot_level") or 0), s["skill1"], s["skill2"]


def has_grid_skill(u: dict) -> bool:
    level, s1, s2 = pilot_of(u)
    return (level >= 1 and s1 == GRID_SKILL) or (level >= 2 and s2 == GRID_SKILL)


def grid_pilots(state: dict) -> int:
    return sum(has_grid_skill(u) for u in state.get("units", []) if u.get("mech") and u.get("team") == 1)


def difficulty(state: dict):
    return state.get("difficulty", (state.get("mission_state") or {}).get("difficulty"))


def grid_defense(state: dict) -> int | None:
    """The Grid Defense percent the engine reads from a state
    (load_grid_defense): grid_defense when given, else the base
    (grid_defense_base; without it 0 on Unfair, else 15) + grid_overflow
    + 3 per Grid pilot. None: neither field (the engine then takes
    DEFAULT_GRID_DEFENSE)."""
    if isinstance(state.get("grid_defense"), (int, float)):
        return max(0, min(100, int(state["grid_defense"])))
    ov = state.get("grid_overflow")
    if not isinstance(ov, (int, float)):
        return None
    base = state.get("grid_defense_base")
    base = max(0, int(base)) if isinstance(base, (int, float)) else (0 if difficulty(state) == UNFAIR else 15)
    return max(0, min(100, base + max(0, int(ov)) + GRID_SKILL_BONUS * grid_pilots(state)))


def overpower(bonus: int, points: int) -> int:
    """The overpower bonus after `points` grid power past a full grid
    (PowerBar::Modify): each point +2 while the bonus is below 10, else +1,
    none once it is 25."""
    for _ in range(max(0, points)):
        if bonus < OVERPOWER_MAX:
            bonus += 2 if bonus < 10 else 1
    return bonus


def fill_grid_overflow(state: dict, ts: TurnStart) -> bool:
    """A state without Grid Defense gets the overpower bonus from its save
    checkpoint (as live_play does for --grid-survival on older bridges).
    True when it changed."""
    if "grid_overflow" in state or "grid_defense" in state or ts.save is None:
        return False
    ov = lp.save_grid_overflow(ts.save)
    if ov is None:
        return False
    state["grid_overflow"] = ov
    return True


def resolve_defense(cand: Candidate, state: dict) -> str:
    """Set cand.pp, the overpower bonus a def:/power: candidate adds, on
    the decision's board (the newest turn start): power:N as the shop gives
    it there (a purchase is 2 points on Unfair), def:N as given; refused
    past the cap of 25. Returns a note."""
    ov = state.get("grid_overflow")
    if not isinstance(ov, (int, float)):
        raise CandidateError("the newest turn start has no overpower bonus (grid_overflow) to add to")
    ov, gain = int(ov), 0
    if cand.power:
        gain = overpower(ov, cand.power * (2 if difficulty(state) == UNFAIR else 1)) - ov
        if gain <= 0:
            raise CandidateError(f"the overpower bonus is at its maximum ({ov}): no Grid Power to buy")
    if cand.defense and ov + gain + cand.defense > OVERPOWER_MAX:
        raise CandidateError(f"the overpower bonus caps at {OVERPOWER_MAX}: {ov} now"
                             + (f", {ov + gain} after power:{cand.power}" if gain else "")
                             + f", so def: at most {OVERPOWER_MAX - ov - gain}")
    cand.pp = gain + cand.defense
    return f"overpower bonus {ov} -> {ov + cand.pp} (+{cand.pp}% Grid Defense)"


def expected_lost(g0: int, rolls: list, lost: int, p: float) -> float:
    """Expected grid lost: the predicted path's rolls (grid at stake each)
    resisted independently with chance p, the grid floored at 0, the rest
    of the turn as predicted; what the path lost besides its rolls (lost
    minus what the rolls took) applied after them."""
    g = g0
    for a in rolls:
        g = max(0, g - a)
    other = lost - (g0 - g)
    dist = {g0: 1.0}
    for a in rolls:
        nxt: dict = {}
        for v, w in dist.items():
            nxt[v] = nxt.get(v, 0.0) + w * p
            nxt[max(0, v - a)] = nxt.get(max(0, v - a), 0.0) + w * (1 - p)
        dist = nxt
    return g0 - sum(w * max(0, v - other) for v, w in dist.items())


def expected_grid(reply: dict, state: dict) -> dict | None:
    """The solve's expected grid loss (expected_lost) from its prediction:
    the steps' and the enemy phase's Grid Defense rolls, the grid at the
    start and after the enemy phase, the chance the engine used (else the
    state's, else the engine's default). None without a predicted enemy
    phase."""
    start, end = reply.get("start") or {}, reply.get("after_enemy") or {}
    if not isinstance(start.get("grid_power"), int) or not isinstance(end.get("grid_power"), int):
        return None
    rolls = [int(r.get("amount") or 1) for s in reply.get("steps") or [] for r in s.get("grid_defense") or []]
    rolls += [int(r.get("amount") or 1) for r in (reply.get("enemy_phase") or {}).get("grid_defense") or []]
    pct = (reply.get("grid_defense") or {}).get("percent")
    if not isinstance(pct, (int, float)):
        pct = grid_defense(state)
        pct = DEFAULT_GRID_DEFENSE if pct is None else pct
    g0, lost = start["grid_power"], start["grid_power"] - end["grid_power"]
    return {"gd": pct, "lost": lost, "rolls": rolls,
            "expected": round(expected_lost(g0, rolls, lost, max(0.0, min(1.0, pct / 100))), 4)}


def squad_types(state: dict) -> dict:
    return {u["uid"]: u.get("type") for u in lp.units(state) if u.get("mech") and u.get("team") == 1}


def mech_unit(state: dict, index: int) -> dict | None:
    return next((u for u in state.get("units", []) if u.get("mech") and u.get("team") == 1
                 and u.get("uid") == index), None)


def apply_candidate(items: lo.Items, ts: TurnStart, cand: Candidate) -> tuple[dict, dict, dict]:
    """The turn start's solve input with the candidate: (data with the
    patched bridge_state, notes {"cores": n, "notes": [...]}, the state).
    CandidateError when the loadout is illegal there. The board's Grid
    Defense follows (pilots with Grid, the overpower bonus; info["gd"]:
    [before, after] percent)."""
    state, data = read_state(ts.path)
    fill_grid_overflow(state, ts)
    gd_before, grid_before = grid_defense(state), grid_pilots(state)
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
    for k, s1, s2, level in cand.skills:
        info["notes"] += set_pilot_skills(items, state, k, s1, s2, level)
    grid_after = grid_pilots(state)
    if cand.pp or grid_after != grid_before:
        if isinstance(state.get("grid_defense"), (int, float)):
            # A state that gives the chance itself: the engine adds nothing.
            state["grid_defense"] = max(0, min(100, int(state["grid_defense"]) + cand.pp
                                               + GRID_SKILL_BONUS * (grid_after - grid_before)))
        elif cand.pp:
            if not isinstance(state.get("grid_overflow"), (int, float)):
                raise CandidateError("no overpower bonus on this board (no grid_overflow in the state or its save)")
            state["grid_overflow"] = int(state["grid_overflow"]) + cand.pp
    info["gd"] = [gd_before, grid_defense(state)]
    return dict(data, bridge_state=state), info, state


def set_pilot_skills(items: lo.Items, state: dict, k: int, s1: int, s2: int, level: int | None) -> list:
    """Mech k's pilot with level-up skills s1, s2 at `level` (None: its
    own), on the board as the bridge would show it (pilot record,
    pilot_level, pilot_skills), its HP and move changed by what the
    learned skills add or take (Health / Skilled +2 HP, Move / Skilled +1
    Move; a Zoltan mech's HP stays 1). Notes for what the board cannot
    show (a Reactor skill's core)."""
    u = mech_unit(state, k)
    if u is None:
        raise CandidateError(f"no mech {k} on the board")
    rec = u.get("pilot") if isinstance(u.get("pilot"), dict) else None
    pid = (rec or {}).get("id") or u.get("pilot_id")
    if rec is None and not pid and "pilot_level" not in u:
        raise CandidateError(f"mech {k} has no pilot on the board")
    old_level, o1, o2 = pilot_of(u)
    level = old_level if level is None else level
    pilot = items.pilots.get(pid) if pid else None
    if pilot is not None:
        banned = [lo.LEVEL_SKILLS[s] for s in (s1, s2) if lo.LEVEL_SKILLS[s] in pilot.blacklist]
        if banned:
            raise CandidateError(f"mech {k}: {pid} cannot learn {banned[0]} (its Blacklist)")
    if pid == "Pilot_Artificial" and level > 0:
        raise CandidateError(f"mech {k}: the A.I. unit does not level")
    old = lo._learned({"level": old_level, "skill1": o1, "skill2": o2})
    new = lo._learned({"level": level, "skill1": s1, "skill2": s2})
    notes = [f"mech {k}: pilot {pid or '?'} level {level}, skills {lo.LEVEL_SKILLS[s1]}, {lo.LEVEL_SKILLS[s2]}"
             + (f" (learned: {', '.join(new)})" if new else " (none learned yet)")]
    dhp = sum(lo.SKILL_HEALTH.get(s, 0) for s in new) - sum(lo.SKILL_HEALTH.get(s, 0) for s in old)
    if dhp and not (pilot is not None and pilot.skill == "Zoltan_Skill"):
        if (u.get("hp") or 0) > 0:  # a dead mech stays dead
            u["hp"] = max(1, u["hp"] + dhp)
        u["max_hp"] = max(1, (u.get("max_hp") or 0) + dhp)
    dmove = sum(lo.SKILL_MOVE.get(s, 0) for s in new) - sum(lo.SKILL_MOVE.get(s, 0) for s in old)
    for key in ("move", "base_move"):
        if dmove and isinstance(u.get(key), int):
            u[key] = max(0, u[key] + dmove)
    if ("Reactor" in new) != ("Reactor" in old):
        notes.append(f"mech {k}: the Reactor skill's core is not on the board")
    if rec is not None:
        u["pilot"] = dict(rec, level=level, skill1=s1, skill2=s2)
    u["pilot_level"] = level
    u["pilot_skills"] = [f"skill{n}={sid}" for n, sid in ((1, s1), (2, s2)) if sid]
    return notes


def owned_core_upgrades(turns: list) -> tuple[dict, str]:
    """The mechs' own core upgrades in the newest turn start's save:
    ({squad index: {"hp": bool, "move": bool}}, that turn's key). A run
    buys each once (healthPower / movePower), so an hp:/move: candidate for
    a mech that has it already is no option. ({}, "") without a save."""
    for ts in reversed(turns):
        if ts.save is None:
            continue
        try:
            mechs = lo.squad(lo.parse_save(ts.save.read_text(errors="replace")))
        except (lo.LoadoutError, ValueError, OSError):
            continue
        powered = lambda cells: bool(cells) and cells[0] != 0  # noqa: E731
        return {m.index: {"hp": powered(m.health_power), "move": powered(m.move_power)} for m in mechs}, ts.key
    return {}, ""


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


def survival_split(base: dict, cand: dict) -> tuple:
    """A turn's grid_survival change as (sure, odds): sure +1 / -1 when a
    line surely keeps the grid (odds 1) on the candidate's side only / the
    baseline's only (a certain outcome, like a grid tier change), else
    odds: the change of the odds where both can lose the grid (chance,
    priced with the expected values)."""
    b, c = (1.0 if (w.get("grid_survival") is None) else float(w["grid_survival"]) for w in (base, cand))
    sure = int(c >= 1) - int(b >= 1)
    return sure, 0.0 if sure or (b >= 1 and c >= 1) else round(c - b, 4)


def row_terms(r: dict) -> tuple:
    """(sure, odds, E[grid] saved) of a result row; rows without them
    (older reports, hand-made ones) split their grid_survival delta: a
    change of 1 is sure."""
    if "sure" in r:
        return r["sure"], r.get("odds", 0.0), r.get("exp_saved", 0.0)
    d = r["delta"].get("grid_survival", 0.0)
    return (int(d), 0.0, r.get("exp_saved", 0.0)) if abs(d) >= 1 else (0, d, r.get("exp_saved", 0.0))


@dataclass
class Result:
    cand: Candidate
    # per turn: {"turn", "verdict", "delta", "proven", "cores", "sure", "odds", "exp_saved", "exp", "gd", ...}
    rows: list = field(default_factory=list)
    skipped: list = field(default_factory=list)  # (turn key, why)

    @staticmethod
    def expected(rows: list) -> dict:
        terms = [row_terms(r) for r in rows]
        return {"sure_survival": sum(t[0] for t in terms), "survival_odds": round(sum(t[1] for t in terms), 4),
                "grid_saved": round(sum(t[2] for t in terms), 4)}

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
        gd = next((r["gd"] for r in reversed(self.rows) if r.get("gd")), None)
        return {
            "candidate": self.cand.text, "turns": len(self.rows),
            "better": sum(r["verdict"] > 0 for r in self.rows), "worse": sum(r["verdict"] < 0 for r in self.rows),
            "same": sum(r["verdict"] == 0 for r in self.rows),
            "proven_turns": len(proven), "proven_better": sum(r["verdict"] > 0 for r in proven),
            "proven_worse": sum(r["verdict"] < 0 for r in proven),
            "sums": {t: round(v, 4) for t, v in sums.items() if v},
            "proven_sums": {t: round(v, 4) for t, v in proven_sums.items() if v},
            "expected": self.expected(self.rows), "proven_expected": self.expected(proven),
            "gd": gd,
            "cores": max((r["cores"] for r in self.rows), default=self.cand.cores()),
            "skipped": len(self.skipped),
        }

    def rank_key(self) -> tuple:
        """Proven turns first (no search noise): their certain outcomes
        above mech HP (a line that surely keeps the grid, grid, building HP,
        mechs, objectives failed), then the expected values (grid survival
        odds where every line can lose the grid, E[grid] saved), then how
        many turns the solver itself ranked better minus worse (that
        verdict includes the next-turn estimates the sums leave out: a
        candidate that only buys a safer next turn still counts), then the
        rest of their sums; then the same over all turns. Expected grid is
        grid-tier value the worst case cannot see, so it beats the
        verdict's next-turn estimates, but no proven certain gain."""
        s = self.summary()
        head, tail = TIERS[1:5], TIERS[5:]

        def part(sums: dict, ex: dict, verdict: int) -> tuple:
            return ((ex["sure_survival"],) + tuple(sums.get(t, 0.0) for t in head),
                    (ex["survival_odds"], ex["grid_saved"]), verdict, tuple(sums.get(t, 0.0) for t in tail))

        return (part(s["proven_sums"], s["proven_expected"], s["proven_better"] - s["proven_worse"])
                + part(s["sums"], s["expected"], s["better"] - s["worse"]))


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


def solve(engine, path: Path, args, state: dict, scale: float = 1.0) -> dict:
    t, mt = args.time * scale, args.max_time * scale
    reply = engine.solve(path, t, max_time=mt, min_tiers=args.min_tiers if mt > t else 0, **RANKING)
    return {"worst": reply.get("worst_case") or {}, "proven": bool(reply.get("proven_optimal")),
            "plan": [a.get("description") for a in reply.get("plan") or reply.get("engine_plan") or []],
            "exp": expected_grid(reply, state)}


def turn_row(ts: TurnStart, base: dict, got: dict, info: dict) -> dict:
    verdict, delta = compare(base["worst"], got["worst"])
    sure, odds = survival_split(base["worst"], got["worst"])
    eb, ec = base["exp"], got["exp"]
    saved = round(eb["expected"] - ec["expected"], 4) if eb and ec else 0.0
    return {"turn": ts.key, "verdict": verdict, "delta": delta, "proven": base["proven"] and got["proven"],
            "cores": info["cores"], "sure": sure, "odds": odds, "exp_saved": saved,
            "exp": {"base": eb, "cand": ec}, "gd": [eb["gd"] if eb else None, ec["gd"] if ec else None],
            "base": base["worst"], "cand": got["worst"], "plan": got["plan"]}


def evaluate(args, engine, cands: list, turns: list, out_dir: Path, items: lo.Items) -> list:
    results = [Result(c) for c in cands]
    for i, ts in enumerate(turns):
        st, data = read_state(ts.path)
        lp.say(f"[{i + 1}/{len(turns)}] {ts.key} {st.get('mission_id')} turn {st.get('turn')}: baseline")
        base_path = out_dir / f"{ts.run}_m{ts.mission:02d}_t{ts.turn:02d}_base.json"
        if fill_grid_overflow(st, ts):  # as the candidates get it
            base_path.write_text(json.dumps({"run_id": ts.run, "label": "solve_input",
                                             "data": dict(data, bridge_state=st)}))
        else:
            base_path.write_text(ts.path.read_text())
        base = solve(engine, base_path, args, st)
        for k, res in enumerate(results):
            try:
                data, info, cst = apply_candidate(items, ts, res.cand)
            except CandidateError as e:
                res.skipped.append((ts.key, str(e)))
                lp.say(f"    {res.cand.text}: skipped ({e})")
                continue
            path = out_dir / f"{ts.run}_m{ts.mission:02d}_t{ts.turn:02d}_c{k}.json"
            path.write_text(json.dumps({"run_id": ts.run, "label": "solve_input", "data": data}))
            got = solve(engine, path, args, cst, 1.5 if res.cand.control else 1.0)
            row = turn_row(ts, base, got, info)
            res.rows.append(row)
            mark = {1: "better", -1: "WORSE", 0: "same"}[row["verdict"]]
            lp.say(f"    {res.cand.text}: {mark}" + (f" {row['delta']}" if row["delta"] else "")
                   + (f" E[grid] saved {row['exp_saved']:+g}" if row["exp_saved"] else "")
                   + ("" if row["proven"] else " (unproven)"))
    return results


def fmt_sums(sums: dict) -> str:
    return ", ".join(f"{t} {v:+g}" for t, v in sums.items()) or "no change"


def fmt_expected(ex: dict) -> str:
    out = [f"E[grid] {ex['grid_saved']:+.2f}"]
    if ex["survival_odds"]:
        out.append(f"odds {ex['survival_odds']:+.3f}")
    return " ".join(out)


def fmt_gd(gd) -> str:
    if not gd or gd[0] is None or gd[1] is None:
        return "-"
    return f"{gd[0]}%" if gd[0] == gd[1] else f"{gd[0]}->{gd[1]}%"


def report(results: list) -> list:
    """The ranking (rank_key: proven turns' certain sums, then their
    expected values, ...), the control apart: how often solving the same
    board again differed is the noise floor."""
    lines = ["", f"{'candidate':34} {'turns':>5} {'better':>6} {'worse':>5} {'proven b/w':>10} {'cores':>5} "
                 f"{'GD':>8}  tier sums; expected (grid saved, survival odds): proven turns | all turns"]
    ranked = sorted((r for r in results if not r.cand.control), key=lambda r: r.rank_key(), reverse=True)
    control = [r for r in results if r.cand.control]
    for res in ranked + control:
        s = res.summary()
        lines.append(f"{s['candidate'][:34]:34} {s['turns']:>5} {s['better']:>6} {s['worse']:>5} "
                     f"{s['proven_better']:>4}/{s['proven_worse']:<5} {s['cores']:>5} {fmt_gd(s['gd']):>8}  "
                     f"{fmt_sums(s['proven_sums'])}; {fmt_expected(s['proven_expected'])} | "
                     f"{fmt_sums(s['sums'])}; {fmt_expected(s['expected'])}"
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
        owned, newest = owned_core_upgrades(turns)
        for c in [c for c in cands if not c.control]:
            have = [f"hp:{k}" for k in c.hp if owned.get(k, {}).get("hp")] + \
                   [f"move:{k}" for k in c.move if owned.get(k, {}).get("move")]
            if have:
                lp.say(f"skipped {c.text}: the mech already has {', '.join(have)} (save of {newest})")
                cands.remove(c)
        # def: / power: buy overpower bonus now: what it adds, on the
        # newest turn start's board, goes onto every turn's.
        decision, _ = read_state(turns[-1].path)
        fill_grid_overflow(decision, turns[-1])
        for c in [c for c in cands if c.grid_bonus]:
            try:
                lp.say(f"{c.text}: {resolve_defense(c, decision)} (turn start {turns[-1].key})")
            except CandidateError as e:
                lp.say(f"skipped {c.text}: {e} (turn start {turns[-1].key})")
                cands.remove(c)
        if not any(not c.control for c in cands):
            raise lp.Refused("no candidate left to evaluate")
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
