#!/usr/bin/env python3
"""Loadout injection for the fuzzer: rewrite a turn-start checkpoint so the
squad's mechs carry chosen weapons, upgrades, passives and pilots.

    python3 scripts/loadout.py --check                 # item tables vs every recorded checkpoint
    python3 scripts/loadout.py --show SAVE [--loadout '0:Prime_Laser_A,Passive_Ammo;pilot=Pilot_Rock']

The game keeps a mission's units in its save (RegionData, the battle
region's map_data, `pawn<N>` tables; in a mission SquadData has
`skip_pawns` and no pawns). A mech's loadout there is:

  primary / secondary            the weapon's base table name
  <slot>_power                   reactor cells of the weapon itself: its
                                 PowerCost, plus one when off-class
                                 (Skill::IsClassPenalty, below)
  <slot>_power_class             that penalty (the game recomputes it on load)
  <slot>_mod1 / _mod2            cells of upgrade A / B, sized by
                                 Skill:GetUpgradeCost (UpgradeCost[i], 1 past
                                 the list); an upgrade is on when its first
                                 cell is (PowerState::IsPowered), and the
                                 table fired is base.._A / _B / _AB
  <slot>_uses                    uses left of a limited weapon
  <slot>_damaged / _starting
  movePower / healthPower        the mech's own upgrades (left alone here)
  pilot                          id, name, name_id, skill1/skill2 (learned at
                                 level 1 / 2), exp, level, power (cells of a
                                 pilot Skill with a PowerCost: Pilot::IsAbility
                                 grants that Skill only while the first is
                                 set, the learned skills always)
  reactor                        iNormalPower / iUsedPower (free / used
                                 cores; cell value 1), iBonusPower /
                                 iUsedBonus (pilot Reactor skill and Zoltan;
                                 value 2), iUndoPower / iUsedUndo (value 3)
  health / max_health            max_health must equal what
                                 Pawn::ComputeHealthTotal gives, else
                                 Pawn::GetHealthTotal resets it and heals the
                                 mech to full: Zoltan_Skill 1 (always: it has
                                 no PowerCost, so Pilot::IsAbility grants it
                                 while the pilot is in the mech); otherwise the
                                 type's Health + the learned skills' health
                                 (Health, Skilled: +2) + 2 for healthPower +
                                 3 for Rock_Skill + Networked Armor (+1, +2
                                 upgraded; squad-wide) + 1 for a health
                                 mutation.

GameData.current (the title screen's run summary) repeats the weapons
(upgrade suffix included) and pilots; it is rewritten to match.

Skill::IsClassPenalty: a TechnoVek mech pays one more core for every
weapon that is not TechnoVek; any other mech for a weapon whose Class is
not its own, unless the weapon's Class is "", Unique, Death or Enemy.

Legal loadouts only: weapons a run can hold (the shop and pod lists of
drops.lua, the squads' starting weapons), upgrades the weapon has, pilots
from CreatePilot (the A.I. unit included), level-up skills not on the
pilot's Blacklist, no weapon twice in the squad or its storage, no named
pilot twice. Missing reactor power is added as cores (a player buys them).

The bridge reads weapons_exact, weapon_slots and the pilot from the save
the game holds, so after a load they show what was written; the live
checks that the game took it are the mechs' HP (the game would reset a
max_health that does not fit the loadout), the pilot Skill's IsAbility,
and every fuzzed step.

Only names and numbers are read from the game's scripts; nothing of them
is copied here.
"""

from __future__ import annotations

import argparse
import copy
import json
import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DEFAULT_GAME = REPO / ".local_decompile" / "builds" / "linux_21601364"
ENGINE_SRC = REPO / "engine" / "src"

# Level-up skills by index (engine board.hpp LevelSkill) and what they add.
LEVEL_SKILLS = ("Health", "Move", "Grid", "Reactor", "Opener", "Closer", "Popular", "Thick", "Skilled",
                "Invulnerable", "Adrenaline", "Pain", "Regen", "Conservative")
SKILL_HEALTH = {"Health": 2, "Skilled": 2}
SKILL_MOVE = {"Move": 1, "Skilled": 1}
SKILL_REACTOR = {"Reactor": 1}
# A replacement for a level-up skill the new pilot's Blacklist forbids:
# the same move bonus where one exists, else one with no effect on a board.
SKILL_STANDINS = {"Skilled": ("Move", "Popular", "Invulnerable"), "Move": ("Popular", "Invulnerable")}
NEUTRAL_SKILLS = ("Popular", "Invulnerable")

PENALTY_FREE = ("", "Unique", "Death", "Enemy")
UPGRADE_RE = re.compile(r"^(.*?)_(A|B|AB)$")
# Pilots a squad may hold more than once (generic recruits, the A.I.).
GENERIC_PILOTS = {"Pilot_Archive", "Pilot_Rust", "Pilot_Detritus", "Pilot_Pinnacle", "Pilot_Artificial"}
# Passives whose effect the engine reads from the bridge's state rather
# than modelling it: Networked Armor is in max_hp, the Ammo Generator in
# the limited weapons' uses.
STATE_PASSIVES = {"Bonus_Health": "its HP bonus is in the bridge's max_hp",
                  "Passive_Ammo": "its extra use is in the bridge's weapon uses"}
# Pilot skills and passives the engine does not model whose effect lies
# outside a player turn and its enemy phase as the fuzzer plays them (the
# game's tooltips): played, and listed apart.
NO_TURN_EFFECT = {"TimeTravel": "an extra Reset Turn, which the fuzzer never uses",
                  "Reset_Bonus": "a bonus after a Reset Turn, which the fuzzer never uses",
                  "Self_Shield": "a shield at the mission's start (a turn start's save holds it)",
                  "Deploy_Anywhere": "deployment only",
                  "Survive_Death": "the pilot survives its mech's defeat (XP only)",
                  "Passive_Medical": "pilots survive their mech's defeat"}
VEK_PILOT_SKILL = "Survive_Death"  # "Normal Pilots cannot be equipped": a Vek pilot stays in its TechnoVek mech


class LoadoutError(Exception):
    """An illegal or inapplicable loadout."""


# ------------------------------------------------------------------ Lua text

def strip_comments(text: str) -> str:
    """Lua with comments blanked (same length, newlines kept), strings intact."""
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            i = j + 1
            continue
        if text.startswith("--", i):
            m = re.match(r"--\[(=*)\[", text[i:])
            if m:
                close = "]" + m.group(1) + "]"
                j = text.find(close, i + len(m.group(0)))
                j = n if j < 0 else j + len(close)
            else:
                j = text.find("\n", i)
                j = n if j < 0 else j
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
            continue
        i += 1
    return "".join(out)


def match_brace(text: str, i: int) -> int:
    """Index just past the brace/paren/bracket group opening at text[i]."""
    pairs = {"{": "}", "(": ")", "[": "]"}
    stack = [pairs[text[i]]]
    j, n = i + 1, len(text)
    while j < n and stack:
        c = text[j]
        if c in "\"'":
            k = j + 1
            while k < n and text[k] != c:
                k += 2 if text[k] == "\\" else 1
            j = k + 1
            continue
        if c in pairs:
            stack.append(pairs[c])
        elif c == stack[-1]:
            stack.pop()
        j += 1
    if stack:
        raise ValueError(f"unbalanced {text[i]} at {i}")
    return j


def split_top(body: str) -> list:
    """The entries of a table body, split at depth-0 ',' and ';'."""
    out, depth, start, j, n = [], 0, 0, 0, len(body)
    while j < n:
        c = body[j]
        if c in "\"'":
            k = j + 1
            while k < n and body[k] != c:
                k += 2 if body[k] == "\\" else 1
            j = k + 1
            continue
        if c in "{([":
            depth += 1
        elif c in "})]":
            depth -= 1
        elif c in ",;" and depth == 0:
            out.append(body[start:j])
            start = j + 1
        j += 1
    out.append(body[start:])
    return [e.strip() for e in out if e.strip()]


_NOT = object()


def literal(text: str):
    """A Lua literal (number, string, boolean, nil, table of literals) as
    Python (tables: a list when positional only, else a dict); _NOT for
    anything else (an expression)."""
    t = text.strip()
    if not t:
        return _NOT
    if t[0] in "\"'" and t[-1] == t[0] and len(t) >= 2:
        return bytes(t[1:-1], "utf-8").decode("unicode_escape", errors="replace")
    if t in ("true", "false"):
        return t == "true"
    if t == "nil":
        return None
    if re.fullmatch(r"-?\d+", t):
        return int(t)
    if re.fullmatch(r"-?\d*\.\d+(e-?\d+)?|-?\d+e-?\d+", t):
        return float(t)
    try:
        whole_table = t[0] == "{" and match_brace(t, 0) == len(t)
    except ValueError:
        return _NOT
    if whole_table:
        pos, named = [], {}
        for e in split_top(t[1:-1]):
            m = re.match(r"^(?:\[\s*(\"[^\"]*\"|-?\d+)\s*\]|([A-Za-z_]\w*))\s*=(?!=)\s*(.*)$", e, re.S)
            if m:
                key = literal(m.group(1)) if m.group(1) else m.group(2)
                named[key] = literal(m.group(3))
            else:
                pos.append(literal(e))
        if named:
            named.update({i + 1: v for i, v in enumerate(pos)})
            return named
        return pos
    return _NOT


