#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "itb/movement.hpp"
#include "itb/recording.hpp"

using namespace itb;

namespace {
const std::string kFixture = std::string(ITB_FIXTURE_DIR) + "/board_m07_turn01.json";
}

TEST_CASE("loads a recorded bridge board") {
  std::string error;
  auto rec = load_recording(kFixture, nullptr, &error);
  REQUIRE_MESSAGE(rec.has_value(), error);

  CHECK(rec->run_id == "20260713_052159_731");
  CHECK(rec->mission_index == 7);
  CHECK(rec->turn == 1);
  CHECK(rec->mission_id == "Mission_Survive");
  CHECK(rec->phase == "combat_player");

  const Board& b = rec->board;
  CHECK(b.grid_power == 7);
  CHECK(b.grid_power_max == 7);
  CHECK(b.total_turns == 4);
  CHECK(b.pawns().size() == 11);
  REQUIRE(b.spawn_points.size() == 1);
  CHECK(b.spawn_points[0] == Point{6, 4});
  CHECK(rec->attack_order == std::vector<int32_t>{969, 970, 972, 973});

  const Tile& mountain = b.tile(Point{6, 0});
  CHECK(mountain.terrain == Terrain::Mountain);
  CHECK(mountain.hp == 2);
}

TEST_CASE("recorded units keep their live stats and queued attacks") {
  std::string error;
  auto rec = load_recording(kFixture, nullptr, &error);
  REQUIRE(rec.has_value());
  const Board& b = rec->board;

  const Pawn* punch = b.find_pawn(0);
  REQUIRE(punch != nullptr);
  CHECK(symbol_name(punch->type) == "PunchMech");
  CHECK(punch->pos == Point{3, 5});
  CHECK(punch->hp == 6);
  CHECK(punch->max_hp == 6);
  CHECK(punch->mech);
  CHECK(punch->massive);
  CHECK(punch->team == Team::Player);
  // Recorded base move 3, effective move 5: Skilled (level 2) +1 is read
  // live (level_skill_move), the other +1 stands.
  CHECK(punch->move == 3);
  CHECK(punch->movement.pilot_bonus == 1);
  CHECK(base_move(b, *punch) == 5);

  const Pawn* digger = b.find_pawn(969);
  REQUIRE(digger != nullptr);
  CHECK(symbol_name(digger->type) == "Digger2");
  CHECK(digger->team == Team::Enemy);
  CHECK(digger->queued.active());
  CHECK(digger->queued.target == Point{4, 3});
  CHECK(symbol_name(digger->weapons[0]) == "DiggerAtk2");

  // Player units come first in the board list.
  CHECK(b.pawns().front().team == Team::Player);
}

TEST_CASE("bridge quirks are corrected on load") {
  const auto path = std::filesystem::temp_directory_path() / "itb_recording_quirks.json";
  {
    std::ofstream out(path);
    out << R"({"tiles": [
        {"x": 1, "y": 1, "terrain": "lava", "terrain_id": 5},
        {"x": 2, "y": 2, "terrain": "lava", "terrain_id": 3, "lava": true},
        {"x": 3, "y": 3, "terrain": "ground", "terrain_id": 0}],
      "units": [
        {"uid": 0, "type": "PunchMech", "x": 3, "y": 3, "hp": 5, "max_hp": 3, "team": 1, "mech": true,
         "move": 0, "base_move": 3, "web": true, "web_source_uid": 9,
         "weapons": ["Prime_Punchmech", "Passive_ForceAmp"], "pilot_id": "Pilot_Rock"},
        {"uid": 9, "type": "Scorpion1", "x": 3, "y": 4, "hp": 3, "max_hp": 3, "team": 6}],
      "teleporter_pairs": [[0, 0, 7, 7]]})";
  }
  std::string error;
  auto rec = load_recording(path, nullptr, &error);
  std::filesystem::remove(path);
  REQUIRE_MESSAGE(rec.has_value(), error);
  const Board& b = rec->board;
  // Bridges before 2026-05-04 named ice (id 5) "lava"; real lava is water + flag.
  CHECK(b.tile({1, 1}).terrain == Terrain::Ice);
  CHECK_FALSE(b.tile({1, 1}).lava);
  CHECK(b.tile({2, 2}).terrain == Terrain::Water);
  CHECK(b.tile({2, 2}).lava);
  const Pawn& mech = *b.find_pawn(0);
  CHECK(mech.max_hp == 5);                   // base Health was reported as max_hp
  CHECK(mech.movement.pilot_bonus == 0);     // GetMoveSpeed() read 0 while webbed
  // No queued attack shows the web: the reported source's tile webs it.
  CHECK(b.tile({3, 4}).web_out == 1u << static_cast<int>(Dir::Up));
  CHECK(b.tile({3, 3}).web_in == 1);
  CHECK(b.has_passive(kPassiveForceAmp));    // squad passives from the mechs' weapons
  REQUIRE(rec->pilots.size() == 1);
  CHECK(rec->pilots[0].second == "Pilot_Rock");
  REQUIRE(b.teleporters.size() == 2);
  CHECK(b.teleporters[1] == Point{7, 7});
  CHECK(b.tile({0, 0}).teleporter);
  CHECK(b.teleporter_occupants == std::vector<int32_t>{-1, -1});
}

