// itb_inspect --ai DIR (ai_tool.hpp).

#include "ai_tool.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <compare>
#include <tuple>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "itb/ai_phase.hpp"
#include "itb/engine.hpp"
#include "itb/format.hpp"
#include "itb/game_data.hpp"
#include "itb/recording.hpp"

namespace fs = std::filesystem;

namespace itb::tools {

std::optional<SavedAi> read_saved_ai(const fs::path& save, const std::string& mission_key) {
  std::ifstream f(save);
  if (!f) return std::nullopt;
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string text = ss.str();
  const std::string tag = "[\"sMission\"] = \"" + mission_key + "\"";
  const size_t at = text.find(tag);
  if (at == std::string::npos) return std::nullopt;
  // The region's block runs to the next region (or the end).
  size_t end = text.find("[\"region", at);
  if (end == std::string::npos) end = text.size();
  const std::string block = text.substr(at, end - at);
  SavedAi out;
  std::smatch m;
  // The board's turn precedes sMission in the same "player" table.
  const size_t turn_at = text.rfind("[\"iCurrentTurn\"] = ", at);
  if (turn_at != std::string::npos && at - turn_at < 200) {
    out.turn = std::atoi(text.c_str() + turn_at + std::strlen("[\"iCurrentTurn\"] = "));
  }
  static const std::regex seed_re(R"(\["aiSeed"\] = (-?\d+))");
  if (!std::regex_search(block, m, seed_re)) return std::nullopt;
  out.seed = static_cast<uint32_t>(std::stoll(m[1].str()));
  static const std::regex pawn_re(R"(\["id"\] = (\d+),)");
  static const std::regex hist_re(R"(\["targetHistory"\] = Point\((-?\d+),(-?\d+)\))");
  for (auto it = std::sregex_iterator(block.begin(), block.end(), pawn_re); it != std::sregex_iterator(); ++it) {
    const size_t from = static_cast<size_t>(it->position(0));
    const size_t next = block.find("[\"id\"] = ", from + 8);
    const std::string pawn = block.substr(from, (next == std::string::npos ? block.size() : next) - from);
    std::smatch h;
    if (std::regex_search(pawn, h, hist_re)) {
      const Point p{std::stoi(h[1].str()), std::stoi(h[2].str())};
      if (p.valid()) out.last_target[std::stoi((*it)[1].str())] = p;
    }
  }
  return out;
}

namespace {

std::string mission_key_of(const fs::path& state) {
  std::ifstream f(state);
  nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
  if (j.is_discarded()) return "";
  const nlohmann::json* s = &j;
  if (j.contains("data")) {
    s = &j["data"];
    if (s->contains("bridge_state")) s = &(*s)["bridge_state"];
  }
  if (!s->contains("mission_state") || !(*s)["mission_state"].is_object()) return "";
  return (*s)["mission_state"].value("native_key", "");
}

struct VekState {
  Symbol type = kNoSymbol;
  Point pos = kInvalidPoint;
  Point target = kInvalidPoint;
  bool operator==(const VekState&) const = default;
  auto operator<=>(const VekState& o) const {
    return std::tie(type, pos.x, pos.y, target.x, target.y) <=> std::tie(o.type, o.pos.x, o.pos.y, o.target.x, o.target.y);
  }
};

// Living enemy-team pawns on the board: uid -> tile and queued target.
std::map<int32_t, VekState> vek_of(const Board& b) {
  std::map<int32_t, VekState> out;
  for (const Pawn& p : b.pawns()) {
    if (p.team != Team::Enemy || !p.alive() || !p.pos.valid()) continue;
    out[p.uid] = VekState{p.type, p.pos, p.queued.active() ? p.queued.target : kInvalidPoint};
  }
  return out;
}

std::string vek_str(const VekState& v) {
  return std::string(symbol_name(v.type)) + " " + to_visual(v.pos) + (v.target.valid() ? "->" + to_visual(v.target) : " (no shot)");
}

struct Tally {
  int boards = 0, boards_exact = 0;
  int vek = 0, vek_pos = 0, vek_target = 0, vek_both = 0;
  int new_vek = 0, new_vek_matched = 0;
};

// Compares the engine's board after the AI phase with the game's next board.
// `known` = uids on the board before the phase (others are this phase's
// spawns, matched by type, tile and target).
bool compare(const Board& engine, const Board& game, const std::map<int32_t, VekState>& known, Tally& t,
             std::vector<std::string>* diffs) {
  const auto e = vek_of(engine);
  const auto g = vek_of(game);
  bool exact = true;
  std::vector<VekState> e_new, g_new;
  for (const auto& [uid, v] : e) {
    if (!known.count(uid)) e_new.push_back(v);
  }
  for (const auto& [uid, gv] : g) {
    if (!known.count(uid)) {
      g_new.push_back(gv);
      continue;
    }
    ++t.vek;
    auto it = e.find(uid);
    const bool pos = it != e.end() && it->second.pos == gv.pos;
    const bool tgt = it != e.end() && it->second.target == gv.target;
    t.vek_pos += pos;
    t.vek_target += tgt;
    t.vek_both += pos && tgt;
    if (!(pos && tgt)) {
      exact = false;
      if (diffs) {
        diffs->push_back("#" + std::to_string(uid) + " engine " + (it == e.end() ? std::string("absent") : vek_str(it->second)) +
                         " | game " + vek_str(gv));
      }
    }
  }
  for (const VekState& gv : g_new) {
    ++t.new_vek;
    auto it = std::find(e_new.begin(), e_new.end(), gv);
    if (it != e_new.end()) {
      ++t.new_vek_matched;
      e_new.erase(it);
    } else {
      exact = false;
      if (diffs) diffs->push_back("new " + vek_str(gv) + " not predicted");
    }
  }
  ++t.boards;
  t.boards_exact += exact;
  return exact;
}

void print_tally(const char* what, const Tally& t) {
  auto pct = [](int a, int b) { return b ? 100.0 * a / b : 0.0; };
  std::printf("%s: boards %d, all Vek exact %d (%.1f%%)\n", what, t.boards, t.boards_exact, pct(t.boards_exact, t.boards));
  std::printf("  Vek on the board before the phase: %d; tile %d (%.1f%%), target %d (%.1f%%), both %d (%.1f%%)\n",
              t.vek, t.vek_pos, pct(t.vek_pos, t.vek), t.vek_target, pct(t.vek_target, t.vek), t.vek_both,
              pct(t.vek_both, t.vek));
  std::printf("  Vek spawned in the phase: %d, matched (type, tile, target) %d (%.1f%%)\n", t.new_vek,
              t.new_vek_matched, pct(t.new_vek_matched, t.new_vek));
}

struct TurnFile {
  fs::path dir;
  int mission = 0;
  int turn = 0;
};

fs::path turn_path(const TurnFile& f, int turn, const std::string& suffix) {
  char name[64];
  std::snprintf(name, sizeof name, "m%02d_turn_%02d_%s", f.mission, turn, suffix.c_str());
  return f.dir / name;
}

}  // namespace

int run_ai_check(const AiToolOptions& options) {
  std::unique_ptr<Engine> engine = Engine::create(options.game);
  fs::path live = options.dir / "live";
  if (!fs::exists(live)) live = options.dir;
  std::vector<TurnFile> ends, deploys;
  static const std::regex end_re(R"(m(\d+)_turn_(\d+)_end_turn_01\.json)");
  static const std::regex dep_re(R"(m(\d+)_turn_00_deployed_01\.json)");
  for (const auto& e : fs::recursive_directory_iterator(live)) {
    if (!e.is_regular_file()) continue;
    const std::string name = e.path().filename().string();
    std::smatch m;
    if (std::regex_match(name, m, end_re)) {
      ends.push_back({e.path().parent_path(), std::stoi(m[1].str()), std::stoi(m[2].str())});
    } else if (std::regex_match(name, m, dep_re)) {
      deploys.push_back({e.path().parent_path(), std::stoi(m[1].str()), 0});
    }
  }
  auto by_path = [](const TurnFile& a, const TurnFile& b) {
    return std::tie(a.dir, a.mission, a.turn) < std::tie(b.dir, b.mission, b.turn);
  };
  std::sort(ends.begin(), ends.end(), by_path);
  std::sort(deploys.begin(), deploys.end(), by_path);

  // ---- Turns 1+: the recorded seed.
  Tally seeded;
  std::map<size_t, std::pair<int, int>> by_index;  // planning index -> (Vek, matched)
  int chain = 0, chain_ok = 0;
  int shown = 0, skipped_save = 0, skipped_load = 0, skipped_end = 0, lua_err_boards = 0;
  for (const TurnFile& f : ends) {
    const fs::path start = turn_path(f, f.turn, "end_turn_01.json");
    const fs::path save = turn_path(f, f.turn, "saveData.lua");
    const fs::path next = turn_path(f, f.turn + 1, "solve_input.json");
    if (!fs::exists(save) || !fs::exists(next)) {
      ++skipped_save;
      continue;
    }
    std::string err;
    auto rec = load_recording(start, &engine->data(), &err);
    auto nrec = load_recording(next, &engine->data(), &err);
    if (!rec || !nrec) {
      ++skipped_load;
      continue;
    }
    const auto saved = read_saved_ai(save, mission_key_of(start));
    if (!saved) {
      ++skipped_save;
      continue;
    }
    Board b = rec->board;
    TurnContext ctx = turn_context(*rec, Visibility::Full);
    // Grid Defense as the game rolled it: a building the next board shows
    // undamaged resisted (the AI plans on the board those rolls left).
    const Board& before = rec->board;
    const Board& after = nrec->board;
    ctx.grid_resist = [&before, &after](Point p, int) {
      return p.valid() && after.tile(p).hp >= before.tile(p).hp && before.tile(p).hp > 0;
    };
    const PhaseResult phase = engine->end_turn(b, ctx);
    if (phase.mission_ended) {
      ++skipped_end;
      continue;
    }
    std::map<int32_t, VekState> known;
    for (const Pawn& p : rec->board.pawns()) known[p.uid] = {};
    AiPhaseOptions ao;
    ao.seed = saved->seed;
    ao.last_target = saved->last_target;
    ao.actions = action_options(ctx);
    ao.keep_plans = options.verbose;
    const AiPhaseResult ai = engine->ai_phase(b, ao);
    if (!ai.lua_errors.empty()) ++lua_err_boards;
    {
      const auto g = vek_of(nrec->board);
      const auto e = vek_of(b);
      for (size_t i = 0; i < ai.pawns.size(); ++i) {
        auto gi = g.find(ai.pawns[i].uid);
        auto ei = e.find(ai.pawns[i].uid);
        if (gi == g.end() || ei == e.end() || !known.count(ai.pawns[i].uid)) continue;
        const size_t k = std::min<size_t>(i, 5) + (ai.pawns[i].best_ties > 1 ? 10 : 0);
        by_index[k].first += 1;
        by_index[k].second += gi->second == ei->second;
      }
    }
    // The whole phase's draws: the seed it leaves is the next turn's saved one.
    if (auto sn = read_saved_ai(turn_path(f, f.turn + 1, "saveData.lua"), mission_key_of(start))) {
      ++chain;
      chain_ok += sn->seed == ai.seed;
    }
    std::vector<std::string> diffs;
    if (!compare(b, nrec->board, known, seeded, &diffs) && shown < options.show) {
      ++shown;
      std::printf("seeded %s/m%02d t%d (seed %u):\n", f.dir.filename().string().c_str(), f.mission, f.turn,
                  saved->seed);
      for (const std::string& d : diffs) std::printf("    %s\n", d.c_str());
      for (const AiPawnResult& pr : ai.pawns) {
        std::printf("      plan #%d %s %s -> %s tgt %s (t%d p%d) ties %d prev %d%s\n", pr.uid,
                    std::string(symbol_name(pr.type)).c_str(), to_visual(pr.from).c_str(), to_visual(pr.plan.loc).c_str(),
                    to_visual(pr.plan.target).c_str(), pr.plan.target_score, pr.plan.position_score, pr.best_ties,
                    pr.previous_best, pr.deviated ? " DEVIATED" : "");
        if (pr.lua_draws) std::printf("        lua draws %d\n", pr.lua_draws);
        for (const AiPlan& pl : pr.plans) {
          std::printf("          %s tgt %s t%d p%d\n", to_visual(pl.loc).c_str(), to_visual(pl.target).c_str(),
                      pl.target_score, pl.position_score);
        }
      }
      if (options.verbose) {
        std::printf("      draws:");
        for (const auto& d : ai.draws) std::printf(" %d/%d", d.pick, d.options);
        std::printf("\n");
      }
    }
  }
  print_tally("AI phase after turns 1+ (recorded seed)", seeded);
  for (const auto& [i, v] : by_index) {
    std::printf("  planned %s%zu%s: %d Vek, %d exact\n", i % 10 == 5 ? ">=" : "", i % 10 + 1, i >= 10 ? " (tied plans)" : "", v.first, v.second);
  }
  std::printf("  seed after the phase == next turn's saved seed: %d of %d\n", chain_ok, chain);
  std::printf("  skipped: no save/next board %d, load failures %d, mission ended %d; boards with Lua errors %d\n",
              skipped_save, skipped_load, skipped_end, lua_err_boards);

  // ---- Deployment: sampled seeds.
  Tally modal, dep_exact;
  int dep_boards = 0, dep_any = 0, dep_chain = 0, dep_chain_ok = 0;
  double share_sum = 0;
  shown = 0;
  for (const TurnFile& f : deploys) {
    const fs::path start = turn_path(f, 0, "deployed_01.json");
    const fs::path next = turn_path(f, 1, "solve_input.json");
    if (!fs::exists(next)) continue;
    std::string err;
    auto rec = load_recording(start, &engine->data(), &err);
    auto nrec = load_recording(next, &engine->data(), &err);
    if (!rec || !nrec) continue;
    const TurnContext ctx = turn_context(*rec, Visibility::Full);
    Board base = rec->board;
    const PhaseResult phase = engine->end_turn(base, ctx);
    (void)phase;
    std::map<int32_t, VekState> known;
    for (const Pawn& p : rec->board.pawns()) known[p.uid] = {};
    // The seed this phase used, when an earlier save of the run holds the
    // mission's board before it was played (the game builds the island's
    // boards ahead; each keeps its AiController seed).
    {
      const std::string key = mission_key_of(start);
      // The last save of the closest earlier mission (mission keys repeat
      // from island to island).
      std::optional<SavedAi> pre;
      bool searched = false;
      for (int m = f.mission - 1; m >= 0 && !searched; --m) {
        for (int t = 9; t >= 0 && !searched; --t) {
          TurnFile g = f;
          g.mission = m;
          const fs::path save = turn_path(g, t, "saveData.lua");
          if (!fs::exists(save)) continue;
          searched = true;
          auto s = read_saved_ai(save, key);
          if (s && s->turn == 0) pre = s;
        }
      }
      if (pre) {
        Board b = base;
        AiPhaseOptions ao;
        ao.seed = pre->seed;
        ao.actions = action_options(ctx);
        const AiPhaseResult ai = engine->ai_phase(b, ao);
        std::vector<std::string> diffs;
        if (!compare(b, nrec->board, known, dep_exact, &diffs) && shown < options.show) {
          ++shown;
          std::printf("deploy %s/m%02d (seed %u from an earlier save):\n", f.dir.filename().string().c_str(), f.mission,
                      pre->seed);
          for (const std::string& d : diffs) std::printf("    %s\n", d.c_str());
        }
        if (auto s1 = read_saved_ai(turn_path(f, 1, "saveData.lua"), key)) {
          ++dep_chain;
          dep_chain_ok += s1->seed == ai.seed;
        }
      }
    }
    // Outcomes by the Vek they leave; the share of seeds giving each.
    std::map<std::map<int32_t, VekState>, int> seen;
    std::map<std::map<int32_t, VekState>, Board> boards;
    for (int s = 0; s < options.samples; ++s) {
      Board b = base;
      AiPhaseOptions ao;
      ao.seed = static_cast<uint32_t>(0x9E3779B9u * static_cast<uint32_t>(s + 1));
      ao.actions = action_options(ctx);
      engine->ai_phase(b, ao);
      auto key = vek_of(b);
      if (seen[key]++ == 0) boards.emplace(key, b);
    }
    ++dep_boards;
    const auto game = vek_of(nrec->board);
    // The game's outcome among the sampled ones (Vek that existed before).
    bool any = false;
    int game_share = 0;
    for (const auto& [key, n] : seen) {
      Tally scratch;
      if (compare(boards.at(key), nrec->board, known, scratch, nullptr)) {
        any = true;
        game_share += n;
      }
    }
    dep_any += any;
    share_sum += static_cast<double>(game_share) / options.samples;
    // The most common outcome.
    auto mode = std::max_element(seen.begin(), seen.end(),
                                 [](const auto& a, const auto& b) { return a.second < b.second; });
    std::vector<std::string> diffs;
    compare(boards.at(mode->first), nrec->board, known, modal, &diffs);
    if (!any && shown < options.show) {
      ++shown;
      std::printf("deploy %s/m%02d: game outcome not among %zu sampled outcomes; most common (%d/%d):\n",
                  f.dir.filename().string().c_str(), f.mission, seen.size(), mode->second, options.samples);
      for (const std::string& d : diffs) std::printf("    %s\n", d.c_str());
    }
  }
  std::printf("\nAI phase before turn 1 (deployment, %d sampled seeds each): boards %d\n", options.samples, dep_boards);
  std::printf("  game outcome among the sampled outcomes: %d (%.1f%%); mean share of seeds giving it %.1f%%\n", dep_any,
              dep_boards ? 100.0 * dep_any / dep_boards : 0.0, dep_boards ? 100.0 * share_sum / dep_boards : 0.0);
  print_tally("  most common sampled outcome vs the game", modal);
  print_tally("AI phase before turn 1 with the seed from an earlier save", dep_exact);
  std::printf("  seed after the phase == turn 1's saved seed: %d of %d\n", dep_chain_ok, dep_chain);
  return 0;
}

}  // namespace itb::tools
