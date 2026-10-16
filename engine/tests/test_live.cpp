// itb_live (tools/live_tool.hpp): the live driver's JSON requests, and the
// archived live-session bridge states under recordings/live/.

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "itb/engine.hpp"
#include "itb/game_data.hpp"
#include "itb/recording.hpp"
#include "itb/tile_rules.hpp"
#include "live_tool.hpp"

namespace fs = std::filesystem;
using nlohmann::json;
using namespace itb;

namespace {

const fs::path kLiveDir = fs::path(ITB_REPO_ROOT) / "recordings" / "live";
const fs::path kCaveT2 = kLiveDir / "2026-10-09" / "Mission_Final_Cave_t2_start_1791602065398.json";

bool have_game() {
  const auto root = GameData::default_game_root();
  return !root.empty() && fs::exists(root / "scripts" / "scripts.lua");
}

tools::LiveSession* session() {
  static std::unique_ptr<tools::LiveSession> s = [] {
    return have_game() ? std::make_unique<tools::LiveSession>(GameData::default_game_root(), 1) : nullptr;
  }();
  return s.get();
}

#define NEED_SESSION()                                                                    \
  if (!session()) {                                                                       \
    WARN_MESSAGE(false, "no game install found; set ITB_GAME_DIR to run engine tests"); \
    return;                                                                               \
  }                                                                                       \
  tools::LiveSession& S = *session()

// Files scripts/live_play.py writes next to the bridge states.
bool is_state_file(const fs::path& p) {
  const std::string n = p.filename().string();
  if (p.extension() != ".json") return false;
  for (const char* suffix : {"_prediction.json", "_solve.json", "manifest.json", "_last_prediction.json"}) {
    if (n.ends_with(suffix)) return false;
  }
  return true;
}

// A dump taken after a mission ended (live_play records the state it sees
// once the enemy phase finishes the mission): no combat board to check.
bool mission_over_dump(const fs::path& p) {
  const json j = json::parse(std::ifstream(p), nullptr, false);
  if (j.is_discarded()) return false;
  const json* s = &j;
  if (s->contains("data")) s = &(*s)["data"];
  if (s->contains("bridge_state")) s = &(*s)["bridge_state"];
  return s->value("in_active_mission", true) == false;
}

std::set<int> uids(const json& board) {
  std::set<int> out;
  for (const json& u : board["units"]) out.insert(u["uid"].get<int>());
  return out;
}

fs::path temp_file(const std::string& name, const json& content) {
  const fs::path p = fs::temp_directory_path() / name;
  std::ofstream(p) << content.dump();
  return p;
}

}  // namespace

TEST_CASE("live: every archived live-session bridge state loads") {
  REQUIRE(fs::is_directory(kLiveDir));
  std::unique_ptr<GameData> data;
  if (have_game()) data = std::make_unique<GameData>(GameData::load(GameData::default_game_root()));
  int loaded = 0, session_2026_10_09 = 0;
  for (const auto& e : fs::recursive_directory_iterator(kLiveDir)) {
    if (!e.is_regular_file() || !is_state_file(e.path())) continue;
    if (mission_over_dump(e.path())) continue;
    std::string error;
    const auto rec = load_recording(e.path(), data.get(), &error);
    CHECK_MESSAGE(rec.has_value(), e.path().string() << ": " << error);
    if (!rec) continue;
    ++loaded;
    CHECK_MESSAGE(!rec->mission_id.empty(), e.path().string());
    CHECK_MESSAGE(!rec->board.pawns().empty(), e.path().string());
    if (e.path().parent_path().filename() == "2026-10-09") ++session_2026_10_09;
  }
  CHECK(session_2026_10_09 == 78);
  MESSAGE("live states loaded: " << loaded);
}

TEST_CASE("live: board, solve and predict on a live state") {
  NEED_SESSION();
  const json board = S.handle({{"cmd", "board"}, {"state", kCaveT2.string()}});
  REQUIRE_MESSAGE(board["ok"].get<bool>(), board.dump());
  CHECK(board["mission_id"] == "Mission_Final_Cave");
  CHECK(board["turn"] == 2);
  CHECK(board["active_units"] == 3);
  const json& units = board["board"]["units"];
  REQUIRE(units.size() >= 3);
  CHECK(std::is_sorted(units.begin(), units.end(),
                       [](const json& a, const json& b) { return a["uid"].get<int>() < b["uid"].get<int>(); }));
  for (const char* key : {"uid", "type", "x", "y", "hp", "team", "mech", "fire", "acid", "frozen", "shield", "web"}) {
    CHECK(units[0].contains(key));
  }

  const json solved = S.handle({{"cmd", "solve"}, {"state", kCaveT2.string()}, {"time_limit", 3.0}});
  REQUIRE_MESSAGE(solved["ok"].get<bool>(), solved.dump());
  CHECK(solved["format"] == tools::kLiveFormat);
  CHECK(solved["searched"] == true);
  for (const char* key : {"worst_case", "upper_bound", "proven_optimal", "proven_components", "chance_exact",
                          "timed_out", "contingent", "stats", "warnings", "plan", "steps", "start", "after_player",
                          "after_enemy", "enemy_phase", "predicted_score"}) {
    CHECK_MESSAGE(solved.contains(key), key);
  }
  CHECK(solved["refused"] == -1);
  CHECK(solved["worst_case"].contains("grid"));
  const json& plan = solved["plan"];
  REQUIRE_FALSE(plan.empty());
  // One step per move that changes tile and per weapon or repair, in order,
  // each with the predicted board after it.
  size_t expected = 0;
  for (const json& a : plan) {
    if (a["kind"] != "none") ++expected;
  }
  const json& steps = solved["steps"];
  CHECK(steps.size() >= expected);
  CHECK(steps.size() <= expected + plan.size());
  for (size_t i = 0; i < steps.size(); ++i) {
    const json& s = steps[i];
    CHECK(s["index"] == i);
    CHECK(s["status"] == "ok");
    CHECK(s["board"].contains("units"));
    const std::string sub = s["sub"];
    CHECK((sub == "move" || sub == "weapon" || sub == "repair"));
    if (sub == "weapon") CHECK(s["weapon_index"].get<int>() >= 0);
  }

  // The same plan through `predict` gives the same boards.
  const json predicted = S.handle({{"cmd", "predict"}, {"state", kCaveT2.string()}, {"plan", plan}});
  REQUIRE_MESSAGE(predicted["ok"].get<bool>(), predicted.dump());
  CHECK(predicted["steps"].size() == steps.size());
  CHECK(predicted["after_enemy"] == solved["after_enemy"]);
  CHECK(predicted["after_player"] == solved["after_player"]);
  CHECK_FALSE(predicted.contains("worst_case"));
}

