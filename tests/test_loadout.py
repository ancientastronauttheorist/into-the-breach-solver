"""scripts/loadout.py: the save rewrite for the fuzzer's loadout injection.

The game's scripts are not needed: FakeScripts writes a small set of
weapon, mech and pilot tables (made-up numbers, sized to fit the recorded
checkpoint fragment in tests/fixtures/) and a fake engine source tree.

    python3 -m unittest tests.test_loadout
"""

from __future__ import annotations

import contextlib
import copy
import io
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "scripts"))
import loadout as lo  # noqa: E402

FIXTURES = Path(__file__).resolve().parent / "fixtures"
SAVE = FIXTURES / "loadout_m26_t02_saveData.lua"  # recorded 20261011_222439 m26 turn 2, trimmed
MECHS = FIXTURES / "loadout_m26_t02_mechs.json"  # the bridge's mech units of that turn start

SCRIPTS = {
    "scripts.lua": "-- fake\n",
    "global.lua": """
Skill = {
	Upgrades = 0,
	UpgradeCost = {},
	Limited = 0,
	PowerCost = 0,
	Class = "",
	Name = "Weapon Name",
	Passive = "",
}
PassiveSkill = Skill:new{ Class = "" }
""",
    "weapons_prime.lua": """
Prime_TC_Punt = Skill:new{ Class = "Prime", Upgrades = 2, UpgradeCost = {1, 2} }
Prime_TC_Punt_A = Prime_TC_Punt:new{ }
Prime_KO_Crack = Skill:new{
	Class = "Prime",
	Limited = 1,
	Upgrades = 2,
	UpgradeCost = { 2, 1 },
}
Prime_Lasermech = Skill:new{ Class = "Prime", Upgrades = 2, UpgradeCost = {1, 3} }
Prime_Lasermech_A = Prime_Lasermech:new{ }
Prime_Lasermech_B = Prime_Lasermech:new{ }
Prime_Lasermech_AB = Prime_Lasermech:new{ }
Prime_Gone = Skill:new{ Class = "Prime" }
--[[
Prime_Hidden = Skill:new{ Class = "Prime" }
]]
""",
    "weapons_brute.lua": """
Brute_Jetmech = Skill:new{ Class = "Brute", Upgrades = 2, UpgradeCost = {2, 2} }
Brute_Sniper = Skill:new{ Class = "Brute", PowerCost = 1, Upgrades = 2, UpgradeCost = {1, 2} }
Brute_Sniper_B = Brute_Sniper:new{ }
""",
    "weapons_ranged.lua": "",
    "weapons_science.lua": """
Science_TC_Control = Skill:new{ Class = "Science", Upgrades = 2, UpgradeCost = {1, 2} }
Science_Placer =
{
	Class = "Science",
	Upgrades = 2,
	UpgradeCost = {1, 2},
}
Science_Shield = Skill:new{ Class = "Science", Limited = 2, Upgrades = 2, UpgradeCost = {2, 2} }
Science_Shield_A = Science_Shield:new{ Limited = 3 }
""",
    "weapons_support.lua": """
Support_Missiles = Skill:new{ PowerCost = 1, Limited = 1, Upgrades = 1, UpgradeCost = {2} }
Support_Missiles_A = Support_Missiles:new{ }
Support_Missiles_B = Support_Missiles:new{ }
""",
    "weapons_passive.lua": """
Passive_FlameImmune = PassiveSkill:new{ Passive = "Flame_Immune" }
Passive_Defenses = PassiveSkill:new{ PowerCost = 1, Passive = "Bonus_Health", Upgrades = 1, UpgradeCost = {2} }
Passive_Defenses_A = Passive_Defenses:new{ Passive = "Bonus_Health_A" }
Passive_Leech = PassiveSkill:new{ Passive = "Leech_Kill" }
Passive_Medical = PassiveSkill:new{ Passive = "Passive_Medical" }
Passive_Strange = PassiveSkill:new{ Passive = "Strange_Thing" }
Passive_Psions = PassiveSkill:new{ Passive = "Psion_Leech" }
""",
    "weapons_deploy.lua": "",
    "weapons_technovek.lua": 'Vek_Beetle = Skill:new{ Class = "TechnoVek", Upgrades = 2, UpgradeCost = {1, 3} }\n',
    "advanced/ae_weapons_base.lua": "",
    "advanced/ae_weapons.lua": "",
    "drops.lua": """
local weapon_list = {
	"Prime_KO_Crack",
	"Prime_Lasermech",
	"Brute_Sniper",
	"Science_Shield",
	"Support_Missiles",
	--"Prime_Gone",
	"Passive_FlameImmune",
	"Passive_Defenses",
	"Passive_Leech",
	"Passive_Medical",
	"Passive_Strange",
	"Passive_Psions",
}
local ae_weapon_list = {
	"Science_Placer",
}
""",
    "squad.lua": """
function getStartingSquad(choice)
	if choice == 1 then
		pawns = {"BottlecapMech", "JetMech", "SupermanMech"}
	elseif choice == 2 then
		pawns = {"BeetleMech", "JetMech", "SupermanMech"}
	end
end
""",
    "pawns.lua": """
BottlecapMech = { Class = "Prime", Health = 4, SkillList = { "Prime_TC_Punt" } }
JetMech = { Class = "Brute", Health = 2, SkillList = { "Brute_Jetmech" } }
""",
    "advanced/ae_pawns.lua": """
SupermanMech =
{
	Class = "Science",
	Health = 2,
	SkillList = { "Science_TC_Control" },
}
BeetleMech = Pawn:new{ Class = "TechnoVek", Health = 3, SkillList = { "Vek_Beetle" } }
""",
    "pilots.lua": """
Pilot = { PowerCost = 0, Skill = "", Rarity = 1, Blacklist = {} }
CreatePilot{ Id = "Pilot_Miner", Skill = "Double_Shot", PowerCost = 2, Name = "Pilot_Miner_Name" }
CreatePilot{ Id = "Pilot_Detritus", Rarity = 0 }
CreatePilot{ Id = "Pilot_Archive", Rarity = 0 }
CreatePilot{ Id = "Pilot_Artificial", Rarity = 0, Name = "Pilot_Artificial_Name" }
CreatePilot{ Id = "Pilot_Recycler", Skill = "Flying", PowerCost = 1, Name = "Pilot_Recycler_Name" }
CreatePilot{ Id = "Pilot_Medic", Skill = "TimeTravel", Name = "Pilot_Medic_Name" }
CreatePilot{ Id = "Pilot_Odd", Skill = "Odd_Skill", Name = "Pilot_Odd_Name" }
CreatePilot{ Id = "Pilot_BeetleMech", Skill = "Survive_Death", Rarity = 0 }
CreatePilot{
	Id = "Pilot_Rock",
	Skill = "Rock_Skill",
	Name = "Pilot_Rock_Name",
	Rarity = 0,
	Blacklist = {"Health", "Skilled"},
}
 CreatePilot{
	Id = "Pilot_Zoltan",
	Skill = "Zoltan_Skill",
	Name = "Pilot_Zoltan_Name",
	Rarity = 0,
	Blacklist = {"Pain", "Skilled", "Regen", "Health"},
}
""",
    "advanced/ae_pilots.lua": "",
    "text_pawns.lua": """
	Pilot_Miner_Name = "Silica",
	Pilot_Rock_Name = "Ariadne",
	Pilot_Zoltan_Name = "Mafan",
	Pilot_Recycler_Name = "Prospero",
	Pilot_Artificial_Name = "A.I. Unit",
""",
}

