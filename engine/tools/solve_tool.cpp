// itb_inspect --solve: the stage 9 search on recorded boards.
//
// For one recording: prints the plan (A1-H8 notation), its worst-case score
// and proof status, and compares it with the plan the old bot executed
// (m<NN>_turn_<NN>_solve.json), evaluated the same way (evaluate_plan: every
// chance outcome, worst case). For a directory: every *_solve_input.json
// under it (optionally an evenly spaced sample or a shard), then a summary.
// The recorded plan is one candidate of the search space, so when the search
// completes our worst case can never be below it.

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "itb/core.hpp"
#include "itb/engine.hpp"
#include "itb/format.hpp"
#include "itb/game_data.hpp"
#include "itb/recording.hpp"
#include "itb/solver.hpp"
#include "solve_tool.hpp"

namespace fs = std::filesystem;
using nlohmann::json;

namespace itb::tools {
namespace {

const char* kTierNames[kScoreKeys] = {"grid",    "building_hp", "mechs", "objectives_failed", "next_turn", "threat",
                                      "mech_hp", "objectives",  "kills", "vek_hp",            "position"};

Point json_point(const json& j) {
  if (!j.is_array() || j.size() < 2 || !j[0].is_number() || !j[1].is_number()) return kInvalidPoint;
  return {j[0].get<int>(), j[1].get<int>()};
}

// The tiers in use (score.hpp tier_used): Threat only with the threat tier.
std::string score_text(const Score& s, TierSet threat) {
  std::string out;
  for (int i = 0; i < kScoreKeys; ++i) {
    if (!tier_used(i, threat)) continue;
    if (!out.empty()) out += " ";
    out += kTierNames[i];
    out += "=";
    out += s.v[static_cast<size_t>(i)] == INT32_MAX ? std::string("inf") : std::to_string(s.v[static_cast<size_t>(i)]);
  }
  return out;
}

std::vector<int32_t> score_vector(const Score& s, TierSet threat) {
  std::vector<int32_t> v;
  for (int i = 0; i < kScoreKeys; ++i) {
    if (tier_used(i, threat)) v.push_back(s.v[static_cast<size_t>(i)]);
  }
  return v;
}

// The first tier where a and b differ, or -1.
int first_diff(const Score& a, const Score& b) {
  for (int i = 0; i < kScoreKeys; ++i) {
    if (a.v[static_cast<size_t>(i)] != b.v[static_cast<size_t>(i)]) return i;
  }
  return -1;
}

// The old bot's executed plan for this board, if recorded.
std::optional<std::vector<PlayerAction>> recorded_plan(const fs::path& input, const Recording& rec, Engine& engine) {
  const std::string name = input.filename().string();
  const std::string suffix = "_solve_input.json";
  if (!name.ends_with(suffix)) return std::nullopt;
  fs::path solve = input;
  solve.replace_filename(name.substr(0, name.size() - suffix.size()) + "_solve.json");
  if (!fs::exists(solve)) return std::nullopt;
  json root;
  try {
    std::ifstream in(solve);
    in >> root;
  } catch (const std::exception&) {
    return std::nullopt;
  }
  if (!root.contains("data")) return std::nullopt;
  const json& d = root["data"];
  if (d.contains("partial_re_solve") || !d.contains("actions") || !d["actions"].is_array()) return std::nullopt;
  std::vector<PlayerAction> plan;
  for (const json& a : d["actions"]) {
    PlayerAction p;
    p.uid = a.value("mech_uid", -1);
    p.move = json_point(a.value("move_to", json()));
    const std::string weapon = a.value("weapon_id", "");
    const Point target = json_point(a.value("target", json()));
    if (weapon == "_REPAIR") {
      p.kind = PlayerAction::Kind::Repair;
      p.weapon = "Skill_Repair";
      for (const auto& [uid, pilot] : rec.pilots) {
        if (uid == p.uid) p.weapon = engine.repair_skill(pilot);
      }
    } else if (!weapon.empty() && weapon != "Unknown" && target.valid()) {
      p.kind = PlayerAction::Kind::Weapon;
      p.weapon = weapon;
      p.target = target;
    }
    plan.push_back(std::move(p));
  }
  return plan;
}

struct Row {
  std::string file;
  bool proven = false;
  double time = 0;
  SolveStats stats;
  Score ours;
  std::optional<Score> recorded;
  std::string recorded_refused;
  int compare = 0;  // +1 ours better, 0 equal, -1 worse
  int tier = -1;    // first differing tier
  int proven_components = 0;
  int units = 0;
};

double percentile(std::vector<double> v, double q) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  const size_t i = std::min(v.size() - 1, static_cast<size_t>(q * static_cast<double>(v.size() - 1) + 0.5));
  return v[i];
}

}  // namespace

