// itb_inspect --threat-check DIR: checks the next-turn threat proxy
// (threat.hpp) against the engine on recorded boards and times it.
//
// Recall: every recorded board starts a player turn, with the enemies'
// attacks already queued. For each enemy with a queued attack, the enemy
// phase runs twice, once with every queued attack removed and once with only
// that enemy's attack kept; if the second loses more building HP or grid,
// the attack hits a building, and the proxy (on the recorded board, where
// that enemy already stands on its firing tile) must count it.
//
// Cost: count_threats on the board after the enemy phase, against one
// enemy phase on the same board.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "itb/engine.hpp"
#include "itb/recording.hpp"
#include "itb/threat.hpp"
#include "solve_tool.hpp"

namespace fs = std::filesystem;

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

}  // namespace

int run_threat_check(const fs::path& dir, const fs::path& game) {
  std::unique_ptr<Engine> engine = Engine::create(game);
  std::vector<fs::path> files;
  for (const auto& e : fs::recursive_directory_iterator(dir)) {
    const std::string name = e.path().filename().string();
    if (e.is_regular_file() && name.ends_with("_solve_input.json")) files.push_back(e.path());
  }
  std::sort(files.begin(), files.end());
  int boards = 0, attackers = 0, hitting = 0, counted = 0, flagged = 0, enemies = 0;
  std::map<std::string, std::pair<int, int>> missed;  // weapon -> (missed, hitting)
  std::vector<double> threat_us, phase_us;
  for (const fs::path& f : files) {
    std::string error;
    std::optional<Recording> rec = load_recording(f, &engine->data(), &error);
    if (!rec) continue;
    ++boards;
    const Board& board = rec->board;
    const TurnContext ctx = turn_context(*rec);
    auto phase = [&](Board b) {
      TurnContext tc = ctx;
      engine->lua().seed(1);
      engine->end_turn(b, tc);
      return b;
    };
    const std::vector<int32_t> proxy = threatening_enemies(board);
    flagged += static_cast<int>(proxy.size());
    Board quiet = board;
    for (Pawn& p : quiet.pawns()) {
      if (p.team == Team::Enemy && !p.neutral && !p.mech && p.alive() && p.pos.valid()) ++enemies;
      if (p.team == Team::Enemy) p.queued = QueuedShot{};
    }
    const Board base = phase(quiet);
    for (const Pawn& p : board.pawns()) {
      if (p.team != Team::Enemy || p.neutral || p.mech || !p.queued.active()) continue;
      ++attackers;
      Board one = quiet;
      one.find_pawn(p.uid)->queued = p.queued;
      const Board after = phase(one);
      if (building_hp(after) >= building_hp(base) && after.grid_power >= base.grid_power) continue;
      ++hitting;
      const std::string weapon =
          p.queued.weapon >= 0 && p.queued.weapon < kMaxWeapons
              ? std::string(symbol_name(p.weapons[static_cast<size_t>(p.queued.weapon)]))
              : std::string("?");
      ++missed[weapon].second;
      if (std::find(proxy.begin(), proxy.end(), p.uid) != proxy.end()) {
        ++counted;
      } else {
        ++missed[weapon].first;
        std::printf("missed: %s %s at %s -> %s\n", f.filename().string().c_str(), weapon.c_str(),
                    to_visual(p.pos).c_str(), to_visual(p.queued.target).c_str());
      }
    }
    // Cost on the board the solver scores: after the enemy phase.
    const Board after = phase(board);
    const int reps = 2000;
    int sink = 0;
    auto t0 = Clock::now();
    for (int i = 0; i < reps; ++i) sink += count_threats(after);
    threat_us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count() / reps);
    t0 = Clock::now();
    for (int i = 0; i < 5; ++i) phase(board);
    phase_us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count() / 5);
    if (sink < 0) std::printf("?\n");
  }
  std::printf("boards %d, enemies %d, counted by the proxy %d (%.1f%%)\n", boards, enemies, flagged,
              100.0 * flagged / std::max(1, enemies));
  std::printf("queued attacks %d, hitting a building %d, counted %d (recall %.1f%%)\n", attackers, hitting, counted,
              100.0 * counted / std::max(1, hitting));
  for (const auto& [w, n] : missed) {
    if (n.first) std::printf("  missed %-20s %d of %d\n", w.c_str(), n.first, n.second);
  }
  std::printf("count_threats: median %.2f us, p95 %.2f us; enemy phase (one run): median %.1f us, p95 %.1f us\n",
              percentile(threat_us, 0.5), percentile(threat_us, 0.95), percentile(phase_us, 0.5),
              percentile(phase_us, 0.95));
  return 0;
}

}  // namespace itb::tools