TEST_CASE("the bridge's grappled probe wins over its web guess") {
  // "grappled" is Pawn:IsGrappled; "web" is a legacy guess that missed spider
  // eggs (live 2026-10-10 m26). Without "grappled", "web" is read.
  const auto path = std::filesystem::temp_directory_path() / "itb_recording_grappled.json";
  {
    std::ofstream out(path);
    out << R"({"tiles": [], "units": [
        {"uid": 0, "type": "PunchMech", "x": 3, "y": 3, "hp": 3, "max_hp": 3, "team": 1, "mech": true,
         "web": false, "grappled": true},
        {"uid": 1, "type": "PunchMech", "x": 5, "y": 5, "hp": 3, "max_hp": 3, "team": 1, "mech": true,
         "web": true, "grappled": false},
        {"uid": 2, "type": "PunchMech", "x": 0, "y": 0, "hp": 3, "max_hp": 3, "team": 1, "mech": true,
         "web": true},
        {"uid": 9, "type": "WebbEgg1", "x": 2, "y": 3, "hp": 1, "max_hp": 1, "team": 6}]})";
  }
  std::string error;
  auto rec = load_recording(path, nullptr, &error);
  std::filesystem::remove(path);
  REQUIRE_MESSAGE(rec.has_value(), error);
  const Board& b = rec->board;
  CHECK(b.find_pawn(0)->webbed);
  CHECK_FALSE(b.find_pawn(1)->webbed);
  CHECK(b.find_pawn(2)->webbed);
  // The egg next to the webbed mech is its source.
  CHECK(b.tile({2, 3}).web_out == 1u << static_cast<int>(Dir::Right));
  CHECK(b.tile({3, 3}).web_in == 1);
  CHECK(b.tile({5, 5}).web_in == 0);
}

TEST_CASE("a unit's moved flag is loaded when the bridge reports it") {
  const auto path = std::filesystem::temp_directory_path() / "itb_recording_moved.json";
  {
    std::ofstream out(path);
    out << R"({"tiles": [], "units": [
        {"uid": 1, "type": "PunchMech", "x": 2, "y": 2, "hp": 3, "max_hp": 3, "team": 1, "mech": true,
         "active": true, "moved": true},
        {"uid": 2, "type": "PunchMech", "x": 3, "y": 3, "hp": 3, "max_hp": 3, "team": 1, "mech": true,
         "active": true}]})";
  }
  std::string error;
  auto rec = load_recording(path, nullptr, &error);
  std::filesystem::remove(path);
  REQUIRE_MESSAGE(rec.has_value(), error);
  CHECK(rec->board.find_pawn(1)->moved);
  CHECK_FALSE(rec->board.find_pawn(2)->moved);
}

TEST_CASE("rejects files that are not boards") {
  std::string error;
  auto rec = load_recording(std::string(ITB_FIXTURE_DIR) + "/does_not_exist.json", nullptr, &error);
  CHECK_FALSE(rec.has_value());
  CHECK(error.find("cannot open") != std::string::npos);
}

