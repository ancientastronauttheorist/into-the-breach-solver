// Pilot experience and level-ups during a battle (itb/pilot_xp.hpp): kill
// credit (Pawn::ProcessDeath), Pawn::UpdateKills, BoardPlayer::UpdateXP and
// the level-up's immediate effects.

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "itb/engine.hpp"
#include "itb/game_data.hpp"
#include "itb/movement.hpp"
#include "itb/pilot_xp.hpp"
#include "itb/recording.hpp"
#include "itb/timing.hpp"

using namespace itb;

namespace {

Engine* engine() {
  static Engine* e = [] {
    const auto root = GameData::default_game_root();
    if (root.empty() || !std::filesystem::exists(root / "scripts" / "scripts.lua")) {
      return static_cast<Engine*>(nullptr);
    }
    return Engine::create(root).release();
  }();
  return e;
}

#define NEED_ENGINE()                                                                     \
  if (!engine()) {                                                                        \
    WARN_MESSAGE(false, "no game install found; set ITB_GAME_DIR to run engine tests"); \
    return;                                                                               \
  }                                                                                       \
  Engine& E = *engine()

int32_t place(Board& b, const char* type, Point p, bool mech = false) {
  const PawnDef* def = engine()->data().pawn(type);
  REQUIRE_MESSAGE(def != nullptr, type);
  int32_t uid = 0;
  for (const Pawn& q : b.pawns()) uid = std::max(uid, q.uid + 1);
  Pawn pawn = engine()->data().make_pawn(*def, uid, p);
  if (mech) {
    pawn.mech = true;
    pawn.corpse = true;
    pawn.team = Team::Player;
    pawn.active = true;
  }
  b.add_pawn(pawn);
  return uid;
}

Pawn& P(Board& b, int32_t uid) {
  Pawn* p = b.find_pawn(uid);
  REQUIRE(p != nullptr);
  return *p;
}

void pilot(Pawn& p, int level, int xp, LevelSkill s1, LevelSkill s2) {
  p.pilot_level = static_cast<int8_t>(level);
  p.pilot_xp = static_cast<int16_t>(xp);
  p.pilot_skill1 = static_cast<int8_t>(s1);
  p.pilot_skill2 = static_cast<int8_t>(s2);
}

void hp(Board& b, int32_t uid, int now, int max = -1) {
  Pawn& p = P(b, uid);
  p.hp = static_cast<int8_t>(now);
  if (max >= 0) p.max_hp = static_cast<int8_t>(max);
}

bool dead(const Board& b, int32_t uid) {
  const Pawn* p = b.find_pawn(uid);
  return !p || !p->alive() || p->fallen;
}

Recording live_fixture(const char* name) {
  std::string error;
  auto rec = load_recording(std::string(ITB_FIXTURE_DIR) + "/" + name, &engine()->data(), &error);
  REQUIRE_MESSAGE(rec.has_value(), error);
  for (const auto& [uid, id] : rec->pilots) {
    if (Pawn* p = rec->board.find_pawn(uid)) p->pilot_abilities |= engine()->pilot_ability(id);
  }
  return *rec;
}

Pawn plain_mech(int hp_now, int hp_max) {
  Pawn p;
  p.uid = 0;
  p.mech = true;
  p.team = Team::Player;
  p.hp = static_cast<int8_t>(hp_now);
  p.max_hp = static_cast<int8_t>(hp_max);
  p.move = 3;
  return p;
}

}  // namespace

// ---- Pilot::IncreaseXP and the level-up's effects (no game data) -----------------

TEST_CASE("pilot xp: 25 XP to level 1, 50 to level 2, the excess is lost") {
  Board b;
  Pawn p = plain_mech(3, 3);
  pilot(p, 0, 20, LevelSkill::Popular, LevelSkill::Grid);
  CHECK_FALSE(pilot_levels_up(p, 4));
  CHECK(pilot_levels_up(p, 5));
  CHECK_FALSE(increase_pilot_xp(b, p, 4));
  CHECK(p.pilot_xp == 24);
  CHECK(increase_pilot_xp(b, p, 6));  // 30: level 1, XP back to 0 (not 5)
  CHECK(p.pilot_level == 1);
  CHECK(p.pilot_xp == 0);
  CHECK_FALSE(increase_pilot_xp(b, p, 49));
  CHECK(increase_pilot_xp(b, p, 1));
  CHECK(p.pilot_level == 2);
  CHECK(p.pilot_xp == 0);
  // The top level: XP no longer tracked.
  CHECK_FALSE(tracks_pilot_xp(p));
  CHECK_FALSE(increase_pilot_xp(b, p, 100));
  CHECK(p.pilot_level == 2);
}