DEF_RE = re.compile(r"^(?:local[ \t]+)?([A-Za-z_]\w*)[ \t]*=\s*(?:([A-Za-z_]\w*)[ \t]*:[ \t]*new\s*\(?\s*)?\{",
                    re.M)
CREATE_PILOT_RE = re.compile(r"\bCreatePilot[ \t]*\(?[ \t]*\{")
FIELD_SET_RE = re.compile(r"^[ \t]*([A-Za-z_]\w*)\.([A-Za-z_]\w*)[ \t]*=[ \t]*([^\n]+)$", re.M)


@dataclass
class TableDef:
    name: str
    parent: str | None
    fields: dict
    file: str


def scan_defs(text: str, file: str = "") -> tuple[dict, list]:
    """Top-level `Name = Parent:new{...}` / `Name = {...}` tables (fields
    that are literals) and CreatePilot{...} blocks of a script."""
    code = strip_comments(text)
    defs, pilots = {}, []
    for m in DEF_RE.finditer(code):
        start = m.end() - 1
        try:
            end = match_brace(code, start)
        except ValueError:
            continue
        fields = {}
        for e in split_top(code[start + 1:end - 1]):
            fm = re.match(r"^([A-Za-z_]\w*)\s*=(?!=)\s*(.*)$", e, re.S)
            if fm:
                v = literal(fm.group(2))
                if v is not _NOT:
                    fields[fm.group(1)] = v
            else:
                v = literal(e)
                if v is not _NOT:
                    fields.setdefault("__list__", []).append(v)
        defs[m.group(1)] = TableDef(m.group(1), m.group(2), fields, file)
    for m in FIELD_SET_RE.finditer(code):
        if m.group(1) in defs:
            v = literal(m.group(3).strip().rstrip(";"))
            if v is not _NOT:
                defs[m.group(1)].fields[m.group(2)] = v
    for m in CREATE_PILOT_RE.finditer(code):
        start = m.end() - 1
        v = literal(code[start:match_brace(code, start)])
        if isinstance(v, dict):
            pilots.append(v)
    return defs, pilots


# ------------------------------------------------------------------ the game's items

WEAPON_FILES = ("global.lua", "weapons_base.lua", "weapons_prime.lua", "weapons_brute.lua", "weapons_ranged.lua",
                "weapons_science.lua", "weapons_support.lua", "weapons_passive.lua", "weapons_deploy.lua",
                "weapons_technovek.lua", "advanced/ae_weapons_base.lua", "advanced/ae_weapons.lua")
PAWN_FILES = ("pawns.lua", "advanced/ae_pawns.lua")
PILOT_FILES = ("pilots.lua", "advanced/ae_pilots.lua")
SKILL_FIELDS = ("Class", "PowerCost", "Upgrades", "UpgradeCost", "Limited", "Passive", "Name")


def game_scripts(arg: str | None = None) -> Path:
    root = Path(arg or os.environ.get("ITB_GAME_DIR") or DEFAULT_GAME)
    return root / "scripts" if (root / "scripts").is_dir() else root


@dataclass
class Weapon:
    id: str
    base: str
    upgrade: str  # "", "A", "B", "AB"
    cls: str
    power_cost: int
    upgrades: int
    upgrade_cost: list
    limited: int
    passive: str  # the Lua Passive string, "" for an active weapon

    def mod_cells(self, i: int) -> int:
        """Skill:GetUpgradeCost(i): the cells of upgrade A (0) / B (1)."""
        return int(self.upgrade_cost[i]) if i < len(self.upgrade_cost) else 1


@dataclass
class Pilot:
    id: str
    skill: str
    power_cost: int
    name_key: str
    blacklist: tuple
    rarity: int
    sex: str


class Items:
    """Weapons, mechs and pilots as the game's scripts define them."""

    def __init__(self, scripts: Path):
        self.scripts = scripts
        self.defs: dict = {}
        for name in WEAPON_FILES + PAWN_FILES:
            try:
                d, _ = scan_defs((scripts / name).read_text(errors="replace"), name)
            except OSError:
                continue
            self.defs.update(d)  # in the game's load order: a later definition wins
        self.pilots: dict = {}
        for name in PILOT_FILES:
            try:
                _, plist = scan_defs((scripts / name).read_text(errors="replace"), name)
            except OSError:
                continue
            for p in plist:
                pid = p.get("Id")
                if not isinstance(pid, str):
                    continue
                bl = p.get("Blacklist") or []
                self.pilots[pid] = Pilot(pid, p.get("Skill") or "", int(p.get("PowerCost") or 0),
                                         p.get("Name") or "", tuple(bl if isinstance(bl, list) else []),
                                         int(p.get("Rarity", 1) if p.get("Rarity") is not None else 1),
                                         str(p.get("Sex") or ""))
        self.names = self._text_names()
        self.shop = self._shop_lists()
        self.squad_mechs = self._squad_mechs()

    # -- generic table lookup with inheritance
    def get(self, table: str, key: str, default=None):
        seen = set()
        while table and table not in seen:
            seen.add(table)
            d = self.defs.get(table)
            if d is None:
                break
            if key in d.fields:
                return d.fields[key]
            table = d.parent
        return default

    def exists(self, table: str) -> bool:
        return table in self.defs

    def _text_names(self) -> dict:
        out = {}
        for name in ("text_pawns.lua", "advanced/ae_text.lua"):
            try:
                text = (self.scripts / name).read_text(errors="replace")
            except OSError:
                continue
            for m in re.finditer(r"\b(Pilot_\w+_Name)\s*=\s*\"([^\"]*)\"", text):
                out.setdefault(m.group(1), m.group(2))
        return out

    def _shop_lists(self) -> set:
        try:
            code = strip_comments((self.scripts / "drops.lua").read_text(errors="replace"))
        except OSError:
            return set()
        out = set()
        for name in ("weapon_list", "ae_weapon_list", "pod_list", "ae_pod_list"):
            m = re.search(r"\b" + name + r"\s*=\s*\{", code)
            if m:
                v = literal(code[m.end() - 1:match_brace(code, m.end() - 1)])
                if isinstance(v, list):
                    out.update(w for w in v if isinstance(w, str))
        return out

    def _squad_mechs(self) -> set:
        try:
            code = strip_comments((self.scripts / "squad.lua").read_text(errors="replace"))
        except OSError:
            return set()
        out = set()
        for m in re.finditer(r"\bpawns\s*=\s*(\{[^}]*\})", code):
            v = literal(m.group(1))
            if isinstance(v, list):
                out.update(t for t in v if isinstance(t, str))
        return out

    # -- weapons
    def weapon(self, wid: str) -> Weapon:
        """A weapon by exact id (upgrade suffix included); LoadoutError if the
        game has no such table."""
        if not self.exists(wid):
            raise LoadoutError(f"no weapon table {wid} in the game's scripts")
        m = UPGRADE_RE.match(wid)
        base, up = (m.group(1), m.group(2)) if m and self.exists(m.group(1)) else (wid, "")
        uc = self.get(base, "UpgradeCost", [])
        upgrades = self.get(base, "Upgrades", 0)
        return Weapon(wid, base, up, str(self.get(base, "Class", "") or ""), int(self.get(base, "PowerCost", 0) or 0),
                      int(upgrades) if isinstance(upgrades, (int, float)) else len(upgrades or []),
                      [int(c) for c in uc] if isinstance(uc, list) else [], int(self.get(wid, "Limited", 0) or 0),
                      str(self.get(wid, "Passive", "") or ""))

    def mech_class(self, mech_type: str) -> str:
        return str(self.get(mech_type, "Class", "") or "")

    def mech_health(self, mech_type: str) -> int:
        return int(self.get(mech_type, "Health", 0) or 0)

    def starting_weapons(self) -> set:
        out = set()
        for t in self.squad_mechs:
            sl = self.get(t, "SkillList", [])
            if isinstance(sl, list):
                out.update(w for w in sl if isinstance(w, str))
        return out

    def obtainable_bases(self) -> set:
        """Weapon base tables a run can hold: the shop and pod lists and the
        squads' starting weapons."""
        return {w for w in self.shop | self.starting_weapons() if self.exists(w)}

    def obtainable(self, wid: str) -> str | None:
        """Why the exact weapon id cannot be held in a run, or None."""
        if not self.exists(wid):
            return f"no weapon table {wid}"
        w = self.weapon(wid)
        if w.base not in self.obtainable_bases():
            return f"{w.base} is in no shop list or squad"
        if w.upgrade:
            need = 2 if "B" in w.upgrade else 1
            if w.upgrades < need:
                return f"{w.base} has {w.upgrades} upgrade(s), not {w.upgrade}"
        return None

    def class_penalty(self, mech_type: str, w: Weapon) -> bool:
        mc = self.mech_class(mech_type)
        if mc == "TechnoVek":
            return w.cls != "TechnoVek"
        return w.cls not in PENALTY_FREE and w.cls != mc

    def all_weapon_ids(self) -> list:
        """Every exact id (bases and their upgrade tables) a run can hold."""
        out = []
        for base in sorted(self.obtainable_bases()):
            out.append(base)
            for up in ("A", "B", "AB"):
                if self.exists(f"{base}_{up}") and self.obtainable(f"{base}_{up}") is None:
                    out.append(f"{base}_{up}")
        return out

    # -- pilots
    def pilot(self, pid: str) -> Pilot:
        if pid not in self.pilots:
            raise LoadoutError(f"no pilot {pid} (CreatePilot) in the game's scripts")
        return self.pilots[pid]

    def pilot_name(self, pid: str) -> tuple[str, str]:
        """(name, name_id) as the save writes them: a named pilot's text key
        and its text; recruits get a generated name."""
        p = self.pilot(pid)
        if p.name_key and p.name_key in self.names:
            return self.names[p.name_key], p.name_key
        return {"Pilot_Pinnacle": "Zera", "Pilot_Artificial": "A.I. Unit"}.get(pid, "Alison Smith"), ""