TEST_CASE("live: the solver never sees the hidden spawn types") {
  NEED_SESSION();
  std::ifstream in(kCaveT2);
  json state = json::parse(in);
  // A bridge that exports the spawn queue: types for every marker.
  json queue = json::array();
  for (const json& p : state["spawning_tiles"]) queue.push_back({{"type", "Scorpion1"}, {"x", p[0]}, {"y", p[1]}});
  REQUIRE(queue.size() == 3);
  state["spawn_queue"] = queue;
  state["spawn_queue_matches_markers"] = true;
  const fs::path path = temp_file("itb_live_test_spawn_queue.json", state);

  const json r = S.handle({{"cmd", "predict"}, {"state", path.string()}, {"plan", json::array()}});
  REQUIRE_MESSAGE(r["ok"].get<bool>(), r.dump());
  const std::set<int> before = uids(r["start"]);
  for (const json& u : r["after_enemy"]["units"]) {
    CHECK_MESSAGE(!(before.count(u["uid"].get<int>()) == 0 && u["type"] == "Scorpion1"), u.dump());
  }
  CHECK_FALSE(r["enemy_phase"]["emerged_unknown"].empty());

  // With full information the same board would get the Scorpions: the test
  // above is meaningful.
  const std::unique_ptr<Engine> owned = Engine::create(GameData::default_game_root());
  Engine& engine = *owned;
  std::string error;
  auto rec = load_recording(path, &engine.data(), &error);
  REQUIRE_MESSAGE(rec, error);
  Board b = rec->board;
  engine.end_turn(b, turn_context(*rec, Visibility::Full));
  int scorpions = 0;
  for (const Pawn& p : b.pawns()) {
    if (p.alive() && symbol_name(p.type) == "Scorpion1") ++scorpions;
  }
  CHECK(scorpions > 0);
  fs::remove(path);
}

TEST_CASE("live: a moved flag in the state keeps the unit from moving again") {
  NEED_SESSION();
  std::ifstream in(kCaveT2);
  json state = json::parse(in);
  int mech = -1;
  for (json& u : state["units"]) {
    if (u["team"] == 1 && u.value("mech", false) && u.value("active", false)) {
      u["moved"] = true;
      mech = u["uid"];
      break;
    }
  }
  REQUIRE(mech >= 0);
  const fs::path path = temp_file("itb_live_test_moved.json", state);
  const json r = S.handle({{"cmd", "solve"}, {"state", path.string()}, {"time_limit", 2.0}});
  REQUIRE_MESSAGE(r["ok"].get<bool>(), r.dump());
  for (const json& s : r["steps"]) CHECK_FALSE((s["sub"] == "move" && s["uid"] == mech));
  fs::remove(path);
}

// The fuzzer's move list (scripts/fuzz_play.py): every sub-action listed
// replays through predict to the board a one-step prefix gives, and a unit
// that moved and fired is offered nothing more.
TEST_CASE("live: actions lists the sub-actions predict accepts") {
  NEED_SESSION();
  const json r = S.handle({{"cmd", "actions"}, {"state", kCaveT2.string()}});
  REQUIRE_MESSAGE(r["ok"].get<bool>(), r.dump());
  CHECK(r["refused"] == -1);
  CHECK(r["board"] == S.handle({{"cmd", "board"}, {"state", kCaveT2.string()}})["board"]);
  REQUIRE(r["units"].size() >= 3);
  const json& acts = r["actions"];
  REQUIRE_FALSE(acts.empty());
  std::set<std::string> subs;
  json first_move, first_weapon;
  for (const json& a : acts) {
    subs.insert(a["sub"].get<std::string>());
    for (const char* key : {"uid", "weapon", "slot", "target", "target2", "target_kind", "end", "effect"}) {
      CHECK_MESSAGE(a.contains(key), key);
    }
    CHECK(a["effect"].contains("hp"));
    if (a["sub"] == "move" && first_move.is_null()) first_move = a;
    if (a["sub"] == "weapon" && first_weapon.is_null()) first_weapon = a;
  }
  CHECK(subs.count("move"));
  CHECK(subs.count("weapon"));
  REQUIRE_FALSE(first_move.is_null());
  REQUIRE_FALSE(first_weapon.is_null());

  auto as_plan = [](const json& a) {
    if (a["sub"] == "move") return json{{"uid", a["uid"]}, {"move", a["target"]}, {"kind", "none"}};
    return json{{"uid", a["uid"]}, {"move", nullptr}, {"kind", a["sub"]}, {"weapon", a["weapon"]},
                {"target", a["target"]}, {"target2", a["target2"]}};
  };
  for (const json& a : {first_move, first_weapon}) {
    const json plan = json::array({as_plan(a)});
    const json p = S.handle({{"cmd", "predict"}, {"state", kCaveT2.string()}, {"plan", plan}});
    REQUIRE_MESSAGE(p["ok"].get<bool>(), p.dump());
    CHECK(p["refused"] == -1);
    REQUIRE(p["steps"].size() == 1);
    const json next = S.handle({{"cmd", "actions"}, {"state", kCaveT2.string()}, {"plan", plan}, {"effects", false}});
    REQUIRE(next["ok"].get<bool>());
    CHECK(next["board"] == p["steps"][0]["board"]);
    for (const json& b : next["actions"]) CHECK_FALSE(b.contains("effect"));
  }

  // The weapon's unit moved first, then fired: nothing more for it.
  const int uid = first_weapon["uid"];
  json mover;
  for (const json& a : acts) {
    if (a["sub"] == "move" && a["uid"] == uid) {
      mover = a;
      break;
    }
  }
  if (!mover.is_null()) {
    const json plan = json::array({as_plan(mover)});
    const json after_move = S.handle({{"cmd", "actions"}, {"state", kCaveT2.string()}, {"plan", plan}});
    json shot;
    for (const json& a : after_move["actions"]) {
      CHECK_FALSE((a["uid"] == uid && a["sub"] == "move"));
      if (a["uid"] == uid && a["sub"] == "weapon" && shot.is_null()) shot = a;
    }
    if (!shot.is_null()) {
      json both = plan;
      both.push_back(as_plan(shot));
      const json done = S.handle({{"cmd", "actions"}, {"state", kCaveT2.string()}, {"plan", both}});
      for (const json& a : done["actions"]) CHECK(a["uid"] != uid);
    }
  }

  // A prefix the engine refuses says so.
  json bad = json::array({json{{"uid", 9999}, {"move", nullptr}, {"kind", "weapon"}, {"weapon", "Prime_Punchmech"},
                               {"target", json::array({0, 0})}}});
  const json refused = S.handle({{"cmd", "actions"}, {"state", kCaveT2.string()}, {"plan", bad}});
  REQUIRE(refused["ok"].get<bool>());
  CHECK(refused["refused"] == 0);
  CHECK(refused["actions"].empty());
}