ENGINE = {
    "src/recording.cpp": """
uint32_t passive_of(std::string_view weapon) {
  static const std::pair<const char*, uint32_t> kNames[] = {
      {"Passive_FlameImmune", kPassiveFlameImmune},
      {"Passive_Leech", kPassiveLeechKill},
  };
  return 0;
}
""",
    "src/game_data.cpp": '{"Rock_Skill", 1}, {"Zoltan_Skill", 2}, {"Double_Shot", 3}, {"Flying", 4},\n',
    "include/itb/board.hpp": "// nothing\n",
}


def write_tree(root: Path, files: dict) -> Path:
    for name, text in files.items():
        p = root / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
    return root


def cells_match(m: lo.MechState) -> bool:
    """The reactor's used counts are the mech's powered cells by value."""
    cells = [c for s in m.slots if s for c in s.power + s.mod1 + s.mod2] + m.move_power + m.health_power + \
        list((m.pilot or {}).get("power") or [])
    r = m.reactor
    return ([sum(1 for c in cells if c == v) for v in (1, 2, 3)]
            == [r.get("iUsedPower"), r.get("iUsedBonus"), r.get("iUsedUndo")])


class LoadoutTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="itb_loadout_"))
        cls.scripts = write_tree(cls.tmp / "game" / "scripts", SCRIPTS)
        cls.engine = write_tree(cls.tmp / "engine", ENGINE)
        cls.items = lo.Items(cls.scripts)
        cls.text = SAVE.read_text()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def inject(self, spec: str, text: str | None = None) -> lo.Injection:
        t = text or self.text
        return lo.inject(self.items, {"saveData.lua": t, "undoSave.lua": t}, lo.parse_spec(spec))

    def mech(self, text: str, index: int) -> lo.MechState:
        return lo.squad(lo.parse_save(text))[index]

    def pawn_text(self, text: str, index: int) -> str:
        node = next(p for p in lo.battle_pawns(lo.parse_save(text)) if p.get("id").value == index)
        return text[node.start:node.end]

    # ------------------------------------------------------------ reading

    def test_the_recorded_fragment_parses_into_its_squad(self):
        doc = lo.parse_save(self.text)
        mechs = lo.squad(doc)
        self.assertEqual([(m.index, m.type) for m in mechs],
                         [(0, "BottlecapMech"), (1, "JetMech"), (2, "SupermanMech")])
        self.assertEqual([[s.exact(self.items) for s in m.slots if s] for m in mechs],
                         [["Prime_TC_Punt", "Prime_KO_Crack"], ["Brute_Jetmech", "Support_Missiles"],
                          ["Science_TC_Control", "Science_Placer"]])
        self.assertEqual([m.pilot["id"] for m in mechs], ["Pilot_Miner", "Pilot_Detritus", "Pilot_Archive"])
        self.assertEqual(mechs[0].pilot["power"], [0, 0])  # an unpowered Silica
        self.assertEqual(mechs[1].move_power, [2])  # a bonus core (the Reactor skill)
        self.assertTrue(all(cells_match(m) for m in mechs))
        self.assertEqual(lo.storage_items(doc), ({"Passive_Leech"}, set()))
        with self.assertRaises(ValueError):
            lo.parse_save("GameData = {")

    def test_the_item_tables_fit_the_recorded_checkpoint(self):
        run = self.tmp / "rec" / "20261011_222439"
        run.mkdir(parents=True, exist_ok=True)
        shutil.copy(SAVE, run / "m26_turn_02_saveData.lua")
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(lo.check_recordings(self.items, run.parent), 0)

    def test_scripts_are_read_with_inheritance_comments_and_lists(self):
        w = self.items.weapon("Science_Shield_A")
        self.assertEqual((w.base, w.upgrade, w.cls, w.limited, w.mod_cells(0), w.mod_cells(1)),
                         ("Science_Shield", "A", "Science", 3, 2, 2))
        self.assertEqual(self.items.weapon("Support_Missiles").mod_cells(1), 1)  # past UpgradeCost: 1
        self.assertFalse(self.items.exists("Prime_Hidden"))  # inside a comment
        self.assertIn("in no shop list", self.items.obtainable("Prime_Gone"))  # commented out of the list
        self.assertIsNone(self.items.obtainable("Prime_TC_Punt"))  # a squad's starting weapon
        self.assertIn("1 upgrade(s), not B", self.items.obtainable("Support_Missiles_B"))
        self.assertEqual(self.items.mech_class("SupermanMech"), "Science")
        self.assertEqual(self.items.pilot("Pilot_Zoltan").blacklist, ("Pain", "Skilled", "Regen", "Health"))
        self.assertEqual(self.items.pilot_name("Pilot_Rock"), ("Ariadne", "Pilot_Rock_Name"))

    def test_class_penalty_rules(self):
        cp, w = self.items.class_penalty, self.items.weapon
        self.assertFalse(cp("BottlecapMech", w("Prime_Lasermech")))
        self.assertTrue(cp("JetMech", w("Prime_Lasermech")))
        self.assertFalse(cp("JetMech", w("Support_Missiles")))  # Class ""
        self.assertFalse(cp("JetMech", w("Passive_FlameImmune")))
        self.assertTrue(cp("JetMech", w("Vek_Beetle")))
        self.assertTrue(cp("BeetleMech", w("Passive_FlameImmune")))  # TechnoVek: everything else costs
        self.assertFalse(cp("BeetleMech", w("Vek_Beetle")))

    # ------------------------------------------------------------ specs

    def test_spec_parser(self):
        specs = lo.parse_spec("0:Prime_Lasermech_A,Passive_FlameImmune;pilot=Pilot_Rock;1:*,Science_Shield")
        self.assertEqual([(s.index, s.weapons, s.pilot) for s in specs],
                         [(0, ["Prime_Lasermech_A", "Passive_FlameImmune"], "Pilot_Rock"),
                          (1, ["*", "Science_Shield"], None)])
        self.assertEqual(lo.spec_text(specs), "0:Prime_Lasermech_A,Passive_FlameImmune;pilot=Pilot_Rock;"
                                              "1:*,Science_Shield")
        self.assertEqual([(s.index, s.weapons, s.pilot) for s in lo.parse_spec("2:pilot=Pilot_Zoltan 0:Brute_Sniper")],
                         [(0, ["Brute_Sniper"], None), (2, None, "Pilot_Zoltan")])
        for bad in ("pilot=Pilot_Rock", "0:A,B,C", "0:A;0:B", "Prime_Laser", "0:A;pilot=X;pilot=Y", "0:A,"):
            with self.assertRaises(lo.LoadoutError, msg=bad):
                lo.parse_spec(bad)

    # ------------------------------------------------------------ rewriting

    def test_a_mech_gets_new_weapons_and_pilot_and_the_rest_stays(self):
        inj = self.inject("0:Prime_Lasermech_A,Passive_FlameImmune;pilot=Pilot_Rock")
        new = inj.texts["saveData.lua"]
        self.assertEqual(inj.texts["undoSave.lua"], new)
        m = self.mech(new, 0)
        self.assertEqual([s.exact(self.items) for s in m.slots], ["Prime_Lasermech_A", "Passive_FlameImmune"])
        laser, flame = m.slots
        self.assertEqual((laser.base, laser.power, laser.power_class, laser.mod1, laser.mod2, laser.uses,
                          laser.starting), ("Prime_Lasermech", [], False, [1], [0, 0, 0], 1, False))
        self.assertEqual((flame.power, flame.mod1, flame.mod2), ([], [0], [0]))
        # Rock forbids Health: skill2 (learned at level 2) becomes Popular;
        # Silica's power cells go with her.
        self.assertEqual((m.pilot["id"], m.pilot["name"], m.pilot["name_id"], m.pilot["skill1"], m.pilot["skill2"],
                          m.pilot["level"], m.pilot["exp"]), ("Pilot_Rock", "Ariadne", "Pilot_Rock_Name", 5, 6, 2, 50))
        self.assertNotIn("power", m.pilot)
        # HP: 4 + Rock 3 (Health's +2 gone) = 7, was 6/6.
        self.assertEqual((m.health, m.max_health), (7, 7))
        # One cell (upgrade A) and no free core: one core added.
        self.assertEqual(m.reactor["iUsedPower"], 1)
        self.assertTrue(cells_match(m))
        self.assertEqual(inj.changed[0].added_cores, 1)
        for i in (1, 2):
            self.assertEqual(self.pawn_text(new, i), self.pawn_text(self.text, i))
        # Only what the loadout changes: no shield, no mutation, the mech's own upgrades as they were.
        self.assertNotIn("bForceField", self.pawn_text(new, 0))
        self.assertNotIn("iMutation", self.pawn_text(new, 0))
        self.assertEqual((m.move_power, m.health_power), ([0], [0]))
        self.assertEqual(inj.changed[0].notes, ["skill2 Health -> Popular (Pilot_Rock's Blacklist)",
                                                "1 reactor core(s) added", "HP 6/6 -> 7/7"])
        cur = lo.parse_save(new)["GameData"].get("current")
        self.assertEqual(cur.get("weapons").py()[:4], ["Prime_Lasermech_A", "Passive_FlameImmune", "Brute_Jetmech",
                                                       "Support_Missiles"])
        self.assertEqual(cur.get("pilot0").py()["id"], "Pilot_Rock")
        self.assertEqual(inj.items(), [("weapon", "Prime_Lasermech_A"), ("passive", "Passive_FlameImmune"),
                                       ("pilot", "Pilot_Rock")])

    def test_an_off_class_weapon_pays_a_core(self):
        inj = self.inject("1:Prime_Lasermech,*")
        m = self.mech(inj.texts["saveData.lua"], 1)
        self.assertEqual((m.slots[0].power, m.slots[0].power_class), ([1], True))
        self.assertEqual(m.slots[1].base, "Support_Missiles")  # kept
        self.assertTrue(cells_match(m))
        self.assertEqual(inj.summary()["mechs"][0]["class_penalty"], ["Prime_Lasermech"])

    def test_a_secondary_is_removed_then_added_again(self):
        one = self.inject("1:Brute_Jetmech").texts["saveData.lua"]
        m = self.mech(one, 1)
        self.assertIsNone(m.slots[1])
        self.assertNotIn('"secondary', self.pawn_text(one, 1))
        # The Missiles' core is free again; the move upgrade keeps its bonus core.
        self.assertEqual((m.reactor["iNormalPower"], m.reactor["iUsedPower"], m.reactor["iBonusPower"],
                          m.reactor["iUsedBonus"]), (1, 1, 0, 1))
        self.assertEqual(m.move_power, [2])
        self.assertTrue(cells_match(m))
        inj = self.inject("1:*,Science_Shield_A", one)
        m = self.mech(inj.texts["saveData.lua"], 1)
        shield = m.slots[1]
        self.assertEqual(shield.exact(self.items), "Science_Shield_A")
        # Off class (+1), upgrade A (2 cells): the free core, then two added.
        self.assertEqual((shield.power, shield.mod1, shield.mod2, shield.uses), ([1], [1, 1], [0, 0], 3))
        self.assertEqual(inj.changed[0].added_cores, 2)
        self.assertTrue(cells_match(m))

    def test_an_unpowered_pilot_stays_unpowered(self):
        inj = self.inject("0:Prime_Lasermech,*")
        m = self.mech(inj.texts["saveData.lua"], 0)
        self.assertEqual(m.pilot["power"], [0, 0])
        self.assertEqual(m.reactor["iUsedPower"], 0)

    def test_zoltan_sets_one_hp_a_shield_and_a_bonus_core(self):
        inj = self.inject("2:pilot=Pilot_Zoltan")
        new = inj.texts["saveData.lua"]
        m = self.mech(new, 2)
        self.assertEqual((m.health, m.max_health), (1, 1))
        self.assertIn('["bForceField"] = true', self.pawn_text(new, 2))
        self.assertEqual(m.reactor["iBonusPower"] + m.reactor["iUsedBonus"], 1)
        self.assertTrue(cells_match(m))

    def test_networked_armor_raises_every_mech(self):
        inj = self.inject("2:*,Passive_Defenses")
        mechs = lo.squad(lo.parse_save(inj.texts["saveData.lua"]))
        self.assertEqual([(m.health, m.max_health) for m in mechs], [(7, 7), (5, 5), (5, 5)])
        self.assertEqual(len(inj.changed), 3)

    def test_a_psionic_receiver_gives_the_mechs_the_board_s_psion(self):
        # A health psion's mutation (1) on the Vek: with the Receiver the
        # mechs carry it too, +1 HP each.
        text = self.text.replace('["type"] = "Bouncer1", ["name"] = "",', '["type"] = "Bouncer1", ["name"] = "", ["iMutation"] = 1,')
        inj = self.inject("2:*,Passive_Psions", text)
        new = inj.texts["saveData.lua"]
        mechs = lo.squad(lo.parse_save(new))
        self.assertEqual([m.mutation for m in mechs], [1, 1, 1])
        self.assertEqual([(m.health, m.max_health) for m in mechs], [(7, 7), (5, 5), (5, 5)])
        # Taken away again: back as it was.
        back = self.inject("2:*,Science_Placer", new)
        mechs = lo.squad(lo.parse_save(back.texts["saveData.lua"]))
        self.assertEqual([(m.mutation, m.health, m.max_health) for m in mechs], [(0, 6, 6), (0, 4, 4), (0, 4, 4)])

    def test_a_mech_without_a_pilot_gets_one(self):
        start = self.text.index('["pilot"] = {["id"] = "Pilot_Archive"')
        end = self.text.index('["iTeamId"]', start)
        text = self.text[:start] + self.text[end:]
        self.assertIsNone(self.mech(text, 2).pilot)
        inj = self.inject("2:pilot=Pilot_Recycler", text)
        m = self.mech(inj.texts["saveData.lua"], 2)
        self.assertEqual((m.pilot["id"], m.pilot["name"], m.pilot["level"], m.pilot["power"]),
                         ("Pilot_Recycler", "Prospero", 0, [1]))
        self.assertTrue(cells_match(m))
        self.assertEqual(inj.items(), [("pilot", "Pilot_Recycler")])

    def test_a_limited_weapon_given_has_every_use(self):
        inj = self.inject("2:*,Science_Shield")
        self.assertEqual(self.mech(inj.texts["saveData.lua"], 2).slots[1].uses, 2)

    def test_an_undo_save_with_another_squad_is_refused(self):
        other = self.text.replace('"SupermanMech", ["name"]', '"JetMech", ["name"]')
        with self.assertRaises(lo.LoadoutError):
            lo.inject(self.items, {"saveData.lua": self.text, "undoSave.lua": other},
                      lo.parse_spec("0:Prime_Lasermech"))

    # ------------------------------------------------------------ legality

    def test_illegal_loadouts_are_refused(self):
        cases = {
            "0:Brute_Jetmech": "twice in the squad",
            "0:*,Passive_Leech": "storage",
            "0:Prime_Gone": "in no shop list",
            "0:Prime_Nope": "no weapon table",
            "1:*,Support_Missiles_B": "not B",
            "1:pilot=Pilot_Miner": "twice in the squad",
            "0:pilot=Pilot_BeetleMech": "Vek pilot",
            "0:pilot=Pilot_Nobody": "no pilot",
            "5:Prime_Lasermech": "no mech 5",
        }
        for spec, why in cases.items():
            with self.assertRaises(lo.LoadoutError, msg=spec) as cm:
                self.inject(spec)
            self.assertIn(why, str(cm.exception), spec)
        # Recruits may come twice.
        self.assertEqual(self.mech(self.inject("0:pilot=Pilot_Detritus").texts["saveData.lua"], 0).pilot["id"],
                         "Pilot_Detritus")

    # ------------------------------------------------------------ the bridge's view

    def test_patch_and_verify_state(self):
        state = json.loads(MECHS.read_text())
        inj = self.inject("0:Prime_Lasermech_A,Passive_FlameImmune;pilot=Pilot_Recycler")
        st = lo.patch_state(state, inj, self.items)
        u = st["units"][0]
        self.assertEqual(u["weapons_exact"], ["Prime_Lasermech_A", "Passive_FlameImmune"])
        self.assertEqual([s["id"] for s in u["weapon_slots"]], ["Prime_Lasermech_A", "Passive_FlameImmune"])
        self.assertEqual((u["pilot_id"], u["pilot"]["power"], u["pilot"]["skill_active"]), ("Pilot_Recycler", [1], True))
        self.assertEqual(st["units"][1], state["units"][1])
        self.assertEqual(lo.verify_state(st, inj), [])
        for k, v in (("weapons_exact", ["Prime_TC_Punt"]), ("hp", 5), ("pilot_id", "Pilot_Miner")):
            bad = copy.deepcopy(st)
            bad["units"][0][k] = v
            self.assertTrue(lo.verify_state(bad, inj), k)
        bad = copy.deepcopy(st)
        bad["units"][0]["pilot"]["skill_active"] = False
        self.assertIn("IsAbility", "; ".join(lo.verify_state(bad, inj)))

    def test_engine_support_by_name(self):
        sup = lo.Support(self.items, self.engine / "src")
        self.assertEqual(sup.passive("Passive_FlameImmune")[0], "supported")
        self.assertEqual(sup.passive("Passive_Defenses")[0], "supported")  # in the bridge's max_hp
        self.assertEqual(sup.passive("Passive_Medical")[0], "no_effect")
        self.assertEqual(sup.passive("Passive_Strange")[0], "unsupported")
        self.assertEqual(sup.pilot("Pilot_Rock")[0], "supported")
        self.assertEqual(sup.pilot("Pilot_Archive")[0], "supported")  # no Skill
        self.assertEqual(sup.pilot("Pilot_Medic")[0], "no_effect")
        self.assertEqual(sup.pilot("Pilot_Odd"), ("unsupported", "pilot skill Odd_Skill not modelled (no engine reference)"))