# ------------------------------------------------------------------ the save: a span-keeping parser

@dataclass
class Node:
    """A parsed Lua value with its text span. kind: table, str, num, bool,
    nil, call (Point(...), CreateEffect(...)), name."""
    kind: str
    start: int
    end: int
    value: object = None  # scalars; call: (name, [args]); table: list of (key or None, Node, entry_start)
    key_spans: dict = field(default_factory=dict)

    def get(self, key):
        if self.kind != "table":
            return None
        for k, v, _ in self.value:
            if k == key:
                return v
        return None

    def items(self):
        return [(k, v) for k, v, _ in self.value] if self.kind == "table" else []

    def py(self):
        """Plain Python: dict (named keys) / list (positional), Point as tuple."""
        if self.kind == "table":
            named = {k: v.py() for k, v, _ in self.value if k is not None}
            pos = [v.py() for k, v, _ in self.value if k is None]
            if named:
                named.update({i + 1: v for i, v in enumerate(pos)})
                return named
            return pos
        if self.kind == "call":
            name, args = self.value
            return (name, *[a.py() for a in args])
        return self.value


class SaveParser:
    TOKEN = re.compile(r"\s*(?:(\"(?:[^\"\\]|\\.)*\")|(-?\d+\.\d+(?:e-?\d+)?|-?\d+)|([A-Za-z_]\w*)|(\S))", re.S)

    def __init__(self, text: str):
        self.text = text
        self.pos = 0

    def peek(self):
        m = self.TOKEN.match(self.text, self.pos)
        if not m or m.end() == m.start() or self.pos >= len(self.text):
            return None, None, len(self.text)
        kind = "str" if m.group(1) else "num" if m.group(2) else "name" if m.group(3) else "op"
        return kind, m.group(m.lastindex), m.start(m.lastindex)

    def take(self):
        kind, tok, start = self.peek()
        if kind is None:
            raise ValueError(f"unexpected end of save at {self.pos}")
        self.pos = start + len(tok)
        return kind, tok, start

    def expect(self, op):
        kind, tok, start = self.take()
        if tok != op:
            raise ValueError(f"expected {op!r} at {start}, got {tok!r}")
        return start

    def value(self) -> Node:
        kind, tok, start = self.take()
        if kind == "str":
            return Node("str", start, self.pos, bytes(tok[1:-1], "utf-8").decode("unicode_escape", errors="replace"))
        if kind == "num":
            return Node("num", start, self.pos, float(tok) if "." in tok or "e" in tok else int(tok))
        if kind == "name":
            if tok in ("true", "false"):
                return Node("bool", start, self.pos, tok == "true")
            if tok == "nil":
                return Node("nil", start, self.pos, None)
            if self.peek()[1] == "(":
                self.take()
                args = []
                while self.peek()[1] != ")":
                    args.append(self.value())
                    if self.peek()[1] == ",":
                        self.take()
                self.expect(")")
                return Node("call", start, self.pos, (tok, args))
            return Node("name", start, self.pos, tok)
        if tok == "{":
            entries = []
            while True:
                k2, t2, s2 = self.peek()
                if t2 == "}":
                    self.take()
                    break
                key = None
                if t2 == "[":
                    self.take()
                    key = self.value().value
                    self.expect("]")
                    self.expect("=")
                elif k2 == "name":
                    save = self.pos
                    self.take()
                    if self.peek()[1] == "=":
                        self.take()
                        key = t2
                    else:
                        self.pos = save
                v = self.value()
                entries.append((key, v, s2))
                if self.peek()[1] in (",", ";"):
                    self.take()
            return Node("table", start, self.pos, entries)
        raise ValueError(f"unexpected {tok!r} at {start}")

    def document(self) -> dict:
        """The save's top-level assignments: {name: Node}."""
        out = {}
        while self.peek()[0] is not None:
            kind, name, _ = self.take()
            if kind != "name":
                raise ValueError(f"expected a name at {self.pos}, got {name!r}")
            self.expect("=")
            out[name] = self.value()
        return out


def parse_save(text: str) -> dict:
    return SaveParser(text).document()


# ------------------------------------------------------------------ the save's squad

@dataclass
class SlotState:
    base: str
    power: list
    power_class: bool
    mod1: list
    mod2: list
    uses: int
    damaged: bool
    starting: bool

    def exact(self, items: Items | None = None) -> str:
        a = bool(self.mod1) and self.mod1[0] != 0
        b = bool(self.mod2) and self.mod2[0] != 0
        for up in (("AB",) if a and b else ()) + (("A",) if a else ()) + (("B",) if b else ()):
            if items is None or items.exists(f"{self.base}_{up}"):
                return f"{self.base}_{up}"
        return self.base


@dataclass
class MechState:
    index: int  # the pawn's id: its squad position, the bridge uid
    type: str
    node: Node  # the pawn table
    slots: list  # [SlotState | None] * 2
    pilot: dict | None
    reactor: dict
    move_power: list
    health_power: list
    health: int
    max_health: int
    mutation: int


def battle_pawns(doc: dict) -> list:
    """The pawn tables of the mission being played (RegionData's
    iBattleRegion: region<N>, or final_region), in file order."""
    rd = doc.get("RegionData")
    if rd is None:
        raise LoadoutError("the save has no RegionData")
    br = rd.get("iBattleRegion")
    region = None
    if br is not None and br.kind == "num":
        region = rd.get(f"region{br.value}")
    if region is None or (region.get("player") is None):
        region = rd.get("final_region")
    player = region.get("player") if region is not None else None
    md = player.get("map_data") if player is not None else None
    if md is None:
        raise LoadoutError("the save holds no mission in progress (no battle region map_data)")
    return [v for k, v in md.items() if isinstance(k, str) and re.fullmatch(r"pawn\d+", k) and v.kind == "table"]


def _ints(node: Node | None) -> list:
    return [int(v.value) for _, v in node.items()] if node is not None and node.kind == "table" else []


def squad(doc: dict) -> list:
    """The mechs of the battle region, by squad index."""
    out = []
    for p in battle_pawns(doc):
        mech = p.get("mech")
        if not (mech is not None and mech.value is True):
            continue
        slots = []
        for slot in ("primary", "secondary"):
            b = p.get(slot)
            if b is None or b.kind != "str" or not b.value:
                slots.append(None)
                continue
            g = lambda k: p.get(f"{slot}_{k}")  # noqa: E731
            slots.append(SlotState(b.value, _ints(g("power")), bool(g("power_class") and g("power_class").value),
                                   _ints(g("mod1")), _ints(g("mod2")),
                                   int(g("uses").value) if g("uses") is not None else 1,
                                   bool(g("damaged") and g("damaged").value), bool(g("starting") and g("starting").value)))
        pilot = p.get("pilot")
        reactor = p.get("reactor")
        out.append(MechState(int(p.get("id").value), p.get("type").value, p, slots,
                             pilot.py() if pilot is not None else None,
                             reactor.py() if reactor is not None else {}, _ints(p.get("movePower")),
                             _ints(p.get("healthPower")), int(p.get("health").value), int(p.get("max_health").value),
                             int(p.get("iMutation").value) if p.get("iMutation") is not None else 0))
    return sorted(out, key=lambda m: m.index)


def storage_items(doc: dict) -> tuple[set, set]:
    """(weapons, pilot ids) in the squad's storage (SquadData storage_N)."""
    sd = doc.get("SquadData")
    weapons, pilots = set(), set()
    for k, v in sd.items() if sd is not None else []:
        if isinstance(k, str) and k.startswith("storage_") and v.kind == "table":
            w = v.get("weapon")
            if w is not None and w.kind == "str":
                weapons.add(w.value)
            p = v.get("pilot") or v.get("Pilot")
            if p is not None and p.kind == "table" and p.get("id") is not None:
                pilots.add(p.get("id").value)
            elif p is not None and p.kind == "str":
                pilots.add(p.value)
    return weapons, pilots


# ------------------------------------------------------------------ loadout specs

@dataclass
class MechSpec:
    index: int
    weapons: list | None = None  # exact ids; "*" keeps that slot; None keeps both
    pilot: str | None = None

    def text(self) -> str:
        s = f"{self.index}:" + (",".join(self.weapons) if self.weapons is not None else "")
        return s + (f";pilot={self.pilot}" if self.pilot else "")