namespace {

const json* board_unit(const json& board, int uid) {
  for (const json& u : board["units"]) {
    if (u["uid"] == uid) return &u;
  }
  return nullptr;
}

}  // namespace

TEST_CASE("live: boards carry each unit's queued shot; smoke clears it (live 2026-10-10 m36 turn 2)") {
  NEED_SESSION();
  // Mission_Filler turn 2. DStrikeMech's Defensestrike pushes Firefly2#941
  // into smoke; JetMech's Aerial Bombs smoke F3 under Beetle2#943; GravMech
  // then pulls the Beetle out to E3. Both shots are gone for good (Pawn::OnLoop
  // clears a smoked Vek's queued shot); Scarab2#942's only moves with it.
  const std::string start = std::string(ITB_FIXTURE_DIR) + "/live_filler_t2_solve_input.json";
  const json plan = json::array({
      {{"uid", 1}, {"move", {1, 4}}, {"kind", "weapon"}, {"weapon", "Ranged_Defensestrike_A"}, {"target", {4, 4}}},
      {{"uid", 0}, {"move", {5, 3}}, {"kind", "weapon"}, {"weapon", "Brute_Jetmech_A"}, {"target", {5, 1}}},
      {{"uid", 2}, {"move", {5, 4}}, {"kind", "weapon"}, {"weapon", "Science_Gravwell"}, {"target", {5, 2}}},
  });
  const json r = S.handle({{"cmd", "predict"}, {"state", start}, {"plan", plan}});
  REQUIRE_MESSAGE(r["ok"].get<bool>(), r.dump());
  REQUIRE(r["refused"] == -1);
  const json* beetle = board_unit(r["start"], 943);
  REQUIRE(beetle);
  CHECK((*beetle)["queued"]["target"] == json::array({4, 2}));
  CHECK((*beetle)["queued"]["weapon"] == 0);
  const json* mech = board_unit(r["start"], 0);
  REQUIRE(mech);
  CHECK((*mech)["queued"].is_null());

  const json& steps = r["steps"];
  REQUIRE(steps.size() == 6);
  // Step 1: the Defensestrike; step 3: the Aerial Bombs; step 5: the Gravwell.
  CHECK((*board_unit(steps[1]["board"], 941))["queued"].is_null());
  CHECK_FALSE((*board_unit(steps[1]["board"], 943))["queued"].is_null());
  CHECK((*board_unit(steps[3]["board"], 943))["queued"].is_null());
  const json* pulled = board_unit(steps[5]["board"], 943);
  REQUIRE(pulled);
  CHECK(((*pulled)["x"] == 5 && (*pulled)["y"] == 3));
  CHECK((*pulled)["queued"].is_null());
  const json* scarab = board_unit(steps[5]["board"], 942);
  REQUIRE(scarab);
  CHECK((*scarab)["queued"]["target"] == json::array({4, 3}));

  // The bridge's snapshot before End Turn still has the Beetle's shot (read
  // from the save). As exported, the engine charges the Beetle into the
  // Earth Mover; with the shot dropped (what scripts/live_play.py now gives
  // it) the Beetle blocks the E3 spawn and dies, and the Filler_Pawn lives,
  // as in the game.
  std::ifstream in(std::string(ITB_FIXTURE_DIR) + "/live_filler_t2_end_turn.json");
  json wrapped = json::parse(in);
  const fs::path stale = temp_file("itb_live_test_filler_stale.json", wrapped);
  const json before = S.handle({{"cmd", "predict"}, {"state", stale.string()}, {"plan", json::array()}});
  REQUIRE_MESSAGE(before["ok"].get<bool>(), before.dump());
  CHECK(board_unit(before["after_enemy"], 943) != nullptr);
  CHECK(board_unit(before["after_enemy"], 938) == nullptr);
  for (json& u : wrapped["data"]["bridge_state"]["units"]) {
    if (u["uid"] == 943 || u["uid"] == 941) {
      u["has_queued_attack"] = false;
      u.erase("queued_target");
      u.erase("queued_origin");
    }
  }
  const fs::path fixed = temp_file("itb_live_test_filler_fixed.json", wrapped);
  const json after = S.handle({{"cmd", "predict"}, {"state", fixed.string()}, {"plan", json::array()}});
  REQUIRE_MESSAGE(after["ok"].get<bool>(), after.dump());
  CHECK(board_unit(after["after_enemy"], 943) == nullptr);
  CHECK(board_unit(after["after_enemy"], 938) != nullptr);
  fs::remove(stale);
  fs::remove(fixed);
}