TEST_CASE("pilot xp: unrecorded XP, no pilot or a dead mech gain nothing") {
  Board b;
  Pawn p = plain_mech(3, 3);
  pilot(p, 0, 0, LevelSkill::Health, LevelSkill::Move);
  p.pilot_xp = -1;  // old bridge: XP not exported
  CHECK_FALSE(increase_pilot_xp(b, p, 100));
  CHECK(p.pilot_level == 0);
  Pawn none = plain_mech(3, 3);
  CHECK_FALSE(has_pilot_record(none));
  CHECK_FALSE(increase_pilot_xp(b, none, 100));
  Pawn corpse = plain_mech(0, 3);
  pilot(corpse, 0, 24, LevelSkill::Health, LevelSkill::Move);
  CHECK_FALSE(increase_pilot_xp(b, corpse, 5));
  CHECK(corpse.hp == 0);
}

TEST_CASE("pilot xp: Health and Skilled raise max and current HP by 2 at once") {
  Board b;
  Pawn p = plain_mech(2, 4);
  pilot(p, 1, 46, LevelSkill::Closer, LevelSkill::Health);
  CHECK(pilot_hp_room(p) == 2);
  CHECK(increase_pilot_xp(b, p, 5));
  CHECK(p.pilot_level == 2);
  CHECK(p.max_hp == 6);
  CHECK(p.hp == 4);
  CHECK(pilot_hp_room(p) == 0);

  Pawn s = plain_mech(3, 3);
  pilot(s, 0, 24, LevelSkill::Skilled, LevelSkill::Health);
  CHECK(pilot_hp_room(s) == 4);
  const int move_before = base_move(b, s);
  CHECK(increase_pilot_xp(b, s, 1));
  CHECK(s.max_hp == 5);
  CHECK(s.hp == 5);
  CHECK(base_move(b, s) == move_before + 1);

  // Zoltan: the mech's health total is always 1.
  Pawn z = plain_mech(1, 1);
  z.pilot_abilities = kPilotZoltan;
  pilot(z, 0, 24, LevelSkill::Health, LevelSkill::Move);
  CHECK(pilot_hp_room(z) == 0);
  CHECK(increase_pilot_xp(b, z, 1));
  CHECK(z.max_hp == 1);
  CHECK(z.hp == 1);
}

TEST_CASE("pilot xp: Move, Opener, Closer, Pain and Adrenaline move") {
  Board b;
  b.turn = 2;
  b.total_turns = 4;
  Pawn p = plain_mech(3, 3);
  p.movement.turn_count = 2;
  pilot(p, 0, 24, LevelSkill::Move, LevelSkill::Pain);
  CHECK(base_move(b, p) == 3);
  CHECK(increase_pilot_xp(b, p, 1));
  CHECK(base_move(b, p) == 4);
  // Pain (Masochist) once learned: +2 while not at full HP.
  p.pilot_level = 2;
  CHECK(base_move(b, p) == 4);
  p.hp = 2;
  CHECK(base_move(b, p) == 6);

  // Opener: Boost at once on the pawn's first turn, and +2 move.
  Pawn o = plain_mech(3, 3);
  o.movement.turn_count = 1;
  pilot(o, 0, 24, LevelSkill::Opener, LevelSkill::Move);
  Board first;
  first.turn = 1;
  CHECK(increase_pilot_xp(first, o, 1));
  CHECK(o.boosted);
  CHECK(base_move(first, o) == 5);
  Pawn o2 = plain_mech(3, 3);
  o2.movement.turn_count = 2;
  pilot(o2, 0, 24, LevelSkill::Opener, LevelSkill::Move);
  CHECK(increase_pilot_xp(b, o2, 1));
  CHECK_FALSE(o2.boosted);
  CHECK(base_move(b, o2) == 3);

  // Closer: Boost and +2 on the last turn only.
  Pawn c = plain_mech(3, 3);
  c.movement.turn_count = 4;
  pilot(c, 0, 24, LevelSkill::Closer, LevelSkill::Move);
  Board last;
  last.turn = 4;
  last.total_turns = 4;
  CHECK(increase_pilot_xp(last, c, 1));
  CHECK(c.boosted);
  CHECK(base_move(last, c) == 5);
  CHECK(base_move(b, c) == 3);
}