namespace {

std::optional<Recording> load_text(const std::string& name, const std::string& text, std::string* error) {
  const auto path = std::filesystem::temp_directory_path() / name;
  {
    std::ofstream out(path);
    out << text;
  }
  auto rec = load_recording(path, nullptr, error);
  std::filesystem::remove(path);
  return rec;
}

// A board in the bridge extension's format (bridge_ext_version 1).
const char* kExtBoard = R"({
  "turn": 2, "total_turns": 5, "grid_power": 6, "mission_id": "Mission_Crack", "phase": "combat_player",
  "bridge_ext_version": 1,
  "bridge_errors": [{"where": "tile.IsDangerous", "error": "boom"}],
  "tiles": [
    {"x": 0, "y": 0, "terrain": "building", "terrain_id": 1, "building_hp": 1, "populated": false},
    {"x": 1, "y": 0, "terrain": "building", "terrain_id": 1, "building_hp": 2},
    {"x": 2, "y": 0, "terrain": "ice", "terrain_id": 5, "ice_hp": 1},
    {"x": 3, "y": 0, "terrain": "ground", "terrain_id": 0, "custom_tile": "conveyor2.png"}],
  "units": [
    {"uid": 0, "type": "PunchMech", "x": 3, "y": 3, "hp": 3, "max_hp": 3, "team": 1, "mech": true, "active": true,
     "weapons": ["Prime_Punchmech", "Brute_Heavyrocket"],
     "weapons_exact": ["Prime_Punchmech_B", "Brute_Heavyrocket_A"],
     "weapon_slots": [{"slot": 0, "id": "Prime_Punchmech_B", "limited": 0, "uses_saved": 1},
                      {"slot": 1, "id": "Brute_Heavyrocket_A", "limited": 1, "uses": 0, "uses_saved": 1}],
     "pilot_id": "Pilot_Original", "pilot_level": 1, "pilot_skills": [],
     "pilot": {"id": "Pilot_Original", "level": 2, "xp": 31, "skill1": 0, "skill2": 7}},
    {"uid": 1, "type": "LimitMech", "x": 4, "y": 3, "hp": 3, "max_hp": 3, "team": 1, "mech": true,
     "weapons_exact": ["Ranged_Limited"],
     "weapon_slots": [{"slot": 0, "id": "Ranged_Limited", "limited": 2, "uses_ambiguous": true, "uses_saved": 2}],
     "pilot": {"id": "Pilot_Rock", "level": 0, "xp": 3, "skill1": 12, "skill2": 12}},
    {"uid": 50, "type": "Firefly1", "x": 5, "y": 5, "hp": 3, "max_hp": 3, "team": 6, "mutation": 1,
     "has_queued_attack": true, "queued_target": [5, 4], "queued_origin": [5, 5]},
    {"uid": 51, "type": "MysteryVek", "x": 6, "y": 5, "hp": 2, "max_hp": 2, "team": 6, "mutation": 0,
     "traits": {"leader": 1, "explodes": true, "ignore_smoke": true, "burns": true, "minor": true}},
    {"uid": 60, "type": "Train_Pawn", "x": 4, "y": 6, "hp": 1, "max_hp": 1, "team": 1, "weapons": ["Train_Move"],
     "extra_spaces": [[0, 1]],
     "queued_any": {"skill": 1, "target": [4, 5], "origin": [4, 6], "source": "save"}},
    {"uid": 60, "type": "Train_Pawn", "x": 4, "y": 7, "hp": 1, "max_hp": 1, "team": 1, "is_extra_tile": true}],
  "attack_order": [50],
  "attack_order_all": [50, 60],
  "spawning_tiles": [[1, 6], [6, 3]],
  "spawn_queue": [{"type": "Scorpion1", "x": 6, "y": 3, "uid": 70}, {"type": "Firefly2", "x": 1, "y": 6, "uid": 71}],
  "spawn_queue_source": "save",
  "spawn_queue_matches_markers": true,
  "mission_power_start": 7,
  "mission_blocked_spawns": 2,
  "bonus_objective_ids": [5],
  "zones": {"enemy": [[6, 0], [7, 1]], "deployment": [[1, 3]]},
  "mission_state": {"key": 3, "native_key": "Mission3", "id": "Mission_Crack", "turn": 2, "turn_limit": 5,
    "class_chain": ["Mission_Crack", "Mission"], "env_class_chain": ["Env_Seismic", "Env_Attack", "Environment"],
    "instance": {"ID": "Mission_Crack", "BlockedSpawns": 2},
    "env_instance": {"Locations": [{"x": 2, "y": 4}, {"x": 3, "y": 4}, {"x": 4, "y": 4}],
                     "Planned": [{"x": 2, "y": 4}, {"x": 3, "y": 4}, {"x": 4, "y": 4}]}},
  "env_strike_log": [{"kind": "env_step", "turn": 1, "current_attack": {"x": 1, "y": 4}},
                     {"kind": "env_step", "turn": 1, "current_attack": [{"x": 2, "y": 2}, {"x": 2, "y": 3}]}]
})";

}  // namespace

