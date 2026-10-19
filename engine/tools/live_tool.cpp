// itb_live: solve / predict / load bridge states for the live driver
// (live_tool.hpp has the request format).
//
//   itb_live [--game DIR] solve <state.json> [--time S] [--nodes N] [--threads N]
//            [--beam W] [--threat-tier] [--threat-kills] [--objectives-first] [--mech-grid N [--mech-grid-turns]] [--out FILE] [--pretty]
//   itb_live [--game DIR] predict <state.json> --plan JSON|@FILE [--branches N] [--resist JSON|@FILE] [--threat-tier] [--out FILE] [--pretty]
//   itb_live [--game DIR] board <state.json> [--pretty]
//   itb_live [--game DIR] actions <state.json> [--plan JSON|@FILE] [--no-effects] [--out FILE] [--pretty]
//   itb_live [--game DIR] deploy <state.json> [--time S] [--threads N] [--samples N] [--finalists N]
//            [--screen-samples N] [--solve-time S] [--solve-nodes N] [--seed N]
//            [--threat-tier ... --lookahead ...: the turn-1 solves' ranking, as for solve]
//            [--placements JSON|@FILE] [--always JSON|@FILE] [--out FILE] [--pretty]
//   itb_live [--game DIR] serve [--threads N]
//
// `serve` loads the engines once, prints {"ok": true, "ready": true, ...},
// then answers one JSON request per stdin line with one JSON line on stdout
// ({"cmd": "quit"} or end of input stops it). The one-shot commands print a
// single JSON object (exit 1 when it says "ok": false).

#include "live_tool.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "itb/deploy.hpp"
#include "itb/engine.hpp"
#include "itb/environment.hpp"
#include "itb/format.hpp"
#include "itb/game_data.hpp"
#include "itb/lua_host.hpp"
#include "itb/movement.hpp"
#include "itb/recording.hpp"
#include "itb/tile_rules.hpp"
#include "itb/score.hpp"
#include "itb/threat.hpp"
#include "itb/solver.hpp"
#include "itb/symbols.hpp"

namespace fs = std::filesystem;
using nlohmann::json;

