// The C++ weapon ports against the Lua scripts they mirror (a fast subset of
// `itb_inspect --diff-weapons`): every ported weapon table, fired by every
// pawn of a few random boards and of a recorded board at every tile, must
// give exactly what Lua gives. Skipped (with a warning) without a game
// install; set ITB_GAME_DIR to point at one.

#include <doctest/doctest.h>

#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "itb/game_data.hpp"
#include "itb/lua_host.hpp"
#include "itb/recording.hpp"
#include "itb/weapon_diff.hpp"

using namespace itb;

namespace {

struct Game {
  const GameData* data = nullptr;
  LuaHost* host = nullptr;
};

Game& game() {
  static Game g = [] {
    Game out;
    const auto root = GameData::default_game_root();
    if (root.empty() || !std::filesystem::exists(root / "scripts" / "scripts.lua")) return out;
    out.data = new GameData(GameData::load(root));
    out.host = LuaHost::create(root).release();
    return out;
  }();
  return g;
}

#define NEED_GAME()                                                                         \
  Game& G = game();                                                                         \
  if (!G.host) {                                                                            \
    WARN_MESSAGE(false, "no game install found; set ITB_GAME_DIR to run weapon port tests"); \
    return;                                                                                 \
  }                                                                                         \
  LuaHost& H = *G.host

void check(const WeaponDiffStats& s) {
  for (const std::string& e : s.examples) MESSAGE(e);
  CHECK(s.mismatches == 0);
}

}  // namespace

TEST_CASE("weapon ports: the hot weapon methods have ports") {
  NEED_GAME();
  for (const char* w : {"Move", "Skill_Repair", "Ranged_Rocket", "Science_Repulse", "Prime_Punchmech",
                        "Prime_Lightning", "Brute_Tankmech", "Ranged_Artillerymech", "Prime_Lasermech",
                        "BouncerAtk1", "FireflyAtk1", "ScarabAtk1", "HornetAtk1", "LeaperAtk1"}) {
    INFO(w);
    CHECK_FALSE(H.native_port(w, "GetSkillEffect").empty());
  }
  CHECK(H.native_port("Move", "GetTargetArea") == "weapons_base.lua:156");
  // A method without a port runs in Lua.
  CHECK(H.native_port("Disposal_Attack", "GetSkillEffect").empty());
  CHECK(ported_weapons(H).size() >= 100);
}

TEST_CASE("weapon ports: switched off, every call runs in Lua") {
  NEED_GAME();
  Board b;
  Pawn p = G.data->make_pawn(*G.data->pawn("PunchMech"), 0, {3, 3});
  p.mech = true;
  b.add_pawn(p);
  const Pawn& shooter = b.pawns().front();
  H.set_native_weapons(false);
  const uint64_t n0 = H.native_calls();
  const LuaSkillEffect lua = H.skill_effect_raw(b, shooter, "Prime_Punchmech", {3, 3}, {3, 2});
  CHECK(H.native_calls() == n0);
  H.set_native_weapons(true);
  const LuaSkillEffect native = H.skill_effect_raw(b, shooter, "Prime_Punchmech", {3, 3}, {3, 2});
  CHECK(H.native_calls() == n0 + 1);
  CHECK(lua == native);
  CHECK_FALSE(native.effect.empty());
}

TEST_CASE("weapon ports: identical to Lua on random boards") {
  NEED_GAME();
  const std::vector<std::string> weapons = ported_weapons(H);
  const std::vector<std::string> pool = H.weapon_ids();
  std::mt19937 rng(20261009);
  WeaponDiffStats total;
  for (int i = 0; i < 3; ++i) {
    const Board b = random_board(*G.data, pool, rng);
    for (const std::string& w : weapons) diff_weapon(H, b, w, total);
  }
  check(total);
  CHECK(total.effects > 10000);
  // Nearly every call is answered by a port (the rest fall back to Lua
  // where the Lua would raise, e.g. firing at the shooter's own tile).
  CHECK(total.native * 10 > (total.areas + total.effects) * 9);
}

TEST_CASE("weapon ports: identical to Lua on a recorded board") {
  NEED_GAME();
  std::string error;
  auto rec = load_recording(std::string(ITB_FIXTURE_DIR) + "/board_m07_turn01.json", G.data, &error);
  REQUIRE_MESSAGE(rec, error);
  WeaponDiffStats total;
  for (const std::string& w : ported_weapons(H)) diff_weapon(H, rec->board, w, total);
  check(total);
  CHECK(total.effects > 1000);
}

TEST_CASE("weapon ports: the deployable weapons and their tanks' shots") {
  NEED_GAME();
  const char* tables[] = {"DeploySkill_Tank",     "DeploySkill_Tank_AB",    "DeploySkill_AcidTank_B",
                          "DeploySkill_PullTank", "DeploySkill_ShieldTank", "DeploySkill_IceTank_A",
                          "Deploy_AcidTankShot",  "Deploy_AcidTankShot2",   "Deploy_ShieldTankShot",
                          "Deploy_ShieldTankShot2"};
  for (const char* w : tables) {
    INFO(w);
    CHECK_FALSE(H.native_port(w, "GetSkillEffect").empty());
  }
  CHECK(H.native_port("DeploySkill_Tank", "GetSkillEffect") == "weapons_base.lua:355");
  const std::vector<std::string> pool = H.weapon_ids();
  std::mt19937 rng(20261014);
  WeaponDiffStats total;
  for (int i = 0; i < 3; ++i) {
    const Board b = random_board(*G.data, pool, rng);
    for (const char* w : tables) diff_weapon(H, b, w, total);
  }
  check(total);
  CHECK(total.effects > 1000);
}