def parse_spec(text: str) -> list:
    """'<mech>:<primary>[,<secondary>][;pilot=<id>]...' -> [MechSpec].
    Groups are separated by ';' (or whitespace); 'pilot=ID' applies to the
    mech before it; '*' keeps that slot's weapon; a mech given one weapon
    loses its secondary; '<mech>:pilot=ID' changes only the pilot."""
    out: dict = {}
    cur = None
    for tok in re.split(r"[;\s]+", text.strip()):
        if not tok:
            continue
        m = re.fullmatch(r"(\d+):(.*)", tok)
        if m:
            idx = int(m.group(1))
            if idx in out:
                raise LoadoutError(f"mech {idx} given twice in {text!r}")
            cur = out[idx] = MechSpec(idx)
            rest = m.group(2)
            if rest.startswith("pilot="):
                cur.pilot = rest[6:]
            elif rest:
                ws = [w.strip() for w in rest.split(",")]
                if len(ws) > 2 or not all(ws):
                    raise LoadoutError(f"mech {idx}: one or two weapons, comma-separated: {rest!r}")
                cur.weapons = ws
            continue
        if tok.startswith("pilot="):
            if cur is None:
                raise LoadoutError(f"pilot= before any '<mech>:' in {text!r}")
            if cur.pilot:
                raise LoadoutError(f"mech {cur.index}: two pilots")
            cur.pilot = tok[6:]
            if not cur.pilot:
                raise LoadoutError(f"mech {cur.index}: empty pilot id")
            continue
        raise LoadoutError(f"cannot read {tok!r} in loadout {text!r} (want '<mech>:<weapon>[,<weapon>]' or pilot=<id>)")
    return [out[k] for k in sorted(out)]


def spec_text(specs: list) -> str:
    return ";".join(s.text() for s in specs)


# ------------------------------------------------------------------ planning a loadout

@dataclass
class MechPlan:
    """What a mech carries after the rewrite (and what the bridge should
    then report)."""
    index: int
    type: str
    slots: list  # [{"id", "base", "power", "power_class", "mod1", "mod2", "uses", "limited", "passive",
    #                "power_cost", "damaged", "starting", "changed"} | None] * 2
    pilot: dict | None
    reactor: dict
    hp: int
    max_hp: int
    changed: bool
    shield: bool = False  # give the pawn a shield (bForceField): Zoltan's at the turn start
    new_pilot: bool = False  # the pilot was replaced
    mutation: int | None = None  # the psion mutation it now carries (iMutation; None: as it was)
    move_power: list = field(default_factory=list)
    health_power: list = field(default_factory=list)
    added_cores: int = 0
    notes: list = field(default_factory=list)

    @property
    def weapons_exact(self) -> list:
        return [s["id"] for s in self.slots if s]

    def summary(self) -> dict:
        return {"uid": self.index, "type": self.type, "weapons": self.weapons_exact,
                "pilot": (self.pilot or {}).get("id"), "hp": self.hp, "max_hp": self.max_hp,
                "added_cores": self.added_cores, "notes": self.notes,
                "class_penalty": [s["id"] for s in self.slots if s and s["power_class"]]}


def _learned(pilot: dict | None) -> list:
    if not pilot:
        return []
    level = int(pilot.get("level") or 0)
    out = []
    for n, key in ((1, "skill1"), (2, "skill2")):
        sid = pilot.get(key)
        if level >= n and isinstance(sid, int) and 0 <= sid < len(LEVEL_SKILLS):
            out.append(LEVEL_SKILLS[sid])
    return out


def pilot_skill(items: Items, pilot: dict | None) -> str:
    if not pilot or not pilot.get("id") or pilot["id"] not in items.pilots:
        return ""
    return items.pilots[pilot["id"]].skill


def pilot_powered(items: Items, pilot: dict | None) -> bool:
    """Pilot::IsAbility for the pilot's own Skill: always for one without a
    PowerCost, else while its first power cell is set (PowerState::IsPowered;
    a pilot the player never powered in the mech bay has {0} / {0, 0})."""
    if not pilot or pilot.get("id") not in items.pilots:
        return False
    if items.pilots[pilot["id"]].power_cost <= 0:
        return True
    return cells_on(pilot.get("power"))


def cells_on(cells) -> bool:
    """PowerState::IsPowered: the first cell set."""
    return isinstance(cells, list) and bool(cells) and cells[0] != 0


def squad_passive(items: Items, mechs: list, passive: str) -> bool:
    """SquadControl::IsPassive: a powered squad weapon whose Passive string
    starts with `passive`."""
    for m in mechs:
        for s in m.slots:
            if s and s["passive"].startswith(passive) and slot_powered(s):
                return True
    return False


def slot_powered(s: dict) -> bool:
    """Skill::IsPowered: its first power cell set, or no cost at all."""
    return (bool(s["power"]) and s["power"][0] != 0) or (s["power_cost"] == 0 and not s["power_class"])


def health_total(items: Items, mech_type: str, pilot: dict | None, health_power: list, mechs: list,
                 mutation: int) -> int:
    """Pawn::ComputeHealthTotal of a mech."""
    skill = pilot_skill(items, pilot)
    if skill == "Zoltan_Skill":
        return 1
    hp = items.mech_health(mech_type)
    hp += sum(SKILL_HEALTH.get(s, 0) for s in _learned(pilot))
    hp += 2 * (bool(health_power) and health_power[0] != 0)
    hp += 3 * (skill == "Rock_Skill")
    if squad_passive(items, mechs, "Bonus_Health"):
        hp += 2 if squad_passive(items, mechs, "Bonus_Health_A") else 1
    if mutation in (1, 7):
        hp += 1
    return hp


def slot_dict(items: Items, mech_type: str, s: SlotState) -> dict:
    wid = s.exact(items)
    w = items.weapon(wid) if items.exists(wid) else None
    return {"id": wid, "base": s.base, "power": list(s.power), "power_class": s.power_class, "mod1": list(s.mod1),
            "mod2": list(s.mod2), "uses": s.uses, "limited": w.limited if w else 0, "passive": w.passive if w else "",
            "power_cost": w.power_cost if w else 0, "damaged": s.damaged, "starting": s.starting, "changed": False}


def plan_loadout(items: Items, doc: dict, specs: list, rewrite_all: bool = False) -> list:
    """The squad after the specs, as MechPlans (every mech, changed or not);
    LoadoutError if a spec is illegal. rewrite_all: every mech is rewritten
    (its reactor re-allocated) even where the specs change nothing (the
    --check round trip)."""
    mechs = squad(doc)
    by_index = {m.index: m for m in mechs}
    for sp in specs:
        if sp.index not in by_index:
            raise LoadoutError(f"no mech {sp.index} in the battle (mechs {sorted(by_index)})")
    plans = []
    for m in mechs:
        slots = [slot_dict(items, m.type, s) if s else None for s in m.slots]
        plans.append(MechPlan(m.index, m.type, slots, copy.deepcopy(m.pilot), dict(m.reactor), m.health,
                              m.max_health, rewrite_all, move_power=list(m.move_power),
                              health_power=list(m.health_power)))
    plan_by = {p.index: p for p in plans}
    for sp in specs:
        p, m = plan_by[sp.index], by_index[sp.index]
        if sp.weapons is not None:
            new = []
            for i in range(2):
                wid = sp.weapons[i] if i < len(sp.weapons) else None
                if wid == "*":
                    new.append(p.slots[i])
                elif wid is None:
                    new.append(None)
                else:
                    why = items.obtainable(wid)
                    if why:
                        raise LoadoutError(f"mech {sp.index} ({m.type}): {wid}: {why}")
                    w = items.weapon(wid)
                    old = p.slots[i]
                    if old and old["id"] == wid:
                        new.append(old)
                        continue
                    pen = items.class_penalty(m.type, w)
                    new.append({"id": wid, "base": w.base, "power": [1] * (w.power_cost + pen), "power_class": pen,
                                "mod1": [1 if "A" in w.upgrade else 0] * w.mod_cells(0),
                                "mod2": [1 if "B" in w.upgrade else 0] * w.mod_cells(1),
                                "uses": w.limited if w.limited > 0 else 1, "limited": w.limited, "passive": w.passive,
                                "power_cost": w.power_cost, "damaged": False,
                                "starting": w.base in (items.get(m.type, "SkillList", []) or []), "changed": True})
            if new[0] is None and new[1] is not None:
                new = [new[1], None]  # the game fills the primary slot first
            if new[0] is None:
                raise LoadoutError(f"mech {sp.index} ({m.type}): no weapon left")
            if [s and s["id"] for s in new] != [s and s["id"] for s in p.slots]:
                p.slots, p.changed = new, True
        if sp.pilot:
            if sp.pilot != (p.pilot or {}).get("id"):
                old = p.pilot or {}
                if pilot_skill(items, old) == VEK_PILOT_SKILL:
                    raise LoadoutError(f"mech {sp.index} ({m.type}): its Vek pilot {old.get('id')} stays "
                                       "(Normal Pilots cannot be equipped)")
                if items.pilot(sp.pilot).skill == VEK_PILOT_SKILL:
                    raise LoadoutError(f"mech {sp.index} ({m.type}): {sp.pilot} is a Vek pilot (only in its own "
                                       "TechnoVek mech)")
                p.pilot = new_pilot(items, sp.pilot, p.pilot)
                p.new_pilot = True
                p.changed = True
                for k in ("skill1", "skill2"):
                    if k in old and old[k] != p.pilot.get(k):
                        p.notes.append(f"{k} {LEVEL_SKILLS[old[k]]} -> {LEVEL_SKILLS[p.pilot[k]]} "
                                       f"({sp.pilot}'s Blacklist)")
    check_unique(items, doc, plans)
    finish(items, mechs, plans, board_mutation(doc))
    return plans