namespace itb::tools {
namespace {

// The solver reseeds the Lua stream with this before every sub-action and
// every enemy phase (solver.cpp kLuaSeed); the simulation does the same so
// it follows the plan's default outcome exactly.
constexpr uint32_t kLuaSeed = 1;

const char* kTierNames[kScoreKeys] = {"grid_survival", "grid",       "building_hp", "mechs",  "objectives_failed",
                                      "next_turn",     "threat",     "mech_hp",     "objectives", "kills",
                                      "vek_hp",        "position"};

// The tiers in use: "threat" only with the threat tier, "next_turn" only
// with the lookahead, "grid_survival" (a probability) only with
// grid_survival.
json score_json(const Score& s, TierSet threat = 0) {
  json j = json::object();
  for (int i = 0; i < kScoreKeys; ++i) {
    if (!tier_shown(i, threat)) continue;
    const int32_t v = s.v[static_cast<size_t>(i)];
    if (i == static_cast<int>(ScoreKey::GridSurvival) && v >= 0 && v <= kSurvivalOne) {
      j[kTierNames[i]] = static_cast<double>(v) / kSurvivalOne;
      continue;
    }
    j[kTierNames[i]] = v == INT32_MAX ? json(nullptr) : json(v);
  }
  return j;
}

// A BoardHash as 32 hex digits (hi, then lo), and back.
std::string hash_hex(const BoardHash& h) {
  char b[40];
  std::snprintf(b, sizeof b, "%016llx%016llx", static_cast<unsigned long long>(h.hi),
                static_cast<unsigned long long>(h.lo));
  return b;
}
BoardHash parse_hash_hex(const std::string& s) {
  if (s.size() != 32 || s.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
    throw std::invalid_argument("a board hash is 32 hex digits, got '" + s + "'");
  }
  BoardHash h;
  h.hi = std::stoull(s.substr(0, 16), nullptr, 16);
  h.lo = std::stoull(s.substr(16), nullptr, 16);
  return h;
}

json point_json(Point p) { return p.valid() ? json::array({p.x, p.y}) : json(nullptr); }

Point json_point(const json& j) {
  if (!j.is_array() || j.size() < 2 || !j[0].is_number_integer() || !j[1].is_number_integer()) return kInvalidPoint;
  return {j[0].get<int>(), j[1].get<int>()};
}

const char* kind_name(PlayerAction::Kind k) {
  switch (k) {
    case PlayerAction::Kind::Weapon: return "weapon";
    case PlayerAction::Kind::Repair: return "repair";
    default: return "none";
  }
}

json action_json(const Board& before, const PlayerAction& a) {
  return json{{"uid", a.uid},
              {"move", point_json(a.move)},
              {"kind", kind_name(a.kind)},
              {"weapon", a.weapon},
              {"target", point_json(a.target)},
              {"target2", a.target2 ? point_json(*a.target2) : json(nullptr)},
              {"description", describe_action(before, a)}};
}

PlayerAction parse_action(const json& j) {
  if (!j.is_object()) throw std::invalid_argument("a plan action must be a JSON object");
  PlayerAction a;
  a.uid = j.value("uid", -1);
  if (auto it = j.find("move"); it != j.end()) a.move = json_point(*it);
  const std::string kind = j.value("kind", std::string("none"));
  if (kind == "weapon") {
    a.kind = PlayerAction::Kind::Weapon;
  } else if (kind == "repair") {
    a.kind = PlayerAction::Kind::Repair;
  } else if (kind != "none") {
    throw std::invalid_argument("plan action kind must be none, weapon or repair, got '" + kind + "'");
  }
  if (auto it = j.find("weapon"); it != j.end() && it->is_string()) a.weapon = it->get<std::string>();
  if (auto it = j.find("target"); it != j.end()) a.target = json_point(*it);
  if (auto it = j.find("target2"); it != j.end()) {
    const Point p = json_point(*it);
    if (p.valid()) a.target2 = p;
  }
  if (a.kind == PlayerAction::Kind::Weapon && (a.weapon.empty() || !a.target.valid())) {
    throw std::invalid_argument("a weapon action needs a weapon and a target");
  }
  return a;
}

// A bridge state loaded the way itb_inspect --solve loads a recording: pilot
// abilities and repair skills added, the player's view of the turn.
struct Loaded {
  Recording rec;
  Board board;
  TurnContext ctx;
  std::vector<std::pair<int32_t, std::string>> repair_skills;
  int active_units = 0;
};

Loaded load_state(Engine& engine, const fs::path& path) {
  std::string error;
  std::optional<Recording> rec = load_recording(path, &engine.data(), &error);
  if (!rec) throw std::invalid_argument("state did not load: " + error);
  Loaded out;
  out.rec = std::move(*rec);
  out.board = out.rec.board;
  for (const auto& [uid, pilot] : out.rec.pilots) {
    if (Pawn* p = out.board.find_pawn(uid)) {
      p->pilot_abilities |= engine.pilot_ability(pilot);
      out.repair_skills.emplace_back(uid, engine.repair_skill(pilot));
    }
  }
  for (const Pawn& p : out.board.pawns()) {
    if (p.controlled() && p.alive() && p.active) ++out.active_units;
  }
  // Visibility rule: hidden spawn types and queue order never reach the solver.
  out.ctx = turn_context(out.rec, Visibility::Player);
  return out;
}

json header(const Loaded& l, const std::string& cmd, const fs::path& state) {
  return json{{"ok", true},
              {"format", kLiveFormat},
              {"cmd", cmd},
              {"state", state.string()},
              {"mission_id", l.rec.mission_id},
              {"phase", l.rec.phase},
              {"turn", l.board.turn},
              {"active_units", l.active_units},
              {"load_warnings", l.rec.warnings}};
}

// The chance nodes TurnContext::choose decides (a branch index each).
bool hidden_choice(ChanceKind k) {
  return k == ChanceKind::EnvOrder || k == ChanceKind::EnvChoice || k == ChanceKind::MissionRandom;
}

const char* chance_kind_name(ChanceKind k) {
  switch (k) {
    case ChanceKind::GridDefense: return "GridDefense";
    case ChanceKind::SpiderEgg: return "SpiderEgg";
    case ChanceKind::LuaRandom: return "LuaRandom";
    case ChanceKind::EnvOrder: return "EnvOrder";
    case ChanceKind::EnvChoice: return "EnvChoice";
    case ChanceKind::MissionRandom: return "MissionRandom";
    case ChanceKind::XpSplit: return "XpSplit";
  }
  return "?";
}

json phase_json(const Board& start, const Board& end, const PhaseResult& pr) {
  json events = json::array();
  for (const PhaseEvent& e : pr.events) {
    if (e.type == PhaseEventType::StatusTick && e.detail.empty()) continue;
    json ev{{"type", to_string(e.type)}, {"point", point_json(e.point)}, {"uid", e.uid}, {"amount", e.amount},
            {"detail", e.detail}};
    const Pawn* p = e.uid >= 0 ? start.find_pawn(e.uid) : nullptr;
    if (!p && e.uid >= 0) p = end.find_pawn(e.uid);
    if (p) ev["unit_type"] = std::string(symbol_name(p->type));
    events.push_back(std::move(ev));
  }
  json emerged = json::array();
  for (Point p : pr.emerged_unknown) emerged.push_back(point_json(p));
  // Grid Defense rolls (the prediction takes the default: not resisted).
  json grid_defense = json::array();
  for (const ChanceRecord& c : pr.chances) {
    if (c.kind == ChanceKind::GridDefense) {
      grid_defense.push_back(json{{"point", point_json(c.point)}, {"amount", c.amount}});
    }
  }
  // XP splits whose remainder decided a level-up (the prediction takes the
  // fewest level-ups).
  json xp_split = json::array();
  for (const ChanceRecord& c : pr.chances) {
    if (c.kind == ChanceKind::XpSplit) {
      xp_split.push_back(json{{"point", point_json(c.point)}, {"amount", c.amount}, {"options", c.options}});
    }
  }
  // Hidden choices (environment order / choice, a mission hook's random
  // pick such as the Final Cave's bomb drop), in the order taken: the
  // prediction takes branch 0 of each; "enemy_branches" has the others.
  json hidden = json::array();
  for (const ChanceRecord& c : pr.chances) {
    if (!hidden_choice(c.kind)) continue;
    hidden.push_back(json{{"kind", chance_kind_name(c.kind)}, {"point", point_json(c.point)},
                          {"pick", c.outcome}, {"options", c.options}});
  }
  return json{{"environment", pr.environment},
              {"exact", pr.exact},
              {"xp_split", xp_split},
              {"hidden_choices", hidden},
              {"quiescent", pr.quiescent},
              {"mission_ended", pr.mission_ended},
              {"chance_nodes", pr.chances.size()},
              {"grid_defense", grid_defense},
              {"timing_sensitive", pr.timing_sensitive()},
              {"lua_errors", pr.lua_errors},
              {"unapplied_lua_writes", pr.unapplied.size()},
              {"emerged_unknown", emerged},
              {"events", events}};
}

bool refused(const ActionResult& r) { return !r.ok() && r.status != ActionStatus::NoEffect; }

int weapon_index(const Pawn* p, const std::string& weapon) {
  if (!p) return -1;
  for (size_t i = 0; i < p->weapons.size(); ++i) {
    if (p->weapons[i] != kNoSymbol && symbol_name(p->weapons[i]) == weapon) return static_cast<int>(i);
  }
  return -1;
}

// Runs the plan sub-action by sub-action with the default outcome of every
// chance node (no Grid Defense resist, first branch, the default death
// seed), the outcome the solver's plan follows, then the enemy phase.
// Fills out["plan"], ["steps"], ["refused"], the boards and the predicted
// score, and appends to `warnings`. `branches` > 0: also the enemy phase's
// other hidden-choice outcomes (enemy_branches below), at most that many.
// `resist`: the enemy phase's Grid Defense outcomes instead of none resisted,
// one entry per resisted roll: each resists the next roll on its tile (a
// tile listed twice resists its first two rolls); every other roll does not.
void simulate(Engine& engine, const Loaded& l, const std::vector<PlayerAction>& plan, json& out,
              std::vector<std::string>& warnings, const ScoreOptions& score_options,
              const LookaheadOptions* lookahead = nullptr, int branches = 0,
              const std::vector<Point>& resist = {}) {
  Board b = l.board;
  ActionOptions opts = action_options(l.ctx);  // the mission's per-frame hooks
  opts.check_legal = true;
  json plan_json = json::array();
  json steps = json::array();
  int refused_at = -1;
  std::string refused_why;
  auto note_result = [&](json& step, const ActionResult& r, const std::string& what) {
    step["status"] = to_string(r.status);
    step["chance_nodes"] = r.resolve.chances.size();
    step["timing_sensitive"] = r.resolve.timing_sensitive();
    step["lua_errors"] = r.lua_errors;
    step["unapplied_lua_writes"] = r.unapplied.size();
    if (!r.mission_events.empty()) {
      json hooks = json::array();
      for (const PhaseEvent& e : r.mission_events) {
        hooks.push_back(json{{"type", to_string(e.type)}, {"point", point_json(e.point)}, {"uid", e.uid},
                             {"detail", e.detail}});
      }
      step["mission_hooks"] = std::move(hooks);
    }
    if (!r.resolve.chances.empty()) {
      warnings.push_back(what + ": a chance node (the plan follows its default outcome; re-solve after another)");
    }
    if (r.resolve.timing_sensitive()) warnings.push_back(what + ": frame-timing sensitive");
    if (!r.lua_errors.empty()) warnings.push_back(what + ": Lua error: " + r.lua_errors.front());
    if (!r.unapplied.empty()) warnings.push_back(what + ": Lua writes the engine does not model");
  };
  for (size_t i = 0; i < plan.size() && refused_at < 0; ++i) {
    const PlayerAction& a = plan[i];
    plan_json.push_back(action_json(b, a));
    const Pawn* p = b.find_pawn(a.uid);
    const std::string who = (p ? std::string(symbol_name(p->type)) : std::string("unit")) + "#" + std::to_string(a.uid);
    if (a.move.valid() && p && a.move != p->pos) {
      json step{{"index", steps.size()}, {"action", i}, {"sub", "move"}, {"uid", a.uid},
                {"from", point_json(p->pos)}, {"to", point_json(a.move)}};
      engine.lua().seed(kLuaSeed);
      const ActionResult r = engine.move(b, a.uid, a.move, opts);
      note_result(step, r, who + " move");
      if (const Pawn* q = b.find_pawn(a.uid)) step["end"] = point_json(q->pos);
      if (refused(r)) {
        refused_at = static_cast<int>(steps.size());
        refused_why = who + " move: " + to_string(r.status);
      }
      step["board"] = live_board_json(b);
      steps.push_back(std::move(step));
      if (refused_at >= 0) break;
    }
    if (a.kind == PlayerAction::Kind::None) continue;
    const bool weapon = a.kind == PlayerAction::Kind::Weapon;
    const std::string skill = weapon ? a.weapon : (a.weapon.empty() ? std::string("Skill_Repair") : a.weapon);
    json step{{"index", steps.size()}, {"action", i}, {"sub", weapon ? "weapon" : "repair"}, {"uid", a.uid},
              {"weapon", skill}, {"target", point_json(a.target)},
              {"target2", a.target2 ? point_json(*a.target2) : json(nullptr)}};
    if (weapon) step["weapon_index"] = weapon_index(b.find_pawn(a.uid), a.weapon);
    engine.lua().seed(kLuaSeed);
    const ActionResult r = weapon ? engine.fire_weapon(b, a.uid, a.weapon, a.target, a.target2, opts)
                                  : engine.repair(b, a.uid, a.target, skill, opts);
    note_result(step, r, who + " " + skill);
    if (refused(r)) {
      refused_at = static_cast<int>(steps.size());
      refused_why = who + " " + skill + ": " + to_string(r.status);
    }
    step["board"] = live_board_json(b);
    steps.push_back(std::move(step));
  }
  out["plan"] = plan_json;
  out["steps"] = steps;
  out["refused"] = refused_at;
  out["start"] = live_board_json(l.board);
  if (refused_at >= 0) {
    out["refused_reason"] = refused_why;
    warnings.push_back("the engine refused step " + std::to_string(refused_at) + ": " + refused_why);
    return;
  }
  out["after_player"] = live_board_json(b);
  TurnContext ctx = l.ctx;
  // The resisted rolls (live_play: the game kept buildings the worst case
  // loses). Each run of the phase consumes its own copy of the list.
  auto resisting = [&resist](TurnContext& c) {
    if (resist.empty()) return;
    c.grid_resist = [left = resist](Point p, int) mutable {
      const auto it = std::find(left.begin(), left.end(), p);
      if (it == left.end()) return false;
      left.erase(it);
      return true;
    };
  };
  resisting(ctx);
  const Board after_player = b;
  engine.lua().seed(kLuaSeed);
  const PhaseResult pr = engine.end_turn(b, ctx);
  out["after_enemy"] = live_board_json(b);
  out["outcome_key"] = hash_hex(outcome_key(b));
  out["enemy_phase"] = phase_json(l.board, b, pr);
  if (branches > 0) {
    // The other outcomes of the enemy phase's hidden choices (a random
    // mission pick such as the Final Cave's bomb drop, an environment
    // order): each forces a prefix of picks, then branch 0 for the nodes
    // after it, depth first (a later node's alternatives before an earlier
    // one's). The driver compares the game with them when the default
    // outcome differs.
    std::vector<int> taken;
    for (const ChanceRecord& c : pr.chances) {
      if (hidden_choice(c.kind)) taken.push_back(c.options);
    }
    std::vector<std::vector<int>> todo;
    auto expand = [&todo](const std::vector<int>& prefix, const std::vector<int>& options) {
      for (size_t j = options.size(); j-- > prefix.size();) {
        for (int k = options[j] - 1; k >= 1; --k) {
          std::vector<int> next = prefix;
          next.resize(j, 0);
          next.push_back(k);
          todo.push_back(std::move(next));
        }
      }
    };
    expand({}, taken);
    json list = json::array();
    while (!todo.empty() && static_cast<int>(list.size()) < branches) {
      const std::vector<int> prefix = std::move(todo.back());
      todo.pop_back();
      std::vector<int> options;
      json choices = json::array();
      TurnContext bctx = l.ctx;
      resisting(bctx);
      bctx.choose = [&](const ChanceRecord& node) {
        const int pick = options.size() < prefix.size() ? prefix[options.size()] : 0;
        options.push_back(node.options);
        choices.push_back(json{{"kind", chance_kind_name(node.kind)}, {"point", point_json(node.point)},
                               {"pick", pick}, {"options", node.options}});
        return pick;
      };
      Board bb = after_player;
      engine.lua().seed(kLuaSeed);
      const PhaseResult bpr = engine.end_turn(bb, bctx);
      expand(prefix, options);
      list.push_back(json{{"choices", choices},
                          {"after_enemy", live_board_json(bb)},
                          {"enemy_phase", phase_json(l.board, bb, bpr)}});
    }
    out["enemy_branches"] = list;
    out["enemy_branches_complete"] = todo.empty();
  }
  Score predicted = score_turn(l.board, b, &ctx, &pr, score_options);
  if (lookahead) {
    // The next-turn estimate of the board left (lookahead.hpp), per AI sample.
    std::vector<NextTurnLoss> per;
    predicted[ScoreKey::NextTurn] = lookahead_value(engine, l.board, b, &pr, ctx, *lookahead, &per);
    NextTurnLoss worst;
    for (const NextTurnLoss& s : per) {
      if (next_turn_value(s) < next_turn_value(worst)) worst = s;
    }
    worst.mission_over = per.empty();
    json samples = json::array();
    for (const NextTurnLoss& s : per) samples.push_back(json{{"grid", s.grid}, {"mechs", s.mechs}, {"building_hp", s.building_hp}});
    out["next_turn"] = json{{"grid", worst.grid}, {"mechs", worst.mechs}, {"building_hp", worst.building_hp},
                            {"mission_over", worst.mission_over}, {"samples", samples}};
  }
  out["predicted_score"] = score_json(predicted, tier_set(score_options));
  if (score_options.tier_on()) out["threatening_enemies"] = threatening_enemies(b);
  if (!pr.exact) warnings.push_back("enemy phase: the recorded data does not pin every step down (EnvInexact)");
  if (pr.timing_sensitive()) warnings.push_back("enemy phase: frame-timing sensitive");
  if (!pr.quiescent) warnings.push_back("enemy phase: a step never settled");
  if (!pr.lua_errors.empty()) warnings.push_back("enemy phase: Lua error: " + pr.lua_errors.front());
}

json stats_json(const SolveStats& s) {
  return json{{"threads", s.threads},
              {"nodes", s.nodes},
              {"sub_actions", s.sub_actions},
              {"enemy_phases", s.enemy_phases},
              {"chance_branches", s.chance_branches},
              {"tt_hits", s.tt_hits},
              {"bound_prunes", s.bound_prunes},
              {"time_s", s.time_s},
              {"first_plan_s", s.first_plan_s},
              {"best_plan_s", s.best_plan_s},
              {"base_search_s", s.base_search_s},
              {"base_proven_components", s.base_proven_components},
              {"seed_eval_s", s.seed_eval_s},
              {"recheck_s", s.recheck_s},
              {"recheck_cut", s.recheck_cut}};
}

// The lookahead's request fields: "lookahead" (bool), "lookahead_samples",
// "lookahead_reply" ("greedy" | "idle" | "search"), "lookahead_search_nodes"
// (search), "lookahead_fold" ("worst" | "sum" | "worst_sum"),
// "lookahead_seed", "lookahead_env" (plan the environment's next-turn
// hazards, LookaheadOptions::plan_env).
LookaheadOptions lookahead_options(const json& request, const std::vector<std::pair<int32_t, std::string>>& repair) {
  LookaheadOptions o;
  o.samples = std::max(1, request.value("lookahead_samples", o.samples));
  o.seed = request.value("lookahead_seed", o.seed);
  const std::optional<LookaheadOptions::Fold> fold = lookahead_fold_of(request.value("lookahead_fold", std::string("worst")));
  if (!fold) throw std::invalid_argument("lookahead_fold must be 'worst', 'sum' or 'worst_sum'");
  o.fold = *fold;
  const std::optional<LookaheadOptions::Reply> reply =
      lookahead_reply_of(request.value("lookahead_reply", std::string("greedy")));
  if (!reply) throw std::invalid_argument("lookahead_reply must be 'greedy', 'idle' or 'search'");
  o.reply = *reply;
  o.search_nodes = std::max<uint64_t>(1, request.value("lookahead_search_nodes", o.search_nodes));
  o.plan_env = request.value("lookahead_env", o.plan_env);
  o.repair_skills = repair;
  return o;
}

// The request's ranking options (solve and deploy alike): the experimental
// tiers (SolveOptions::threat_tier, threat_kills, objectives_first), the
// mech exchange (mech_grid, mech_grid_turns, mech_grid_pilot), the
// next-turn lookahead (lookahead.hpp) and the Grid Defense survival odds
// (grid_survival). Absent: all off.
SolveOptions ranking_options(const json& request, const std::vector<std::pair<int32_t, std::string>>& repair) {
  SolveOptions tiers;
  tiers.threat_tier = request.value("threat_tier", false);
  tiers.threat_kills = request.value("threat_kills", false);
  tiers.objectives_first = request.value("objectives_first", false);
  tiers.mech_grid = request.value("mech_grid", -1);
  tiers.mech_grid_turns = request.value("mech_grid_turns", false);
  tiers.mech_grid_pilot = request.value("mech_grid_pilot", false);
  tiers.lookahead = request.value("lookahead", false);
  if (tiers.lookahead) tiers.lookahead_options = lookahead_options(request, repair);
  tiers.grid_survival = request.value("grid_survival", false);
  return tiers;
}

// A solve request's budget and search fields on top of its ranking options
// (`tiers`, ranking_options): "time_limit", "node_limit", "max_time",
// "min_tiers", "beam_width", the survival cap ("survival_cap",
// "survival_cap_share", "survival_cap_strict"). Helper engines are the
// caller's.
SolveOptions solve_options_of(const json& request, const SolveOptions& tiers,
                              const std::vector<std::pair<int32_t, std::string>>& repair) {
  SolveOptions so;
  so.time_limit_s = request.value("time_limit", so.time_limit_s);
  so.node_limit = request.value("node_limit", so.node_limit);
  so.max_time_s = request.value("max_time", so.max_time_s);
  so.min_proven_tiers = request.value("min_tiers", so.min_proven_tiers);
  so.threat_tier = tiers.threat_tier;
  so.threat_kills = tiers.threat_kills;
  so.objectives_first = tiers.objectives_first;
  so.mech_grid = tiers.mech_grid;
  so.mech_grid_turns = tiers.mech_grid_turns;
  so.mech_grid_pilot = tiers.mech_grid_pilot;
  so.lookahead = tiers.lookahead;
  so.lookahead_options = tiers.lookahead_options;
  so.grid_survival = tiers.grid_survival;
  // The survival cap pre-pass (README "Grid survival"): "survival_cap"
  // true, its share of the budget "survival_cap_share" (default 0.5),
  // "survival_cap_strict" (only after the survey's own proof).
  if (so.grid_survival && request.value("survival_cap", false)) {
    so.survival_cap_share = std::clamp(request.value("survival_cap_share", 0.5), 0.0, 1.0);
    so.survival_cap_strict = request.value("survival_cap_strict", false);
  }
  const int beam = request.value("beam_width", -1);
  if (beam >= 0) so.beam_width = beam;
  // "exclude_outcomes": [hex, ...] (SolveOptions::exclude_outcomes; a
  // reply's "outcome_key").
  if (auto it = request.find("exclude_outcomes"); it != request.end() && it->is_array()) {
    for (const json& k : *it) so.exclude_outcomes.push_back(parse_hash_hex(k.get<std::string>()));
  }
  so.repair_skills = repair;
  return so;
}


// ---- two_turn ----------------------------------------------------------------------
//
// A plan's turn and the next one (live_tool.hpp, the "two_turn" request).

json loss_json(const NextTurnLoss& l) {
  return json{{"grid", l.grid}, {"mechs", l.mechs}, {"building_hp", l.building_hp}};
}

// score_turn's plain tiers of a worst case (no ranking options): losses as
// positive numbers, kills and enemy HP removed as gains.
json plain_json(const Score& s) {
  return json{{"grid", std::max(0, -s[ScoreKey::GridLost])},
              {"building_hp", std::max(0, -s[ScoreKey::BuildingHpLost])},
              {"mechs", std::max(0, -s[ScoreKey::MechsLost])},
              {"stars", std::max(0, -s[ScoreKey::ObjectivesFailed])},
              {"mech_hp", -s[ScoreKey::MechHpLost]},
              {"objectives", s[ScoreKey::ObjectiveProgress]},
              {"kills", s[ScoreKey::VekKilled]},
              {"vek_hp", s[ScoreKey::VekHpRemoved]}};
}

json turn_loss_json(const Board& before, const Board& after, const TurnContext& ctx, const PhaseResult& pr) {
  return plain_json(score_turn(before, after, &ctx, &pr));
}

// The context of the turn after `ctx`'s, as the lookahead plays it
// (lookahead.cpp lookahead_sample): no hidden spawn types, every chance node
// at its default outcome, no logs.
TurnContext next_context(const TurnContext& ctx) {
  TurnContext next = ctx;
  next.spawn_types.clear();
  next.events = nullptr;
  next.log = nullptr;
  next.grid_resist = nullptr;
  next.choose = nullptr;
  next.death_seed = nullptr;
  next.spider_egg = nullptr;
  next.stop_early = nullptr;
  return next;
}

// A plan played as predict plays it (Lua reseeded before every sub-action
// and the enemy phase): its default chance outcome.
struct Played {
  int refused = -1;  // the action the engine refused (no enemy phase), else -1
  Board after_player;
  Board after;
  PhaseResult pr;
};
Played play_seeded(Engine& engine, const Board& start, const TurnContext& ctx, const std::vector<PlayerAction>& plan) {
  Played out;
  Board b = start;
  ActionOptions opts = action_options(ctx);
  opts.check_legal = true;
  for (size_t i = 0; i < plan.size() && out.refused < 0; ++i) {
    const PlayerAction& a = plan[i];
    const Pawn* p = b.find_pawn(a.uid);
    if (a.move.valid() && p && a.move != p->pos) {
      engine.lua().seed(kLuaSeed);
      if (refused(engine.move(b, a.uid, a.move, opts))) out.refused = static_cast<int>(i);
    }
    if (out.refused >= 0 || a.kind == PlayerAction::Kind::None) continue;
    engine.lua().seed(kLuaSeed);
    const ActionResult r =
        a.kind == PlayerAction::Kind::Weapon
            ? engine.fire_weapon(b, a.uid, a.weapon, a.target, a.target2, opts)
            : engine.repair(b, a.uid, a.target, a.weapon.empty() ? std::string("Skill_Repair") : a.weapon, opts);
    if (refused(r)) out.refused = static_cast<int>(i);
  }
  out.after_player = b;
  if (out.refused >= 0) return out;
  engine.lua().seed(kLuaSeed);
  out.pr = engine.end_turn(b, ctx);
  out.after = std::move(b);
  return out;
}

using AddHelpers = std::function<void(SolveOptions&, int threads)>;

std::vector<PlayerAction> parse_plan(const json& j);

// What a turn-start board forces: the greedy and idle replies' losses
// (lookahead.hpp reply_loss) and, with `next`, a real solve of it and the
// plain worst case of its plan.
json judge_start(Engine& engine, const Board& start, const TurnContext& ctx,
                 const std::vector<std::pair<int32_t, std::string>>& repair, const json& request,
                 const SolveOptions* next) {
  json j = json::object();
  if (request.value("greedy", true)) {
    LookaheadOptions lo;
    lo.repair_skills = repair;
    lo.plan_env = request.value("lookahead_env", false);  // the reply answers the planned hazards too
    const auto t0 = std::chrono::steady_clock::now();
    j["greedy"] = loss_json(reply_loss(engine, start, ctx, lo));
    j["greedy"]["time_s"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  }
  if (request.value("idle", false)) j["idle"] = loss_json(idle_loss(engine, start, ctx));
  if (const int nodes = request.value("search_nodes", 0); nodes > 0) {
    // The lookahead's Search reply (a node-budget solve, plain tiers).
    LookaheadOptions lo;
    lo.repair_skills = repair;
    lo.reply = LookaheadOptions::Reply::Search;
    lo.search_nodes = static_cast<uint64_t>(nodes);
    const auto t0 = std::chrono::steady_clock::now();
    j["search"] = loss_json(reply_loss(engine, start, ctx, lo));
    j["search"]["time_s"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  }
  if (next) {
    bool active = false;
    for (const Pawn& p : start.pawns()) active = active || (p.controlled() && p.alive() && p.active);
    json s = json::object();
    std::vector<PlayerAction> plan;
    if (active) {
      const SolveResult r = solve_turn(engine, start, ctx, *next);
      plan = r.best.actions;
      s["worst_case"] = score_json(r.best.worst_case, next->tiers());
      s["proven_optimal"] = r.proven_optimal;
      s["proven_components"] = r.proven_components;
      s["timed_out"] = r.timed_out;
      s["time_s"] = r.stats.time_s;
    }
    SolveOptions plain;
    plain.repair_skills = repair;
    if (std::optional<Score> v = evaluate_plan(engine, start, ctx, plan, plain)) {
      s["plain"] = plain_json(*v);
    } else {
      s["plain"] = nullptr;
    }
    if (request.value("next_plans", false)) {
      json pj = json::array();
      for (const PlayerAction& a : plan) pj.push_back(action_json(start, a));
      s["plan"] = pj;
    }
    j["solve"] = s;
  }
  return j;
}

// The "two_turn" request (live_tool.hpp).
json two_turn_request(Engine& engine, const Loaded& l, const json& request, const AddHelpers& add_helpers) {
  json out = json::object();
  const int threads = request.value("threads", 1);
  std::optional<SolveOptions> next;
  if (auto it = request.find("next"); it != request.end() && it->is_object()) {
    next = solve_options_of(*it, ranking_options(*it, l.repair_skills), l.repair_skills);
    add_helpers(*next, threads);
  }
  const bool boards = request.value("boards", false);
  if (request.value("start_only", false)) {
    // The state is itself the turn to judge (e.g. the game's actual next
    // turn, to compare with the sampled ones).
    out["start"] = judge_start(engine, l.board, l.ctx, l.repair_skills, request, next ? &*next : nullptr);
    out["start"]["start_hash"] = hash_hex(hash_board_stable(l.board, HashMode::Search));
    return out;
  }
  // The AI seeds of the next turn: "samples" N (0..N-1) or a list of
  // sample indices, from the base "seed" (deploy_sample_seed, as the
  // lookahead's own samples; default 101, independent of the solver's 7).
  std::vector<int> ks;
  if (auto it = request.find("samples"); it != request.end()) {
    if (it->is_number_integer()) {
      for (int k = 0; k < it->get<int>(); ++k) ks.push_back(k);
    } else if (it->is_array()) {
      for (const json& k : *it) ks.push_back(k.get<int>());
    }
  }
  LookaheadOptions lo;
  lo.seed = request.value("seed", 101u);
  lo.repair_skills = l.repair_skills;
  // "lookahead_env": each sample's next turn runs with the environment's
  // hazards planned for it (lookahead.hpp next_turn_mission).
  lo.plan_env = request.value("lookahead_env", false);
  // "score": also the plan's worst case under the request's ranking options
  // (as the live solve ranks it, lookahead included).
  const bool ranked = request.value("score", false);
  const SolveOptions rank = ranked ? solve_options_of(request, ranking_options(request, l.repair_skills), l.repair_skills)
                                   : SolveOptions{};
  SolveOptions plain;
  plain.repair_skills = l.repair_skills;
  const TurnContext next_ctx = next_context(l.ctx);
  std::vector<std::vector<PlayerAction>> plans;
  if (auto it = request.find("plans"); it != request.end() && it->is_array()) {
    for (const json& p : *it) plans.push_back(parse_plan(p));
  } else if (request.contains("plan")) {
    plans.push_back(parse_plan(request["plan"]));
  } else {
    throw std::invalid_argument("two_turn needs 'plans', 'plan' or 'start_only'");
  }
  json results = json::array();
  for (const std::vector<PlayerAction>& plan : plans) {
    json r = json::object();
    const Played played = play_seeded(engine, l.board, l.ctx, plan);
    r["refused"] = played.refused;
    if (request.value("describe", false)) {
      json pj = json::array();
      for (const PlayerAction& a : plan) pj.push_back(describe_action(l.board, a));
      r["plan"] = pj;
    }
    r["end_hash"] = hash_hex(hash_board_stable(played.after_player, HashMode::EndTurn));
    if (played.refused >= 0) {
      r["ok"] = false;
      results.push_back(std::move(r));
      continue;
    }
    std::string why;
    const std::optional<Score> now = evaluate_plan(engine, l.board, l.ctx, plan, plain, &why);
    r["ok"] = now.has_value();
    if (!now) {
      r["refused_reason"] = why;
      results.push_back(std::move(r));
      continue;
    }
    r["now"] = plain_json(*now);
    if (ranked) {
      const std::optional<Score> v = evaluate_plan(engine, l.board, l.ctx, plan, rank);
      r["score"] = v ? score_json(*v, rank.tiers()) : json(nullptr);
    }
    // The default outcome's end of the enemy phase: the board the next turn
    // starts from (Grid Defense never resists: the worst case for buildings).
    r["after_hash"] = hash_hex(hash_board_stable(played.after, HashMode::Search));
    r["outcome_key"] = hash_hex(outcome_key(played.after));
    r["default_outcome"] = turn_loss_json(l.board, played.after, l.ctx, played.pr);
    if (boards) r["after_enemy"] = live_board_json(played.after);
    const bool over = played.pr.mission_ended || l.board.turn >= l.board.total_turns;
    r["mission_over"] = over;
    json samples = json::array();
    if (!over) {
      for (int k : ks) {
        const uint32_t ai_seed = lookahead_seed(lo, k);
        TurnContext sample_ctx = next_ctx;
        if (lo.plan_env) sample_ctx.mission = next_turn_mission(l.board, played.after, next_ctx, ai_seed, lo);
        const Board start = next_turn_start(engine, l.board, played.after, &played.pr, sample_ctx, ai_seed, lo);
        json s = judge_start(engine, start, sample_ctx, l.repair_skills, request, next ? &*next : nullptr);
        s["k"] = k;
        s["ai_seed"] = ai_seed;
        if (lo.plan_env) {
          // The planned hazard tiles (danger marks, ice storm block).
          json hz = json::array();
          for (const DangerTile& d : sample_ctx.mission.danger) hz.push_back(json::array({d.p.x, d.p.y}));
          for (Point p : sample_ctx.mission.freeze) hz.push_back(json::array({p.x, p.y}));
          s["env_hazard"] = hz;
        }
        s["start_hash"] = hash_hex(hash_board_stable(start, HashMode::Search));
        if (boards) s["board"] = live_board_json(start);
        samples.push_back(std::move(s));
      }
    }
    r["samples"] = samples;
    results.push_back(std::move(r));
  }
  out["results"] = results;
  return out;
}

std::string read_text_arg(const std::string& arg) {
  if (arg.empty() || arg[0] != '@') return arg;
  std::ifstream in(arg.substr(1));
  if (!in) throw std::invalid_argument("cannot open " + arg.substr(1));
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::vector<PlayerAction> parse_plan(const json& j) {
  const json& list = j.is_object() && j.contains("plan") ? j["plan"] : j;
  if (!list.is_array()) throw std::invalid_argument("plan must be a JSON list of actions");
  std::vector<PlayerAction> plan;
  for (const json& a : list) plan.push_back(parse_action(a));
  return plan;
}

// The bridge's drop zone (drop_zone, else deployment_zone) of a state file.
std::vector<Point> read_drop_zone(const fs::path& path) {
  std::ifstream in(path);
  json j = json::parse(in, nullptr, false);
  if (j.is_discarded()) throw std::invalid_argument("state is not valid JSON");
  const json* st = &j;
  if (j.contains("data") && j["data"].is_object()) {
    st = &j["data"];
    if (st->contains("bridge_state")) st = &(*st)["bridge_state"];
  }
  std::vector<Point> zone;
  for (const char* key : {"drop_zone", "deployment_zone"}) {
    if (!st->contains(key) || !(*st)[key].is_array() || (*st)[key].empty()) continue;
    for (const json& p : (*st)[key]) {
      if (p.is_array() && p.size() >= 2) zone.push_back(Point{p[0].get<int>(), p[1].get<int>()});
    }
    break;
  }
  return zone;
}

// [[[x,y], ...], ...]: placements, one tile per mech (ascending uid).
std::vector<std::vector<Point>> parse_placements(const json& j) {
  if (!j.is_array()) throw std::invalid_argument("placements must be a list of tile lists");
  std::vector<std::vector<Point>> out;
  for (const json& pl : j) {
    if (!pl.is_array()) throw std::invalid_argument("a placement must be a list of [x, y] tiles");
    std::vector<Point> tiles;
    for (const json& p : pl) {
      const Point q = json_point(p);
      if (!q.valid()) throw std::invalid_argument("placement tiles must be [x, y] on the board");
      tiles.push_back(q);
    }
    out.push_back(std::move(tiles));
  }
  return out;
}

// `tiers`: the optional tiers the turn-1 solves ranked by (threat, next turn).
json summary_json(const DeploySummary& s, TierSet tiers) {
  json mean = json::object();
  for (int i = 0; i < kScoreKeys && i < static_cast<int>(s.mean.size()); ++i) {
    if ((i == static_cast<int>(ScoreKey::Threat) || i == static_cast<int>(ScoreKey::GridSurvival)) &&
        !tier_shown(i, tiers)) {
      continue;
    }
    mean[kTierNames[i]] = s.mean[static_cast<size_t>(i)];
  }
  return json{{"worst_case", score_json(s.worst, tiers)},
              {"tier_min", score_json(s.tier_min, tiers)},
              {"mean", mean},
              {"outcomes", s.outcomes},
              {"seeds", s.seeds}};
}

json candidate_json(const DeployCandidate& c, const std::vector<int32_t>& mechs, TierSet tiers) {
  json tiles = json::array();
  json by_uid = json::object();
  for (size_t i = 0; i < c.tiles.size(); ++i) {
    tiles.push_back(point_json(c.tiles[i]));
    if (i < mechs.size()) by_uid[std::to_string(mechs[i])] = point_json(c.tiles[i]);
  }
  json visual = json::array();
  for (Point p : c.tiles) visual.push_back(to_visual(p));
  return json{{"tiles", tiles},
              {"visual", visual},
              {"by_uid", by_uid},
              {"screen", summary_json(c.screen, tiers)},
              {"value", c.value ? summary_json(*c.value, tiers) : json(nullptr)},
              {"all_proven", c.all_proven}};
}

// ---- "actions": the legal sub-actions after a plan prefix (the fuzzer) ----

// One sub-action a unit may try: the solver's Action model (solver.hpp),
// enumerated the way its search does (solver.cpp unit_actions): moves to
// every tile of the Move skill's target area but the unit's own, every
// non-passive weapon slot at every tile of its target area (two-click
// weapons at every tile of the second area unless the first click is an
// exception), and Repair (mechs; the pilot's repair skill) at its area or
// the unit's tile. Only can_move / can_act units; the engine's legality
// check then accepts or refuses each when it runs.
struct Candidate {
  enum Kind : uint8_t { Move, Weapon, Repair };
  Kind kind = Move;
  int32_t uid = -1;
  int slot = -1;
  std::string skill;
  Point target = kInvalidPoint;
  Point target2 = kInvalidPoint;
};

void valid_unique(std::vector<Point>& pts) {
  std::vector<Point> out;
  for (Point p : pts) {
    if (p.valid() && std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
  }
  pts = std::move(out);
}

bool passive_skill(LuaHost& lua, const std::string& name) {
  const std::optional<std::string> p = lua.lua_string(name, "Passive");
  return (p && !p->empty()) || !lua.lua_string(name, "Name");
}

std::vector<Candidate> unit_candidates(Engine& engine, const Board& b, const Pawn& p, const std::string& repair) {
  LuaHost& lua = engine.lua();
  std::vector<Candidate> out;
  if (can_act(b, p) && !p.frozen) {
    for (int slot = 0; slot < kMaxWeapons; ++slot) {
      const Symbol w = p.weapons[static_cast<size_t>(slot)];
      if (w == kNoSymbol) continue;
      const std::string name(symbol_name(w));
      if (passive_skill(lua, name)) continue;
      std::vector<Point> targets = lua.target_area(b, p, name, p.pos);
      valid_unique(targets);
      const bool two = lua.is_two_click(name);
      for (Point t : targets) {
        Candidate c;
        c.kind = Candidate::Weapon;
        c.uid = p.uid;
        c.slot = slot;
        c.skill = name;
        c.target = t;
        if (two && !lua.two_click_exception(b, p, name, p.pos, t)) {
          const Point first = lua.translate_first_click(b, p, name, p.pos, t);
          std::vector<Point> second = lua.second_target_area(b, p, name, p.pos, first);
          valid_unique(second);
          for (Point t2 : second) {
            c.target2 = t2;
            out.push_back(c);
          }
        } else {
          out.push_back(c);
        }
      }
    }
    if (p.mech) {
      std::vector<Point> targets = lua.target_area(b, p, repair, p.pos);
      valid_unique(targets);
      if (targets.empty()) targets.push_back(p.pos);
      for (Point t : targets) {
        Candidate c;
        c.kind = Candidate::Repair;
        c.uid = p.uid;
        c.skill = repair;
        c.target = t;
        out.push_back(c);
      }
    }
  }
  if (can_move(p)) {
    std::vector<Point> dests = lua.target_area(b, p, "Move", p.pos);
    valid_unique(dests);
    for (Point d : dests) {
      if (d == p.pos) continue;
      Candidate c;
      c.kind = Candidate::Move;
      c.uid = p.uid;
      c.skill = "Move";
      c.target = d;
      out.push_back(c);
    }
  }
  return out;
}

ActionResult run_candidate(Engine& engine, Board& b, const Candidate& c, const ActionOptions& o) {
  engine.lua().seed(kLuaSeed);
  switch (c.kind) {
    case Candidate::Move:
      return engine.move(b, c.uid, c.target, o);
    case Candidate::Weapon:
      return engine.fire_weapon(b, c.uid, c.slot, c.target,
                                c.target2.valid() ? std::optional<Point>(c.target2) : std::nullopt, o);
    case Candidate::Repair:
      return engine.repair(b, c.uid, c.target, c.skill, o);
  }
  return {};
}

const char* candidate_kind(Candidate::Kind k) {
  switch (k) {
    case Candidate::Weapon: return "weapon";
    case Candidate::Repair: return "repair";
    default: return "move";
  }
}

const char* team_key(Team t) {
  switch (t) {
    case Team::Player: return "player";
    case Team::Enemy: return "enemy";
    default: return "neutral";
  }
}

// What is on tile p before the action: the target's kind, for the fuzzer's
// weighting ("building", "mountain", "unit_player", "water", ...).
std::string tile_kind(const Board& b, Point p) {
  if (!p.valid()) return "none";
  if (const Pawn* q = b.pawn_at(p); q && q->alive()) return std::string("unit_") + team_key(q->team);
  const Tile& t = b.tile(p);
  if (t.is_building()) return t.hp > 0 ? "building" : "rubble";
  if (t.lava) return "lava";
  return terrain_name(t.terrain);
}

// The sub-action's effect, before -> after, as counts the fuzzer weights:
// HP lost and deaths by team, the actor's own HP, pushes and bumps (the
// resolution log), falls and drownings, units created, status changes,
// building / mountain HP and grid lost, tiles changed, and the engine's
// own uncertainty (chance nodes, timing flags, Lua errors, unmodelled
// writes, mission hooks).
json effect_json(const Board& before, const Board& after, const Candidate& c, const ActionResult& r,
                 const std::vector<ResolveEvent>& log) {
  json hp = {{"player", 0}, {"enemy", 0}, {"neutral", 0}};
  json kills = {{"player", 0}, {"enemy", 0}, {"neutral", 0}};
  int self_hp = 0, moved = 0, statuses = 0, fell = 0, drowned = 0;
  for (const Pawn& p : before.pawns()) {
    if (!p.alive() || !p.pos.valid()) continue;
    const Pawn* q = after.find_pawn(p.uid);
    const int lost = q ? std::max(0, p.hp - std::max<int>(0, q->hp)) : p.hp;
    if (p.uid == c.uid) {
      self_hp += lost;
    } else {
      hp[team_key(p.team)] = hp[team_key(p.team)].get<int>() + lost;
    }
    const bool dead = !q || !q->alive();
    if (dead) kills[team_key(p.team)] = kills[team_key(p.team)].get<int>() + 1;
    if (q && q->fallen && !p.fallen) ++fell;
    if (q && dead && q->pos.valid()) {
      const Tile& t = after.tile(q->pos);
      if (t.is_liquid() || t.is_chasm()) ++drowned;
    }
    if (q && p.uid != c.uid && q->pos != p.pos) ++moved;
    if (q) {
      statuses += (p.fire != q->fire) + (p.acid != q->acid) + (p.frozen != q->frozen) + (p.shield != q->shield) +
                  (p.webbed != q->webbed);
    }
  }
  json created = json::array();
  for (const Pawn& q : after.pawns()) {
    if (!before.find_pawn(q.uid) && q.alive()) created.push_back(std::string(symbol_name(q.type)));
  }
  int building_hp = 0, buildings_destroyed = 0, mountain_hp = 0, tiles = 0, terrain = 0, fire = 0, smoke = 0,
      acid = 0;
  for (int i = 0; i < kTileCount; ++i) {
    const Point p = Point::from_index(i);
    const Tile& a = before.tile(p);
    const Tile& z = after.tile(p);
    if (a == z) continue;
    ++tiles;
    if (a.terrain != z.terrain) ++terrain;
    if (a.is_building()) {
      const int now = z.is_building() ? z.hp : 0;
      building_hp += std::max(0, a.hp - now);
      if (a.hp > 0 && now <= 0) ++buildings_destroyed;
    }
    if (a.is_mountain()) mountain_hp += std::max(0, a.hp - (z.is_mountain() ? z.hp : 0));
    fire += a.on_fire() != z.on_fire();
    smoke += a.smoke != z.smoke;
    acid += a.acid != z.acid;
  }
  int pushes = 0, bumps = 0;
  for (const ResolveEvent& e : log) {
    if (e.type == ResolveEventType::PushStarted) ++pushes;
    if (e.type == ResolveEventType::PushBlocked) ++bumps;
  }
  const bool same = before.pawns() == after.pawns() && tiles == 0;
  return json{{"hp", hp},
              {"kills", kills},
              {"self_hp", self_hp},
              {"moved_units", moved},
              {"pushes", pushes},
              {"bumps", bumps},
              {"fell", fell},
              {"drowned", drowned},
              {"created", created},
              {"statuses", statuses},
              {"building_hp", building_hp},
              {"buildings_destroyed", buildings_destroyed},
              {"grid", before.grid_power - after.grid_power},
              {"mountain_hp", mountain_hp},
              {"tiles", tiles},
              {"terrain", terrain},
              {"fire_tiles", fire},
              {"smoke_tiles", smoke},
              {"acid_tiles", acid},
              {"chance_nodes", r.resolve.chances.size()},
              {"timing_sensitive", r.resolve.timing_sensitive()},
              {"lua_errors", r.lua_errors.size()},
              {"unapplied_lua_writes", r.unapplied.size()},
              {"mission_hooks", r.mission_events.size()},
              {"unchanged", same}};
}

// The "actions" request: plays the plan prefix exactly as simulate() does
// (the same options and Lua seeding, so the board is predict's board after
// those steps), then lists every sub-action the engine accepts from there,
// each run once on a copy of the board (with "effects", the default: its
// effect_json). Sub-actions the engine refuses or that do nothing are only
// counted ("rejected", by status).
void list_actions(Engine& engine, const Loaded& l, const json& request, json& out) {
  const std::vector<PlayerAction> plan =
      request.contains("plan") ? parse_plan(request["plan"]) : std::vector<PlayerAction>{};
  const bool effects = request.value("effects", true);
  Board b = l.board;
  ActionOptions opts = action_options(l.ctx);
  opts.check_legal = true;
  int refused_at = -1;
  std::string why;
  int step = 0;
  for (size_t i = 0; i < plan.size() && refused_at < 0; ++i) {
    const PlayerAction& a = plan[i];
    const Pawn* p = b.find_pawn(a.uid);
    if (a.move.valid() && p && a.move != p->pos) {
      engine.lua().seed(kLuaSeed);
      const ActionResult r = engine.move(b, a.uid, a.move, opts);
      if (refused(r)) {
        refused_at = step;
        why = "move: " + std::string(to_string(r.status));
        break;
      }
      ++step;
    }
    if (a.kind == PlayerAction::Kind::None) continue;
    const bool weapon = a.kind == PlayerAction::Kind::Weapon;
    const std::string skill = weapon ? a.weapon : (a.weapon.empty() ? std::string("Skill_Repair") : a.weapon);
    engine.lua().seed(kLuaSeed);
    const ActionResult r = weapon ? engine.fire_weapon(b, a.uid, a.weapon, a.target, a.target2, opts)
                                  : engine.repair(b, a.uid, a.target, skill, opts);
    if (refused(r)) {
      refused_at = step;
      why = skill + ": " + to_string(r.status);
      break;
    }
    ++step;
  }
  out["refused"] = refused_at;
  if (refused_at >= 0) {
    out["refused_reason"] = why;
    out["actions"] = json::array();
    return;
  }
  out["board"] = live_board_json(b);
  std::vector<const Pawn*> units;
  for (const Pawn& p : b.pawns()) {
    if (p.controlled() && p.alive() && p.pos.valid()) units.push_back(&p);
  }
  std::sort(units.begin(), units.end(), [](const Pawn* a, const Pawn* c) { return a->uid < c->uid; });
  json list = json::array();
  json rejected = json::object();
  json unit_info = json::array();
  for (const Pawn* p : units) {
    std::string repair = "Skill_Repair";
    for (const auto& [uid, skill] : l.repair_skills) {
      if (uid == p->uid) repair = skill;
    }
    unit_info.push_back(json{{"uid", p->uid}, {"type", std::string(symbol_name(p->type))}, {"active", p->active},
                             {"moved", p->moved}, {"bonus_shift", p->movement.bonus_shift},
                             {"can_move", can_move(*p)}, {"can_act", can_act(b, *p) && !p->frozen}});
    for (const Candidate& c : unit_candidates(engine, b, *p, repair)) {
      Board next = b;
      std::vector<ResolveEvent> log;
      ActionOptions o = opts;
      o.log = &log;
      const ActionResult r = run_candidate(engine, next, c, o);
      if (!r.ok()) {
        const std::string k = to_string(r.status);
        rejected[k] = rejected.value(k, 0) + 1;
        continue;
      }
      json a{{"sub", candidate_kind(c.kind)},
             {"uid", c.uid},
             {"weapon", c.skill},
             {"slot", c.slot},
             {"target", point_json(c.target)},
             {"target2", c.target2.valid() ? point_json(c.target2) : json(nullptr)},
             {"target_kind", tile_kind(b, c.target)},
             {"end", point_json(r.end)}};
      if (effects) a["effect"] = effect_json(b, next, c, r, log);
      list.push_back(std::move(a));
    }
  }
  out["units"] = unit_info;
  out["actions"] = list;
  out["rejected"] = rejected;
}

}  // namespace

json live_board_json(const Board& b) {
  json buildings = json::array();
  // Acid pools (tiles with ACID): live_play excuses a Vek that picks one up
  // while the AI plans its next move, which the engine doesn't model.
  json acid_tiles = json::array();
  // Smoked tiles: smoke the game shows that the prediction has not was laid
  // while the AI planned (a Mosquito smokes the target it queues on), and a
  // burning unit on it was put out then.
  json smoke_tiles = json::array();
  // Shielded buildings and mountains: the bridge cannot read a structure's
  // shield on vanilla (no Board:IsShield), so the driver carries the
  // engine's forward into re-solves and the end-of-turn prediction.
  json shield_tiles = json::array();
  for (int i = 0; i < kTileCount; ++i) {
    const Point p = Point::from_index(i);
    const Tile& t = b.tile(p);
    if (t.is_building()) buildings.push_back(json{{"x", p.x}, {"y", p.y}, {"hp", static_cast<int>(t.hp)}});
    if (t.acid) acid_tiles.push_back(json::array({p.x, p.y}));
    if (t.smoke) smoke_tiles.push_back(json::array({p.x, p.y}));
    if (t.shield && (t.is_building() || t.is_mountain())) shield_tiles.push_back(json::array({p.x, p.y}));
  }
  std::vector<const Pawn*> ps;
  for (const Pawn& p : b.pawns()) {
    if (p.alive() && p.pos.valid() && !p.fallen) ps.push_back(&p);
  }
  std::sort(ps.begin(), ps.end(), [](const Pawn* a, const Pawn* c) { return a->uid < c->uid; });
  json units = json::array();
  for (const Pawn* p : ps) {
    json u{{"uid", p->uid},
           {"type", std::string(symbol_name(p->type))},
           {"x", p->pos.x},
           {"y", p->pos.y},
           {"hp", static_cast<int>(p->hp)},
           {"team", static_cast<int>(p->team)},
           {"mech", p->mech},
           {"fire", p->fire},
           {"acid", p->acid},
           {"frozen", p->frozen},
           {"shield", p->shield},
           {"web", p->webbed}};
    // A psion's mutation (Leader: 1 Soldier = +1 HP to every Vek, ...).
    if (p->leader != Leader::None) u["leader"] = static_cast<int>(p->leader);
    // A mech pilot's level and XP as the engine has them, with the max HP a
    // Health / Skilled level-up raises. The bridge reads level and XP from
    // the save (written at turn start), so it misses a level-up earned this
    // turn and the skill it unlocks (Thick Skin: no ACID, no fire); the
    // driver carries these forward from the predictions.
    if (p->mech && p->pilot_level >= 0) {
      u["pilot_level"] = static_cast<int>(p->pilot_level);
      u["pilot_xp"] = static_cast<int>(p->pilot_xp);
      u["max_hp"] = static_cast<int>(p->max_hp);
    }
    // The queued shot as the engine has it. The bridge reads queued shots
    // from the save (written at turn start), so it cannot see one cleared
    // during the turn (smoke, freezing, water: Pawn::OnLoop) or retargeted
    // (DIR_FLIP); the driver carries these forward from the predictions.
    u["queued"] = p->queued.active() ? json{{"weapon", static_cast<int>(p->queued.weapon)},
                                            {"origin", point_json(p->queued.origin)},
                                            {"target", point_json(p->queued.target)}}
                                     : json(nullptr);
    units.push_back(std::move(u));
  }
  // Burrowers underground (dove after a hit, until the AI moves them): not on
  // the board, so not in the bridge's units either; "x"/"y" are the tile they
  // dove from. A state unit with "underground": true loads as one.
  json underground = json::array();
  for (const Pawn& p : b.pawns()) {
    if (!p.alive() || p.pos.valid() || p.fallen || !p.burrows) continue;
    underground.push_back(json{{"uid", p.uid},
                               {"type", std::string(symbol_name(p.type))},
                               {"x", p.movement.prev_pos.valid() ? p.movement.prev_pos.x : -1},
                               {"y", p.movement.prev_pos.valid() ? p.movement.prev_pos.y : -1},
                               {"hp", static_cast<int>(p.hp)},
                               {"max_hp", static_cast<int>(p.max_hp)},
                               {"team", static_cast<int>(p.team)},
                               {"underground", true}});
  }
  std::sort(underground.begin(), underground.end(),
            [](const json& a, const json& c) { return a["uid"].get<int>() < c["uid"].get<int>(); });
  return json{{"grid_power", b.grid_power}, {"buildings", buildings}, {"units", units},
              {"underground", underground}, {"acid_tiles", acid_tiles}, {"smoke_tiles", smoke_tiles},
              {"shield_tiles", shield_tiles}};
}

LiveSession::LiveSession(const fs::path& game, int threads) : game_(game) {
  main_ = Engine::create(game_);
  ensure_threads(threads);
}

LiveSession::~LiveSession() = default;

// One engine (and Lua state) per extra search thread, loaded in parallel.
void LiveSession::ensure_threads(int threads) {
  const size_t want = static_cast<size_t>(std::max(1, threads)) - 1;
  if (helpers_.size() >= want) return;
  const size_t need = want - helpers_.size();
  std::vector<std::unique_ptr<Engine>> made(need);
  std::vector<std::string> errors(need);
  std::vector<std::thread> pool;
  for (size_t i = 0; i < need; ++i) {
    pool.emplace_back([&, i] {
      try {
        made[i] = Engine::create(game_);
      } catch (const std::exception& e) {
        errors[i] = e.what();
      }
    });
  }
  for (auto& t : pool) t.join();
  for (size_t i = 0; i < need; ++i) {
    if (!made[i]) throw std::runtime_error("helper engine failed to load: " + errors[i]);
    helpers_.push_back(std::move(made[i]));
  }
}

void LiveSession::add_helpers(SolveOptions& so, int threads) {
  ensure_threads(threads);
  so.helper_engines.clear();
  for (int i = 0; i < threads - 1 && i < static_cast<int>(helpers_.size()); ++i) {
    so.helper_engines.push_back(helpers_[static_cast<size_t>(i)].get());
  }
}

json LiveSession::handle(const json& request) {
  try {
    if (!request.is_object()) throw std::invalid_argument("a request must be a JSON object");
    const std::string cmd = request.value("cmd", std::string());
    if (cmd == "ping") return json{{"ok", true}, {"format", kLiveFormat}, {"cmd", cmd}, {"threads", threads()}};
    if (cmd != "board" && cmd != "solve" && cmd != "predict" && cmd != "deploy" && cmd != "actions" &&
        cmd != "two_turn") {
      throw std::invalid_argument("unknown cmd '" + cmd + "' (board, solve, predict, deploy, actions, two_turn, ping)");
    }
    if (!request.contains("state") || !request["state"].is_string()) {
      throw std::invalid_argument("'state' must be the path of a bridge state file");
    }
    const fs::path state = request["state"].get<std::string>();
    Loaded l = load_state(*main_, state);
    json out = header(l, cmd, state);
    if (cmd == "board") {
      out["board"] = live_board_json(l.board);
      return out;
    }
    if (cmd == "actions") {
      list_actions(*main_, l, request, out);
      return out;
    }
    if (cmd == "two_turn") {
      out.update(two_turn_request(*main_, l, request, [this](SolveOptions& so, int want) { add_helpers(so, want); }));
      return out;
    }
    if (cmd == "deploy") {
      // Deployment search (deploy.hpp): the mechs must be on the board (a
      // provisional placement), the drop zone comes from the state.
      const std::vector<Point> zone = read_drop_zone(state);
      if (zone.empty()) throw std::invalid_argument("the state has no drop_zone / deployment_zone");
      std::vector<std::string> warnings;
      if (l.board.turn != 0) warnings.push_back("the state is not at deployment (turn " + std::to_string(l.board.turn) + ")");
      DeployOptions o;
      o.time_limit_s = request.value("time_limit", o.time_limit_s);
      o.screen_samples = request.value("screen_samples", o.screen_samples);
      o.finalists = request.value("finalists", o.finalists);
      o.samples = request.value("samples", o.samples);
      o.solve_time_s = request.value("solve_time", o.solve_time_s);
      o.solve_nodes = request.value("solve_nodes", o.solve_nodes);
      o.seed = request.value("seed", o.seed);
      if (request.contains("placements")) o.placements = parse_placements(request["placements"]);
      if (request.contains("always")) o.always = parse_placements(request["always"]);
      // The turn-1 solves rank as the turns will (the same ranking options).
      o.solve = ranking_options(request, l.repair_skills);
      o.solve.repair_skills = l.repair_skills;
      const int beam = request.value("beam_width", -1);
      if (beam >= 0) o.solve.beam_width = beam;
      const int want = request.value("threads", threads());
      ensure_threads(want);
      for (int i = 0; i < want - 1 && i < static_cast<int>(helpers_.size()); ++i) {
        o.helper_engines.push_back(helpers_[static_cast<size_t>(i)].get());
      }
      const DeployResult r = deploy_search(*main_, l.board, l.ctx, zone, o);
      for (const std::string& w : r.warnings) warnings.push_back(w);
      out["ok"] = r.ok;
      if (!r.ok) out["error"] = r.error;
      out["mechs"] = r.mechs;
      json z = json::array();
      for (Point p : r.zone) z.push_back(point_json(p));
      out["zone"] = z;
      out["sets"] = r.sets;
      out["solves"] = r.solves;
      out["timed_out"] = r.timed_out;
      out["time"] = json{{"screen_s", r.screen_s}, {"finalists_s", r.finalist_s}, {"total_s", r.total_s}};
      out["best"] = r.best ? candidate_json(*r.best, r.mechs, o.solve.tiers()) : json(nullptr);
      json fin = json::array();
      for (const DeployCandidate& c : r.finalists) fin.push_back(candidate_json(c, r.mechs, o.solve.tiers()));
      out["finalists"] = fin;
      out["warnings"] = warnings;
      return out;
    }
    std::vector<std::string> warnings;
    std::vector<PlayerAction> plan;
    std::vector<Point> resist;  // predict: the enemy phase's resisted Grid Defense rolls
    const SolveOptions tiers = ranking_options(request, l.repair_skills);
    if (const int rate = mech_grid_rate(l.board, tiers.score_options()); rate >= 0) {
      out["mech_grid_rate"] = rate;
      if (tiers.mech_grid_pilot) {
        json rates = json::object();
        for (const Pawn& p : l.board.pawns()) {
          if (p.mech && p.team == Team::Player) rates[std::to_string(p.uid)] = mech_rate(l.board, p, tiers.score_options());
        }
        out["mech_rates"] = rates;
      }
    }
    const TierSet threat = tiers.tiers();
    if (tiers.grid_survival) {
      // The Grid Defense chance the survival odds use, and where it came from.
      const int known = l.ctx.mission.grid_defense;
      out["grid_defense"] = json{{"percent", known >= 0 ? known : l.board.grid_defense},
                                 {"source", known >= 0 ? "state" : "default"}};
    }
    if (cmd == "predict") {
      if (!request.contains("plan")) throw std::invalid_argument("predict needs a 'plan'");
      plan = parse_plan(request["plan"]);
      if (auto it = request.find("resist"); it != request.end() && it->is_array()) {
        for (const json& p : *it) {
          const Point q = json_point(p);
          if (!q.valid()) throw std::invalid_argument("predict: 'resist' takes [x, y] tiles");
          resist.push_back(q);
        }
      }
    } else {
      out["searched"] = l.active_units > 0;
      if (l.active_units > 0) {
        SolveOptions so = solve_options_of(request, tiers, l.repair_skills);
        add_helpers(so, request.value("threads", threads()));
        // "end_states": N > 0 collects up to N end-of-turn boards the search
        // reached (SolveOptions::end_states), listed in the reply.
        const int end_states = request.value("end_states", 0);
        std::unique_ptr<EndStateSink> sink;
        if (end_states > 0) {
          sink = std::make_unique<EndStateSink>(static_cast<size_t>(end_states));
          so.end_states = sink.get();
        }
        const SolveResult r = solve_turn(*main_, l.board, l.ctx, so);
        if (sink) {
          json list = json::array();
          for (const EndState& e : sink->states()) {
            json plan_j = json::array();
            for (const PlayerAction& a : e.actions) plan_j.push_back(action_json(l.board, a));
            list.push_back(json{{"plan", plan_j}, {"key", score_json(e.key, threat)}, {"exact", e.exact},
                                {"end_hash", hash_hex(e.end_hash)}});
          }
          out["end_states"] = list;
          out["end_states_offered"] = sink->offered();
        }
        plan = r.best.actions;
        out["worst_case"] = score_json(r.best.worst_case, threat);
        out["upper_bound"] = r.upper_bound ? score_json(*r.upper_bound, threat) : json(nullptr);
        out["proven_optimal"] = r.proven_optimal;
        out["proven_components"] = r.proven_components;
        out["chance_exact"] = r.chance_exact;
        out["timed_out"] = r.timed_out;
        out["extended"] = r.extended;
        out["contingent"] = r.best.contingent;
        out["stats"] = stats_json(r.stats);
        if (so.survival_cap_share > 0) {
          const SurvivalCap& c = r.survival_cap;
          out["survival_cap"] = json{{"ran", c.ran},       {"survey_proof", c.survey_proof}, {"m", c.m},
                                     {"k", c.k},           {"bound", c.bound},               {"proven", c.proven},
                                     {"proofs", c.proofs}, {"time_s", c.time_s},             {"share", so.survival_cap_share},
                                     {"log", c.log}};
        }
        if (so.lookahead) {
          out["stats"]["lookahead_evals"] = r.stats.lookahead_evals;
          out["stats"]["lookahead_hits"] = r.stats.lookahead_hits;
          out["stats"]["lookahead_s"] = r.stats.lookahead_s;
        }
        warnings = r.warnings;
        if (r.best.contingent) warnings.push_back("plan is contingent on a chance node during the player's actions");
      } else {
        warnings.push_back("no active unit: nothing to solve (the enemy phase is still predicted)");
      }
    }
    simulate(*main_, l, plan, out, warnings, tiers.score_options(),
             tiers.lookahead ? &tiers.lookahead_options : nullptr, std::max(0, request.value("branches", 0)),
             resist);
    std::vector<std::string> unique;
    for (const std::string& w : warnings) {
      if (std::find(unique.begin(), unique.end(), w) == unique.end()) unique.push_back(w);
    }
    out["warnings"] = unique;
    return out;
  } catch (const std::exception& e) {
    return json{{"ok", false}, {"format", kLiveFormat}, {"error", e.what()}};
  }
}

namespace {

int usage() {
  std::cerr << "usage: itb_live [--game DIR] (solve <state.json> [--time S] [--max-time S --min-tiers K] [--nodes N] "
               "[--threads N] [--beam W] [--threat-tier] [--threat-kills] [--objectives-first] [--mech-grid N [--mech-grid-turns] [--mech-grid-pilot]] "
               "[--lookahead [--lookahead-samples N] [--lookahead-reply greedy|idle|search] [--lookahead-search-nodes N] [--lookahead-fold worst|sum|worst_sum] [--lookahead-env]] [--grid-survival [--survival-cap] [--survival-cap-share F] [--survival-cap-strict]] | "
               "predict <state.json> --plan JSON|@FILE | board <state.json> | "
               "actions <state.json> [--plan JSON|@FILE] [--no-effects] | "
               "deploy <state.json> [--time S] [--threads N] [--samples N] [--finalists N] [--screen-samples N] "
               "[--solve-time S] [--solve-nodes N] [--seed N] [--placements JSON|@FILE] [--always JSON|@FILE] "
               "[ranking options as for solve] | "
               "serve [--threads N]) "
               "[--out FILE] [--pretty]\n";
  return 2;
}

int serve(LiveSession& session) {
  std::cout << json{{"ok", true}, {"ready", true}, {"format", kLiveFormat}, {"threads", session.threads()}}.dump()
            << std::endl;
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
    json reply;
    json request = json::parse(line, nullptr, false);
    if (request.is_discarded()) {
      reply = json{{"ok", false}, {"format", kLiveFormat}, {"error", "request is not valid JSON"}};
    } else if (request.is_object() && request.value("cmd", std::string()) == "quit") {
      std::cout << json{{"ok", true}, {"cmd", "quit"}}.dump() << std::endl;
      return 0;
    } else {
      reply = session.handle(request);
      if (request.is_object() && request.contains("id")) reply["id"] = request["id"];
    }
    std::cout << reply.dump() << std::endl;
  }
  return 0;
}

}  // namespace

int run_live(int argc, char** argv) {
  fs::path game = GameData::default_game_root();
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--game" && i + 1 < argc) {
      game = argv[++i];
    } else {
      args.push_back(a);
    }
  }
  if (args.empty()) return usage();
  const std::string cmd = args[0];
  json request{{"cmd", cmd}};
  int threads = 1;
  bool pretty = false;
  fs::path out_path;
  size_t i = 1;
  if (cmd == "solve" || cmd == "predict" || cmd == "board" || cmd == "deploy" || cmd == "actions") {
    if (args.size() < 2) return usage();
    request["state"] = args[1];
    i = 2;
  } else if (cmd != "serve") {
    return usage();
  }
  // Ranking options: solve, predict and deploy (its turn-1 solves).
  const bool ranked = cmd == "solve" || cmd == "predict" || cmd == "deploy";
  try {
    for (; i < args.size(); ++i) {
      const bool more = i + 1 < args.size();
      if (args[i] == "--time" && more && (cmd == "solve" || cmd == "deploy")) {
        request["time_limit"] = std::stod(args[++i]);
      } else if (args[i] == "--samples" && more && cmd == "deploy") {
        request["samples"] = std::stoi(args[++i]);
      } else if (args[i] == "--finalists" && more && cmd == "deploy") {
        request["finalists"] = std::stoi(args[++i]);
      } else if (args[i] == "--screen-samples" && more && cmd == "deploy") {
        request["screen_samples"] = std::stoi(args[++i]);
      } else if (args[i] == "--solve-time" && more && cmd == "deploy") {
        request["solve_time"] = std::stod(args[++i]);
      } else if (args[i] == "--solve-nodes" && more && cmd == "deploy") {
        request["solve_nodes"] = std::stoull(args[++i]);
      } else if (args[i] == "--seed" && more && cmd == "deploy") {
        request["seed"] = std::stoul(args[++i]);
      } else if (args[i] == "--placements" && more && cmd == "deploy") {
        request["placements"] = json::parse(read_text_arg(args[++i]));
      } else if (args[i] == "--always" && more && cmd == "deploy") {
        request["always"] = json::parse(read_text_arg(args[++i]));
      } else if (args[i] == "--nodes" && more && cmd == "solve") {
        request["node_limit"] = std::stoull(args[++i]);
      } else if (args[i] == "--max-time" && more && cmd == "solve") {
        request["max_time"] = std::stod(args[++i]);
      } else if (args[i] == "--min-tiers" && more && cmd == "solve") {
        request["min_tiers"] = std::stoi(args[++i]);
      } else if (args[i] == "--threat-tier" && ranked) {
        request["threat_tier"] = true;
      } else if (args[i] == "--threat-kills" && ranked) {
        request["threat_kills"] = true;
      } else if (args[i] == "--objectives-first" && ranked) {
        request["objectives_first"] = true;
      } else if (args[i] == "--mech-grid" && more && ranked) {
        request["mech_grid"] = std::stoi(args[++i]);
      } else if (args[i] == "--mech-grid-turns" && ranked) {
        request["mech_grid_turns"] = true;
      } else if (args[i] == "--mech-grid-pilot" && ranked) {
        request["mech_grid_pilot"] = true;
      } else if (args[i] == "--lookahead" && ranked) {
        request["lookahead"] = true;
      } else if (args[i] == "--lookahead-samples" && more && ranked) {
        request["lookahead_samples"] = std::stoi(args[++i]);
      } else if (args[i] == "--lookahead-reply" && more && ranked) {
        request["lookahead_reply"] = args[++i];
      } else if (args[i] == "--lookahead-fold" && more && ranked) {
        request["lookahead_fold"] = args[++i];
      } else if (args[i] == "--lookahead-search-nodes" && more && ranked) {
        request["lookahead_search_nodes"] = std::stoi(args[++i]);
      } else if (args[i] == "--lookahead-env" && ranked) {
        request["lookahead_env"] = true;
      } else if (args[i] == "--grid-survival" && ranked) {
        request["grid_survival"] = true;
      } else if (args[i] == "--survival-cap" && cmd == "solve") {
        request["survival_cap"] = true;
      } else if (args[i] == "--survival-cap-share" && more && cmd == "solve") {
        request["survival_cap"] = true;
        request["survival_cap_share"] = std::stod(args[++i]);
      } else if (args[i] == "--survival-cap-strict" && cmd == "solve") {
        request["survival_cap"] = true;
        request["survival_cap_strict"] = true;
      } else if (args[i] == "--beam" && more && (cmd == "solve" || cmd == "deploy")) {
        request["beam_width"] = std::stoi(args[++i]);
      } else if (args[i] == "--threads" && more && (cmd == "solve" || cmd == "serve" || cmd == "deploy")) {
        threads = std::max(1, std::stoi(args[++i]));
        request["threads"] = threads;
      } else if (args[i] == "--branches" && more && cmd == "predict") {
        request["branches"] = std::stoi(args[++i]);
      } else if (args[i] == "--resist" && more && cmd == "predict") {
        request["resist"] = json::parse(read_text_arg(args[++i]));
      } else if (args[i] == "--plan" && more && (cmd == "predict" || cmd == "actions")) {
        request["plan"] = json::parse(read_text_arg(args[++i]));
      } else if (args[i] == "--no-effects" && cmd == "actions") {
        request["effects"] = false;
      } else if (args[i] == "--out" && more && cmd != "serve") {
        out_path = args[++i];
      } else if (args[i] == "--pretty" && cmd != "serve") {
        pretty = true;
      } else {
        return usage();
      }
    }
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 2;
  }
  std::unique_ptr<LiveSession> session;
  try {
    session = std::make_unique<LiveSession>(game, cmd == "board" || cmd == "predict" || cmd == "actions" ? 1 : threads);
  } catch (const std::exception& e) {
    std::cout << json{{"ok", false}, {"format", kLiveFormat}, {"error", e.what()}}.dump() << std::endl;
    return 1;
  }
  if (cmd == "serve") return serve(*session);
  const json reply = session->handle(request);
  const std::string text = reply.dump(pretty ? 1 : -1);
  if (!out_path.empty()) {
    std::ofstream f(out_path);
    f << text << "\n";
    if (!f) {
      std::cerr << "error: cannot write " << out_path.string() << "\n";
      return 1;
    }
  }
  std::cout << text << std::endl;
  return reply.value("ok", false) ? 0 : 1;
}

}  // namespace itb::tools