TEST_CASE("loads the bridge extension fields") {
  std::string error;
  auto rec = load_text("itb_recording_ext.json", kExtBoard, &error);
  REQUIRE_MESSAGE(rec.has_value(), error);
  const Board& b = rec->board;
  CHECK(rec->bridge_ext_version == 1);
  CHECK(rec->bridge_errors == std::vector<std::string>{"tile.IsDangerous: boom"});

  // Tiles.
  CHECK_FALSE(b.tile({0, 0}).populated);
  CHECK(b.tile({1, 0}).populated);
  CHECK(b.tile({2, 0}).hp == 1);
  CHECK(symbol_name(b.tile({3, 0}).custom_tile) == "conveyor2.png");

  // Weapons, uses, pilots.
  const Pawn& m0 = *b.find_pawn(0);
  CHECK(symbol_name(m0.weapons[0]) == "Prime_Punchmech_B");
  CHECK(symbol_name(m0.weapons[1]) == "Brute_Heavyrocket_A");
  CHECK(m0.uses[0] == -1);
  CHECK(m0.uses[1] == 0);
  CHECK(m0.has_pilot(kPilotThick));  // skill2 = 7 at level 2
  const Pawn& m1 = *b.find_pawn(1);
  CHECK(m1.uses[0] == 2);                  // ambiguous live count: the saved one
  CHECK_FALSE(m1.has_pilot(kPilotRegen));  // level 0: no skill yet
  REQUIRE(rec->pilot_info.size() == 2);
  CHECK(rec->pilot_info[0].xp == 31);
  CHECK(rec->pilot_info[1].id == "Pilot_Rock");
  CHECK(rec->pilots.size() == 2);

  // Mutation and traits.
  CHECK(b.find_pawn(50)->health_bonus);
  const Pawn& mystery = *b.find_pawn(51);
  CHECK(mystery.leader == Leader::Health);
  CHECK(mystery.explodes);
  CHECK(mystery.ignore_smoke);
  CHECK(mystery.burns);
  CHECK(mystery.minor);

  // The train is loaded once, with its rear tile (ExtraSpaces {(0,1)}).
  int trains = 0;
  for (const Pawn& p : b.pawns()) trains += p.uid == 60 ? 1 : 0;
  CHECK(trains == 1);
  CHECK(b.find_pawn(60)->extra_tile() == Point{4, 7});
  CHECK(b.pawn_at(Point{4, 7}) == b.find_pawn(60));
  for (const std::string& w : rec->warnings) CHECK_MESSAGE(w.find("extra") == std::string::npos, w);
  CHECK_FALSE(mystery.health_bonus);

  // Every queued shot, any team.
  const Pawn& train = *b.find_pawn(60);
  CHECK(train.pos == Point{4, 6});
  CHECK(train.queued.active());
  CHECK(train.queued.weapon == 0);
  CHECK(train.queued.target == Point{4, 5});
  CHECK(rec->attack_order_all == std::vector<int32_t>{50, 60});
  CHECK(rec->mission.all_queued_known);

  // Spawns in queue order; their types stay out of the player's context.
  CHECK(b.spawn_points == std::vector<Point>{{6, 3}, {1, 6}});
  CHECK(rec->spawn_types == std::vector<std::string>{"Scorpion1", "Firefly2"});
  CHECK(rec->spawn_order_known);
  const TurnContext player = turn_context(*rec);
  CHECK(player.spawn_types.empty());
  CHECK_FALSE(player.spawn_order_known);
  CHECK(player.mission.mission_key == 3);
  const TurnContext full = turn_context(*rec, Visibility::Full);
  CHECK(full.spawn_types == rec->spawn_types);
  CHECK(full.spawn_order_known);

  // Mission.
  const MissionData& m = rec->mission;
  CHECK(m.turn_limit == 5);
  CHECK(m.mission_classes.front() == "Mission_Crack");
  CHECK(m.env_classes.front() == "Env_Seismic");
  CHECK(m.ordered_locations == std::vector<Point>{{2, 4}, {3, 4}, {4, 4}});
  CHECK(m.mission_instance_json.find("BlockedSpawns") != std::string::npos);
  CHECK(m.env_instance_json.find("Planned") != std::string::npos);
  REQUIRE(m.zones.count("enemy") == 1);
  CHECK(m.zones.at("enemy") == std::vector<Point>{{6, 0}, {7, 1}});
  CHECK(m.objectives.power_start == 7);
  CHECK(m.objectives.blocked_spawns == 2);
  CHECK(m.objectives.bonus == std::vector<int>{5});
  REQUIRE(rec->env_strikes.size() == 2);
  CHECK(rec->env_strikes[0].tiles == std::vector<Point>{{1, 4}});
  CHECK(rec->env_strikes[1].tiles.size() == 2);
}

