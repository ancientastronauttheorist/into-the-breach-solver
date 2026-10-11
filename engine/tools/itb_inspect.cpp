// itb_inspect: look at what the engine sees.
//
//   itb_inspect [--game DIR] <recording.json>   render a recorded board
//   itb_inspect [--game DIR] --pawns            list pawn definitions
//   itb_inspect [--game DIR] --scripts          report the script load
//   itb_inspect [--game DIR] --corpus DIR       load every *_solve_input.json under DIR
//   itb_inspect [--game DIR] --moves DIR        check recorded first moves are reachable
//   itb_inspect [--game DIR] --weapons DIR      run every mech weapon's Lua on recorded boards
//   itb_inspect [--game DIR] --replay DIR [--show N] [--no-sync] [--weapon ID] [--json FILE]
//                                               replay recorded actions, compare with the game
//   itb_inspect [--game DIR] --replay DIR --turns [--show N] [--trace RUN/M/T]
//                                               replay whole turns through the enemy phase
//   itb_inspect [--game DIR] --score <recording.json>
//                                               score the recorded plan (tiers, objectives, position)
//   itb_inspect [--game DIR] --solve (<recording.json> | DIR) [--time S] [--nodes N]
//               [--max-time S --min-tiers K] [--threads N] [--beam W] [--sample N] [--shard I/N] [--verbose] [--json FILE]
//               [--lua-counts] [--tt ENTRIES] [--threat-tier] [--kills-tier] [--objectives-first]
//               [--threat-eval]
//                                               the perfect-turn search vs the recorded plan
//   itb_inspect [--game DIR] --threat-check DIR
//                                               the next-turn threat proxy vs the engine, and its cost
//   itb_inspect [--game DIR] --predict <state.json> [--actions JSON|@FILE] [--no-enemy]
//               [--branches [N]] [--json FILE]
//                                               the engine's outcome for a bridge state (live tests)
//   itb_inspect [--game DIR] --diff-weapons [DIR] [--random N] [--seed S] [--weapon ID] [--all-tables]
//   itb_inspect [--game DIR] --diff-weapons --ports
//                                               C++ weapon ports against the Lua they mirror

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <fstream>

#include <nlohmann/json.hpp>

#include "itb/format.hpp"
#include "itb/game_data.hpp"
#include "itb/lua_host.hpp"
#include "itb/movement.hpp"
#include "itb/recording.hpp"
#include "predict_tool.hpp"
#include "replay.hpp"
#include "solve_tool.hpp"

namespace fs = std::filesystem;
using namespace itb;