TEST_CASE("pilot xp: Conservative, Thick Skin and Technician apply at once") {
  Board b;
  Pawn p = plain_mech(3, 3);
  p.uses = {-1, 1, 0, -1};
  pilot(p, 0, 24, LevelSkill::Conservative, LevelSkill::Thick);
  CHECK(increase_pilot_xp(b, p, 3));
  CHECK(p.uses[0] == -1);
  CHECK(p.uses[1] == 2);
  CHECK(p.uses[2] == 0);  // used up or unpowered: Pawn::uses cannot tell
  CHECK_FALSE(p.has_pilot(kPilotThick));
  CHECK(increase_pilot_xp(b, p, 50));
  CHECK(p.has_pilot(kPilotThick));

  Pawn r = plain_mech(3, 3);
  pilot(r, 0, 24, LevelSkill::Regen, LevelSkill::Thick);
  CHECK(increase_pilot_xp(b, r, 1));
  CHECK(r.has_pilot(kPilotRegen));
}

// ---- Kill credit through the executor ---------------------------------------------

TEST_CASE("pilot xp: a mech's kill gives its pilot the victim's max HP and can level it up") {
  NEED_ENGINE();
  Board b;
  const int32_t m = place(b, "PunchMech", {3, 3}, true);
  REQUIRE(m == 0);
  const int32_t v = place(b, "Scorpion1", {3, 4});
  hp(b, v, 1, 3);
  pilot(P(b, m), 0, 21, LevelSkill::Health, LevelSkill::Move);
  hp(b, m, 2, 3);
  REQUIRE(E.fire_weapon(b, m, "Prime_Punchmech", {3, 4}).ok());
  CHECK(dead(b, v));
  // 21 + 3 = 24: no level yet.
  CHECK(P(b, m).pilot_xp == 24);
  CHECK(P(b, m).pilot_level == 0);
  CHECK(P(b, m).hp == 2);

  Board b2;
  const int32_t m2 = place(b2, "PunchMech", {3, 3}, true);
  const int32_t v2 = place(b2, "Scorpion1", {3, 4});
  hp(b2, v2, 1, 3);
  pilot(P(b2, m2), 0, 22, LevelSkill::Health, LevelSkill::Move);
  hp(b2, m2, 2, 3);
  REQUIRE(E.fire_weapon(b2, m2, "Prime_Punchmech", {3, 4}).ok());
  CHECK(dead(b2, v2));
  CHECK(P(b2, m2).pilot_level == 1);
  CHECK(P(b2, m2).pilot_xp == 0);
  CHECK(P(b2, m2).max_hp == 5);
  CHECK(P(b2, m2).hp == 4);
}

TEST_CASE("pilot xp: Minor victims give no XP; Experienced pilots get +2 per kill") {
  NEED_ENGINE();
  Board b;
  const int32_t m = place(b, "PunchMech", {3, 3}, true);
  const int32_t v = place(b, "Scorpion1", {3, 4});
  hp(b, v, 1, 3);
  P(b, v).minor = true;
  pilot(P(b, m), 0, 0, LevelSkill::Health, LevelSkill::Move);
  REQUIRE(E.fire_weapon(b, m, "Prime_Punchmech", {3, 4}).ok());
  CHECK(dead(b, v));
  CHECK(P(b, m).pilot_xp == 0);

  Board b2;
  const int32_t m2 = place(b2, "PunchMech", {3, 3}, true);
  const int32_t v2 = place(b2, "Scorpion1", {3, 4});
  hp(b2, v2, 1, 3);
  pilot(P(b2, m2), 0, 0, LevelSkill::Health, LevelSkill::Move);
  P(b2, m2).pilot_abilities |= kPilotExtraXp;
  REQUIRE(E.fire_weapon(b2, m2, "Prime_Punchmech", {3, 4}).ok());
  CHECK(P(b2, m2).pilot_xp == 5);
}

