// itb_inspect --lookahead-study: what a turn's plan leaves for the next turn.
//
// For each recorded turn-start board: solve the turn (the live flags, and
// with --compare also with the next-turn lookahead), play each plan through
// its enemy phase (default chance outcomes), let the AI plan the next turn
// with sampled seeds (lookahead.hpp next_turn_start) and measure on each of
// those next-turn boards:
//   - the reference: a real solve of the next turn (--next-time, the live
//     flags) and the plain losses of its plan (grid, mechs, building HP),
//     i.e. what that board forces as far as the solver can tell;
//   - the cheap estimates: the idle loss and the greedy reply's loss;
//   - their cost.
// With the recorded next turn present (same run and mission), its board is
// solved the same way: what the game's board actually forced.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "itb/ai_phase.hpp"
#include "itb/engine.hpp"
#include "itb/format.hpp"
#include "itb/game_data.hpp"
#include "itb/lookahead.hpp"
#include "itb/recording.hpp"
#include "itb/solver.hpp"
#include "lookahead_tool.hpp"

namespace fs = std::filesystem;
using nlohmann::json;

namespace itb::tools {
namespace {

using Clock = std::chrono::steady_clock;
double since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

json loss_json(const NextTurnLoss& l) { return json::array({l.grid, l.mechs, l.building_hp}); }
std::string loss_text(const NextTurnLoss& l) {
  return "g" + std::to_string(l.grid) + "/m" + std::to_string(l.mechs) + "/b" + std::to_string(l.building_hp);
}

NextTurnLoss plain_loss(const Score& s) {
  NextTurnLoss l;
  l.grid = std::max(0, -s[ScoreKey::GridLost]);
  l.mechs = std::max(0, -s[ScoreKey::MechsLost]);
  l.building_hp = std::max(0, -s[ScoreKey::BuildingHpLost]);
  return l;
}

struct Loaded {
  Recording rec;
  Board board;
  TurnContext ctx;
  std::vector<std::pair<int32_t, std::string>> repair;
};

std::optional<Loaded> load(const fs::path& f, Engine& engine) {
  std::string error;
  std::optional<Recording> rec = load_recording(f, &engine.data(), &error);
  if (!rec) return std::nullopt;
  Loaded l{*rec, rec->board, turn_context(*rec), {}};
  for (const auto& [uid, pilot] : rec->pilots) {
    if (Pawn* p = l.board.find_pawn(uid)) {
      p->pilot_abilities |= engine.pilot_ability(pilot);
      l.repair.emplace_back(uid, engine.repair_skill(pilot));
    }
  }
  return l;
}

// The recorded turn after `f` (same run and mission), if any.
fs::path next_turn_file(const fs::path& f) {
  static const std::regex re(R"(m(\d+)_turn_(\d+)_solve_input\.json)");
  std::smatch m;
  const std::string name = f.filename().string();
  if (!std::regex_match(name, m, re)) return {};
  char buf[64];
  std::snprintf(buf, sizeof buf, "m%s_turn_%02d_solve_input.json", m[1].str().c_str(), std::stoi(m[2].str()) + 1);
  fs::path n = f;
  n.replace_filename(buf);
  return fs::exists(n) ? n : fs::path{};
}

}  // namespace

int run_lookahead_study(const LookaheadStudyOptions& opt) {
  std::unique_ptr<Engine> engine;
  try {
    engine = Engine::create(opt.game);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  std::vector<std::unique_ptr<Engine>> helpers;
  for (int i = 1; i < opt.threads; ++i) helpers.push_back(Engine::create(opt.game));
  std::vector<Engine*> helper_ptrs;
  for (auto& h : helpers) helper_ptrs.push_back(h.get());

  std::vector<fs::path> files;
  if (fs::is_directory(opt.target)) {
    for (const auto& e : fs::recursive_directory_iterator(opt.target)) {
      const std::string name = e.path().filename().string();
      if (e.is_regular_file() && name.ends_with("_solve_input.json")) files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
  } else {
    files.push_back(opt.target);
  }
  std::ofstream jout;
  if (!opt.json_out.empty()) jout.open(opt.json_out, std::ios::app);

  auto live_options = [&](const std::vector<std::pair<int32_t, std::string>>& repair, double time) {
    SolveOptions so;
    so.time_limit_s = time;
    so.threat_tier = true;
    so.objectives_first = true;
    so.mech_grid = 2;
    so.mech_grid_turns = true;
    so.helper_engines = helper_ptrs;
    so.repair_skills = repair;
    return so;
  };
  // The plain losses of the next turn's best plan (a real solve).
  auto reference = [&](const Board& start, const TurnContext& ctx,
                       const std::vector<std::pair<int32_t, std::string>>& repair, double* secs,
                       bool* proven) {
    const Clock::time_point t0 = Clock::now();
    SolveOptions so = live_options(repair, opt.next_time);
    const SolveResult r = solve_turn(*engine, start, ctx, so);
    SolveOptions plain;
    plain.repair_skills = repair;
    const std::optional<Score> s = evaluate_plan(*engine, start, ctx, r.best.actions, plain);
    if (opt.verbose) {
      for (const PlayerAction& p : r.best.actions) std::printf("        ref: %s\n", describe_action(start, p).c_str());
    }
    if (secs) *secs = since(t0);
    if (proven) *proven = r.proven_components >= 3;
    return s ? plain_loss(*s) : NextTurnLoss{};
  };

  for (const fs::path& f : files) {
    std::optional<Loaded> l = load(f, *engine);
    if (!l) continue;
    int units = 0;
    for (const Pawn& p : l->board.pawns()) units += (p.controlled() && p.alive() && p.active) ? 1 : 0;
    if (units == 0) continue;
    const std::string name = f.parent_path().filename().string() + "/" + f.filename().string();
    json row;
    row["file"] = name;
    row["turn"] = l->board.turn;
    row["total_turns"] = l->board.total_turns;
    std::printf("== %s (turn %d/%d)\n", name.c_str(), l->board.turn, l->board.total_turns);

    LookaheadOptions la = opt.la;
    la.repair_skills = l->repair;
    TurnContext next_ctx = l->ctx;
    next_ctx.spawn_types.clear();

    struct Arm {
      std::string name;
      SolveOptions so;
    };
    std::vector<Arm> arms;
    if (!opt.skip_base) arms.push_back({"base", live_options(l->repair, opt.time)});
    if (opt.compare) {
      SolveOptions so = live_options(l->repair, opt.time);
      so.lookahead = true;
      so.lookahead_options = opt.solver_la;
      so.lookahead_options.repair_skills = l->repair;
      arms.push_back({"lookahead", so});
    }
    for (const Arm& arm : arms) {
      const Clock::time_point t0 = Clock::now();
      const SolveResult r = solve_turn(*engine, l->board, l->ctx, arm.so);
      json a;
      a["time"] = since(t0);
      a["proven_components"] = r.proven_components;
      a["proven"] = r.proven_optimal;
      a["stats"] = {{"nodes", r.stats.nodes},
                    {"sub_actions", r.stats.sub_actions},
                    {"enemy_phases", r.stats.enemy_phases},
                    {"sub_action_s", r.stats.sub_action_s},
                    {"enemy_phase_s", r.stats.enemy_phase_s},
                    {"lookahead_evals", r.stats.lookahead_evals},
                    {"lookahead_hits", r.stats.lookahead_hits},
                    {"lookahead_s", r.stats.lookahead_s},
                    {"best_plan_s", r.stats.best_plan_s}};
      std::vector<int32_t> wc(r.best.worst_case.v.begin(), r.best.worst_case.v.end());
      a["worst_case"] = wc;
      json plan = json::array();
      for (const PlayerAction& p : r.best.actions) plan.push_back(describe_action(l->board, p));
      a["plan"] = plan;
      std::printf("  [%s] %.1fs proven %d: %s\n", arm.name.c_str(), since(t0), r.proven_components,
                  r.best.worst_case.describe(arm.so.tiers()).c_str());
      for (const PlayerAction& p : r.best.actions) std::printf("      %s\n", describe_action(l->board, p).c_str());
      Board after = l->board;
      const TurnResult tr = engine->play_turn(after, r.best.actions, l->ctx);
      if (!tr.ok()) {
        std::printf("      plan refused at action %d\n", tr.refused);
        continue;
      }
      a["this_turn_plain"] = loss_json(turn_loss(l->board, after, l->ctx, tr.enemy));
      if (tr.enemy.mission_ended || l->board.turn >= l->board.total_turns) {
        std::printf("      last turn of the mission\n");
        row[arm.name] = a;
        continue;
      }
      json samples = json::array();
      NextTurnLoss worst_ref, worst_idle, worst_greedy;
      // Sums over the samples: grid lost, mechs lost (greedy, reference),
      // samples with any grid or mech lost.
      double g_grid = 0, g_mech = 0, r_grid = 0, r_mech = 0;
      int g_any = 0, r_any = 0;
      double t_ai = 0, t_idle = 0, t_greedy = 0, t_ref = 0;
      for (int k = 0; k < opt.samples; ++k) {
        LookaheadOptions lo = la;
        lo.seed = opt.seed;
        Clock::time_point t = Clock::now();
        AiPhaseResult ai;
        const Board start =
            next_turn_start(*engine, l->board, after, &tr.enemy, next_ctx, lookahead_seed(lo, k), lo, &ai);
        if (opt.verbose) {
          std::printf("      seed %d AI:", k);
          for (const AiPawnResult& p : ai.pawns) {
            std::printf(" %s%s->%s%s(t%d,p%d,ties%d/%d)", std::string(symbol_name(p.type)).c_str(),
                        to_visual(p.end).c_str(), p.plan.target.valid() ? to_visual(p.plan.target).c_str() : "-",
                        p.deviated ? "*" : "", p.plan.target_score, p.plan.position_score, p.best_ties, p.previous_best);
          }
          std::printf("\n");
        }
        t_ai += since(t);
        t = Clock::now();
        const NextTurnLoss idle = idle_loss(*engine, start, next_ctx);
        t_idle += since(t);
        t = Clock::now();
        lo.reply = LookaheadOptions::Reply::Greedy;
        std::vector<PlayerAction> gplan;
        const NextTurnLoss greedy = reply_loss(*engine, start, next_ctx, lo, &gplan);
        t_greedy += since(t);
        // The greedy reply re-checked over every chance outcome.
        SolveOptions plain;
        plain.repair_skills = l->repair;
        const std::optional<Score> gcheck = evaluate_plan(*engine, start, next_ctx, gplan, plain);
        double rs = 0;
        bool proven = false;
        const NextTurnLoss ref = opt.next_time > 0 ? reference(start, next_ctx, l->repair, &rs, &proven) : NextTurnLoss{};
        t_ref += rs;
        samples.push_back({{"idle", loss_json(idle)}, {"greedy", loss_json(greedy)}, {"ref", loss_json(ref)},
                           {"ref_proven3", proven}});
        auto w = [](NextTurnLoss& acc, const NextTurnLoss& v, int k) {
          if (k == 0 || next_turn_value(v) < next_turn_value(acc)) acc = v;
        };
        w(worst_ref, ref, k);
        w(worst_idle, idle, k);
        w(worst_greedy, greedy, k);
        g_grid += greedy.grid;
        g_mech += greedy.mechs;
        g_any += (greedy.grid > 0 || greedy.mechs > 0) ? 1 : 0;
        r_grid += ref.grid;
        r_mech += ref.mechs;
        r_any += (ref.grid > 0 || ref.mechs > 0) ? 1 : 0;
        std::printf("      seed %d: ref %s%s  greedy %s (checked %s)  idle %s\n", k, loss_text(ref).c_str(),
                    proven ? "" : "?", loss_text(greedy).c_str(),
                    gcheck ? loss_text(plain_loss(*gcheck)).c_str() : "refused", loss_text(idle).c_str());
        if (opt.verbose) {
          for (const PlayerAction& p : gplan) std::printf("        greedy: %s\n", describe_action(start, p).c_str());
          Board replay = start;
          const TurnResult gr = engine->play_turn(replay, gplan, next_ctx);
          std::printf("        greedy replayed (default chance): %s, chance nodes %zu:",
                      gr.ok() ? loss_text(turn_loss(start, replay, next_ctx, gr.enemy)).c_str() : "refused",
                      gr.enemy.chances.size());
          for (const ChanceRecord& c : gr.enemy.chances) std::printf(" %d/%d", static_cast<int>(c.kind), c.options);
          std::printf("\n");
        }
      }
      const double n = std::max(1, opt.samples);
      a["mean_greedy"] = json::array({g_grid / n, g_mech / n});
      a["mean_ref"] = json::array({r_grid / n, r_mech / n});
      a["any_greedy"] = g_any;
      a["any_ref"] = r_any;
      std::printf("      mean grid/mechs lost next turn: greedy %.2f/%.2f (%d of %d samples lose any), ref %.2f/%.2f (%d)\n",
                  g_grid / n, g_mech / n, g_any, opt.samples, r_grid / n, r_mech / n, r_any);
      a["samples"] = samples;
      a["worst_ref"] = loss_json(worst_ref);
      a["worst_greedy"] = loss_json(worst_greedy);
      a["worst_idle"] = loss_json(worst_idle);
      a["t_ai"] = t_ai / n;
      a["t_idle"] = t_idle / n;
      a["t_greedy"] = t_greedy / n;
      a["t_ref"] = t_ref / n;
      std::printf("      worst: ref %s greedy %s idle %s | per sample: ai %.1f ms, idle %.2f ms, greedy %.1f ms\n",
                  loss_text(worst_ref).c_str(), loss_text(worst_greedy).c_str(), loss_text(worst_idle).c_str(),
                  1000 * t_ai / n, 1000 * t_idle / n, 1000 * t_greedy / n);
      row[arm.name] = a;
    }
    if (opt.next_time > 0) {
      const fs::path nf = next_turn_file(f);
      if (!nf.empty()) {
        if (std::optional<Loaded> nl = load(nf, *engine)) {
          double rs = 0;
          bool proven = false;
          const NextTurnLoss real = reference(nl->board, nl->ctx, nl->repair, &rs, &proven);
          row["real_next"] = loss_json(real);
          std::printf("  recorded next turn: forced %s (%.1fs)\n", loss_text(real).c_str(), rs);
        }
      }
    }
    if (jout) jout << row.dump() << "\n" << std::flush;
    std::fflush(stdout);
  }
  return 0;
}

}  // namespace itb::tools