def new_pilot(items: Items, pid: str, old: dict | None) -> dict:
    """The save's pilot table for `pid` in a mech that had `old`: the old
    pilot's experience and level-up skills (any blacklisted for the new one
    replaced), its own name, power cells for a Skill with a PowerCost."""
    p = items.pilot(pid)
    name, name_id = items.pilot_name(pid)
    out = {"id": pid, "name": name, "name_id": name_id, "renamed": False}
    if old:
        for k in ("skill1", "skill2", "exp", "level", "travel", "final", "starting", "last_end"):
            if k in old:
                out[k] = old[k]
    else:
        out.update(skill1=LEVEL_SKILLS.index("Popular"), skill2=LEVEL_SKILLS.index("Invulnerable"), exp=0, level=0,
                   travel=0, final=0, starting=False)
    if pid == "Pilot_Artificial":
        out.update(exp=0, level=0)  # the A.I. unit does not level
    taken = set()
    for key in ("skill1", "skill2"):
        sid = out.get(key)
        sname = LEVEL_SKILLS[sid] if isinstance(sid, int) and 0 <= sid < len(LEVEL_SKILLS) else None
        if sname in p.blacklist or sname in taken:
            for alt in SKILL_STANDINS.get(sname, ()) + NEUTRAL_SKILLS:
                if alt not in p.blacklist and alt not in taken and alt != sname:
                    out[key] = LEVEL_SKILLS.index(alt)
                    sname = alt
                    break
        taken.add(sname)
    if p.power_cost > 0:
        out["power"] = [1] * p.power_cost
    return out


def check_unique(items: Items, doc: dict, plans: list) -> None:
    """No weapon base twice in the squad (or squad and storage, for a newly
    given one), no named pilot twice."""
    st_weapons, st_pilots = storage_items(doc)
    seen: dict = {}
    for p in plans:
        for s in p.slots:
            if not s:
                continue
            if s["base"] in seen:
                raise LoadoutError(f"{s['base']} twice in the squad (mechs {seen[s['base']]} and {p.index})")
            seen[s["base"]] = p.index
            if s["changed"] and s["base"] in st_weapons:
                raise LoadoutError(f"{s['base']} is in the squad's storage already")
    pilots: dict = {}
    for p in plans:
        pid = (p.pilot or {}).get("id")
        if not pid or pid in GENERIC_PILOTS:
            continue
        if pid in pilots:
            raise LoadoutError(f"pilot {pid} twice in the squad (mechs {pilots[pid]} and {p.index})")
        pilots[pid] = p.index
        if p.changed and pid in st_pilots:
            raise LoadoutError(f"pilot {pid} is in the squad's storage already")


def board_mutation(doc: dict) -> int:
    """The psion mutation the battle's Vek carry (iMutation), 0 for none."""
    from collections import Counter
    c = Counter(int(p.get("iMutation").value) for p in battle_pawns(doc)
                if p.get("iMutation") is not None and not (p.get("mech") is not None and p.get("mech").value is True)
                and p.get("iMutation").value)
    return c.most_common(1)[0][0] if c else 0


def finish(items: Items, mechs: list, plans: list, mutation: int = 0) -> None:
    """Reactor power and HP of each plan, after all weapon and pilot
    changes (Networked Armor counts squad-wide; so does the Psionic
    Receiver, which gives the mechs the board's psion `mutation`)."""
    old_by = {m.index: m for m in mechs}
    # HP: a changed squad passive changes every mech's total.
    old_plans = squad_plans(items, mechs)
    ammo = squad_passive(items, plans, "Passive_Ammo")
    leech = squad_passive(items, plans, "Psion_Leech")
    if leech != squad_passive(items, old_plans, "Psion_Leech"):
        for p in plans:
            m = old_by[p.index]
            want = mutation if leech else 0
            if want != m.mutation:
                p.mutation, p.changed = want, True
                p.notes.append(f"psion mutation {m.mutation} -> {want} (Psionic Receiver)")
    for p in plans:
        m = old_by[p.index]
        if p.changed:
            reactor_plan(items, m, p)
        # A limited weapon given now has every use left: Limited, +1 for a
        # learned Conservative, +1 for the Ammo Generator (Skill::ResetUses).
        conservative = "Conservative" in _learned(p.pilot)
        for s in p.slots:
            if s and s["changed"] and s["limited"] > 0:
                s["uses"] = s["limited"] + conservative + ammo
        old_total = health_total(items, m.type, m.pilot, m.health_power, old_plans, m.mutation)
        new_total = health_total(items, m.type, p.pilot, m.health_power, plans,
                                 m.mutation if p.mutation is None else p.mutation)
        if old_total != m.max_health:
            p.notes.append(f"recorded max_health {m.max_health}, computed {old_total}: kept the difference")
        p.max_hp = max(1, m.max_health + new_total - old_total)
        if pilot_skill(items, p.pilot) == "Zoltan_Skill":
            p.max_hp = 1  # whatever the recorded difference: ComputeHealthTotal gives 1
        if m.health <= 0:
            p.hp = 0
        else:
            p.hp = max(1, min(p.max_hp, m.health + p.max_hp - m.max_health))
        if (p.hp, p.max_hp) != (m.health, m.max_health):
            p.changed = True
            p.notes.append(f"HP {m.health}/{m.max_health} -> {p.hp}/{p.max_hp}")
        # Zoltan_Skill shields the mech at every turn start, so a turn
        # start with one has it.
        bf = m.node.get("bForceField")
        if p.hp > 0 and pilot_skill(items, p.pilot) == "Zoltan_Skill" and not (bf is not None and bf.value is True):
            p.shield = p.changed = True
            p.notes.append("shielded (Zoltan_Skill at the turn start)")


def reactor_plan(items: Items, m: MechState, p: MechPlan) -> None:
    """Re-allocates the mech's cores over its powered cells (unpowered ones
    stay so): its move/health upgrades, then each weapon's power and
    upgrades and the pilot's Skill; normal cores first (value 1, undo cores
    folded in), then the bonus cores (value 2); cores added when short."""
    r = m.reactor
    normal = int(r.get("iNormalPower", 0)) + int(r.get("iUsedPower", 0)) + int(r.get("iUndoPower", 0)) + \
        int(r.get("iUsedUndo", 0))
    # Bonus cores (Pawn::CheckReactorPower: the pilot's Reactor skill and
    # Zoltan) are set in the mech bay, so a skill learned in this mission
    # has none yet: the pool stays, but for a Zoltan coming or going.
    zoltan = int(pilot_skill(items, p.pilot) == "Zoltan_Skill") - int(pilot_skill(items, m.pilot) == "Zoltan_Skill")
    bonus = max(0, int(r.get("iBonusPower", 0)) + int(r.get("iUsedBonus", 0)) + zoltan)
    pools = {"normal": normal, "bonus": bonus}
    used = {"normal": 0, "bonus": 0}

    def draw() -> int:
        if used["normal"] < pools["normal"]:
            used["normal"] += 1
            return 1
        if used["bonus"] < pools["bonus"]:
            used["bonus"] += 1
            return 2
        pools["normal"] += 1
        used["normal"] += 1
        p.added_cores += 1
        return 1

    def keep(cells: list) -> list:
        """An existing powered cell keeps its kind of core while its pool
        lasts (None: drawn later)."""
        out = []
        for c in cells:
            pool = "bonus" if c == 2 else "normal"
            if not c:
                out.append(0)
            elif used[pool] < pools[pool]:
                used[pool] += 1
                out.append(2 if pool == "bonus" else 1)
            else:
                out.append(None)
        return out

    # (holder, key, cells, existing): what was there keeps its cores first
    # (unchanged cells stay as written), then the new cells draw.
    groups = [(p, "move_power", m.move_power, True), (p, "health_power", m.health_power, True)]
    for s in p.slots:
        if s:
            groups += [(s, k, s[k], not s["changed"]) for k in ("power", "mod1", "mod2")]
    if p.pilot and isinstance(p.pilot.get("power"), list):
        groups.append((p.pilot, "power", p.pilot["power"], not p.new_pilot))
    filled = [keep(cells) if existing else [None if c else 0 for c in cells] for _, _, cells, existing in groups]
    for (holder, key, _, _), cells in zip(groups, filled):
        cells = [draw() if c is None else c for c in cells]
        if isinstance(holder, dict):
            holder[key] = cells
        else:
            setattr(holder, key, cells)
    p.reactor = {"iNormalPower": pools["normal"] - used["normal"], "iUsedPower": used["normal"],
                 "iBonusPower": pools["bonus"] - used["bonus"], "iUsedBonus": used["bonus"], "iUndoPower": 0,
                 "iUsedUndo": 0}
    if p.added_cores:
        p.notes.append(f"{p.added_cores} reactor core(s) added")