TEST_CASE("pilot xp: only shooters with id 0-2 are credited; the rest is split over the squad") {
  NEED_ENGINE();
  // Pawn ids 0-2 are the mechs natively. A shooter with a higher id (here a
  // fourth mech) sends the XP to every living mech with a pilot.
  Board b;
  const int32_t a = place(b, "PunchMech", {0, 0}, true);
  const int32_t c = place(b, "PunchMech", {0, 2}, true);
  const int32_t d = place(b, "PunchMech", {0, 4}, true);
  const int32_t e = place(b, "PunchMech", {3, 3}, true);
  REQUIRE(e == 3);
  const int32_t v = place(b, "Scorpion1", {3, 4});
  hp(b, v, 1, 3);
  pilot(P(b, a), 0, 0, LevelSkill::Health, LevelSkill::Move);
  pilot(P(b, c), 1, 10, LevelSkill::Health, LevelSkill::Move);
  // d has no pilot; e (the shooter) is not counted either.
  pilot(P(b, e), 0, 0, LevelSkill::Health, LevelSkill::Move);
  REQUIRE(E.fire_weapon(b, e, "Prime_Punchmech", {3, 4}).ok());
  CHECK(dead(b, v));
  // 3 XP over a, c and e: 1 each.
  CHECK(P(b, a).pilot_xp == 1);
  CHECK(P(b, c).pilot_xp == 11);
  CHECK(P(b, e).pilot_xp == 1);
  CHECK(P(b, d).pilot_xp == -1);
}

TEST_CASE("pilot xp: a Vek's kill in the enemy phase is the squad's XP; the remainder is a chance node") {
  NEED_ENGINE();
  // Two mechs with pilots; a 3 HP Vek killed by another Vek's shot gives 3
  // XP: 1 each and the remainder +1 to one of them. Mech a levels up with 2,
  // mech b does not care: two outcomes, the first without the level-up.
  auto build = [&](Board& b) {
    const int32_t a = place(b, "PunchMech", {0, 0}, true);
    const int32_t c = place(b, "PunchMech", {7, 7}, true);
    pilot(P(b, a), 0, 23, LevelSkill::Health, LevelSkill::Move);
    pilot(P(b, c), 0, 0, LevelSkill::Health, LevelSkill::Move);
    const int32_t victim = place(b, "Scorpion1", {4, 4});
    hp(b, victim, 1, 3);
    const int32_t shooter = place(b, "Firefly1", {4, 6});
    P(b, shooter).queued = QueuedShot{0, {4, 6}, {4, 5}};
    return std::pair{a, victim};
  };
  for (int outcome = 0; outcome < 2; ++outcome) {
    Board b;
    const auto [a, victim] = build(b);
    TurnContext tc;
    int nodes = 0;
    tc.choose = [&](const ChanceRecord& node) {
      if (node.kind == ChanceKind::XpSplit) {
        ++nodes;
        CHECK(node.options == 2);
        CHECK(node.amount == 3);
        return outcome;
      }
      return 0;
    };
    const PhaseResult r = E.end_turn(b, tc);
    REQUIRE(dead(b, victim));
    CHECK(nodes == 1);
    const Pawn& pa = P(b, a);
    if (outcome == 0) {
      CHECK(pa.pilot_level == 0);
      CHECK(pa.pilot_xp == 24);
      CHECK(P(b, 1).pilot_xp == 2);
    } else {
      CHECK(pa.pilot_level == 1);
      CHECK(pa.max_hp == 5);
      CHECK(P(b, 1).pilot_xp == 1);
    }
    bool listed = false;
    for (const ChanceRecord& c : r.chances) listed = listed || c.kind == ChanceKind::XpSplit;
    CHECK(listed);
  }
}