TEST_CASE("live: a building's shield loads from the state and boards export it (live 20261014_175032 m14 t1)") {
  NEED_SESSION();
  // Unfair, Mission_Survive turn 1. The Defense Mech on D4 fires Science_Shield
  // at F4: F4 and the tile beyond it, G4, are shielded. In the enemy phase the
  // Scarab's shot on F4 and the Digger's tusks on G4 only pop the shields: the
  // game kept grid 2, F4 and G4 at 2 HP. The bridge cannot read a structure's
  // shield on vanilla, so its End Turn snapshot has bare buildings; read as
  // exported, the engine lost G4 and 1 HP of F4 (grid 0).
  const std::string start = std::string(ITB_FIXTURE_DIR) + "/live_survive_t1_solve_input.json";
  const json plan = json::array({
      {{"uid", 2}, {"move", {4, 4}}, {"kind", "weapon"}, {"weapon", "Science_Shield"}, {"target", {4, 2}}},
      {{"uid", 1}, {"move", {3, 1}}, {"kind", "weapon"}, {"weapon", "Brute_PierceShot"}, {"target", {3, 2}}},
      {{"uid", 0}, {"move", {3, 6}}, {"kind", "weapon"}, {"weapon", "Prime_Lasermech"}, {"target", {3, 7}}},
  });
  const json r = S.handle({{"cmd", "predict"}, {"state", start}, {"plan", plan}});
  REQUIRE_MESSAGE(r["ok"].get<bool>(), r.dump());
  REQUIRE(r["refused"] == -1);
  CHECK(r["start"]["shield_tiles"].empty());
  const json shields = json::array({json::array({4, 1}), json::array({4, 2})});
  CHECK(r["steps"][1]["board"]["shield_tiles"] == shields);
  CHECK(r["after_player"]["shield_tiles"] == shields);
  CHECK(r["after_enemy"]["shield_tiles"].empty());  // both popped
  CHECK(r["after_enemy"]["grid_power"] == 2);

  auto building_hp = [](const json& board, int x, int y) {
    for (const json& b : board["buildings"]) {
      if (b["x"] == x && b["y"] == y) return b["hp"].get<int>();
    }
    return 0;
  };
  std::ifstream in(std::string(ITB_FIXTURE_DIR) + "/live_survive_t1_end_turn.json");
  json wrapped = json::parse(in);
  const fs::path bare = temp_file("itb_live_test_survive_bare.json", wrapped);
  const json before = S.handle({{"cmd", "predict"}, {"state", bare.string()}, {"plan", json::array()}});
  REQUIRE_MESSAGE(before["ok"].get<bool>(), before.dump());
  CHECK(before["after_enemy"]["grid_power"] == 0);
  CHECK(building_hp(before["after_enemy"], 4, 1) == 0);
  CHECK(building_hp(before["after_enemy"], 4, 2) == 1);
  // With the shields (scripts/live_play.py carries the engine's forward as
  // the tiles' "shield"), the prediction is the game's.
  for (json& t : wrapped["data"]["bridge_state"]["tiles"]) {
    if (t["x"] == 4 && (t["y"] == 1 || t["y"] == 2)) t["shield"] = true;
  }
  const fs::path fixed = temp_file("itb_live_test_survive_fixed.json", wrapped);
  const json after = S.handle({{"cmd", "predict"}, {"state", fixed.string()}, {"plan", json::array()}});
  REQUIRE_MESSAGE(after["ok"].get<bool>(), after.dump());
  CHECK(after["start"]["shield_tiles"] == shields);
  CHECK(after["after_enemy"]["grid_power"] == 2);
  CHECK(building_hp(after["after_enemy"], 4, 1) == 2);
  CHECK(building_hp(after["after_enemy"], 4, 2) == 2);
  CHECK(after["after_enemy"]["shield_tiles"].empty());
  fs::remove(bare);
  fs::remove(fixed);
}

TEST_CASE("live: deploy searches the drop zone of a deployed board (live 20261012_231559 m00)") {
  NEED_SESSION();
  const std::string state = std::string(ITB_FIXTURE_DIR) + "/live_volatile_t0_deployed.json";
  // Given placements (uid order): the heuristic's and another, two AI seeds
  // each, tiny solves.
  const json placements = json::array({json::array({{2, 2}, {2, 5}, {3, 1}}), json::array({{3, 2}, {3, 6}, {2, 1}})});
  const json r = S.handle({{"cmd", "deploy"},
                           {"state", state},
                           {"placements", placements},
                           {"samples", 2},
                           {"time_limit", 0},
                           {"solve_time", 0},
                           {"solve_nodes", 20}});
  REQUIRE_MESSAGE(r["ok"].get<bool>(), r.dump());
  CHECK(r["cmd"] == "deploy");
  CHECK(r["mechs"] == json::array({0, 1, 2}));
  CHECK(r["zone"].size() == 14);
  REQUIRE(r["finalists"].size() == 2);
  const json& best = r["best"];
  CHECK((best["tiles"] == placements[0] || best["tiles"] == placements[1]));
  CHECK(best["by_uid"]["0"] == best["tiles"][0]);
  CHECK(best["visual"].size() == 3);
  CHECK(best["value"]["seeds"] == 2);
  CHECK(best["value"]["worst_case"].contains("grid"));
  CHECK(best["value"]["mean"].contains("mechs"));
  CHECK(r["finalists"][0]["tiles"] == best["tiles"]);
  // Malformed placements and a board without mechs are errors, not crashes.
  const json bad = S.handle({{"cmd", "deploy"}, {"state", state}, {"placements", json::array({json::array({{2, 2}})})}});
  CHECK_FALSE(bad["ok"].get<bool>());
  const json none = S.handle({{"cmd", "deploy"}, {"state", std::string(ITB_FIXTURE_DIR) + "/board_m07_turn01.json"}});
  CHECK_FALSE(none["ok"].get<bool>());
}

TEST_CASE("live: the dam's second tile is occupied (live 2026-10-10 m46 turn 1)") {
  NEED_SESSION();
  // Mission_Dam: Dam_Pawn#2335 at H4 (4,0) with ExtraSpaces {(1,0)} also
  // stands on H3 (5,0); the bridge lists that tile as a second entry with
  // "is_extra_tile". The solver planned InfernoMech#0 onto H3 and the game
  // refused the move.
  const std::string start = std::string(ITB_FIXTURE_DIR) + "/live_dam_t1_solve_input.json";
  const GameData data = GameData::load(GameData::default_game_root());
  std::string error;
  auto rec = load_recording(start, &data, &error);
  REQUIRE_MESSAGE(rec.has_value(), error);
  const Pawn* dam = rec->board.find_pawn(2335);
  REQUIRE(dam);
  CHECK(dam->extra_tile() == Point{5, 0});
  CHECK(rec->board.pawn_at({5, 0}) == dam);
  for (const std::string& w : rec->warnings) CHECK_MESSAGE(w.find("extra") == std::string::npos, w);
  int dams = 0;
  for (const Pawn& p : rec->board.pawns()) dams += p.uid == 2335 ? 1 : 0;
  CHECK(dams == 1);

  const json bad = json::array({{{"uid", 0}, {"move", {5, 0}}}});
  const json r = S.handle({{"cmd", "predict"}, {"state", start}, {"plan", bad}});
  REQUIRE_MESSAGE(r["ok"].get<bool>(), r.dump());
  CHECK(r["refused"] == 0);
  // The board export lists the dam once.
  int listed = 0;
  for (const json& u : r["start"]["units"]) listed += u["uid"] == 2335 ? 1 : 0;
  CHECK(listed == 1);

  // The plan the engine now finds: from G3 (5,1), flame H3 (5,0), which
  // hits the dam.
  const json good = json::array({
      {{"uid", 0}, {"move", {5, 1}}, {"kind", "weapon"}, {"weapon", "Prime_Flamespreader"}, {"target", {5, 0}}},
  });
  const json r2 = S.handle({{"cmd", "predict"}, {"state", start}, {"plan", good}});
  REQUIRE_MESSAGE(r2["ok"].get<bool>(), r2.dump());
  CHECK(r2["refused"] == -1);
  int hp = -1;
  for (const json& u : r2["after_player"]["units"]) {
    if (u["uid"] == 2335) hp = u["hp"].get<int>();
  }
  CHECK(hp < 2);
}