TEST_CASE("a spawn queue that disagrees with the markers only gives types by tile") {
  std::string error;
  auto rec = load_text("itb_recording_stale_queue.json", R"({
      "tiles": [], "units": [],
      "spawning_tiles": [[1, 6], [6, 3], [2, 2]],
      "spawn_queue": [{"type": "Scorpion1", "x": 6, "y": 3}, {"type": "Firefly2", "x": 1, "y": 6},
                      {"type": "Hornet1", "x": 0, "y": 0}],
      "spawn_queue_matches_markers": false})",
                       &error);
  REQUIRE_MESSAGE(rec.has_value(), error);
  CHECK(rec->board.spawn_points == std::vector<Point>{{1, 6}, {6, 3}, {2, 2}});
  CHECK(rec->spawn_types == std::vector<std::string>{"Firefly2", "Scorpion1", ""});
  CHECK_FALSE(rec->spawn_order_known);
  CHECK(std::find(rec->warnings.begin(), rec->warnings.end(),
                  "spawn_queue does not match the spawn markers (types by tile only)") != rec->warnings.end());
}

TEST_CASE("unpowered weapons cannot fire and unpowered passives do nothing") {
  std::string error;
  auto rec = load_text("itb_recording_powered.json", R"({
      "tiles": [], "units": [
        {"uid": 0, "type": "PunchMech", "x": 1, "y": 1, "hp": 3, "max_hp": 3, "team": 1, "mech": true, "moved": true,
         "weapons_exact": ["Prime_Punchmech", "Passive_ForceAmp"],
         "weapon_slots": [{"id": "Prime_Punchmech", "limited": 0, "powered": false},
                          {"id": "Passive_ForceAmp", "limited": 0, "powered": false}]},
        {"uid": 1, "type": "PunchMech", "x": 2, "y": 1, "hp": 3, "max_hp": 3, "team": 1, "mech": true,
         "weapons_exact": ["Prime_Punchmech", "Passive_FlameImmune"],
         "weapon_slots": [{"id": "Prime_Punchmech", "limited": 0, "powered": true},
                          {"id": "Passive_FlameImmune", "limited": 0, "powered": true}]}]})",
                       &error);
  REQUIRE_MESSAGE(rec.has_value(), error);
  const Board& b = rec->board;
  CHECK(b.find_pawn(0)->uses[0] == 0);
  CHECK(b.find_pawn(0)->moved);
  CHECK(b.find_pawn(1)->uses[0] == -1);
  CHECK_FALSE(b.has_passive(kPassiveForceAmp));
  CHECK(b.has_passive(kPassiveFlameImmune));
}