TEST_CASE("pilot xp: a kill heals with Viscera Nanobots, boosts a KO_Boost pilot, counts for Adrenaline") {
  NEED_ENGINE();
  Board b;
  const int32_t m = place(b, "PunchMech", {3, 3}, true);
  const int32_t v = place(b, "Scorpion1", {3, 4});
  hp(b, v, 1, 3);
  hp(b, m, 1, 3);
  pilot(P(b, m), 2, 0, LevelSkill::Adrenaline, LevelSkill::Move);
  P(b, m).pilot_abilities |= kPilotKoBoost;
  b.passives |= kPassiveLeechKill | kPassiveLeechKillA;
  const int move_before = base_move(b, P(b, m));
  REQUIRE(E.fire_weapon(b, m, "Prime_Punchmech", {3, 4}).ok());
  CHECK(dead(b, v));
  CHECK(P(b, m).hp == 3);
  CHECK(P(b, m).boosted);
  CHECK(base_move(b, P(b, m)) == move_before + 1);
}

// Vice Fist (Prime_Shift) throws the target behind the mech; its damage lands
// in a FULL_DELAY chunk with no animation, followed by AddBounce on that tile.
// The bounce keeps the tile busy for 0.5 s, so the shot is still the last
// shot when the victim's death is processed on the next frame: the kill is
// the Judo's. Without the bounce's busy time the last shot ended first.
TEST_CASE("pilot xp: a Vice Fist throw that kills heals with Viscera Nanobots") {
  NEED_ENGINE();
  Board b;
  const int32_t m = place(b, "JudoMech", {3, 3}, true);
  const int32_t v = place(b, "Mosquito1", {3, 4});
  hp(b, v, 1, 2);
  hp(b, m, 1, 3);
  b.passives |= kPassiveLeechKill;
  const ActionResult r = E.fire_weapon(b, m, "Prime_Shift", {3, 4});
  REQUIRE(r.ok());
  CHECK(dead(b, v));
  CHECK(P(b, m).pos == Point{3, 3});
  CHECK(P(b, m).hp == 2);
}

TEST_CASE("executor: Board:Bounce keeps the tile busy for 0.5 s") {
  NEED_ENGINE();
  auto frames = [&](const std::string& script, Point at) {
    Board b;
    place(b, "PunchMech", {0, 0}, true);
    if (at.valid()) place(b, "Dam_Pawn", at);
    SkillEffect fx;
    SpaceDamage sd;
    sd.script = script;
    fx.effect.push_back(sd);
    fx.owner = 0;
    const ResolveResult r = E.resolve(b, fx, WeaponInfo{}, ActionOptions{});
    return r.end_frame - r.start_frame;
  };
  const int64_t none = frames("", kInvalidPoint);
  const int64_t bounce = frames("Board:Bounce(Point(3,3),3)", kInvalidPoint);
  const int64_t half_second = FrameClock::steady().tracker_updates(0.5f);
  CHECK(half_second >= 30);
  CHECK(bounce - none == half_second);
  // Off the board: the dummy tile bounces and nothing waits.
  CHECK(frames("Board:Bounce(Point(-1,-1),3)", kInvalidPoint) == none);
  // A small bounce skips a tile whose first pawn spans two tiles (the dam);
  // a big one does not.
  CHECK(frames("Board:Bounce(Point(3,3),3)", {3, 3}) == none);
  CHECK(frames("Board:Bounce(Point(3,3),10)", {3, 3}) - none == half_second);
}

// ---- Recorded boards (live 2026-10-14, Unfair, Mission_Dam turn 3) ---------------------