namespace {

bool unit_webbed(const json& board, int uid) {
  for (const json& u : board["units"]) {
    if (u["uid"] == uid) return u.value("web", false);
  }
  FAIL("unit " << uid << " not on the board");
  return false;
}

constexpr uint8_t dir_bit(Dir d) { return static_cast<uint8_t>(1u << static_cast<int>(d)); }

}  // namespace

TEST_CASE("live: a mech webbed by two Scorpions stays webbed when one is swapped away (live 2026-10-10 m10 t2)") {
  NEED_SESSION();
  // Mission_Train turn 2: InfernoMech#0 at F3 (5,2) is webbed by
  // Scorpion1#2391 at G3 (5,1) and Scorpion2#2392 at E3 (5,3), both queued
  // on F3. The bridge names one source (2391). TeleMech#2 moves to H3 (5,0)
  // and swaps with the Scorpion at G3: that web breaks, the other holds; the
  // game kept the mech webbed.
  const std::string start = std::string(ITB_FIXTURE_DIR) + "/live_train_t2_solve_input.json";
  const GameData data = GameData::load(GameData::default_game_root());
  std::string error;
  auto rec = load_recording(start, &data, &error);
  REQUIRE_MESSAGE(rec.has_value(), error);
  const Board& b = rec->board;
  REQUIRE(b.find_pawn(0)->webbed);
  CHECK(b.tile({5, 2}).web_in == 2);
  CHECK(b.tile({5, 1}).web_out == dir_bit(Dir::Down));
  CHECK(b.tile({5, 3}).web_out == dir_bit(Dir::Up));
  CHECK(web_sources(b, {5, 2}) == (dir_bit(Dir::Up) | dir_bit(Dir::Down)));

  const json plan = json::array(
      {{{"uid", 2}, {"move", {5, 0}}, {"kind", "weapon"}, {"weapon", "Science_Swap"}, {"target", {5, 1}}}});
  const json r = S.handle({{"cmd", "predict"}, {"state", start}, {"plan", plan}});
  REQUIRE_MESSAGE(r["ok"].get<bool>(), r.dump());
  CHECK(r["refused"] == -1);
  CHECK(unit_webbed(r["after_player"], 0));
  CHECK_FALSE(unit_webbed(r["after_player"], 2));
}

TEST_CASE("live: the train is webbed through its second tile (live 2026-10-10 m10 t3)") {
  NEED_SESSION();
  // Mission_Train turn 3: Train_Pawn#2390 at F4 (4,2) also stands on F5
  // (4,3). Scorpion2#2392 at F6 (4,4) is queued on F5 and webs the train
  // through that tile; the bridge guessed Scorpion1#2391 at F3 (4,1), which
  // is queued elsewhere. IgniteMech#1 fires Ranged_Ignite at F4, pushing the
  // Scorpion at F3 away: the game kept the train webbed.
  const std::string start = std::string(ITB_FIXTURE_DIR) + "/live_train_t3_solve_input.json";
  const GameData data = GameData::load(GameData::default_game_root());
  std::string error;
  auto rec = load_recording(start, &data, &error);
  REQUIRE_MESSAGE(rec.has_value(), error);
  const Board& b = rec->board;
  REQUIRE(b.find_pawn(2390)->webbed);
  CHECK(b.tile({4, 3}).web_in == 1);
  CHECK(b.tile({4, 4}).web_out == dir_bit(Dir::Up));
  CHECK(b.tile({4, 1}).web_out == 0);
  CHECK(b.tile({4, 2}).web_in == 0);
  // TeleMech#2 at G4 (5,2) is webbed by Scorpion1#2405 at G5 (5,3).
  CHECK(b.tile({5, 3}).web_out == dir_bit(Dir::Up));

  const json plan = json::array(
      {{{"uid", 1}, {"move", {2, 2}}, {"kind", "weapon"}, {"weapon", "Ranged_Ignite"}, {"target", {4, 2}}}});
  const json r = S.handle({{"cmd", "predict"}, {"state", start}, {"plan", plan}});
  REQUIRE_MESSAGE(r["ok"].get<bool>(), r.dump());
  CHECK(r["refused"] == -1);
  CHECK(unit_webbed(r["after_player"], 2390));
  CHECK_FALSE(unit_webbed(r["after_player"], 2));  // pushed off its tile
}