# ------------------------------------------------------------------ writing the save

def lua(v) -> str:
    """A value in the save's own style."""
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, int):
        return str(v)
    if isinstance(v, str):
        return '"' + v.replace("\\", "\\\\").replace('"', '\\"') + '"'
    if isinstance(v, list):
        return "{" + "".join(f"{lua(x)}, " for x in v) + "}"
    if isinstance(v, dict):
        return "{" + "".join(f"[{lua(k)}] = {lua(x)}, " for k, x in v.items()) + "}"
    raise TypeError(v)


PILOT_KEYS = ("id", "name", "name_id", "renamed", "skill1", "skill2", "exp", "level", "travel", "final", "starting",
              "last_end", "power")


def pilot_lua(p: dict) -> str:
    keys = [k for k in PILOT_KEYS if k in p] + [k for k in p if k not in PILOT_KEYS]
    body = "".join(f"[{lua(k)}] = {lua(p[k])}, " for k in keys if k != "power")
    if "power" in p:
        body += f"[\"power\"] = {lua(p['power'])},\n"
    return "{" + body + "}"


def slot_lua(slot: str, s: dict) -> str:
    """A weapon slot's entries as the game writes them (a newline after
    each table value)."""
    return (f'["{slot}"] = {lua(s["base"])}, ["{slot}_power"] = {lua(s["power"])},\n'
            f'["{slot}_power_class"] = {lua(s["power_class"])}, ["{slot}_mod1"] = {lua(s["mod1"])},\n'
            f'["{slot}_mod2"] = {lua(s["mod2"])},\n'
            f'["{slot}_damaged"] = {lua(s["damaged"])}, ["{slot}_starting"] = {lua(s["starting"])}, '
            f'["{slot}_uses"] = {lua(s["uses"])}, ')


def rewrite_pawn(text: str, node: Node, p: MechPlan) -> list:
    """Edits [(start, end, replacement)] for one mech pawn table."""
    edits = []
    keys = [k for k, _, _ in node.value]

    def entries_between(first: str, last: str):
        i, j = keys.index(first), keys.index(last)
        start = node.value[i][2]
        end = node.value[j][1].end
        # include the separator and trailing space after the last entry
        m = re.match(r"\s*,[ \t]*", text[end:])
        return start, end + (m.end() if m else 0)

    for i, slot in enumerate(("primary", "secondary")):
        s = p.slots[i]
        present = slot in keys
        if present:
            last = f"{slot}_uses" if f"{slot}_uses" in keys else max(
                (k for k in keys if isinstance(k, str) and k.startswith(slot + "_")), key=keys.index)
            a, b = entries_between(slot, last)
            edits.append((a, b, slot_lua(slot, s) if s else ""))
        elif s:
            # after the primary block (or the healthPower entry)
            anchor = "primary_uses" if slot == "secondary" and "primary_uses" in keys else "healthPower"
            j = keys.index(anchor)
            end = node.value[j][1].end
            m = re.match(r"\s*,[ \t]*\n?", text[end:])
            at = end + (m.end() if m else 0)
            edits.append((at, at, slot_lua(slot, s)))
    if p.pilot is not None:
        if "pilot" in keys:
            v = node.get("pilot")
            edits.append((v.start, v.end, pilot_lua(p.pilot)))
        else:
            j = keys.index("iTeamId")
            at = node.value[j][2]
            edits.append((at, at, f'["pilot"] = {pilot_lua(p.pilot)}, \n'))
    if p.shield:
        v = node.get("bForceField")
        if v is not None:
            edits.append((v.start, v.end, "true"))
        else:
            at = node.value[keys.index("iTeamId")][2]
            edits.append((at, at, '["bForceField"] = true, '))
    if p.mutation is not None:
        if "iMutation" in keys:
            i = keys.index("iMutation")
            start, end = node.value[i][2], node.value[i][1].end
            m = re.match(r"\s*,[ \t]*", text[end:])
            edits.append((start, end + (m.end() if m else 0), f'["iMutation"] = {p.mutation}, ' if p.mutation else ""))
        elif p.mutation:
            at = node.value[keys.index("iTeamId")][2]
            edits.append((at, at, f'["iMutation"] = {p.mutation}, '))
    for key, val in (("reactor", p.reactor), ("movePower", p.move_power), ("healthPower", p.health_power),
                     ("health", p.hp), ("max_health", p.max_hp)):
        v = node.get(key)
        if v is not None:
            edits.append((v.start, v.end, lua(val)))
    return edits


def apply_edits(text: str, edits: list) -> str:
    edits = sorted(edits, key=lambda e: (e[0], e[1]))
    out, pos = [], 0
    for a, b, rep in edits:
        if a < pos:
            raise LoadoutError("overlapping save edits")
        out.append(text[pos:a])
        out.append(rep)
        pos = b
    out.append(text[pos:])
    return "".join(out)


def summary_edits(text: str, doc: dict, plans: list) -> list:
    """GameData.current's weapons list and pilot<N> tables."""
    gd = doc.get("GameData")
    cur = gd.get("current") if gd is not None else None
    if cur is None:
        return []
    edits = []
    weapons = cur.get("weapons")
    if weapons is not None and weapons.kind == "table":
        vals = [v.value for _, v in weapons.items()]
        for p in plans:
            if p.changed and 2 * p.index + 1 < len(vals):
                vals[2 * p.index] = p.slots[0]["id"] if p.slots[0] else ""
                vals[2 * p.index + 1] = p.slots[1]["id"] if p.slots[1] else ""
        edits.append((weapons.start, weapons.end, lua(vals)))
    for p in plans:
        node = cur.get(f"pilot{p.index}")
        if p.changed and node is not None and p.pilot is not None:
            edits.append((node.start, node.end, pilot_lua(p.pilot)))
    return edits


def rewrite_save(text: str, plans: list, doc: dict | None = None) -> str:
    """The save text with each changed mech rewritten as planned (`doc`:
    the text already parsed)."""
    doc = doc or parse_save(text)
    nodes = {int(p.get("id").value): p for p in battle_pawns(doc)
             if p.get("mech") is not None and p.get("mech").value is True}
    edits = []
    for p in plans:
        if p.changed:
            edits += rewrite_pawn(text, nodes[p.index], p)
    edits += summary_edits(text, doc, plans)
    out = apply_edits(text, edits)
    parse_save(out)  # still a save the game can read
    return out


@dataclass
class Injection:
    specs: list
    plans: list
    texts: dict  # {"saveData.lua": text, "undoSave.lua": text}

    @property
    def changed(self) -> list:
        return [p for p in self.plans if p.changed]

    def injected(self) -> list:
        """(kind, id, mech index) of every item the injection puts in:
        kind weapon, passive or pilot."""
        out = []
        for p in self.changed:
            for s in p.slots:
                if s and s["changed"]:
                    out.append(("passive" if s["passive"] else "weapon", s["id"], p.index))
            if p.new_pilot and p.pilot:
                out.append(("pilot", p.pilot["id"], p.index))
        return out

    def items(self) -> list:
        """The (kind, id) of every item the injection puts in."""
        return [(k, i) for k, i, _ in self.injected()]

    def summary(self) -> dict:
        return {"spec": spec_text(self.specs), "mechs": [p.summary() for p in self.changed]}


def inject(items: Items, save_texts: dict, specs: list) -> Injection:
    """Plans the specs on the checkpoint (saveData.lua; undoSave.lua must
    hold the same squad) and rewrites both."""
    main = save_texts["saveData.lua"]
    plans = plan_loadout(items, parse_save(main), specs)
    out = {}
    for name, text in save_texts.items():
        if text is None:
            continue
        if name != "saveData.lua":
            other = squad(parse_save(text))
            if [(m.index, m.type) for m in other] != [(p.index, p.type) for p in plans]:
                raise LoadoutError(f"{name} holds another squad than saveData.lua")
        out[name] = rewrite_save(text, plans)
    return Injection(specs, plans, out)


# ------------------------------------------------------------------ the bridge's view

def slot_bridge(s: dict, slot: int) -> dict:
    return {"slot": slot, "id": s["id"], "base": s["base"], "power": list(s["power"]), "mod1": list(s["mod1"]),
            "mod2": list(s["mod2"]), "limited": s["limited"], "passive": bool(s["passive"]),
            "uses_saved": s["uses"], **({"uses": s["uses"]} if s["limited"] > 0 else {}),
            "damaged": s["damaged"], "power_cost": s["power_cost"],
            "powered": sum(1 for c in s["power"] if c > 0) >= s["power_cost"]}