int run_solve(const SolveToolOptions& opt) {
  const TierSet tier_on =
      ((opt.threat_tier || opt.kills_tier) ? kTierThreat : 0u) | (opt.lookahead ? kTierNextTurn : 0u);
  std::unique_ptr<Engine> engine;
  try {
    engine = Engine::create(opt.game);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  // One engine per extra search thread, loaded once for every board.
  std::vector<std::unique_ptr<Engine>> helpers;
  for (int i = 1; i < opt.threads; ++i) helpers.push_back(Engine::create(opt.game));
  std::vector<Engine*> helper_ptrs;
  for (auto& h : helpers) helper_ptrs.push_back(h.get());
  if (opt.lua_counts) {
    engine->lua().set_call_counting(true);
    for (Engine* h : helper_ptrs) h->lua().set_call_counting(true);
  }
  // Lua calls by "table:method", summed over the engines (--lua-counts).
  auto print_lua_counts = [&] {
    if (!opt.lua_counts) return;
    std::map<std::string, uint64_t> total;
    for (const auto& [k, n] : engine->lua().call_counts()) total[k] += n;
    for (Engine* h : helper_ptrs) {
      for (const auto& [k, n] : h->lua().call_counts()) total[k] += n;
    }
    std::vector<std::pair<uint64_t, std::string>> by_count;
    uint64_t sum = 0;
    for (const auto& [k, n] : total) {
      by_count.emplace_back(n, k);
      sum += n;
    }
    std::sort(by_count.rbegin(), by_count.rend());
    std::printf("\nLua calls: %llu\n", static_cast<unsigned long long>(sum));
    uint64_t run = 0;
    for (const auto& [n, k] : by_count) {
      run += n;
      std::printf("  %12llu %5.1f%% %5.1f%%  %s\n", static_cast<unsigned long long>(n), 100.0 * static_cast<double>(n) / static_cast<double>(std::max<uint64_t>(1, sum)),
                  100.0 * static_cast<double>(run) / static_cast<double>(std::max<uint64_t>(1, sum)), k.c_str());
    }
  };
  std::vector<fs::path> files;
  if (fs::is_directory(opt.target)) {
    for (const auto& e : fs::recursive_directory_iterator(opt.target)) {
      const std::string name = e.path().filename().string();
      if (e.is_regular_file() && name.ends_with("_solve_input.json")) files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    if (opt.sample > 0 && static_cast<size_t>(opt.sample) < files.size()) {
      std::vector<fs::path> picked;
      for (int i = 0; i < opt.sample; ++i) {
        picked.push_back(files[static_cast<size_t>(i) * files.size() / static_cast<size_t>(opt.sample)]);
      }
      files = std::move(picked);
    }
    if (opt.shards > 1) {
      std::vector<fs::path> mine;
      for (size_t i = 0; i < files.size(); ++i) {
        if (static_cast<int>(i % static_cast<size_t>(opt.shards)) == opt.shard) mine.push_back(files[i]);
      }
      files = std::move(mine);
    }
  } else {
    files.push_back(opt.target);
  }
  const bool single = files.size() == 1;
  std::ofstream jout;
  if (!opt.json_out.empty()) jout.open(opt.json_out);

  std::vector<Row> rows;
  std::map<std::string, int> skipped;
  for (const fs::path& f : files) {
    std::string error;
    std::optional<Recording> rec = load_recording(f, &engine->data(), &error);
    if (!rec) {
      ++skipped["did not load"];
      continue;
    }
    Board board = rec->board;
    SolveOptions so;
    so.time_limit_s = opt.time_limit;
    so.node_limit = opt.node_limit;
    so.max_time_s = opt.max_time;
    so.min_proven_tiers = opt.min_tiers;
    so.threat_tier = opt.threat_tier;
    so.threat_kills = opt.kills_tier;
    so.objectives_first = opt.objectives_first;
    so.mech_grid = opt.mech_grid;
    so.mech_grid_turns = opt.mech_grid_turns;
    so.mech_grid_pilot = opt.mech_grid_pilot;
    so.lookahead = opt.lookahead;
    so.lookahead_options.samples = std::max(1, opt.lookahead_samples);
    if (opt.lookahead_idle) so.lookahead_options.reply = LookaheadOptions::Reply::Idle;
    so.lookahead_options.sum = opt.lookahead_sum;
    so.helper_engines = helper_ptrs;
    if (opt.beam >= 0) so.beam_width = opt.beam;
    if (opt.tt_entries > 0) so.tt_max_entries = opt.tt_entries;
    for (const auto& [uid, pilot] : rec->pilots) {
      if (Pawn* p = board.find_pawn(uid)) {
        p->pilot_abilities |= engine->pilot_ability(pilot);
        so.repair_skills.emplace_back(uid, engine->repair_skill(pilot));
      }
    }
    so.lookahead_options.repair_skills = so.repair_skills;
    int units = 0;
    for (const Pawn& p : board.pawns()) {
      if (p.controlled() && p.alive() && p.active) ++units;
    }
    if (units == 0) {
      ++skipped["no active unit"];
      continue;
    }
    const TurnContext ctx = turn_context(*rec);

    const SolveResult r = solve_turn(*engine, board, ctx, so);
    Row row;
    row.file = f.parent_path().filename().string() + "/" + f.filename().string();
    row.proven = r.proven_optimal;
    row.time = r.stats.time_s;
    row.stats = r.stats;
    row.ours = r.best.worst_case;
    row.proven_components = r.proven_components;
    row.units = units;
    if (std::optional<std::vector<PlayerAction>> plan = recorded_plan(f, *rec, *engine)) {
      row.recorded = evaluate_plan(*engine, board, ctx, *plan, so, &row.recorded_refused);
      if (row.recorded) {
        row.tier = first_diff(row.ours, *row.recorded);
        row.compare = row.ours > *row.recorded ? 1 : row.ours < *row.recorded ? -1 : 0;
      }
    }

    // With the mech exchange: the plan's worst case with plain tiers.
    std::optional<Score> plain;
    if (so.mech_grid >= 0) {
      SolveOptions po = so;
      po.mech_grid = -1;
      po.helper_engines.clear();
      plain = evaluate_plan(*engine, board, ctx, r.best.actions, po);
    }

    if (single || opt.verbose) {
      std::printf("%s m%02d turn %d %s\n", rec->run_id.c_str(), rec->mission_index, rec->turn, rec->mission_id.c_str());
      if (single) std::fputs(render_board(board).c_str(), stdout);
      std::printf("plan (%s%s%s, %.2fs):\n", r.proven_optimal ? "proven optimal" : "best found",
                  r.best.contingent ? ", contingent on a chance node" : "",
                  r.extended ? ", extended past the base time" : "", r.stats.time_s);
      Board cur = board;
      int i = 0;
      for (const PlayerAction& a : r.best.actions) std::printf("  %d. %s\n", ++i, describe_action(cur, a).c_str());
      if (r.best.actions.empty()) std::printf("  (end turn)\n");
      std::printf("worst case: %s\n", score_text(r.best.worst_case, tier_on).c_str());
      if (plain) std::printf("plain tiers (no mech exchange): %s\n", score_text(*plain, tier_on).c_str());
      if (!r.proven_optimal && r.upper_bound) {
        std::printf("upper bound: %s (optimal in the first %d tiers)\n", score_text(*r.upper_bound, tier_on).c_str(),
                    r.proven_components);
      }
      std::printf("search (%d threads): %llu nodes, %llu sub-actions (%.2fs), %llu enemy phases (%.2fs), %llu chance branches, "
                  "%llu TT hits, %llu leaf hits, %llu duplicates, %llu bound prunes, first plan %.3fs, best %.3fs\n",
                  r.stats.threads, static_cast<unsigned long long>(r.stats.nodes),
                  static_cast<unsigned long long>(r.stats.sub_actions),
                  r.stats.sub_action_s, static_cast<unsigned long long>(r.stats.enemy_phases), r.stats.enemy_phase_s,
                  static_cast<unsigned long long>(r.stats.chance_branches),
                  static_cast<unsigned long long>(r.stats.tt_hits), static_cast<unsigned long long>(r.stats.leaf_hits),
                  static_cast<unsigned long long>(r.stats.duplicate_children),
                  static_cast<unsigned long long>(r.stats.bound_prunes), r.stats.first_plan_s, r.stats.best_plan_s);
      if (so.lookahead) {
        std::printf("lookahead: %llu estimates (%.2fs), %llu reused\n",
                    static_cast<unsigned long long>(r.stats.lookahead_evals), r.stats.lookahead_s,
                    static_cast<unsigned long long>(r.stats.lookahead_hits));
      }
      for (const std::string& w : r.warnings) std::printf("warning: %s\n", w.c_str());
      if (row.recorded) {
        std::printf("recorded plan: %s -> %s%s\n", score_text(*row.recorded, tier_on).c_str(),
                    row.compare > 0 ? "ours better in " : row.compare < 0 ? "OURS WORSE in " : "equal",
                    row.tier >= 0 ? kTierNames[row.tier] : "");
      } else if (!row.recorded_refused.empty()) {
        std::printf("recorded plan refused: %s\n", row.recorded_refused.c_str());
      }
      std::fflush(stdout);
    }
    if (jout.is_open()) {
      json j{{"file", row.file},
             {"proven", row.proven},
             {"time", row.time},
             {"units", units},
             {"threads", r.stats.threads},
             {"nodes", r.stats.nodes},
             {"sub_actions", r.stats.sub_actions},
             {"enemy_phases", r.stats.enemy_phases},
             {"sub_action_s", r.stats.sub_action_s},
             {"enemy_phase_s", r.stats.enemy_phase_s},
             {"chance_branches", r.stats.chance_branches},
             {"tt_hits", r.stats.tt_hits},
             {"tt_entries", r.stats.tt_entries},
             {"leaf_entries", r.stats.leaf_entries},
             {"first_plan_s", r.stats.first_plan_s},
             {"best_plan_s", r.stats.best_plan_s},
             {"ours", score_vector(r.best.worst_case, tier_on)},
             {"contingent", r.best.contingent},
             {"proven_components", r.proven_components},
             {"chance_exact", r.chance_exact},
             {"extended", r.extended},
             {"warnings", r.warnings}};
      if (r.upper_bound) j["upper"] = score_vector(*r.upper_bound, tier_on);
      if (opt.threat_eval) {
        json plan = json::array();
        for (const PlayerAction& a : r.best.actions) plan.push_back(describe_action(board, a));
        j["plan"] = plan;
        SolveOptions eo = so;
        eo.threat_tier = true;
        eo.threat_kills = false;
        eo.objectives_first = false;
        eo.mech_grid = -1;
        eo.helper_engines.clear();
        if (const auto e = evaluate_plan(*engine, board, ctx, r.best.actions, eo)) j["threat_eval"] = e->v;
      }
      if (plain) j["plain"] = score_vector(*plain, tier_on);
      if (row.recorded) {
        j["recorded"] = score_vector(*row.recorded, tier_on);
        j["compare"] = row.compare;
        // As a position among the tiers in use (the "ours" vector's index).
        int tier = row.tier;
        for (int i = 0; i < row.tier; ++i) {
          if (!tier_used(i, tier_on)) --tier;
        }
        j["tier"] = tier;
      } else if (!row.recorded_refused.empty()) {
        j["recorded_refused"] = row.recorded_refused;
      }
      jout << j.dump() << "\n";
      jout.flush();
    }
    if (!single && !opt.verbose) {
      std::printf("%-60s %s %7.2fs %9llu nodes%s\n", row.file.c_str(), row.proven ? "PROVEN" : "open  ", row.time,
                  static_cast<unsigned long long>(row.stats.nodes),
                  row.recorded ? (row.compare > 0 ? "  ours better" : row.compare < 0 ? "  OURS WORSE" : "  = recorded") : "");
      std::fflush(stdout);
    }
    rows.push_back(std::move(row));
  }
  if (single) {
    print_lua_counts();
    return rows.empty() ? 1 : 0;
  }

  // Summary.
  std::vector<double> proof_times;
  int proven = 0, better = 0, equal = 0, worse = 0, worse_proven = 0, refused = 0;
  std::map<std::string, int> better_tier, worse_tier;
  for (const Row& r : rows) {
    if (r.proven) {
      ++proven;
      proof_times.push_back(r.time);
    }
    if (r.recorded) {
      if (r.compare > 0) {
        ++better;
        ++better_tier[kTierNames[r.tier]];
      } else if (r.compare < 0) {
        ++worse;
        if (r.proven) ++worse_proven;
        ++worse_tier[kTierNames[r.tier]];
      } else {
        ++equal;
      }
    } else if (!r.recorded_refused.empty()) {
      ++refused;
    }
  }
  const double n = std::max<double>(1, static_cast<double>(rows.size()));
  std::printf("\nboards solved: %zu (skipped:", rows.size());
  for (const auto& [why, k] : skipped) std::printf(" %s %d", why.c_str(), k);
  std::printf(")\nproven optimal: %d (%.1f%%) within %.0fs", proven, 100.0 * proven / n, opt.time_limit);
  for (double t : {1.0, 10.0, 60.0}) {
    if (t >= opt.time_limit) continue;
    const auto k = std::count_if(proof_times.begin(), proof_times.end(), [t](double x) { return x <= t; });
    std::printf(", %.1f%% within %.0fs", 100.0 * static_cast<double>(k) / n, t);
  }
  std::printf("\ntime to prove: median %.2fs, p95 %.2fs\n", percentile(proof_times, 0.5), percentile(proof_times, 0.95));
  std::printf("vs recorded plan: ours better %d, equal %d, ours worse %d (%d of them proven: search bugs), "
              "recorded plan refused %d\n",
              better, equal, worse, worse_proven, refused);
  for (const auto& [t, k] : better_tier) std::printf("  better in %-18s %d\n", t.c_str(), k);
  for (const auto& [t, k] : worse_tier) std::printf("  worse in  %-18s %d\n", t.c_str(), k);
  print_lua_counts();
  return worse_proven == 0 ? 0 : 1;
}

}  // namespace itb::tools