// Spider eggs (live 2026-10-10 m26, Mission_AcidStorm). SpiderAtk1/2 land a
// WebbEgg1 and AddGrapple from the egg tile to each neighbour in the same
// effect; BoardSpace::SetGrappled(d) (@0091b300) only webs a neighbour that
// is grappleable then, and OnLoop breaks a web whose held tile empties. So an
// egg webs who stood next to it when it landed, never a newcomer, and the
// web goes when the egg leaves its tile. The bridge's "web" missed every egg
// web (a legacy guess); "grappled" (Pawn:IsGrappled) is the game's flag.
TEST_CASE("live: an egg webs who was next to it on landing, not newcomers (live 2026-10-10 m26 t1)") {
  NEED_SESSION();
  // IgniteMech#1 at D5 (3,4) was next to WebbEgg1#2906 when it landed on D6
  // (2,4): grappled, though the bridge's web says no.
  const std::string start = std::string(ITB_FIXTURE_DIR) + "/live_acidstorm_egg_t1_solve_input.json";
  const GameData data = GameData::load(GameData::default_game_root());
  std::string error;
  auto rec = load_recording(start, &data, &error);
  REQUIRE_MESSAGE(rec.has_value(), error);
  const Board& b = rec->board;
  REQUIRE(b.find_pawn(1)->webbed);
  CHECK_FALSE(b.find_pawn(0)->webbed);
  CHECK(b.tile({2, 4}).web_out == dir_bit(Dir::Right));
  CHECK(b.tile({3, 4}).web_in == 1);
  CHECK(unit_webbed(S.handle({{"cmd", "board"}, {"state", start}})["board"], 1));

  // InfernoMech#0 walks to E6 (2,3), next to the egg, and punches E5; then
  // TeleMech#2 walks to C6 (2,5), next to the egg, and swaps with it. In
  // game neither newcomer was webbed, IgniteMech stayed webbed until the
  // swap moved the egg off D6, and then nobody was webbed.
  const json plan = json::array(
      {{{"uid", 0}, {"move", {2, 3}}, {"kind", "weapon"}, {"weapon", "Prime_Punchmech_B"}, {"target", {3, 3}}},
       {{"uid", 2}, {"move", {2, 5}}, {"kind", "weapon"}, {"weapon", "Science_Swap"}, {"target", {2, 4}}}});
  const json r = S.handle({{"cmd", "predict"}, {"state", start}, {"plan", plan}});
  REQUIRE_MESSAGE(r["ok"].get<bool>(), r.dump());
  CHECK(r["refused"] == -1);
  const json& steps = r["steps"];
  REQUIRE(steps.size() == 4);
  for (size_t i = 0; i < 3; ++i) {
    CAPTURE(i);
    CHECK_FALSE(unit_webbed(steps[i]["board"], 0));
    CHECK(unit_webbed(steps[i]["board"], 1));
    CHECK_FALSE(unit_webbed(steps[i]["board"], 2));
  }
  for (int uid : {0, 1, 2}) {
    CAPTURE(uid);
    CHECK_FALSE(unit_webbed(r["after_player"], uid));
  }
}

TEST_CASE("live: a mech next to a freshly laid egg is webbed (live 2026-10-10 m26 t2, t4)") {
  NEED_SESSION();
  // Turn 2: WebbEgg1#2947 landed on E7 (1,3) next to InfernoMech#0 at E6
  // (2,3). IgniteMech#1's Ranged_Ignite on F6 pushes the mech to D6: free.
  const std::string t2 = std::string(ITB_FIXTURE_DIR) + "/live_acidstorm_egg_t2_step1.json";
  const json board2 = S.handle({{"cmd", "board"}, {"state", t2}});
  REQUIRE_MESSAGE(board2["ok"].get<bool>(), board2.dump());
  CHECK(unit_webbed(board2["board"], 0));
  const json plan2 = json::array({{{"uid", 1}, {"kind", "weapon"}, {"weapon", "Ranged_Ignite_A"}, {"target", {2, 2}}},
                                  {{"uid", 0},
                                   {"move", {4, 5}},
                                   {"kind", "weapon"},
                                   {"weapon", "Prime_Flamespreader"},
                                   {"target", {6, 5}}}});
  const json r2 = S.handle({{"cmd", "predict"}, {"state", t2}, {"plan", plan2}});
  REQUIRE_MESSAGE(r2["ok"].get<bool>(), r2.dump());
  CHECK(r2["refused"] == -1);
  REQUIRE(r2["steps"].size() == 3);
  CHECK_FALSE(unit_webbed(r2["steps"][0]["board"], 0));
  CHECK_FALSE(unit_webbed(r2["after_player"], 0));

  // Turn 4: WebbEgg1#3065 landed on C7 (1,5) next to InfernoMech#0 at C6
  // (2,5). The mech fires in place, IgniteMech moves and fires: the web
  // holds all turn (the game's IsGrappled stayed true).
  const std::string t4 = std::string(ITB_FIXTURE_DIR) + "/live_acidstorm_egg_t4_step1.json";
  const GameData data = GameData::load(GameData::default_game_root());
  std::string error;
  auto rec = load_recording(t4, &data, &error);
  REQUIRE_MESSAGE(rec.has_value(), error);
  REQUIRE(rec->board.find_pawn(0)->webbed);
  CHECK(rec->board.tile({1, 5}).web_out == dir_bit(Dir::Right));
  const json plan4 = json::array(
      {{{"uid", 1}, {"move", {0, 5}}, {"kind", "weapon"}, {"weapon", "Ranged_Ignite_A"}, {"target", {4, 5}}}});
  const json r4 = S.handle({{"cmd", "predict"}, {"state", t4}, {"plan", plan4}});
  REQUIRE_MESSAGE(r4["ok"].get<bool>(), r4.dump());
  CHECK(r4["refused"] == -1);
  for (const json& step : r4["steps"]) CHECK(unit_webbed(step["board"], 0));
  CHECK(unit_webbed(r4["after_player"], 0));
}

TEST_CASE("live: bad requests answer ok=false") {
  NEED_SESSION();
  CHECK(S.handle({{"cmd", "ping"}})["ok"] == true);
  CHECK(S.handle({{"cmd", "dance"}})["ok"] == false);
  CHECK(S.handle({{"cmd", "board"}})["ok"] == false);
  CHECK(S.handle({{"cmd", "board"}, {"state", "/nonexistent/state.json"}})["ok"] == false);
  CHECK(S.handle(json::array())["ok"] == false);
  const json no_target = S.handle({{"cmd", "predict"},
                                   {"state", kCaveT2.string()},
                                   {"plan", json::array({{{"uid", 0}, {"kind", "weapon"}, {"weapon", "Prime_Punchmech"}}})}});
  CHECK(no_target["ok"] == false);
  // A plan the engine refuses is reported, not executed past.
  const json refused = S.handle({{"cmd", "predict"},
                                 {"state", kCaveT2.string()},
                                 {"plan", json::array({{{"uid", 0}, {"move", {0, 0}}, {"kind", "none"}}})}});
  REQUIRE(refused["ok"] == true);
  CHECK(refused["refused"] == 0);
  CHECK_FALSE(refused.contains("after_enemy"));
}