namespace {

int usage() {
  std::cerr << "usage: itb_inspect [--game DIR] (<recording.json> | --pawns | --scripts | --corpus DIR | "
               "--moves DIR | --weapons DIR | --replay DIR [--turns] [--show N] [--no-sync] [--weapon ID] [--trace RUN/M/T] [--json FILE] | --score FILE | "
               "--solve (<recording.json> | DIR) [--time S] [--max-time S --min-tiers K] [--nodes N] [--threads N] [--beam W] [--sample N] [--shard I/N] [--verbose] [--json FILE] [--lua-counts] [--tt ENTRIES] [--threat-tier] [--kills-tier] [--objectives-first] [--threat-eval] | "
               "--threat-check DIR | "
               "--predict <state.json> [--actions JSON|@FILE] [--no-enemy] [--branches [N]] [--json FILE] | "
               "--diff-weapons [DIR] [--random N] [--seed S] [--weapon ID] [--all-tables] [--ports])\n";
  return 2;
}

// Enemy pawns with a queued attack, in board-list order.
std::vector<int32_t> queued_vek_in_list_order(const Board& b) {
  std::vector<int32_t> out;
  for (const Pawn& p : b.pawns()) {
    if (p.team == Team::Enemy && p.queued.active()) out.push_back(p.uid);
  }
  return out;
}

int run_corpus(const fs::path& dir, const GameData& data) {
  std::vector<fs::path> files;
  for (const auto& e : fs::recursive_directory_iterator(dir)) {
    const std::string name = e.path().filename().string();
    if (e.is_regular_file() && name.ends_with("_solve_input.json")) files.push_back(e.path());
  }
  std::sort(files.begin(), files.end());

  int loaded = 0, failed = 0, with_order = 0, order_match = 0, order_match_by_uid = 0;
  std::map<std::string, int> warning_counts;
  std::vector<std::string> mismatches;
  for (const fs::path& f : files) {
    std::string error;
    auto rec = load_recording(f, &data, &error);
    if (!rec) {
      ++failed;
      std::cerr << "FAIL " << error << "\n";
      continue;
    }
    ++loaded;
    for (const std::string& w : rec->warnings) ++warning_counts[w];

    // attack_order lists Vek uids in the order the game will fire them.
    std::vector<int32_t> expected;
    for (int32_t uid : rec->attack_order) {
      const Pawn* p = rec->board.find_pawn(uid);
      if (p && p->queued.active()) expected.push_back(uid);
    }
    if (expected.size() < 2) continue;
    ++with_order;
    const std::vector<int32_t> actual = queued_vek_in_list_order(rec->board);
    if (actual == expected) {
      ++order_match;
    } else if (mismatches.size() < 8) {
      std::string line = f.parent_path().filename().string() + "/" + f.filename().string() + " expected";
      for (int32_t u : expected) line += " " + std::to_string(u);
      line += " | list order";
      for (int32_t u : actual) line += " " + std::to_string(u);
      mismatches.push_back(line);
    }
    std::vector<int32_t> by_uid = actual;
    std::sort(by_uid.begin(), by_uid.end());
    if (by_uid == expected) ++order_match_by_uid;
  }

  std::printf("recordings: %zu  loaded: %d  failed: %d\n", files.size(), loaded, failed);
  // Bridges before 2026-06-23 wrote attack_order sorted by uid (a bot-side
  // assumption), so only later recordings reflect the game's actual order.
  std::printf("boards with >=2 queued Vek: %d (attack_order before 2026-06-23 is uid-sorted by the bot)\n",
              with_order);
  std::printf("  attack order == board list order: %d\n", order_match);
  std::printf("  attack order == ascending uid:     %d\n", order_match_by_uid);
  for (const std::string& m : mismatches) std::printf("  mismatch: %s\n", m.c_str());
  if (!warning_counts.empty()) {
    std::printf("warnings:\n");
    for (const auto& [w, n] : warning_counts) std::printf("  %5d  %s\n", n, w.c_str());
  }
  return failed == 0 ? 0 : 1;
}

// The old bot recorded the move it executed for each mech. The first action
// runs on the untouched recorded board, so its destination must be in that
// mech's reachable set.
int run_moves(const fs::path& dir, const GameData& data) {
  int checked = 0, reachable_ok = 0, skipped = 0;
  std::vector<std::string> misses;
  for (const auto& e : fs::recursive_directory_iterator(dir)) {
    const std::string name = e.path().filename().string();
    if (!e.is_regular_file() || !name.ends_with("_solve_input.json")) continue;
    fs::path solve = e.path();
    solve.replace_filename(name.substr(0, name.size() - std::string("_input.json").size()) + ".json");
    if (!fs::exists(solve)) continue;
    std::string error;
    auto rec = load_recording(e.path(), &data, &error);
    if (!rec) continue;
    nlohmann::json root;
    try {
      std::ifstream in(solve);
      in >> root;
    } catch (const std::exception&) {
      continue;
    }
    const auto& actions = root["data"]["actions"];
    if (!actions.is_array() || actions.empty()) continue;
    const auto& a = actions[0];
    if (!a.contains("move_to") || !a["move_to"].is_array()) continue;
    const Point dest{a["move_to"][0].get<int>(), a["move_to"][1].get<int>()};
    const Pawn* mech = rec->board.find_pawn(a.value("mech_uid", -1));
    if (!mech || dest == mech->pos) {
      ++skipped;
      continue;
    }
    ++checked;
    const std::vector<Point> area = reachable(rec->board, *mech);
    if (std::find(area.begin(), area.end(), dest) != area.end()) {
      ++reachable_ok;
    } else if (misses.size() < 12) {
      misses.push_back(e.path().parent_path().filename().string() + "/" + name + ": " +
                       describe_pawn(*mech) + " -> " + to_visual(dest));
    }
  }
  std::printf("first moves checked: %d  reachable: %d  (skipped stay-in-place: %d)\n", checked,
              reachable_ok, skipped);
  for (const std::string& m : misses) std::printf("  not reachable: %s\n", m.c_str());
  return reachable_ok == checked ? 0 : 1;
}

// Runs the game's Lua for every recorded board: each player mech's weapons
// (target area from its current tile, then the effect on every target) and
// every queued Vek attack (recomputed from the Vek's tile, as when it fires).
// Recorded weapon ids are the Lua table names, upgrade suffix included.
int run_weapons(const fs::path& dir, const fs::path& game, const GameData& data) {
  std::unique_ptr<LuaHost> host;
  try {
    host = LuaHost::create(game);
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
  std::vector<fs::path> files;
  for (const auto& e : fs::recursive_directory_iterator(dir)) {
    const std::string name = e.path().filename().string();
    if (e.is_regular_file() && name.ends_with("_solve_input.json")) files.push_back(e.path());
  }
  std::sort(files.begin(), files.end());

  int boards = 0, weapons = 0, suffixed = 0, unknown = 0, empty_areas = 0, targets = 0, effects = 0;
  int queued = 0, queued_in_area = 0, queued_errors = 0;
  std::map<std::string, int> errors;
  std::map<std::string, int> unknown_ids;
  std::map<std::string, int> empty_area_ids;
  std::vector<std::string> fizzles;
  for (const fs::path& f : files) {
    std::string error;
    auto rec = load_recording(f, &data, &error);
    if (!rec) continue;
    ++boards;
    const Board& b = rec->board;
    for (const Pawn& p : b.pawns()) {
      if (!p.alive()) continue;
      if (p.mech && p.team == Team::Player) {
        for (Symbol w : p.weapons) {
          if (w == kNoSymbol) continue;
          const std::string id(symbol_name(w));
          ++weapons;
          if (id.ends_with("_A") || id.ends_with("_B") || id.ends_with("_AB")) ++suffixed;
          if (!host->lua_string(id, "Name")) {
            ++unknown;
            ++unknown_ids[id];
            continue;
          }
          LuaCall call;
          const std::vector<Point> area = host->target_area(b, p, id, p.pos, &call);
          if (!call.ok) {
            ++errors[id + ": " + call.error];
            continue;
          }
          if (area.empty()) {
            ++empty_areas;
            ++empty_area_ids[id];
          }
          for (Point t : area) {
            ++targets;
            LuaCall c2;
            const LuaSkillEffect se = host->skill_effect_raw(b, p, id, p.pos, t, &c2);
            if (!c2.ok) {
              ++errors[id + " -> " + to_visual(t) + ": " + c2.error];
              break;
            }
            if (!se.effect.empty() || !se.q_effect.empty()) ++effects;
          }
        }
      } else if (p.queued.active() && p.weapons[0] != kNoSymbol) {
        ++queued;
        const std::string id(symbol_name(p.weapons[0]));
        LuaCall call;
        const std::vector<Point> area = host->target_area(b, p, id, p.pos, &call);
        if (!call.ok) {
          ++queued_errors;
          ++errors[id + " (queued): " + call.error];
          continue;
        }
        if (std::find(area.begin(), area.end(), p.queued.target) == area.end()) {
          if (fizzles.size() < 8) {
            fizzles.push_back(f.parent_path().filename().string() + "/" + f.filename().string() + ": " +
                              id + " at " + to_visual(p.pos) + " -> " + to_visual(p.queued.target));
          }
          continue;
        }
        ++queued_in_area;
        host->queued_effect_raw(b, p, id, p.queued.target, &call);
        if (!call.ok) {
          ++queued_errors;
          ++errors[id + " (queued): " + call.error];
        }
      }
    }
  }
  std::printf("boards: %d  mech weapons: %d (%d with an upgrade suffix)  unknown ids: %d\n", boards,
              weapons, suffixed, unknown);
  std::printf("targets: %d  non-empty effects: %d  empty target areas: %d\n", targets, effects,
              empty_areas);
  std::printf("queued Vek attacks: %d  target still in the recomputed area: %d  errors: %d\n", queued,
              queued_in_area, queued_errors);
  for (const auto& [id, n] : unknown_ids) std::printf("  unknown weapon id: %s (%d)\n", id.c_str(), n);
  for (const auto& [id, n] : empty_area_ids) std::printf("  empty target area: %s (%d)\n", id.c_str(), n);
  for (const std::string& m : fizzles) std::printf("  queued target outside the area: %s\n", m.c_str());
  for (const auto& [e, n] : errors) std::printf("  %5d  %s\n", n, e.c_str());
  return errors.empty() ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  fs::path game = GameData::default_game_root();
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--game" && i + 1 < argc) {
      game = argv[++i];
    } else {
      args.push_back(a);
    }
  }
  if (args.empty()) return usage();