def patch_state(state: dict, inj: Injection, items: Items) -> dict:
    """A bridge state as it should read after the injected save loads: the
    changed mechs' weapons_exact, weapon_slots, pilot, HP (for the engine in
    a dry run, and as the reference after a live load)."""
    st = copy.deepcopy(state)
    by = {p.index: p for p in inj.changed}
    for u in st.get("units", []):
        p = by.get(u.get("uid")) if u.get("mech") and u.get("team") == 1 else None
        if p is None:
            continue
        u["weapon_slots"] = [slot_bridge(s, i) for i, s in enumerate(p.slots) if s]
        u["weapons_exact"] = p.weapons_exact
        if p.pilot:
            old = u.get("pilot") if isinstance(u.get("pilot"), dict) else {}
            pil = {"id": p.pilot["id"], "level": p.pilot.get("level"), "xp": p.pilot.get("exp"),
                   "skill1": p.pilot.get("skill1"), "skill2": p.pilot.get("skill2")}
            if "power" in p.pilot:
                pil["power"] = list(p.pilot["power"])
                pil["skill_active"] = pilot_powered(items, p.pilot)
            # level-up move bonus differences (a blacklisted skill replaced)
            dmove = sum(SKILL_MOVE.get(s, 0) for s in _learned(p.pilot)) - sum(
                SKILL_MOVE.get(LEVEL_SKILLS[old[k]], 0) for n, k in ((1, "skill1"), (2, "skill2"))
                if isinstance(old.get(k), int) and 0 <= old[k] < len(LEVEL_SKILLS) and (old.get("level") or 0) >= n)
            u["pilot"] = pil
            u["pilot_id"] = p.pilot["id"]
            u["pilot_level"] = p.pilot.get("level")
            u["pilot_skills"] = [f"skill{n}={p.pilot.get(f'skill{n}')}" for n in (1, 2) if p.pilot.get(f"skill{n}")]
            if dmove:
                for k in ("move", "base_move"):
                    if isinstance(u.get(k), int):
                        u[k] = max(0, u[k] + dmove)
        u["hp"], u["max_hp"] = p.hp, p.max_hp
        if p.shield:
            u["shield"] = True
        if p.mutation is not None:
            if p.mutation:
                u["mutation"] = p.mutation
            else:
                u.pop("mutation", None)
        u["loadout_injected"] = True
    st["loadout"] = inj.summary()
    return st


def verify_state(state: dict, inj: Injection) -> list:
    """Differences between a bridge state after the load and the injected
    loadout: weapons_exact, weapon_slots ids, power and whether each is
    powered (as injected: a kept weapon at {0} is off), pilot id, HP; the
    pilot Skill's live IsAbility (skill_active) against its power cells for
    one with a PowerCost (on or off as injected: a kept pilot the player
    never powered is off). A Zoltan mech's max_hp is 1 in the game; bridges
    before 2026-10-17 said the type's Health (live_play.fill_zoltan_max_hp
    fills it in)."""
    out = []
    units = {u.get("uid"): u for u in state.get("units", []) if u.get("mech") and u.get("team") == 1}
    for p in inj.changed:
        u = units.get(p.index)
        if u is None:
            out.append(f"mech {p.index} ({p.type}) not on the board")
            continue
        got = u.get("weapons_exact")
        if got != p.weapons_exact:
            out.append(f"mech {p.index} {p.type}: weapons_exact {got}, injected {p.weapons_exact}")
        slots = u.get("weapon_slots") or []
        for s in (x for x in p.slots if x):
            ws = next((w for w in slots if w.get("id") == s["id"]), None)
            if ws is None:
                continue
            for k in ("power", "mod1", "mod2"):
                if list(ws.get(k) or []) != list(s[k]):
                    out.append(f"mech {p.index} {s['id']}: {k} {ws.get(k)}, injected {s[k]}")
            # On or off as injected: a kept weapon the player never powered
            # stays off (a Ranged_Dual at {0}).
            want = slot_bridge(s, 0)["powered"]
            if isinstance(ws.get("powered"), bool) and ws["powered"] != want:
                out.append(f"mech {p.index} {s['id']}: {'powered' if ws['powered'] else 'unpowered'} in the game, "
                           f"injected {'powered' if want else 'unpowered'} (power {s['power']})")
        if p.pilot:
            pid = u.get("pilot_id") or (u.get("pilot") or {}).get("id")
            if pid != p.pilot["id"]:
                out.append(f"mech {p.index} {p.type}: pilot {pid}, injected {p.pilot['id']}")
            # The skill as injected: a kept pilot keeps its cells, so one
            # the player never powered stays off (fuzz 20261017_164513:
            # Silica and Archimedes at {0} were taken for a failed rewrite).
            got = (u.get("pilot") or {}).get("skill_active")
            want = cells_on(p.pilot.get("power"))
            if "power" in p.pilot and isinstance(got, bool) and got != want:
                out.append(f"mech {p.index}: pilot {p.pilot['id']}'s skill is {'on' if got else 'off'} in the game "
                           f"(IsAbility), injected {'on' if want else 'off'} (power {p.pilot['power']})")
        if (u.get("hp"), u.get("max_hp")) != (p.hp, p.max_hp):
            out.append(f"mech {p.index} {p.type}: HP {u.get('hp')}/{u.get('max_hp')}, injected {p.hp}/{p.max_hp}")
        if p.shield and u.get("shield") is False:
            out.append(f"mech {p.index} {p.type}: no shield in the game, injected one (bForceField)")
    return out


# ------------------------------------------------------------------ engine support

def engine_names(src: Path = ENGINE_SRC) -> tuple[set, set]:
    """(squad passive weapon prefixes of the engine's passive_of, every
    string literal in the engine's sources): what the engine knows by
    name."""
    passives, literals = set(), set()
    for p in sorted(src.glob("*.cpp")) + sorted((src.parent / "include").rglob("*.hpp")):
        try:
            text = p.read_text(errors="replace")
        except OSError:
            continue
        literals.update(re.findall(r'"([A-Za-z_][A-Za-z0-9_]*)"', text))
        if p.name == "recording.cpp":
            m = re.search(r"uint32_t passive_of\(.*?\n\}", text, re.S)
            if m:
                passives.update(re.findall(r'\{"(Passive_\w+)"', m.group(0)))
    return passives, literals


def game_lua_passive_checks(scripts: Path) -> set:
    """Passive strings the game's own scripts test (IsPassiveSkill): the
    engine's Lua host answers those like the game."""
    out = set()
    for p in sorted(scripts.rglob("*.lua")):
        try:
            out.update(re.findall(r'IsPassiveSkill\(\s*"([^"]+)"', p.read_text(errors="replace")))
        except OSError:
            pass
    return out


class Support:
    """Whether the engine models an item, by what it knows of it by name:
    a passive by the engine's passive_of, a string the engine tests, or the
    game's scripts testing it (IsPassiveSkill, which the engine's Lua host
    answers); one whose effect is in the bridge's state (STATE_PASSIVES);
    a pilot by its Skill appearing in the engine. Weapons run as the game's
    Lua; the dry run checks those on a board (fuzz_play)."""

    def __init__(self, items: Items, src: Path = ENGINE_SRC):
        self.items = items
        self.passives, self.literals = engine_names(src)
        self.lua_checked = game_lua_passive_checks(items.scripts)

    def passive(self, wid: str) -> tuple:
        """(status, why) of a passive weapon: "supported" (why None or how),
        "no_effect" (not modelled, but nothing in a player turn or an enemy
        phase depends on it), "unsupported"."""
        w = self.items.weapon(wid)
        if any(wid.startswith(p) for p in self.passives):
            return "supported", None
        for k, how in STATE_PASSIVES.items():
            if w.passive.startswith(k):
                return "supported", how
        if w.passive in self.literals or any(c.startswith(w.passive) or w.passive.startswith(c)
                                             for c in self.lua_checked):
            return "supported", None
        if w.passive in NO_TURN_EFFECT:
            return "no_effect", f"passive {w.passive} not modelled: {NO_TURN_EFFECT[w.passive]}"
        return "unsupported", f"passive {w.passive} not modelled (not in the engine's passive_of, no engine reference)"

    def pilot(self, pid: str) -> tuple:
        """(status, why) of a pilot, as for passive(): its own Skill."""
        p = self.items.pilot(pid)
        if not p.skill or p.skill in self.literals:
            return "supported", None
        if p.skill in NO_TURN_EFFECT:
            return "no_effect", f"pilot skill {p.skill} not modelled: {NO_TURN_EFFECT[p.skill]}"
        return "unsupported", f"pilot skill {p.skill} not modelled (no engine reference)"


# ------------------------------------------------------------------ CLI

def squad_plans(items: Items, mechs: list) -> list:
    """The squad as MechPlans as it is (for squad_passive)."""
    return [MechPlan(m.index, m.type, [slot_dict(items, m.type, s) if s else None for s in m.slots], m.pilot,
                     {}, m.health, m.max_health, False) for m in mechs]