namespace {

const json* underground_unit(const json& board, int uid) {
  if (!board.contains("underground")) return nullptr;
  for (const json& u : board["underground"]) {
    if (u["uid"] == uid) return &u;
  }
  return nullptr;
}

}  // namespace

TEST_CASE("live: a hurt Burrower leaves the board and is exported underground (live 2026-10-10 m20 turn 2)") {
  NEED_SESSION();
  // Mission_Reactivation turn 2: LaserMech at F7 fires at E7; the beam hits
  // Burrower1#1683 at D7 for 2 (3 -> 1). It dives and the bridge's next
  // snapshot no longer lists it.
  const std::string start = std::string(ITB_FIXTURE_DIR) + "/live_reactivation_t2_step2.json";
  const json plan = json::array(
      {{{"uid", 0}, {"kind", "weapon"}, {"weapon", "Prime_Lasermech_A"}, {"target", {1, 3}}}});
  const json r = S.handle({{"cmd", "predict"}, {"state", start}, {"plan", plan}});
  REQUIRE_MESSAGE(r["ok"].get<bool>(), r.dump());
  REQUIRE(r["refused"] == -1);
  REQUIRE(board_unit(r["start"], 1683) != nullptr);
  CHECK(r["start"]["underground"].empty());
  const json& after = r["steps"].back()["board"];
  CHECK(board_unit(after, 1683) == nullptr);
  const json* dove = underground_unit(after, 1683);
  REQUIRE(dove);
  CHECK((*dove)["hp"] == 1);
  CHECK(((*dove)["x"] == 1 && (*dove)["y"] == 4));

  // The end-turn board as the bridge has it (no Burrower) plus the Burrower
  // as live_play carries it over: it loads underground, stays alive and
  // underground through the enemy phase (it resurfaces in the AI's move).
  std::ifstream in(std::string(ITB_FIXTURE_DIR) + "/live_reactivation_t2_end_turn.json");
  json wrapped = json::parse(in);
  json unit = *dove;
  wrapped["data"]["bridge_state"]["units"].push_back(unit);
  const fs::path carried = temp_file("itb_live_test_reactivation_underground.json", wrapped);
  const json e = S.handle({{"cmd", "predict"}, {"state", carried.string()}, {"plan", json::array()}});
  REQUIRE_MESSAGE(e["ok"].get<bool>(), e.dump());
  CHECK(board_unit(e["start"], 1683) == nullptr);
  REQUIRE(underground_unit(e["start"], 1683) != nullptr);
  CHECK(board_unit(e["after_enemy"], 1683) == nullptr);
  const json* still = underground_unit(e["after_enemy"], 1683);
  REQUIRE(still);
  CHECK((*still)["hp"] == 1);
  fs::remove(carried);
}

// ---- Units a weapon creates (Bombling bombs, deploy tanks) ------------------------------
//
// Live 20261013_104302 m00 (Mission_Airstrike) turn 2: the Bombling Mech
// dropped a walking bomb (Ranged_DeployBomb at E6); the engine numbered it
// 7507, the game 7511. live_play matched it by type and tile, then moved it
// to E5 and fired DeployUnit_SelfDamage there under 7511. The bridge lists a
// unit created this turn active, with its type's SkillList as `weapons` and
// no weapons_exact / weapon_slots (no save record until the next turn); a
// deploy tank (DeploySkill_*) comes out the same way.

namespace {

json fixture_json(const std::string& name) {
  std::ifstream in(std::string(ITB_FIXTURE_DIR) + "/" + name);
  return json::parse(in);
}

json* state_unit(json& wrapped, int uid) {
  for (json& u : wrapped["data"]["bridge_state"]["units"]) {
    if (u["uid"] == uid) return &u;
  }
  return nullptr;
}

}  // namespace

TEST_CASE("live: a unit created this turn acts under the game's uid (live 20261013_104302 m00 t2)") {
  NEED_SESSION();
  const std::string start = std::string(ITB_FIXTURE_DIR) + "/live_bomby_t2_step2.json";
  const json board = S.handle({{"cmd", "board"}, {"state", start}});
  REQUIRE_MESSAGE(board["ok"].get<bool>(), board.dump());
  const json* bomb = board_unit(board["board"], 7511);
  REQUIRE(bomb);
  CHECK((*bomb)["type"] == "DeployUnit_Bomby");
  CHECK((*bomb)["team"] == 1);
  CHECK((*bomb)["mech"] == false);
  // The rest of the turn's plan for the bomb, as played: move to E5, blow up.
  const json plan = json::array({{{"uid", 7511},
                                  {"move", {3, 3}},
                                  {"kind", "weapon"},
                                  {"weapon", "DeployUnit_SelfDamage"},
                                  {"target", {3, 3}}}});
  const json r = S.handle({{"cmd", "predict"}, {"state", start}, {"plan", plan}});
  REQUIRE_MESSAGE(r["ok"].get<bool>(), r.dump());
  REQUIRE(r["refused"] == -1);
  REQUIRE(r["steps"].size() == 2);
  // The game's board after the blast (step 4 of that turn): same units, tiles
  // and HP, and the same buildings.
  const json live = S.handle({{"cmd", "board"}, {"state", std::string(ITB_FIXTURE_DIR) + "/live_bomby_t2_step4.json"}});
  REQUIRE(live["ok"].get<bool>());
  const json& want = r["steps"].back()["board"];
  CHECK(uids(want) == uids(live["board"]));
  for (const json& u : live["board"]["units"]) {
    const json* p = board_unit(want, u["uid"].get<int>());
    REQUIRE(p);
    INFO(u.dump());
    CHECK((*p)["type"] == u["type"]);
    CHECK((*p)["x"] == u["x"]);
    CHECK((*p)["y"] == u["y"]);
    CHECK((*p)["hp"] == u["hp"]);
  }
  CHECK(want["buildings"] == live["board"]["buildings"]);
  // The solver plans for the bomb too, under its live uid.
  const json solved = S.handle({{"cmd", "solve"}, {"state", start}, {"time_limit", 3.0}});
  REQUIRE_MESSAGE(solved["ok"].get<bool>(), solved.dump());
  bool bomb_acts = false;
  for (const json& s : solved["steps"]) bomb_acts = bomb_acts || s["uid"] == 7511;
  CHECK(bomb_acts);
}