class CoverageReportTest(unittest.TestCase):
    """scripts/coverage_report.py counts live fuzz sessions apart."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="itb_coverage_"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def session(self, name: str, dry: bool) -> Path:
        d = self.tmp / "fuzz" / name
        s = d / "s000"
        s.mkdir(parents=True)
        (d / "session.json").write_text(json.dumps({"dry_run": dry}))
        st = json.loads(MECHS.read_text())
        st["units"][0]["weapons_exact"] = ["Prime_Lasermech_A", "Passive_FlameImmune"]
        st["units"][0]["pilot"]["id"] = st["units"][0]["pilot_id"] = "Pilot_Rock"
        (s / "m00_turn_02_solve_input.json").write_text(json.dumps({"data": {"bridge_state": st}}))
        plan = [{"uid": 0, "kind": "weapon", "weapon": "Prime_Lasermech_A"},
                {"uid": 1, "kind": "weapon", "weapon": "Brute_Jetmech"}]
        (s / "plan.json").write_text(json.dumps(plan))
        (s / "m00_turn_02_step1_01.json").write_text("{}")  # only the first step was played
        return d

    def test_live_fuzz_sessions_are_a_column_of_their_own(self):
        import coverage_report as cr
        self.session("20261017_000001", dry=False)
        self.session("20261017_000002", dry=True)
        f = cr.seen_fuzz(self.tmp / "fuzz")
        self.assertEqual(f["turn_starts"], 1)  # the dry run's left out
        self.assertEqual(f["weapons"]["Prime_Lasermech_A"], 1)
        self.assertEqual(f["pilots"]["Pilot_Rock"], 1)
        self.assertEqual(dict(f["fired"]), {"Prime_Lasermech_A": 1})
        live = cr.empty()
        lines = cr.section("weapon upgrades", ["Prime_Lasermech_A", "Prime_Lasermech_B", "Prime_Hidden_A"], live["weapons"],
                           f["weapons"], False, (live["fired"], f["fired"]),
                           holdable={"Prime_Lasermech_A", "Prime_Lasermech_B"})
        self.assertTrue(lines[0].startswith("weapon upgrades: 0/2 seen live, 1 in fuzz sessions, 1 in either"))
        self.assertIn("  never seen (1): Prime_Lasermech_B", lines)
        self.assertIn("  not holdable in a run, never seen (1): Prime_Hidden_A", lines)


if __name__ == "__main__":
    unittest.main()
