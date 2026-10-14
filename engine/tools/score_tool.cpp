// itb_inspect --score <recording>: the turn score of a recorded plan.
//
// Loads m<NN>_turn_<NN>_solve_input.json, replays the plan recorded next to
// it (m<NN>_turn_<NN>_solve.json, data.actions) with Engine::play_turn, no
// syncing to the game, then prints the score tiers, every objective line and
// the position terms. When the game's own outcome was recorded
// (m<NN>_turn_<NN>_post_enemy.json, the next turn's solve_input) the
// objective-relevant fields are shown beside the engine's.

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "itb/engine.hpp"
#include "itb/game_data.hpp"
#include "itb/objectives.hpp"
#include "itb/recording.hpp"
#include "itb/score.hpp"
#include "replay.hpp"

namespace fs = std::filesystem;
using nlohmann::json;

namespace itb::tools {
namespace {

Point jpoint(const json& j) {
  if (!j.is_array() || j.size() < 2 || !j[0].is_number() || !j[1].is_number()) return kInvalidPoint;
  return {j[0].get<int>(), j[1].get<int>()};
}

int jint(const json& j, const char* k, int fallback = -1) {
  auto it = j.find(k);
  return it != j.end() && it->is_number() ? it->get<int>() : fallback;
}

fs::path sibling(const fs::path& input, const std::string& suffix) {
  const std::string name = input.filename().string();
  const std::string stem = name.substr(0, name.size() - std::string("_solve_input.json").size());
  return input.parent_path() / (stem + suffix);
}

}  // namespace

ObjectiveTally objective_counts(const Board& b) {
  ObjectiveTally t;
  for (int i = 0; i < kTileCount; ++i) {
    const Tile& tile = b.tile(Point::from_index(i));
    if (tile.unique_building != kNoSymbol && tile.is_building() && tile.hp > 0) ++t.objective_buildings;
    if (tile.pod == PodState::Present) ++t.pods;
  }
  for (const Pawn& p : b.pawns()) {
    if (p.alive() && !p.fallen && p.infected && p.mech) ++t.mites;
  }
  return t;
}

void print_score(const Score& s, const ObjectiveReport& obj, const PositionTerms& pos) {
  std::printf("score: %s\n", s.describe().c_str());
  std::printf("objectives (%s, mission %s):\n", obj.exact() ? "exact" : "approximate",
              obj.mission_ends ? "ends this turn" : "continues");
  for (const ObjectiveLine& l : obj.lines) {
    std::printf("  %-32s failed %d  progress %5d%s  %s\n", l.id.c_str(), l.failed, l.progress,
                l.exact ? "" : " ~", l.detail.c_str());
  }
  std::printf("  events: enemy kills %d, acid kills %d, spawns blocked %d, mountains %d, repairs %d\n",
              obj.enemy_kills, obj.acid_kills, obj.spawns_blocked, obj.mountains_destroyed, obj.repairs_used);
  std::printf(
      "position %d: mech fire %d acid %d frozen %d smoke %d at-1hp %d | building threat %d unit threat %d | "
      "enemy fire %d frozen %d | deployed units %d hp %d\n",
      pos.total(), pos.mech_fire, pos.mech_acid, pos.mech_frozen, pos.mech_smoke, pos.mech_fragile,
      pos.building_threat, pos.unit_threat, pos.enemy_fire, pos.enemy_frozen, pos.deployed, pos.deployed_hp);
}

int run_score(const fs::path& input, const fs::path& game) {
  std::unique_ptr<Engine> engine;
  try {
    engine = Engine::create(game);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  const GameData& data = engine->data();
  std::string error;
  auto rec = load_recording(input, &data, &error);
  if (!rec) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return 1;
  }
  const fs::path solve_path = sibling(input, "_solve.json");
  json solve;
  try {
    std::ifstream in(solve_path);
    in >> solve;
  } catch (const std::exception&) {
    std::fprintf(stderr, "error: no readable plan at %s\n", solve_path.string().c_str());
    return 1;
  }
  Board before = rec->board;
  for (const auto& [uid, pilot] : rec->pilots) {
    if (Pawn* p = before.find_pawn(uid)) p->pilot_abilities |= engine->pilot_ability(pilot);
  }

  std::vector<PlayerAction> plan;
  for (const json& a : solve["data"]["actions"]) {
    PlayerAction act;
    act.uid = a.value("mech_uid", -1);
    act.move = jpoint(a.value("move_to", json()));
    if (const Pawn* p = before.find_pawn(act.uid); p && act.move == p->pos) act.move = kInvalidPoint;
    const std::string weapon = a.value("weapon_id", "");
    act.target = jpoint(a.value("target", json()));
    if (weapon == "_REPAIR") {
      act.kind = PlayerAction::Kind::Repair;
      for (const auto& [pu, pilot] : rec->pilots) {
        if (pu == act.uid) act.weapon = engine->repair_skill(pilot);
      }
    } else if (!weapon.empty() && weapon != "Unknown" && act.target.valid()) {
      act.kind = PlayerAction::Kind::Weapon;
      act.weapon = weapon;
    }
    plan.push_back(act);
  }

  TurnContext ctx = turn_context(*rec);
  Board after = before;
  TurnResult tr = engine->play_turn(after, plan, ctx);
  std::printf("%s m%02d turn %d %s: %zu actions%s\n", rec->run_id.c_str(), rec->mission_index, rec->turn,
              rec->mission_id.c_str(), plan.size(),
              tr.ok() ? "" : (" (refused at action " + std::to_string(tr.refused) + ")").c_str());
  for (size_t i = 0; i < tr.actions.size(); ++i) {
    std::printf("  action %zu: %s %s\n", i, tr.actions[i].weapon.c_str(), to_string(tr.actions[i].status));
  }
  if (!tr.ok()) return 1;
  const Score s = score_turn(before, after, &ctx, &tr.enemy);
  const ObjectiveReport obj = evaluate_objectives(before, after, &ctx, &tr.enemy);
  print_score(s, obj, position_terms(after));

  // The game's outcome, where recorded.
  const ObjectiveTally mine = objective_counts(after);
  std::printf("engine:   grid %d, objective buildings %d, pods %d, mites %d\n", after.grid_power,
              mine.objective_buildings, mine.pods, mine.mites);
  const fs::path post_path = sibling(input, "_post_enemy.json");
  if (fs::exists(post_path)) {
    try {
      std::ifstream in(post_path);
      json post;
      in >> post;
      const json& a = post["data"]["actual_outcome"];
      auto field = [&a](const char* k) {
        const int v = jint(a, k);
        return v < 0 ? std::string("?") : std::to_string(v);
      };
      std::printf("game:     grid %s, objective buildings %s, pods %s, mites %s (post_enemy)\n",
                  field("grid_power").c_str(), field("objective_buildings_alive").c_str(),
                  field("pods_present").c_str(), field("mites_remaining").c_str());
    } catch (const std::exception&) {
    }
  }
  const std::string name = input.filename().string();
  if (name.size() > 11 && name[0] == 'm') {
    char next_name[64];
    std::snprintf(next_name, sizeof next_name, "%s%02d_solve_input.json", name.substr(0, 9).c_str(),
                  std::stoi(name.substr(9, 2)) + 1);
    const fs::path next_path = input.parent_path() / next_name;
    if (fs::exists(next_path)) {
      if (auto next = load_recording(next_path, &data, &error)) {
        const ObjectiveData& o0 = rec->mission.objectives;
        const ObjectiveData& o1 = next->mission.objectives;
        // Mission.KilledVek only counts while a kill bonus is active.
        const std::vector<BonusId> bonus = active_bonuses(rec->board, rec->mission);
        const bool acid = rec->mission_id == "Mission_AcidTank";
        const bool counted = acid || std::find(bonus.begin(), bonus.end(), BonusId::KillFive) != bonus.end() ||
                             std::find(bonus.begin(), bonus.end(), BonusId::Pacifist) != bonus.end();
        const int gained = acid ? obj.acid_kills : obj.enemy_kills;
        if (counted && o0.kills_done >= 0) {
          std::printf("kills:    engine %d + %d = %d, game %d (next turn's mission_kills_done)\n", o0.kills_done,
                      gained, o0.kills_done + gained, o1.kills_done);
        }
      }
    }
  }
  return 0;
}

}  // namespace itb::tools