  ScriptLoadReport report;
  GameData data;
  try {
    data = GameData::load(game, &report);
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }

  if (args[0] == "--scripts") {
    std::printf("game root: %s\n", game.string().c_str());
    std::printf("scripts ok: %d  failed: %d  passes: %d  pawns: %zu\n", report.files_ok,
                report.files_failed, report.passes, data.pawns().size());
    std::printf("stubbed native globals (%zu):", report.stubbed_globals.size());
    for (const auto& s : report.stubbed_globals) std::printf(" %s", s.c_str());
    std::printf("\n");
    std::printf("stubs later defined by Lua (%zu):", report.overwritten_stubs.size());
    for (const auto& s : report.overwritten_stubs) std::printf(" %s", s.c_str());
    std::printf("\n");
    for (const auto& e : report.errors) std::printf("  error: %s\n", e.c_str());
    for (const auto& w : report.field_warnings) std::printf("  field: %s\n", w.c_str());
    return 0;
  }
  if (args[0] == "--pawns") {
    for (const PawnDef& d : data.pawns()) {
      std::printf("%-26s hp %2d move %d team %d%s%s%s%s  [", d.name.c_str(), d.health, d.move_speed,
                  static_cast<int>(d.default_team), d.massive ? " massive" : "",
                  d.flying ? " flying" : "", d.armor ? " armor" : "", d.pushable ? "" : " stable");
      for (size_t i = 0; i < d.skills.size(); ++i) {
        std::printf("%s%s", i ? ", " : "", d.skills[i].c_str());
      }
      std::printf("]\n");
    }
    return 0;
  }
  if (args[0] == "--moves") {
    if (args.size() < 2) return usage();
    return run_moves(args[1], data);
  }
  if (args[0] == "--weapons") {
    if (args.size() < 2) return usage();
    return run_weapons(args[1], game, data);
  }
  if (args[0] == "--replay") {
    if (args.size() < 2) return usage();
    tools::ReplayOptions o;
    o.recordings = args[1];
    o.game = game;
    for (size_t i = 2; i < args.size(); ++i) {
      if (args[i] == "--show" && i + 1 < args.size()) {
        o.show = std::stoi(args[++i]);
      } else if (args[i] == "--no-sync") {
        o.sync = false;
      } else if (args[i] == "--weapon" && i + 1 < args.size()) {
        o.only_weapon = args[++i];
      } else if (args[i] == "--trace" && i + 1 < args.size()) {
        o.trace = args[++i];
      } else if (args[i] == "--json" && i + 1 < args.size()) {
        o.json_out = args[++i];
      } else if (args[i] == "--turns") {
        o.turns = true;
      } else if (args[i] == "--failure-db" && i + 1 < args.size()) {
        o.failure_db = args[++i];
      } else {
        return usage();
      }
    }
    return tools::run_replay(o);
  }
  if (args[0] == "--threat-check") {
    if (args.size() < 2) return usage();
    return tools::run_threat_check(args[1], game);
  }
  if (args[0] == "--score") {
    if (args.size() < 2) return usage();
    return tools::run_score(args[1], game);
  }
  if (args[0] == "--solve") {
    if (args.size() < 2) return usage();
    tools::SolveToolOptions o;
    o.target = args[1];
    o.game = game;
    for (size_t i = 2; i < args.size(); ++i) {
      if (args[i] == "--time" && i + 1 < args.size()) {
        o.time_limit = std::stod(args[++i]);
      } else if (args[i] == "--max-time" && i + 1 < args.size()) {
        o.max_time = std::stod(args[++i]);
      } else if (args[i] == "--min-tiers" && i + 1 < args.size()) {
        o.min_tiers = std::stoi(args[++i]);
      } else if (args[i] == "--nodes" && i + 1 < args.size()) {
        o.node_limit = std::stoull(args[++i]);
      } else if (args[i] == "--beam" && i + 1 < args.size()) {
        o.beam = std::stoi(args[++i]);
      } else if (args[i] == "--threads" && i + 1 < args.size()) {
        o.threads = std::max(1, std::stoi(args[++i]));
      } else if (args[i] == "--sample" && i + 1 < args.size()) {
        o.sample = std::stoi(args[++i]);
      } else if (args[i] == "--shard" && i + 1 < args.size()) {
        const std::string v = args[++i];
        const size_t slash = v.find('/');
        if (slash == std::string::npos) return usage();
        o.shard = std::stoi(v.substr(0, slash));
        o.shards = std::stoi(v.substr(slash + 1));
      } else if (args[i] == "--verbose") {
        o.verbose = true;
      } else if (args[i] == "--json" && i + 1 < args.size()) {
        o.json_out = args[++i];
      } else if (args[i] == "--lua-counts") {
        o.lua_counts = true;
      } else if (args[i] == "--tt" && i + 1 < args.size()) {
        o.tt_entries = std::stoull(args[++i]);
      } else if (args[i] == "--threat-tier") {
        o.threat_tier = true;
      } else if (args[i] == "--objectives-first") {
        o.objectives_first = true;
      } else if (args[i] == "--kills-tier") {
        o.kills_tier = true;
      } else if (args[i] == "--threat-eval") {
        o.threat_eval = true;
      } else {
        return usage();
      }
    }
    return tools::run_solve(o);
  }
  if (args[0] == "--diff-weapons") {
    tools::DiffWeaponsOptions o;
    o.game = game;
    for (size_t i = 1; i < args.size(); ++i) {
      if (args[i] == "--random" && i + 1 < args.size()) {
        o.random = std::stoi(args[++i]);
      } else if (args[i] == "--seed" && i + 1 < args.size()) {
        o.seed = static_cast<uint32_t>(std::stoul(args[++i]));
      } else if (args[i] == "--weapon" && i + 1 < args.size()) {
        o.weapon = args[++i];
      } else if (args[i] == "--all-tables") {
        o.all_tables = true;
      } else if (args[i] == "--ports") {
        o.ports = true;
      } else if (!args[i].starts_with("--") && o.recordings.empty()) {
        o.recordings = args[i];
      } else {
        return usage();
      }
    }
    return tools::run_diff_weapons(o);
  }
  if (args[0] == "--predict") {
    if (args.size() < 2) return usage();
    tools::PredictOptions o;
    o.state = args[1];
    o.game = game;
    for (size_t i = 2; i < args.size(); ++i) {
      if (args[i] == "--actions" && i + 1 < args.size()) {
        o.actions = args[++i];
      } else if (args[i] == "--no-enemy") {
        o.enemy = false;
      } else if (args[i] == "--branches") {
        o.branches = true;
        if (i + 1 < args.size() && !args[i + 1].empty() && std::isdigit(static_cast<unsigned char>(args[i + 1][0]))) {
          o.max_branches = std::max(1, std::stoi(args[++i]));
        }
      } else if (args[i] == "--json" && i + 1 < args.size()) {
        o.json_out = args[++i];
      } else {
        return usage();
      }
    }
    return tools::run_predict(o);
  }
  if (args[0] == "--corpus") {
    if (args.size() < 2) return usage();
    return run_corpus(args[1], data);
  }

  std::string error;
  auto rec = load_recording(args[0], &data, &error);
  if (!rec) {
    std::cerr << "error: " << error << "\n";
    return 1;
  }
  std::printf("%s m%02d turn %d %s (%s)\n", rec->run_id.c_str(), rec->mission_index, rec->turn,
              rec->mission_id.c_str(), rec->phase.c_str());
  std::fputs(render_board(rec->board).c_str(), stdout);
  for (const std::string& w : rec->warnings) std::printf("warning: %s\n", w.c_str());
  return 0;
}