def mech_problems(items: Items, m: MechState, plans: list) -> list:
    """(kind, text) of each way a mech of a save does not fit the item
    tables: weapon power and mod cells, the class flag, max_health against
    Pawn::ComputeHealthTotal, the reactor's used and bonus cores."""
    out = []
    for s in m.slots:
        if not s:
            continue
        w = items.weapon(s.exact(items)) if items.exists(s.base) else None
        if w is None:
            out.append(("unknown_weapon", f"{m.type} {s.base}: no such weapon table"))
            continue
        pen = items.class_penalty(m.type, w)
        if len(s.power) != w.power_cost + pen:
            out.append(("power_cells", f"{m.type} {s.base}: power {s.power}, cost {w.power_cost} + penalty {pen}"))
        if pen != s.power_class:
            out.append(("power_class", f"{m.type} {s.base}: class flag {s.power_class}, computed {pen}"))
        if len(s.mod1) != w.mod_cells(0) or len(s.mod2) != w.mod_cells(1):
            out.append(("mod_cells", f"{s.base}: mods {s.mod1} {s.mod2}, cost {w.upgrade_cost}"))
    hp = health_total(items, m.type, m.pilot, m.health_power, plans, m.mutation)
    if hp != m.max_health:
        out.append(("max_health", f"{m.type} pilot {(m.pilot or {}).get('id')} {_learned(m.pilot)}: "
                                  f"max_health {m.max_health}, computed {hp}"))
    r = m.reactor
    cells = [c for s in m.slots if s for c in s.power + s.mod1 + s.mod2] + m.move_power + m.health_power + \
        list((m.pilot or {}).get("power") or [])
    used = {v: sum(1 for c in cells if c == v) for v in (1, 2, 3)}
    if (used[1], used[2], used[3]) != (r.get("iUsedPower"), r.get("iUsedBonus"), r.get("iUsedUndo")):
        out.append(("reactor_used", f"{m.type}: cells {used}, reactor {r}"))
    bonus = sum(SKILL_REACTOR.get(s, 0) for s in _learned(m.pilot)) + (pilot_skill(items, m.pilot) == "Zoltan_Skill")
    # (a Reactor skill learned in this mission gets its core in the mech bay)
    if bonus < int(r.get("iBonusPower", 0)) + int(r.get("iUsedBonus", 0)):
        out.append(("reactor_bonus", f"{m.type} {_learned(m.pilot)}: bonus {bonus}, reactor {r}"))
    return out


def powered_items(items: Items, m: MechState) -> tuple:
    """What a mech's cells switch on: each weapon (exact id, IsPowered) and
    the pilot's own Skill (Pilot::IsAbility)."""
    ws = []
    for s in m.slots:
        if s:
            w = items.weapon(s.exact(items)) if items.exists(s.exact(items)) else None
            ws.append((s.exact(items), cells_on(s.power) or (not s.power and not s.power_class
                                                              and (w is None or w.power_cost == 0))))
    return tuple(ws), pilot_powered(items, m.pilot)


def recorded_state(save: Path) -> dict | None:
    """The bridge state recorded with a turn-start checkpoint (its
    _solve_input.json), when it has the loadout fields verify_state reads."""
    try:
        d = json.loads(save.with_name(save.name.replace("_saveData.lua", "_solve_input.json")).read_text())
    except (OSError, ValueError):
        return None
    data = d.get("data", {}) if isinstance(d, dict) else {}
    st = data.get("bridge_state") if isinstance(data, dict) and "bridge_state" in data else d
    mechs = [u for u in (st or {}).get("units", []) if u.get("mech") and u.get("team") == 1]
    return st if mechs and all("weapons_exact" in u and "weapon_slots" in u for u in mechs) else None


# Pilots the round trip injects by turns: each with a PowerCost, and Zoltan.
def round_trip_pilots(items: Items) -> list:
    return sorted(pid for pid, p in items.pilots.items() if p.power_cost > 0) + ["Pilot_Zoltan"]


def round_trip(items: Items, text: str, doc: dict, mechs: list, state: dict | None, n: int) -> list:
    """(kind, text) of each problem of two rewrites of a checkpoint:

    - every mech rewritten with nothing changed (its reactor re-allocated):
      the save still fits the item tables, every weapon and pilot Skill is
      on or off as before (a pilot the player never powered stays off),
      HP stays, and the recorded bridge state passes verify_state as it is
      (the game shows what it showed: fuzz 20261017_164513, Silica and
      Archimedes unpowered);
    - a pilot injected into one mech (turn by turn: each pilot with a
      PowerCost, then Zoltan): it is powered, the save fits the tables
      (Zoltan: max_health 1, as Pawn::ComputeHealthTotal; fuzz
      20261017_135355), and the patched state passes verify_state."""
    out = []
    plans = plan_loadout(items, doc, [], rewrite_all=True)
    new = squad(parse_save(rewrite_save(text, plans, doc)))
    after = squad_plans(items, new)
    for a, b in zip(mechs, new):
        out += [(f"rewrite_{k}", t) for k, t in mech_problems(items, b, after)]
        if powered_items(items, a) != powered_items(items, b):
            out.append(("rewrite_powered", f"{a.type}: on before {powered_items(items, a)}, after "
                                           f"{powered_items(items, b)}"))
        if (a.health, a.max_health) != (b.health, b.max_health):
            out.append(("rewrite_hp", f"{a.type}: HP {a.health}/{a.max_health} -> {b.health}/{b.max_health}"))
    if state is not None:
        out += [("rewrite_verify", t) for t in verify_state(state, Injection([], plans, {}))]
    pilots = round_trip_pilots(items)
    alive = [m for m in mechs if m.health > 0]
    if not alive:
        return out
    target = alive[n % len(alive)]
    for k in range(len(pilots)):
        pid = pilots[(n + k) % len(pilots)]
        if pid == (target.pilot or {}).get("id"):
            continue  # not an injection
        try:
            specs = [MechSpec(target.index, pilot=pid)]
            plans = plan_loadout(items, doc, specs)
        except LoadoutError:
            continue
        new_text = rewrite_save(text, plans, doc)
        new = squad(parse_save(new_text))
        after = squad_plans(items, new)
        b = next(m for m in new if m.index == target.index)
        out += [(f"inject_{k2}", f"{pid}: {t}") for k2, t in mech_problems(items, b, after)]
        if not pilot_powered(items, b.pilot):
            out.append(("inject_pilot_power", f"{target.type} {pid}: power {(b.pilot or {}).get('power')}"))
        if pid == "Pilot_Zoltan":
            bf = b.node.get("bForceField")
            if (b.health, b.max_health) != (1, 1) or not (bf is not None and bf.value is True):
                out.append(("inject_zoltan", f"{target.type}: HP {b.health}/{b.max_health}, shield "
                                             f"{bf.value if bf is not None else None}"))
        if state is not None:
            inj = Injection(specs, plans, {"saveData.lua": new_text})
            out += [("inject_verify", f"{pid}: {t}") for t in verify_state(patch_state(state, inj, items), inj)]
        break
    return out


def check_recordings(items: Items, root: Path, limit: int = 0, round_trips: bool = True) -> int:
    """Item tables against every recorded checkpoint: power, mod and
    max_health as the game wrote them vs. as planned here; then, unless
    round_trips is off, the rewrites of each (round_trip)."""
    from collections import Counter
    bad = Counter()
    n = 0
    examples = []
    files = sorted(root.glob("*/m*_turn_*_saveData.lua"))
    if limit:
        files = files[::max(1, len(files) // limit)]
    for i, f in enumerate(files):
        where = f"{f.parent.name}/{f.name}"
        try:
            text = f.read_text(errors="replace")
            doc = parse_save(text)
            mechs = squad(doc)
        except (ValueError, LoadoutError) as e:
            bad["unparsed"] += 1
            examples.append(f"{f}: {e}")
            continue
        plans = squad_plans(items, mechs)
        problems = []
        for m in mechs:
            n += 1
            problems += mech_problems(items, m, plans)
        if round_trips and not problems:
            try:
                problems += round_trip(items, text, doc, mechs, recorded_state(f), i)
            except (ValueError, LoadoutError) as e:
                problems.append(("round_trip_error", str(e)))
        for kind, t in problems:
            bad[kind] += 1
            examples.append(f"{where} {t}")
    print(f"{n} mechs in {len(files)} checkpoints: " + (", ".join(f"{k} {v}" for k, v in bad.items()) or "all match"))
    for e in examples[:40]:
        print("  " + e)
    return 1 if bad else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", help="game install (default ITB_GAME_DIR, else the local copy)")
    ap.add_argument("--check", action="store_true", help="check the item tables against every recorded checkpoint")
    ap.add_argument("--recordings", default=str(REPO / "recordings" / "live"))
    ap.add_argument("--limit", type=int, default=0, help="--check: about this many checkpoints")
    ap.add_argument("--no-round-trip", action="store_true",
                    help="--check: the recorded saves only, without rewriting them (round_trip)")
    ap.add_argument("--show", metavar="SAVE", help="the squad of a checkpoint (with --loadout: after it)")
    ap.add_argument("--loadout", help="a loadout spec to plan on --show")
    ap.add_argument("--out", help="with --show --loadout: write the rewritten save here")
    args = ap.parse_args(argv)
    items = Items(game_scripts(args.game))
    if args.check:
        return check_recordings(items, Path(args.recordings), args.limit, not args.no_round_trip)
    if args.show:
        text = Path(args.show).read_text(errors="replace")
        doc = parse_save(text)
        for m in squad(doc):
            print(f"mech {m.index} {m.type} ({items.mech_class(m.type)}): "
                  f"{[s.exact(items) for s in m.slots if s]} pilot {(m.pilot or {}).get('id')} "
                  f"HP {m.health}/{m.max_health} reactor {m.reactor}")
        if args.loadout:
            inj = inject(items, {"saveData.lua": text}, parse_spec(args.loadout))
            print(json.dumps(inj.summary(), indent=1))
            if args.out:
                Path(args.out).write_text(inj.texts["saveData.lua"])
        return 0
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
