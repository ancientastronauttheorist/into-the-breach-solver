// itb_inspect --threat-check DIR: checks the next-turn threat proxy
// (threat.hpp) against the engine and the recorded games, and times it.
//
// Which queued attacks hit a building: a recorded board starts a player
// turn, with the enemies' attacks already queued. For each enemy with a
// queued attack, the enemy phase runs twice, once with every queued attack
// removed and once with only that enemy's attack kept; if the second loses
// more building HP or grid, the attack hits a building.
//
// 1. Reach (same board): the proxy on the recorded board, where each enemy
//    already stands on its firing tile, must count every enemy whose queued
//    attack hits a building.
// 2. Prediction (next turn): for a recorded turn followed by the next
//    turn's board, the proxy on the engine's board after this turn's enemy
//    phase (from the recorded End Turn board when there is one, else the
//    recorded plan replayed) against the enemies whose attacks, queued by
//    the game's AI for the next turn, hit a building.
// 3. Cost: count_threats on the board after the enemy phase, against one
//    enemy phase on the same board.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "itb/engine.hpp"
#include "itb/recording.hpp"
#include "itb/threat.hpp"
#include "solve_tool.hpp"

namespace fs = std::filesystem;
using nlohmann::json;

namespace itb::tools {
namespace {

using Clock = std::chrono::steady_clock;

int building_hp(const Board& b) {
  int total = 0;
  for (int i = 0; i < kTileCount; ++i) {
    const Tile& t = b.tile(Point::from_index(i));
    if (t.is_building()) total += std::max<int>(0, t.hp);
  }
  return total;
}

double percentile(std::vector<double> v, double q) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, static_cast<size_t>(q * static_cast<double>(v.size() - 1) + 0.5))];
}

bool scored_enemy(const Pawn& p) { return p.team == Team::Enemy && !p.neutral && !p.mech; }

Board phase(Engine& engine, Board b, const TurnContext& ctx) {
  TurnContext tc = ctx;
  engine.lua().seed(1);
  engine.end_turn(b, tc);
  return b;
}

// Enemies whose queued attack hits a building (file comment), with the
// weapon of each.
std::map<int32_t, std::string> hitters(Engine& engine, const Board& board, const TurnContext& ctx, int* queued) {
  std::map<int32_t, std::string> out;
  Board quiet = board;
  for (Pawn& p : quiet.pawns()) {
    if (p.team == Team::Enemy) p.queued = QueuedShot{};
  }
  const Board base = phase(engine, quiet, ctx);
  for (const Pawn& p : board.pawns()) {
    if (!scored_enemy(p) || !p.queued.active()) continue;
    if (queued) ++*queued;
    Board one = quiet;
    one.find_pawn(p.uid)->queued = p.queued;
    const Board after = phase(engine, one, ctx);
    if (building_hp(after) >= building_hp(base) && after.grid_power >= base.grid_power) continue;
    out[p.uid] = p.queued.weapon >= 0 && p.queued.weapon < kMaxWeapons
                     ? std::string(symbol_name(p.weapons[static_cast<size_t>(p.queued.weapon)]))
                     : std::string("?");
  }
  return out;
}

Point jpoint(const json& j) {
  if (!j.is_array() || j.size() < 2 || !j[0].is_number() || !j[1].is_number()) return kInvalidPoint;
  return {j[0].get<int>(), j[1].get<int>()};
}

// The recorded plan next to a solve_input (m.._solve.json, data.actions).
std::optional<std::vector<PlayerAction>> recorded_plan(const fs::path& input, const Recording& rec, Engine& engine) {
  const std::string name = input.filename().string();
  fs::path solve = input;
  solve.replace_filename(name.substr(0, name.size() - std::string("_solve_input.json").size()) + "_solve.json");
  json root;
  try {
    std::ifstream in(solve);
    if (!in) return std::nullopt;
    in >> root;
  } catch (const std::exception&) {
    return std::nullopt;
  }
  if (!root.contains("data") || !root["data"].contains("actions") || root["data"].contains("partial_re_solve")) {
    return std::nullopt;
  }
  std::vector<PlayerAction> plan;
  for (const json& a : root["data"]["actions"]) {
    PlayerAction p;
    p.uid = a.value("mech_uid", -1);
    p.move = jpoint(a.value("move_to", json()));
    if (const Pawn* q = rec.board.find_pawn(p.uid); q && p.move == q->pos) p.move = kInvalidPoint;
    const std::string weapon = a.value("weapon_id", "");
    p.target = jpoint(a.value("target", json()));
    if (weapon == "_REPAIR") {
      p.kind = PlayerAction::Kind::Repair;
      for (const auto& [uid, pilot] : rec.pilots) {
        if (uid == p.uid) p.weapon = engine.repair_skill(pilot);
      }
    } else if (!weapon.empty() && weapon != "Unknown" && p.target.valid()) {
      p.kind = PlayerAction::Kind::Weapon;
      p.weapon = weapon;
    }
    plan.push_back(std::move(p));
  }
  return plan;
}

// m<NN>_turn_<TT><suffix> next to `input`, for turn `turn`.
fs::path turn_file(const fs::path& input, int turn, const char* suffix) {
  const std::string name = input.filename().string();  // mNN_turn_TT_solve_input.json
  char buf[64];
  std::snprintf(buf, sizeof buf, "_turn_%02d", turn);
  fs::path p = input;
  p.replace_filename(name.substr(0, name.find("_turn_")) + buf + suffix);
  return p;
}

}  // namespace

