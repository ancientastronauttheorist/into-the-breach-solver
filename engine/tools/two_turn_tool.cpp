// itb_inspect --two-turn (two_turn_tool.hpp).

#include "two_turn_tool.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "itb/ai_phase.hpp"
#include "itb/board_hash.hpp"
#include "itb/engine.hpp"
#include "itb/lookahead.hpp"
#include "itb/recording.hpp"
#include "itb/solver.hpp"

namespace fs = std::filesystem;
using nlohmann::json;

namespace itb::tools {
namespace {

using Clock = std::chrono::steady_clock;
double since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

using Repair = std::vector<std::pair<int32_t, std::string>>;

struct Loaded {
  Recording rec;
  Board board;
  TurnContext ctx;
  Repair repair;
};

std::optional<Loaded> load(const fs::path& f, Engine& engine) {
  std::string error;
  std::optional<Recording> rec = load_recording(f, &engine.data(), &error);
  if (!rec) {
    std::fprintf(stderr, "error: %s did not load: %s\n", f.string().c_str(), error.c_str());
    return std::nullopt;
  }
  Loaded l{*rec, rec->board, turn_context(*rec, Visibility::Player), {}};
  for (const auto& [uid, pilot] : rec->pilots) {
    if (Pawn* p = l.board.find_pawn(uid)) {
      p->pilot_abilities |= engine.pilot_ability(pilot);
      l.repair.emplace_back(uid, engine.repair_skill(pilot));
    }
  }
  return l;
}

Point json_pt(const json& j) {
  if (j.is_array() && j.size() == 2) return Point{j[0].get<int>(), j[1].get<int>()};
  return kInvalidPoint;
}

std::vector<Point> read_zone(const fs::path& path) {
  std::ifstream in(path);
  json j = json::parse(in, nullptr, false);
  const json* st = &j;
  if (j.contains("data") && j["data"].is_object()) {
    st = &j["data"];
    if (st->contains("bridge_state")) st = &(*st)["bridge_state"];
  }
  std::vector<Point> zone;
  for (const char* key : {"drop_zone", "deployment_zone"}) {
    if (!st->contains(key) || !(*st)[key].is_array() || (*st)[key].empty()) continue;
    for (const json& p : (*st)[key]) zone.push_back(json_pt(p));
    break;
  }
  return zone;
}

// A plan in itb_live's action format: a list, {"plan": [...]}, or a recorded
// solve ({"data": {"engine_plan": [...]}}).
std::vector<PlayerAction> read_plan(const fs::path& f) {
  std::ifstream in(f);
  json j = json::parse(in);
  if (j.is_object() && j.contains("data")) j = j["data"];
  if (j.is_object() && j.contains("engine_plan")) j = j["engine_plan"];
  if (j.is_object() && j.contains("plan")) j = j["plan"];
  std::vector<PlayerAction> plan;
  for (const json& a : j) {
    PlayerAction p;
    p.uid = a.value("uid", -1);
    if (a.contains("move")) p.move = json_pt(a["move"]);
    const std::string kind = a.value("kind", std::string("none"));
    p.kind = kind == "weapon" ? PlayerAction::Kind::Weapon
             : kind == "repair" ? PlayerAction::Kind::Repair
                                : PlayerAction::Kind::None;
    if (a.contains("weapon") && a["weapon"].is_string()) p.weapon = a["weapon"].get<std::string>();
    if (a.contains("target")) p.target = json_pt(a["target"]);
    if (a.contains("target2")) {
      const Point t = json_pt(a["target2"]);
      if (t.valid()) p.target2 = t;
    }
    plan.push_back(p);
  }
  return plan;
}

// The live flags (live_play.py mission: --threat-tier --objectives-first
// --mech-grid 2 --mech-grid-turns --mech-grid-pilot), plus an arm's
// lookahead.
SolveOptions arm_options(const std::string& arm, const Repair& repair, double time,
                         const std::vector<Engine*>& helpers) {
  SolveOptions so;
  so.time_limit_s = time;
  so.threat_tier = true;
  so.objectives_first = true;
  so.mech_grid = 2;
  so.mech_grid_turns = true;
  so.mech_grid_pilot = true;
  so.helper_engines = helpers;
  so.repair_skills = repair;
  auto number = [&](size_t from) { return std::max(1, std::atoi(arm.c_str() + from)); };
  if (arm.rfind("la", 0) == 0 || arm.rfind("sum", 0) == 0 || arm.rfind("idle", 0) == 0) {
    so.lookahead = true;
    so.lookahead_options.repair_skills = repair;
    if (arm.rfind("la", 0) == 0) {
      so.lookahead_options.samples = number(2);
    } else if (arm.rfind("sum", 0) == 0) {
      so.lookahead_options.samples = number(3);
      so.lookahead_options.sum = true;
    } else {
      so.lookahead_options.samples = number(4);
      so.lookahead_options.reply = LookaheadOptions::Reply::Idle;
    }
  }
  return so;
}

struct Loss {
  int grid = 0, mechs = 0, bhp = 0, stars = 0, mech_hp = 0;
};
Loss plain(const Score& s) {
  Loss l;
  l.grid = std::max(0, -s[ScoreKey::GridLost]);
  l.bhp = std::max(0, -s[ScoreKey::BuildingHpLost]);
  l.mechs = std::max(0, -s[ScoreKey::MechsLost]);
  l.stars = std::max(0, -s[ScoreKey::ObjectivesFailed]);
  l.mech_hp = -s[ScoreKey::MechHpLost];
  return l;
}
std::string text(const Loss& l) {
  char b[96];
  std::snprintf(b, sizeof b, "g%d/m%d/b%d/s%d/hp%d", l.grid, l.mechs, l.bhp, l.stars, l.mech_hp);
  return b;
}
json to_json(const Loss& l) { return json::array({l.grid, l.mechs, l.bhp, l.stars, l.mech_hp}); }

// What a plan leaves: this turn's plain worst case, then the next turn
// judged on `seeds` AI samples by a live-flags solve of each board.
struct Judged {
  bool ok = false;
  Loss now;                // this turn, worst case over chance (plain tiers)
  std::vector<Loss> next;  // per next-turn seed (empty: last turn)
  bool last = false;
};

struct Ctx {
  const TwoTurnOptions& opt;
  Engine& engine;
  std::vector<Engine*> helpers;
};

Judged judge(Ctx& c, const Board& board, const TurnContext& ctx, const Repair& repair,
             const std::vector<PlayerAction>& plan) {
  Judged j;
  SolveOptions pl;
  pl.repair_skills = repair;
  const std::optional<Score> s = evaluate_plan(c.engine, board, ctx, plan, pl);
  if (!s) return j;
  j.ok = true;
  j.now = plain(*s);
  Board after = board;
  const TurnResult tr = c.engine.play_turn(after, plan, ctx);
  if (!tr.ok()) {
    j.ok = false;
    return j;
  }
  if (tr.enemy.mission_ended || board.turn >= board.total_turns) {
    j.last = true;
    return j;
  }
  TurnContext next_ctx = ctx;
  next_ctx.spawn_types.clear();
  LookaheadOptions lo;
  lo.seed = c.opt.seed;
  lo.repair_skills = repair;
  for (int k = 0; k < c.opt.next_seeds; ++k) {
    const Board start = next_turn_start(c.engine, board, after, &tr.enemy, next_ctx, lookahead_seed(lo, k), lo);
    SolveOptions so = arm_options("base", repair, c.opt.next_time, c.helpers);
    const SolveResult r = solve_turn(c.engine, start, next_ctx, so);
    const std::optional<Score> s2 = evaluate_plan(c.engine, start, next_ctx, r.best.actions, pl);
    j.next.push_back(s2 ? plain(*s2) : Loss{});
  }
  return j;
}

struct Summary {
  double grid = 0, mechs = 0, bhp = 0;
  int worst_grid = 0, worst_mechs = 0, any = 0, n = 0;
};
Summary summarize_next(const std::vector<Loss>& v) {
  Summary s;
  for (const Loss& l : v) {
    s.grid += l.grid;
    s.mechs += l.mechs;
    s.bhp += l.bhp;
    s.worst_grid = std::max(s.worst_grid, l.grid);
    s.worst_mechs = std::max(s.worst_mechs, l.mechs);
    s.any += (l.grid > 0 || l.mechs > 0) ? 1 : 0;
    ++s.n;
  }
  if (s.n) {
    s.grid /= s.n;
    s.mechs /= s.n;
    s.bhp /= s.n;
  }
  return s;
}

std::string plan_text(const Board& b, const std::vector<PlayerAction>& plan) {
  std::string out;
  for (const PlayerAction& a : plan) out += (out.empty() ? "" : "; ") + describe_action(b, a);
  return out;
}

json plan_json(const std::vector<PlayerAction>& plan) {
  json a = json::array();
  for (const PlayerAction& p : plan) {
    a.push_back({{"uid", p.uid},
                 {"move", p.move.valid() ? json::array({p.move.x, p.move.y}) : json(nullptr)},
                 {"kind", p.kind == PlayerAction::Kind::Weapon   ? "weapon"
                          : p.kind == PlayerAction::Kind::Repair ? "repair"
                                                                 : "none"},
                 {"weapon", p.weapon},
                 {"target", p.target.valid() ? json::array({p.target.x, p.target.y}) : json(nullptr)},
                 {"target2", p.target2 ? json::array({p.target2->x, p.target2->y}) : json(nullptr)}});
  }
  return a;
}

int run_turn_mode(Ctx& c, std::ofstream& jout) {
  std::optional<Loaded> l = load(c.opt.target, c.engine);
  if (!l) return 1;
  std::printf("== %s (turn %d/%d)\n", c.opt.target.string().c_str(), l->board.turn, l->board.total_turns);
  struct Cand {
    std::vector<std::string> sources;
    std::vector<PlayerAction> plan;
    std::string worst;
  };
  std::vector<Cand> cands;
  auto add = [&](const std::string& src, const std::vector<PlayerAction>& plan, const std::string& worst) {
    const std::string key = plan_text(l->board, plan);
    for (Cand& x : cands) {
      if (plan_text(l->board, x.plan) == key) {
        x.sources.push_back(src);
        return;
      }
    }
    cands.push_back({{src}, plan, worst});
  };
  for (const fs::path& f : c.opt.plan_files) add("file:" + f.filename().string(), read_plan(f), "");
  for (const std::string& arm : c.opt.arms) {
    const Clock::time_point t0 = Clock::now();
    const SolveOptions so = arm_options(arm, l->repair, c.opt.time, c.helpers);
    const SolveResult r = solve_turn(c.engine, l->board, l->ctx, so);
    std::printf("  [%s] %.1fs proven %d/%d: %s\n", arm.c_str(), since(t0), r.proven_components,
                r.proven_optimal ? 1 : 0, r.best.worst_case.describe(so.tiers()).c_str());
    add(arm, r.best.actions, r.best.worst_case.describe(so.tiers()));
  }
  for (const Cand& x : cands) {
    std::string src;
    for (const std::string& s : x.sources) src += (src.empty() ? "" : ",") + s;
    const Clock::time_point t0 = Clock::now();
    const Judged j = judge(c, l->board, l->ctx, l->repair, x.plan);
    const Summary s = summarize_next(j.next);
    std::printf("  plan [%s]: %s\n", src.c_str(), plan_text(l->board, x.plan).c_str());
    if (!j.ok) {
      std::printf("    refused\n");
      continue;
    }
    std::printf("    this turn %s | next turn (%d seeds): grid mean %.2f worst %d, mechs mean %.2f worst %d, "
                "%d lose any, bhp mean %.2f (%.0fs)\n",
                text(j.now).c_str(), s.n, s.grid, s.worst_grid, s.mechs, s.worst_mechs, s.any, s.bhp, since(t0));
    if (c.opt.verbose) {
      std::string per;
      for (const Loss& n : j.next) per += " " + text(n);
      std::printf("    per seed:%s\n", per.c_str());
    }
    if (jout) {
      json row{{"file", c.opt.target.string()}, {"sources", x.sources}, {"plan", plan_json(x.plan)},
               {"now", to_json(j.now)}, {"last", j.last}};
      json nx = json::array();
      for (const Loss& n : j.next) nx.push_back(to_json(n));
      row["next"] = nx;
      jout << row.dump() << "\n" << std::flush;
    }
    std::fflush(stdout);
  }
  return 0;
}

int run_deploy_mode(Ctx& c, std::ofstream& jout) {
  std::optional<Loaded> l = load(c.opt.target, c.engine);
  if (!l) return 1;
  const std::vector<Point> zone = read_zone(c.opt.target);
  const std::vector<int32_t> mechs = deploy_mechs(l->board);
  std::vector<std::vector<Point>> placements = c.opt.placements;
  // The mechs' tiles on the state itself (the provisional placement).
  if (c.opt.state_placement) {
    std::vector<Point> here;
    for (int32_t uid : mechs) here.push_back(l->board.find_pawn(uid)->pos);
    if (std::find(placements.begin(), placements.end(), here) == placements.end()) placements.push_back(here);
  }
  if (c.opt.deploy_top > 0) {
    DeployOptions o = c.opt.deploy_options;
    o.solve.repair_skills = l->repair;
    o.helper_engines = c.helpers;
    const Clock::time_point t0 = Clock::now();
    const DeployResult r = deploy_search(c.engine, l->board, l->ctx, zone, o);
    std::printf("deploy search: %s, %d sets, %d solves, %.1fs\n", r.ok ? "ok" : r.error.c_str(), r.sets, r.solves,
                since(t0));
    for (size_t i = 0; i < r.finalists.size(); ++i) {
      const DeployCandidate& d = r.finalists[i];
      std::string t;
      for (Point p : d.tiles) t += to_visual(p) + " ";
      std::printf("  #%zu %s value %s\n", i, t.c_str(),
                  d.value ? d.value->worst.describe(o.solve.tiers()).c_str() : "-");
      if (static_cast<int>(i) < c.opt.deploy_top &&
          std::find(placements.begin(), placements.end(), d.tiles) == placements.end()) {
        placements.push_back(d.tiles);
      }
    }
  }
  std::printf("== %s: %zu placements, %d deploy seeds, turn-1 arm %s (%.0fs), %d next seeds (%.0fs)\n",
              c.opt.target.string().c_str(), placements.size(), c.opt.deploy_seeds,
              c.opt.arms.empty() ? "la8" : c.opt.arms.front().c_str(), c.opt.time, c.opt.next_seeds,
              c.opt.next_time);
  const std::string arm = c.opt.arms.empty() ? "la8" : c.opt.arms.front();
  for (const auto& tiles : placements) {
    std::string t;
    for (Point p : tiles) t += to_visual(p) + " ";
    std::printf("-- placement %s\n", t.c_str());
    std::unordered_map<BoardHash, size_t, BoardHashOf> seen;
    struct Out {
      Judged j;
      Loss phase;
      int weight = 0;
      std::string plan;
    };
    std::vector<Out> outs;
    const Clock::time_point t0 = Clock::now();
    for (int s = 0; s < c.opt.deploy_seeds; ++s) {
      Score cost;
      Board b = deploy_outcome(c.engine, l->board, l->ctx, mechs, tiles, deploy_sample_seed(c.opt.deploy_seed, s),
                               &cost);
      TurnContext ctx1 = l->ctx;
      for (Point p : c.opt.spawns) {
        if (!b.pawn_at(p)) b.spawn_points.push_back(p);
      }
      ctx1.spawn_types.assign(b.spawn_points.size(), std::string());
      const BoardHash h = hash_board(b, HashMode::Search);
      if (auto it = seen.find(h); it != seen.end()) {
        ++outs[it->second].weight;
        continue;
      }
      seen.emplace(h, outs.size());
      const SolveOptions so = arm_options(arm, l->repair, c.opt.time, c.helpers);
      const SolveResult r = solve_turn(c.engine, b, ctx1, so);
      Out o;
      o.j = judge(c, b, ctx1, l->repair, r.best.actions);
      o.phase = plain(cost);
      o.weight = 1;
      o.plan = plan_text(b, r.best.actions);
      const Summary sm = summarize_next(o.j.next);
      std::printf("   seed %d: phase %s, t1 %s, t2 grid mean %.2f worst %d mechs %.2f any %d/%d\n", s,
                  text(o.phase).c_str(), text(o.j.now).c_str(), sm.grid, sm.worst_grid, sm.mechs, sm.any, sm.n);
      if (c.opt.verbose) std::printf("      plan: %s\n", o.plan.c_str());
      std::fflush(stdout);
      outs.push_back(std::move(o));
    }
    // Per placement: over the deploy seeds (weighted), turn 0+1 plain loss
    // and the next turn's mean and worst.
    double g1 = 0, m1 = 0, g2 = 0, m2 = 0, tot = 0;
    int w = 0, worst1 = 0, worst2 = 0, worst_tot = 0, mechs_any = 0, stars1 = 0;
    for (const Out& o : outs) {
      const Summary sm = summarize_next(o.j.next);
      const int a1 = o.phase.grid + o.j.now.grid;
      g1 += o.weight * a1;
      m1 += o.weight * (o.phase.mechs + o.j.now.mechs);
      g2 += o.weight * sm.grid;
      m2 += o.weight * sm.mechs;
      tot += o.weight * (a1 + sm.grid);
      worst1 = std::max(worst1, a1);
      worst2 = std::max(worst2, sm.worst_grid);
      worst_tot = std::max(worst_tot, a1 + sm.worst_grid);
      mechs_any += o.weight * ((o.j.now.mechs + sm.worst_mechs) > 0 ? 1 : 0);
      stars1 += o.weight * o.j.now.stars;
      w += o.weight;
    }
    const double n = std::max(1, w);
    std::printf("   => %s: turn0+1 grid mean %.2f worst %d, mechs %.2f, stars %.2f | turn 2 grid mean %.2f worst %d, "
                "mechs %.2f | total grid mean %.2f worst %d | seeds losing a mech %d/%d (%.0fs)\n",
                t.c_str(), g1 / n, worst1, m1 / n, stars1 / n, g2 / n, worst2, m2 / n, tot / n, worst_tot,
                mechs_any, w, since(t0));
    std::fflush(stdout);
    if (jout) {
      json row{{"file", c.opt.target.string()}, {"placement", t}};
      json os = json::array();
      for (const Out& o : outs) {
        json nx = json::array();
        for (const Loss& x : o.j.next) nx.push_back(to_json(x));
        os.push_back({{"weight", o.weight}, {"phase", to_json(o.phase)}, {"now", to_json(o.j.now)}, {"next", nx},
                      {"plan", o.plan}});
      }
      row["outcomes"] = os;
      jout << row.dump() << "\n" << std::flush;
    }
  }
  return 0;
}

}  // namespace

int run_two_turn(const TwoTurnOptions& opt) {
  std::unique_ptr<Engine> engine;
  std::vector<std::unique_ptr<Engine>> helpers;
  try {
    engine = Engine::create(opt.game);
    for (int i = 1; i < opt.threads; ++i) helpers.push_back(Engine::create(opt.game));
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  Ctx c{opt, *engine, {}};
  for (auto& h : helpers) c.helpers.push_back(h.get());
  std::ofstream jout;
  if (!opt.json_out.empty()) jout.open(opt.json_out, std::ios::app);
  return opt.deploy ? run_deploy_mode(c, jout) : run_turn_mode(c, jout);
}

}  // namespace itb::tools