TEST_CASE("Mission_Reactivation's visible list marks its pawns; the mission's own list is ignored") {
  // `visible`: live_play's reactivation_visible_list; the bridge's
  // instance.Enemies (the mission's hidden list) is always [7] here.
  auto board = [](const std::string& visible) {
    return R"({"mission_id": "Mission_Reactivation", "tiles": [], "units": [
        {"uid": 7, "type": "Snowtank1", "x": 5, "y": 4, "hp": 1, "max_hp": 1, "team": 6, "frozen": true},
        {"uid": 8, "type": "Leaper2", "x": 5, "y": 5, "hp": 3, "max_hp": 3, "team": 6, "frozen": true}],
      "mission_state": {"instance": {"Enemies": [7]}})" + visible + "}";
  };
  std::string error;
  auto rec = load_text("itb_recording_reactivation.json", board(R"(, "reactivation_visible_list": [8, 99])"), &error);
  REQUIRE_MESSAGE(rec.has_value(), error);
  CHECK(rec->mission.reactivation_known);
  CHECK_FALSE(rec->board.find_pawn(7)->reactivation_listed);
  CHECK(rec->board.find_pawn(8)->reactivation_listed);

  rec = load_text("itb_recording_reactivation.json", board(R"(, "reactivation_visible_list": [])"), &error);
  REQUIRE_MESSAGE(rec.has_value(), error);
  CHECK(rec->mission.reactivation_known);
  CHECK_FALSE(rec->board.find_pawn(7)->reactivation_listed);
  CHECK_FALSE(rec->board.find_pawn(8)->reactivation_listed);

  // No visible list (old recordings): the hidden one is not used.
  rec = load_text("itb_recording_reactivation.json", board(""), &error);
  REQUIRE_MESSAGE(rec.has_value(), error);
  CHECK_FALSE(rec->mission.reactivation_known);
  CHECK_FALSE(rec->board.find_pawn(7)->reactivation_listed);
}

TEST_CASE("boards without the extension fields load as before") {
  std::string error;
  auto rec = load_recording(kFixture, nullptr, &error);
  REQUIRE(rec.has_value());
  CHECK(rec->bridge_ext_version == 0);
  CHECK(rec->attack_order_all.empty());
  CHECK_FALSE(rec->mission.all_queued_known);
  CHECK(rec->spawn_types.empty());
  CHECK_FALSE(rec->spawn_order_known);
  CHECK(rec->mission.ordered_locations.empty());
  for (const Pawn& p : rec->board.pawns()) {
    for (int8_t u : p.uses) CHECK(u == -1);
  }
  CHECK_FALSE(rec->board_busy);
}

TEST_CASE("a state dumped while the board was busy is flagged") {
  const auto path = std::filesystem::temp_directory_path() / "itb_recording_busy.json";
  auto load = [&](const std::string& extra) {
    {
      std::ofstream out(path);
      out << R"({"tiles": [], "units": [{"uid": 1, "type": "PunchMech", "x": 2, "y": 2, "hp": 3,
                 "max_hp": 3, "team": 1, "mech": true, "active": true}])"
          << extra << "}";
    }
    std::string error;
    auto rec = load_recording(path, nullptr, &error);
    std::filesystem::remove(path);
    REQUIRE_MESSAGE(rec.has_value(), error);
    return *rec;
  };
  auto mentions_busy = [](const Recording& r) {
    return std::any_of(r.warnings.begin(), r.warnings.end(),
                       [](const std::string& w) { return w.find("board was busy") != std::string::npos; });
  };
  // Mid-animation (live 2026-10-09): Ranged_Ignite's side pushes still running.
  const Recording busy = load(R"(, "board_busy": true, "busy_state": 1, "stable": false)");
  CHECK(busy.board_busy);
  CHECK(mentions_busy(busy));
  // A bridge command still waiting for its effects.
  CHECK(load(R"(, "board_busy": false, "busy_state": 0, "command_waiting": true, "stable": false)").board_busy);
  // Settled.
  const Recording settled = load(R"(, "board_busy": false, "busy_state": 0, "command_waiting": false, "stable": true)");
  CHECK_FALSE(settled.board_busy);
  CHECK_FALSE(mentions_busy(settled));
}