int run_threat_check(const fs::path& dir, const fs::path& game) {
  std::unique_ptr<Engine> engine = Engine::create(game);
  std::vector<fs::path> files;
  for (const auto& e : fs::recursive_directory_iterator(dir)) {
    const std::string name = e.path().filename().string();
    if (e.is_regular_file() && name.ends_with("_solve_input.json")) files.push_back(e.path());
  }
  std::sort(files.begin(), files.end());
  int boards = 0, queued = 0, hitting = 0, counted = 0, flagged = 0, enemies = 0;
  std::map<std::string, std::pair<int, int>> missed;  // weapon -> (missed, hitting)
  // Prediction: turns, enemies on both boards, flagged, hit next turn, both.
  int turns = 0, from_end_turn = 0, common = 0, p_flagged = 0, p_hit = 0, p_both = 0;
  std::vector<double> threat_us, phase_us;
  for (const fs::path& f : files) {
    std::string error;
    std::optional<Recording> rec = load_recording(f, &engine->data(), &error);
    if (!rec) continue;
    ++boards;
    const Board& board = rec->board;
    const TurnContext ctx = turn_context(*rec);

    // 1. Reach.
    const std::vector<int32_t> proxy = threatening_enemies(board);
    flagged += static_cast<int>(proxy.size());
    for (const Pawn& p : board.pawns()) {
      if (scored_enemy(p) && p.alive() && p.pos.valid()) ++enemies;
    }
    for (const auto& [uid, weapon] : hitters(*engine, board, ctx, &queued)) {
      ++hitting;
      ++missed[weapon].second;
      if (std::find(proxy.begin(), proxy.end(), uid) != proxy.end()) {
        ++counted;
      } else {
        ++missed[weapon].first;
        std::printf("missed: %s %s at %s\n", f.filename().string().c_str(), weapon.c_str(),
                    to_visual(board.find_pawn(uid)->pos).c_str());
      }
    }

    // 2. Prediction.
    const fs::path next_path = turn_file(f, rec->turn + 1, "_solve_input.json");
    if (fs::exists(next_path)) {
      std::string e2;
      std::optional<Recording> next = load_recording(next_path, &engine->data(), &e2);
      std::optional<Board> after;
      const fs::path end_path = turn_file(f, rec->turn, "_end_turn_01.json");
      if (std::optional<Recording> end = fs::exists(end_path) ? load_recording(end_path, &engine->data(), &e2)
                                                              : std::nullopt) {
        after = phase(*engine, end->board, turn_context(*end));
        ++from_end_turn;
      } else if (const auto plan = recorded_plan(f, *rec, *engine)) {
        Board b = board;
        for (const auto& [uid, pilot] : rec->pilots) {
          if (Pawn* p = b.find_pawn(uid)) p->pilot_abilities |= engine->pilot_ability(pilot);
        }
        TurnContext tc = ctx;
        engine->lua().seed(1);
        if (engine->play_turn(b, *plan, tc).ok()) after = b;
      }
      if (next && after) {
        ++turns;
        const std::vector<int32_t> pred = threatening_enemies(*after);
        const std::map<int32_t, std::string> hit = hitters(*engine, next->board, turn_context(*next), nullptr);
        for (const Pawn& p : after->pawns()) {
          if (!scored_enemy(p) || !p.alive()) continue;
          const Pawn* q = next->board.find_pawn(p.uid);
          if (!q || q->type != p.type || !q->alive()) continue;
          ++common;
          const bool f1 = std::find(pred.begin(), pred.end(), p.uid) != pred.end();
          const bool h1 = hit.count(p.uid) > 0;
          p_flagged += f1;
          p_hit += h1;
          p_both += f1 && h1;
        }
      }
    }

    // 3. Cost on the board the solver scores: after the enemy phase.
    const Board after = phase(*engine, board, ctx);
    const int reps = 2000;
    int sink = 0;
    auto t0 = Clock::now();
    for (int i = 0; i < reps; ++i) sink += count_threats(after);
    threat_us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count() / reps);
    t0 = Clock::now();
    for (int i = 0; i < 5; ++i) phase(*engine, board, ctx);
    phase_us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count() / 5);
    if (sink < 0) std::printf("?\n");
  }
  std::printf("reach: boards %d, enemies %d, counted by the proxy %d (%.1f%%)\n", boards, enemies, flagged,
              100.0 * flagged / std::max(1, enemies));
  std::printf("reach: queued attacks %d, hitting a building %d, counted %d (recall %.1f%%)\n", queued, hitting,
              counted, 100.0 * counted / std::max(1, hitting));
  for (const auto& [w, n] : missed) {
    if (n.first) std::printf("  missed %-20s %d of %d\n", w.c_str(), n.first, n.second);
  }
  std::printf("prediction: turns %d (%d from a recorded End Turn board), enemies on both boards %d, counted %d, "
              "hit a building next turn %d, both %d (precision %.1f%%, recall %.1f%%)\n",
              turns, from_end_turn, common, p_flagged, p_hit, p_both, 100.0 * p_both / std::max(1, p_flagged),
              100.0 * p_both / std::max(1, p_hit));
  std::printf("cost: count_threats median %.2f us, p95 %.2f us; enemy phase (one run) median %.1f us, p95 %.1f us\n",
              percentile(threat_us, 0.5), percentile(threat_us, 0.95), percentile(phase_us, 0.5),
              percentile(phase_us, 0.95));
  return 0;
}

}  // namespace itb::tools