TEST_CASE("pilot xp: the Judo's throw kill heals it with Viscera Nanobots (live)") {
  // Live 2026-10-14 run 23 (Unfair, m00 Mission_Dam, turn 3): JudoMech#0 on
  // G3 (4/5 HP, acid) throws Mosquito1 (1 HP) from G4 to G2 with
  // Prime_Shift; the NanoMech carries Passive_Leech. The game showed the
  // Judo at 5 HP; the engine had credited the kill to nobody (4 HP).
  NEED_ENGINE();
  const Recording before = live_fixture("live_dam_t3_step1.json");
  const Recording after = live_fixture("live_dam_t3_step2.json");
  const TurnContext ctx = turn_context(before, Visibility::Player);
  const int32_t judo = 0;
  REQUIRE(symbol_name(before.board.find_pawn(judo)->type) == "JudoMech");
  CHECK(before.board.find_pawn(judo)->hp == 4);
  CHECK(before.board.has_passive(kPassiveLeechKill));

  Board b = before.board;
  const ActionResult r = E.fire_weapon(b, judo, "Prime_Shift", {4, 1}, std::nullopt, action_options(ctx));
  REQUIRE(r.ok());
  CHECK(r.unapplied.empty());
  CHECK(P(b, judo).hp == 5);
  for (const Pawn& q : after.board.pawns()) {
    const Pawn* p = b.find_pawn(q.uid);
    REQUIRE(p != nullptr);
    CHECK_MESSAGE(p->hp == q.hp, symbol_name(q.type));
    CHECK(p->pos == q.pos);
  }
  CHECK(dead(b, 16175));
}

// ---- Recorded boards (live 2026-10-10, Hard, Mission_Survive turn 2) -----------------

TEST_CASE("pilot xp: the kill that levels Gana up gives DStrikeMech +2 HP at once (live)") {
  NEED_ENGINE();
  // DStrikeMech#1, pilot Pilot_Warrior level 1 with 46 XP (skill2 = Health),
  // fires Ranged_Defensestrike_A at D3 and kills Firefly2 (max HP 5): 51 XP,
  // level 2, +2 max HP and +2 HP. The game's next state shows HP 6.
  const Recording before = live_fixture("live_survive_t2_step3.json");
  const Recording after = live_fixture("live_survive_t2_step4.json");
  const TurnContext ctx = turn_context(before, Visibility::Player);
  const int32_t ds = 1;
  const Pawn& d0 = *before.board.find_pawn(ds);
  REQUIRE(symbol_name(d0.type) == "DStrikeMech");
  CHECK(d0.pilot_level == 1);
  CHECK(d0.pilot_xp == 46);
  CHECK(d0.pilot_skill2 == static_cast<int8_t>(LevelSkill::Health));
  CHECK(d0.hp == 4);
  CHECK(d0.max_hp == 4);

  Board b = before.board;
  const ActionResult r =
      E.fire_weapon(b, ds, "Ranged_Defensestrike_A", {5, 4}, std::nullopt, action_options(ctx));
  REQUIRE(r.ok());
  const Pawn& d1 = P(b, ds);
  CHECK(d1.pilot_level == 2);
  CHECK(d1.pilot_xp == 0);
  CHECK(d1.max_hp == 6);
  CHECK(d1.hp == 6);
  for (const Pawn& q : after.board.pawns()) {
    const Pawn* p = b.find_pawn(q.uid);
    REQUIRE(p != nullptr);
    CHECK_MESSAGE(p->hp == q.hp, symbol_name(q.type));
    CHECK(p->pos == q.pos);
  }

  // The game's state after the shot: HP is live, level, XP and max HP are
  // the save's (written at turn start). The loader applies the level-up the
  // HP above the saved maximum shows.
  const Pawn& d2 = *after.board.find_pawn(ds);
  CHECK(d2.hp == 6);
  CHECK(d2.max_hp == 6);
  CHECK(d2.pilot_level == 2);
  CHECK(d2.pilot_xp == 0);
}

// ---- Mission hooks inside a shot ------------------------------------------------------

