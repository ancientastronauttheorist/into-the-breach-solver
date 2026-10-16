// Runs the game's own scripts from the local install. Skipped (with a
// warning) when no install is configured; set ITB_GAME_DIR to point at one.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "itb/game_data.hpp"
#include "itb/movement.hpp"
#include "itb/recording.hpp"

using namespace itb;

namespace {

const GameData* game_data(ScriptLoadReport* report_out = nullptr) {
  static ScriptLoadReport report;
  static const GameData* data = [] {
    const auto root = GameData::default_game_root();
    if (root.empty() || !std::filesystem::exists(root / "scripts" / "scripts.lua")) {
      return static_cast<GameData*>(nullptr);
    }
    return new GameData(GameData::load(root, &report));
  }();
  if (report_out) *report_out = report;
  return data;
}

}  // namespace

TEST_CASE("game scripts load and register pawns") {
  ScriptLoadReport report;
  const GameData* data = game_data(&report);
  if (!data) {
    WARN_MESSAGE(false, "no game install found; set ITB_GAME_DIR to run game-data tests");
    return;
  }
  INFO("scripts ok " << report.files_ok << ", failed " << report.files_failed);
  for (const auto& e : report.errors) MESSAGE(e);
  CHECK(report.files_failed == 0);
  CHECK(report.files_ok > 100);
  // Build 21601364 registers 145 pawns (every live AddPawn in the script list;
  // four more sit inside comment blocks).
  CHECK(data->pawns().size() >= 145);
  // Stubs must only ever replace native bindings, never Lua definitions.
  CHECK(report.overwritten_stubs.empty());
}

TEST_CASE("mech definitions") {
  const GameData* data = game_data();
  if (!data) return;
  const PawnDef* punch = data->pawn("PunchMech");
  REQUIRE(punch != nullptr);
  CHECK(punch->pawn_class == "Prime");
  CHECK(punch->health == 3);
  CHECK(punch->move_speed == 3);
  CHECK(punch->massive);
  CHECK(punch->default_team == Team::Player);
  REQUIRE(punch->skills.size() == 1);
  CHECK(punch->skills[0] == "Prime_Punchmech");
}

TEST_CASE("Vek definitions inherit Pawn defaults") {
  const GameData* data = game_data();
  if (!data) return;
  const PawnDef* firefly = data->pawn("Firefly1");
  REQUIRE(firefly != nullptr);
  CHECK(firefly->health == 3);
  CHECK(firefly->move_speed == 2);
  CHECK(firefly->ranged == 1);
  CHECK(firefly->default_team == Team::Enemy);
  CHECK(firefly->pushable);       // inherited default
  CHECK_FALSE(firefly->flying);   // inherited default
  CHECK_FALSE(firefly->massive);

  const PawnDef* hornet = data->pawn("Hornet1");
  REQUIRE(hornet != nullptr);
  CHECK(hornet->flying);
  CHECK(hornet->large_shield);
  CHECK(hornet->move_speed == 5);
}

TEST_CASE("Advanced Edition and mission-file pawns are present") {
  const GameData* data = game_data();
  if (!data) return;
  const PawnDef* mosquito = data->pawn("Mosquito1");
  REQUIRE(mosquito != nullptr);
  CHECK(mosquito->health == 2);
  CHECK(mosquito->move_speed == 4);
  CHECK(mosquito->flying);
  CHECK(mosquito->default_team == Team::Enemy);
  // Defined in scripts/missions/sand/mission_filler.lua.
  CHECK(data->pawn("Filler_Pawn") != nullptr);
}

TEST_CASE("pawns declared with Pawn:new and derived pawns are discovered") {
  const GameData* data = game_data();
  if (!data) return;
  // `GuardMech = Pawn:new{...}` never calls AddPawn.
  const PawnDef* guard = data->pawn("GuardMech");
  REQUIRE(guard != nullptr);
  CHECK(guard->health == 3);
  CHECK(guard->move_speed == 4);
  CHECK(guard->massive);
  REQUIRE(guard->skills.size() == 1);
  CHECK(guard->skills[0] == "Prime_ShieldBash");

  // Jelly_Explode1 derives from Jelly_Health1, which derives from Pawn.
  const PawnDef* psion = data->pawn("Jelly_Explode1");
  REQUIRE(psion != nullptr);
  CHECK(psion->leader == Leader::Explode);
  CHECK(psion->flying);  // inherited from Jelly_Health1
  CHECK(psion->default_team == Team::Enemy);
}

TEST_CASE("make_pawn copies the definition") {
  const GameData* data = game_data();
  if (!data) return;
  const PawnDef* hornet = data->pawn("Hornet1");
  REQUIRE(hornet != nullptr);
  const Pawn p = data->make_pawn(*hornet, 42, Point{2, 3});
  CHECK(p.uid == 42);
  CHECK(p.pos == Point{2, 3});
  CHECK(p.hp == 2);
  CHECK(p.max_hp == 2);
  CHECK(p.flying);
  CHECK(p.team == Team::Enemy);
  CHECK(symbol_name(p.weapons[0]) == "HornetAtk1");
}