TEST_CASE("live: a deploy weapon's tank, the turn it lands and the turns after") {
  NEED_SESSION();
  // The same board with the GuardMech carrying a Light Tank (Limited = 1)
  // and, in the bomb's place, the tank as the bridge exports one dropped this
  // turn (the B upgrade's 2-damage shot from the type's SkillList).
  json wrapped = fixture_json("live_bomby_t2_step2.json");
  json& guard = *state_unit(wrapped, 0);
  guard["weapons_exact"] = {"Prime_ShieldBash", "DeploySkill_Tank_AB"};
  guard["weapon_slots"] = json::array(
      {{{"slot", 0}, {"id", "Prime_ShieldBash"}, {"limited", 0}, {"powered", true}},
       {{"slot", 1}, {"id", "DeploySkill_Tank_AB"}, {"limited", 1}, {"uses", 1}, {"uses_saved", 1}, {"powered", true}}});
  json& tank = *state_unit(wrapped, 7511);
  tank["type"] = "Deploy_TankAB";
  tank["hp"] = 3;
  tank["max_hp"] = 3;
  tank["weapons"] = {"Deploy_TankShot2"};
  const fs::path mid = temp_file("itb_live_test_deploy_mid.json", wrapped);

  const json board = S.handle({{"cmd", "board"}, {"state", mid.string()}});
  REQUIRE_MESSAGE(board["ok"].get<bool>(), board.dump());
  const json* t = board_unit(board["board"], 7511);
  REQUIRE(t);
  CHECK((*t)["type"] == "Deploy_TankAB");
  CHECK((*t)["hp"] == 3);
  CHECK((*t)["mech"] == false);

  // The tank shoots this turn: Deploy_TankShot2 (2 damage, push) at the
  // Hornet2 at D5 (3,4) from (2,4).
  const json shot = json::array({{{"uid", 7511},
                                  {"move", {2, 4}},
                                  {"kind", "weapon"},
                                  {"weapon", "Deploy_TankShot2"},
                                  {"target", {3, 4}}}});
  const json r = S.handle({{"cmd", "predict"}, {"state", mid.string()}, {"plan", shot}});
  REQUIRE_MESSAGE(r["ok"].get<bool>(), r.dump());
  CHECK(r["refused"] == -1);

  // The Guard drops a second tank (its one use): numbered one past the
  // board's largest uid; after that, no use left.
  const json drop = json::array({{{"uid", 0}, {"kind", "weapon"}, {"weapon", "DeploySkill_Tank_AB"}, {"target", {1, 2}}}});
  const json d = S.handle({{"cmd", "predict"}, {"state", mid.string()}, {"plan", drop}});
  REQUIRE_MESSAGE(d["ok"].get<bool>(), d.dump());
  CHECK(d["refused"] == -1);
  int dropped = -1;
  for (const json& u : d["after_player"]["units"]) {
    if (u["type"] == "Deploy_TankAB" && u["uid"] != 7511) dropped = u["uid"].get<int>();
  }
  CHECK(dropped == 7512);
  guard["weapon_slots"][1]["uses"] = 0;
  const fs::path spent = temp_file("itb_live_test_deploy_spent.json", wrapped);
  const json no = S.handle({{"cmd", "predict"}, {"state", spent.string()}, {"plan", drop}});
  REQUIRE_MESSAGE(no["ok"].get<bool>(), no.dump());
  CHECK(no["refused"] == 0);
  const json solved = S.handle({{"cmd", "solve"}, {"state", spent.string()}, {"time_limit", 2.0}});
  REQUIRE_MESSAGE(solved["ok"].get<bool>(), solved.dump());
  for (const json& s : solved["steps"]) CHECK(s.value("weapon", "") != "DeploySkill_Tank_AB");

  // A later turn: the save now has the tank, so the bridge exports its exact
  // weapon and slots (as for any player unit, e.g. ArchiveArtillery).
  tank["weapons_exact"] = {"Deploy_TankShot2"};
  tank["weapon_slots"] = json::array({{{"slot", 0}, {"id", "Deploy_TankShot2"}, {"limited", 0}, {"powered", true}}});
  const fs::path later = temp_file("itb_live_test_deploy_later.json", wrapped);
  const json l = S.handle({{"cmd", "predict"}, {"state", later.string()}, {"plan", shot}});
  REQUIRE_MESSAGE(l["ok"].get<bool>(), l.dump());
  CHECK(l["refused"] == -1);
  CHECK(l["after_player"] == r["after_player"]);
  fs::remove(mid);
  fs::remove(spent);
  fs::remove(later);
}

TEST_CASE("live: Rosie Rivets' move heals the mech next to where she stops (live 20261015_202706 m40 t4)") {
  NEED_SESSION();
  // Mission_Final turn 4. HydrantMech#2 (pilot Pilot_Caretaker, skill
  // Adjacent_Heal) moves G4 -> C6, next to TrimissileMech#1 (1 of 4 HP) and
  // Crab2#14652 (1 HP). The game healed the mech to 2 and not the Crab; the
  // engine used to leave the mech at 1 (the skill had no pilot ability bit).
  const std::string start = std::string(ITB_FIXTURE_DIR) + "/live_final_t4_solve_input.json";
  const json plan = json::array({{{"uid", 2}, {"move", {2, 5}}}});
  const json r = S.handle({{"cmd", "predict"}, {"state", start}, {"plan", plan}});
  REQUIRE_MESSAGE(r["ok"].get<bool>(), r.dump());
  REQUIRE(r["refused"] == -1);
  REQUIRE(r["steps"].size() == 1);
  const json& want = r["steps"][0]["board"];
  const json* tri = board_unit(want, 1);
  REQUIRE(tri);
  CHECK((*tri)["hp"] == 2);
  // The game's board after the move: same units, places and HP.
  const json live =
      S.handle({{"cmd", "board"}, {"state", std::string(ITB_FIXTURE_DIR) + "/live_final_t4_step1.json"}});
  REQUIRE_MESSAGE(live["ok"].get<bool>(), live.dump());
  CHECK(uids(want) == uids(live["board"]));
  for (const json& u : live["board"]["units"]) {
    const json* p = board_unit(want, u["uid"].get<int>());
    REQUIRE(p);
    INFO(u.dump());
    CHECK((*p)["x"] == u["x"]);
    CHECK((*p)["y"] == u["y"]);
    CHECK((*p)["hp"] == u["hp"]);
  }
}