// Mission_Dam:UpdateMission (missions/grass/mission_dam.lua) floods the two
// columns below a dead dam with a plain SkillEffect(): Board:AddEffect keeps
// its iOwner, -1, and Board::ApplyEffect leaves a recorded shooter in place
// for owner -1. The flood is queued while the shot that killed the dam is
// still on the stack (the shooter is only cleared once nothing is active),
// so the Vek it drowns are that mech's kills, not the squad's (ENV_EFFECT).
TEST_CASE("pilot xp: Vek drowned by the dam flood are the dam killer's kills") {
  NEED_ENGINE();
  Board b;
  b.tile({4, 0}).terrain = Terrain::Water;
  b.tile({5, 0}).terrain = Terrain::Water;
  const int32_t m = place(b, "ChargeMech", {4, 1}, true);
  const int32_t m2 = place(b, "PunchMech", {0, 7}, true);
  const int32_t m3 = place(b, "PunchMech", {1, 7}, true);
  const int32_t dam = place(b, "Dam_Pawn", {4, 0});
  const int32_t v1 = place(b, "Scorpion1", {5, 1});
  const int32_t v2 = place(b, "Scorpion1", {4, 6});
  REQUIRE(P(b, v1).max_hp == 3);
  pilot(P(b, m), 0, 0, LevelSkill::Health, LevelSkill::Move);
  pilot(P(b, m2), 0, 0, LevelSkill::Health, LevelSkill::Move);
  pilot(P(b, m3), 0, 0, LevelSkill::Health, LevelSkill::Move);
  TurnContext ctx;
  ctx.mission.mission_id = "Mission_Dam";
  const ActionResult r = E.fire_weapon(b, m, "Brute_Beetle", {4, 0}, std::nullopt, action_options(ctx));
  REQUIRE(r.ok());
  REQUIRE(r.mission_events.size() == 1);
  CHECK(dead(b, dam));
  CHECK(dead(b, v1));
  CHECK(dead(b, v2));
  CHECK(P(b, m).pilot_xp == 6);
  CHECK(P(b, m2).pilot_xp == 0);
  CHECK(P(b, m3).pilot_xp == 0);
}

TEST_CASE("pilot xp: the dam flood's kills level up the ChargeMech that broke the dam (live)") {
  // Live 2026-10-13 run 18 (Unfair, m04 Mission_Dam, turn 1): ChargeMech#1
  // on G4, pilot Pilot_Archive level 0 with 17 XP (skill1 Skilled), rams the
  // dam on H4 with Brute_Beetle (2 damage, 1 self damage: 3 -> 2 HP). The
  // flood drowns Burnbug1, Burnbug2 and Scarab1 (3 + 4 + 2 XP): 26 XP, level
  // 1, Skilled +2 max HP and +2 HP. The game showed 4 HP; with the flood's
  // kills split over the squad the engine had 2.
  NEED_ENGINE();
  const Recording before = live_fixture("live_dam_t1_step3.json");
  const Recording after = live_fixture("live_dam_t1_step4.json");
  const TurnContext ctx = turn_context(before, Visibility::Player);
  const int32_t cm = 1;
  const Pawn& c0 = *before.board.find_pawn(cm);
  REQUIRE(symbol_name(c0.type) == "ChargeMech");
  CHECK(c0.pilot_level == 0);
  CHECK(c0.pilot_xp == 17);
  CHECK(c0.pilot_skill1 == static_cast<int8_t>(LevelSkill::Skilled));
  CHECK(c0.hp == 3);

  Board b = before.board;
  const ActionResult r = E.fire_weapon(b, cm, "Brute_Beetle", {4, 0}, std::nullopt, action_options(ctx));
  REQUIRE(r.ok());
  const Pawn& c1 = P(b, cm);
  CHECK(c1.pilot_level == 1);
  CHECK(c1.pilot_xp == 0);
  CHECK(c1.max_hp == 5);
  CHECK(c1.hp == 4);
  // The other pilots got nothing from the flood.
  CHECK(P(b, 0).pilot_xp == before.board.find_pawn(0)->pilot_xp);
  CHECK(P(b, 2).pilot_xp == before.board.find_pawn(2)->pilot_xp);
  for (const Pawn& q : after.board.pawns()) {
    const Pawn* p = b.find_pawn(q.uid);
    REQUIRE(p != nullptr);
    CHECK_MESSAGE(p->hp == q.hp, symbol_name(q.type));
    CHECK(p->pos == q.pos);
  }
  CHECK(after.board.find_pawn(cm)->max_hp == 5);
}