TEST_CASE("animation definitions and tuning values") {
  const GameData* data = game_data();
  if (!data) {
    WARN_MESSAGE(false, "no game install found; set ITB_GAME_DIR to run game-data tests");
    return;
  }
  // A Vek's death animation is "<Image>d".
  const PawnDef* firefly = data->pawn("Firefly1");
  REQUIRE(firefly);
  CHECK(firefly->image == "firefly");
  const AnimDef* death = data->animation(firefly->image + "d");
  REQUIRE(death);
  CHECK(death->num_frames == 8);
  CHECK(death->time == doctest::Approx(0.14f));
  CHECK_FALSE(death->loop);
  // Inherited fields resolve through the Animation base class.
  const AnimDef* push = data->animation("explopush1_2");
  REQUIRE(push);
  CHECK(push->num_frames == 9);
  CHECK(push->time == doctest::Approx(0.06f));
  // A Frames list sets the frame count; Lengths the per-frame times.
  const AnimDef* look = data->animation("Pilot_Look");
  REQUIRE(look);
  CHECK(look->num_frames == 5);
  REQUIRE(look->lengths.size() == 5);
  CHECK(look->frame_length(4) == doctest::Approx(1.2f));
  CHECK(data->animation("no_such_animation") == nullptr);
  // Values (game.lua).
  CHECK(data->value("x_velocity", 0) == doctest::Approx(0.7f));
  CHECK(data->value("y_velocity", 0) == doctest::Approx(18.0f));
  CHECK(data->value("gravity", 0) == doctest::Approx(3.0f));
  CHECK(data->value("laser_length", 0) == doctest::Approx(0.5f));
  CHECK(data->value("no_such_value", -1) == -1);
}

TEST_CASE("pilot skills come from the pilot tables") {
  const GameData* data = game_data();
  if (!data) return;
  CHECK(data->pilot_skill("Pilot_Youth") == "Youth_Move");
  CHECK(data->pilot_ability("Pilot_Youth") == kPilotYouthMove);
  CHECK(data->pilot_ability("Pilot_Arrogant") == kPilotArrogantBoost);  // AE pilot
  CHECK(data->pilot_ability("Pilot_Rock") == kPilotRockSkill);
  CHECK(data->pilot_ability("Pilot_Original") == kPilotExtraXp);
  CHECK(data->pilot_ability("Pilot_Caretaker") == kPilotAdjacentHeal);  // AE pilot Rosie Rivets
  CHECK(data->pilot_skill("Pilot_Rust").empty());  // a recruit
  CHECK(data->pilot_ability("Pilot_Rust") == kPilotNone);
  CHECK(data->pilot_ability("no_such_pilot") == kPilotNone);
  CHECK(pilot_skill_ability("Thick") == kPilotThick);
}

TEST_CASE("a recorded Youth_Move speed is not counted twice") {
  // Live 2026-10-13 m30 turn 1 (Mission_Belt): Lily Reed's FourwayMech
  // (MoveSpeed 4) reported move 8 = 4 + 1 standing + 3 Youth_Move. The loader
  // took out the live terms before the pilot's ability was known, so the
  // callers' pilot_ability made it 11 and the solver planned a 10-tile move
  // the game refused.
  const GameData* data = game_data();
  if (!data) return;
  auto load = [&](int turn, int move) {
    const auto path = std::filesystem::temp_directory_path() / "itb_recording_youth.json";
    {
      std::ofstream out(path);
      out << R"({"turn": )" << turn << R"(, "total_turns": 4, "tiles": [], "units": [
          {"uid": 2, "type": "FourwayMech", "x": 0, "y": 0, "hp": 4, "max_hp": 4, "team": 1, "mech": true,
           "move": )" << move << R"(, "base_move": )" << move << R"(, "pilot_id": "Pilot_Youth",
           "pilot": {"id": "Pilot_Youth", "level": 1, "xp": 21, "skill1": 11, "skill2": 13}}]})";
    }
    std::string error;
    auto rec = load_recording(path, data, &error);
    std::filesystem::remove(path);
    REQUIRE_MESSAGE(rec.has_value(), error);
    return *rec;
  };
  {
    Recording rec = load(1, 8);
    Pawn& p = *rec.board.find_pawn(2);
    CHECK(p.has_pilot(kPilotYouthMove));
    CHECK(p.movement.pilot_bonus == 1);
    CHECK(base_move(rec.board, p) == 8);
    // What every caller does next (solve, live, replay): no change.
    p.pilot_abilities |= data->pilot_ability(rec.pilots.at(0).second);
    CHECK(base_move(rec.board, p) == 8);
    const TileMask area = move_area(rec.board, p);
    CHECK(mask_has(area, Point{4, 4}));        // 8 steps
    CHECK_FALSE(mask_has(area, Point{4, 5}));  // 9 steps
    // The next turn start drops Youth_Move.
    start_turn_movement(rec.board, 2);
    CHECK(base_move(rec.board, *rec.board.find_pawn(2)) == 5);
  }
  {
    Recording rec = load(2, 5);
    const Pawn& p = *rec.board.find_pawn(2);
    CHECK(p.movement.pilot_bonus == 1);
    CHECK(base_move(rec.board, p) == 5);
  }
}
