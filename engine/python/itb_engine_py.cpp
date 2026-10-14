// Python bindings for the engine: the live bot's solver (module itb_engine).
//
// JSON in, JSON out, like the old Rust module (itb_solver): every entry point
// takes the bridge state as JSON text (the same object the Lua bridge writes
// and the bot records in m*_solve_input.json) and returns a JSON string.
//
//   Engine(game_root="", threads=1, difficulty=0)
//   Engine.load(bridge_json)                 -> board as loaded + warnings
//   Engine.solve(bridge_json, options_json)  -> plan, score, proof, stats and
//                                               the plan simulated step by step
//   Engine.simulate(bridge_json, plan_json)  -> boards after every sub-action,
//                                               after the player's turn and after
//                                               the enemy phase (default outcome)
//   Engine.evaluate(bridge_json, plan_json, options_json)
//                                            -> worst case of a fixed plan
//
// Boards are serialized in the bridge's own shape (tiles with terrain names
// and ids, units with x/y/hp/statuses/queued shots) so the bot can read them
// with Board.from_bridge_data and diff them against the live game exactly as
// it diffed the old solver's predictions.
//
// Plans are lists of {"uid", "move": [x,y]|null, "kind": "none"|"weapon"|
// "repair", "weapon", "target": [x,y]|null, "target2": [x,y]|null}, the
// engine's PlayerAction: a unit's move and the action that directly follows
// it are one entry; interleaved sub-actions are separate entries.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <pybind11/pybind11.h>

#include "itb/board.hpp"
#include "itb/core.hpp"
#include "itb/engine.hpp"
#include "itb/environment.hpp"
#include "itb/game_data.hpp"
#include "itb/movement.hpp"
#include "itb/recording.hpp"
#include "itb/score.hpp"
#include "itb/solver.hpp"
#include "itb/symbols.hpp"
#include "itb/tile_rules.hpp"

namespace py = pybind11;
using nlohmann::json;

namespace itb::python {
namespace {

// Bump when the engine's predictions change for the same board in a way the
// bot's records should distinguish (the C++ counterpart of the Rust
// SIMULATOR_VERSION; the bot stamps it as engine_version).
// 2: player actions run the mission's per-frame hooks (acid storm, dam, ...).
constexpr const char* kEngineVersion = "cpp-engine-2";

#ifndef ITB_BUILD_GIT
#define ITB_BUILD_GIT "unknown"
#endif

// solver.cpp seeds the Lua stream with 1 before every sub-action and every
// enemy phase, so a run is a function of the board and the choices; the
// simulation does the same to reproduce the solver's default outcome.
constexpr uint32_t kLuaSeed = 1;

// ScoreKey order (score.hpp). Until the threat tier was added, ObjectivesFailed
// and MechHpLost were labelled the other way round here.
const char* kTierNames[kScoreKeys] = {"grid",    "building_hp", "mechs", "objectives_failed", "next_turn", "threat",
                                      "mech_hp", "objectives",  "kills", "vek_hp",            "position"};

// The tiers in use: "threat" only with the threat tier, "next_turn" only
// with the lookahead.
json score_json(const Score& s, TierSet threat = 0) {
  json j = json::object();
  for (int i = 0; i < kScoreKeys; ++i) {
    if (!tier_used(i, threat)) continue;
    const int32_t v = s.v[static_cast<size_t>(i)];
    j[kTierNames[i]] = v == INT32_MAX ? json(nullptr) : json(v);
  }
  return j;
}

json score_vector(const Score& s, TierSet threat = 0) {
  json v = json::array();
  for (int i = 0; i < kScoreKeys; ++i) {
    if (!tier_used(i, threat)) continue;
    const int32_t x = s.v[static_cast<size_t>(i)];
    v.push_back(x == INT32_MAX ? json(nullptr) : json(x));
  }
  return v;
}

json point_json(Point p) { return p.valid() ? json::array({p.x, p.y}) : json(nullptr); }

Point json_point(const json& j) {
  if (!j.is_array() || j.size() < 2 || !j[0].is_number_integer() || !j[1].is_number_integer()) return kInvalidPoint;
  return {j[0].get<int>(), j[1].get<int>()};
}

// Terrain names as the bridge writes them (src/model/board.py
// BRIDGE_TERRAIN_ID_MAP); lava is water with the lava flag.
const char* bridge_terrain_name(const Tile& t) {
  switch (t.terrain) {
    case Terrain::Road: return "ground";
    case Terrain::Building: return "building";
    case Terrain::Rubble: return "rubble";
    case Terrain::Water: return t.lava ? "lava" : "water";
    case Terrain::Mountain: return "mountain";
    case Terrain::Ice: return "ice";
    case Terrain::Forest: return "forest";
    case Terrain::Sand: return "sand";
    case Terrain::Hole: return "chasm";
    default: return "ground";
  }
}

json tile_json(const Board& b, Point p) {
  const Tile& t = b.tile(p);
  json j{{"x", p.x},
         {"y", p.y},
         {"terrain", bridge_terrain_name(t)},
         {"terrain_id", static_cast<int>(t.terrain)},
         {"lava", t.lava},
         {"fire", t.on_fire()},
         {"smoke", t.smoke},
         {"acid", t.acid},
         {"shield", t.shield},
         {"frozen", t.frozen},
         {"cracked", t.cracked || (t.terrain == Terrain::Ice && t.hp == 1)},
         {"pod", t.pod == PodState::Present},
         {"conveyor", static_cast<int>(t.conveyor)},
         {"item", t.item == kNoSymbol ? std::string() : std::string(symbol_name(t.item))}};
  if (t.is_building() || t.is_mountain()) j["building_hp"] = std::max<int>(0, t.hp);
  if (t.is_building()) j["populated"] = t.populated;
  if (t.unique_building != kNoSymbol) {
    j["unique_building"] = true;
    j["objective_name"] = std::string(symbol_name(t.unique_building));
  }
  return j;
}

json unit_json(const Board& b, const Pawn& p) {
  json weapons = json::array();
  for (Symbol w : p.weapons) {
    if (w != kNoSymbol) weapons.push_back(std::string(symbol_name(w)));
  }
  json j{{"uid", p.uid},
         {"type", std::string(symbol_name(p.type))},
         {"x", p.pos.valid() ? p.pos.x : -1},
         {"y", p.pos.valid() ? p.pos.y : -1},
         {"off_board", !p.pos.valid()},
         {"hp", std::max<int>(0, p.hp)},
         {"max_hp", std::max<int>(p.max_hp, p.hp)},
         {"team", static_cast<int>(p.team)},
         {"mech", p.mech},
         {"flying", p.flying},
         {"massive", p.massive},
         {"armor", p.armor},
         {"pushable", p.pushable},
         {"minor", p.minor},
         {"neutral", p.neutral},
         {"active", p.active},
         {"moved", p.moved},
         {"fire", p.fire},
         {"acid", p.acid},
         {"frozen", p.frozen},
         {"shield", p.shield},
         // The bridge reports Pawn:IsBoosted(), which includes the Boost
         // psion; the engine keeps only the pawn's own boost (recording.cpp).
         {"boosted", p.boosted || (p.alive() && mutation_affects(b, p, Leader::Boosted))},
         {"web", p.webbed},
         {"infected", p.infected},
         {"fallen", p.fallen},
         {"weapons", weapons},
         {"move", base_move(b, p)},
         {"base_move", static_cast<int>(p.move)},
         {"has_queued_attack", p.queued.active()}};
  if (p.webbed) {
    // The pawns on the tiles webbing it (webs are tile state); the first
    // also as web_source_uid, the field the bridge reports.
    json sources = json::array();
    for (Point t : {p.pos, p.extra_tile()}) {
      const uint8_t from = web_sources(b, t);
      for (int d = 0; d < 4; ++d) {
        if (!(from >> d & 1)) continue;
        if (const Pawn* src = b.pawn_at(step(t, static_cast<Dir>(d)))) sources.push_back(src->uid);
      }
    }
    if (!sources.empty()) {
      j["web_source_uid"] = sources[0];
      j["web_source_uids"] = sources;
    }
  }
  if (p.queued.active()) {
    j["queued_target"] = point_json(p.queued.target);
    if (p.queued.origin.valid()) j["queued_origin"] = point_json(p.queued.origin);
  }
  return j;
}

json board_json(const Board& b) {
  json tiles = json::array();
  for (int x = 0; x < kBoardSize; ++x) {
    for (int y = 0; y < kBoardSize; ++y) tiles.push_back(tile_json(b, {x, y}));
  }
  json units = json::array();
  for (const Pawn& p : b.pawns()) units.push_back(unit_json(b, p));
  json spawns = json::array();
  for (Point p : b.spawn_points) spawns.push_back(point_json(p));
  return json{{"grid_power", b.grid_power},
              {"grid_power_max", b.grid_power_max},
              {"turn", b.turn},
              {"total_turns", b.total_turns},
              {"player_phase", b.player_phase},
              {"spawning_tiles", spawns},
              {"tiles", tiles},
              {"units", units}};
}

const char* kind_name(PlayerAction::Kind k) {
  switch (k) {
    case PlayerAction::Kind::Weapon: return "weapon";
    case PlayerAction::Kind::Repair: return "repair";
    default: return "none";
  }
}

json action_json(const Board& before, const PlayerAction& a) {
  json j{{"uid", a.uid},
         {"move", point_json(a.move)},
         {"kind", kind_name(a.kind)},
         {"weapon", a.weapon},
         {"target", point_json(a.target)},
         {"target2", a.target2 ? point_json(*a.target2) : json(nullptr)},
         {"description", describe_action(before, a)}};
  return j;
}

PlayerAction parse_action(const json& j) {
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
  return a;
}

std::vector<PlayerAction> parse_plan(const std::string& text) {
  const json j = json::parse(text);
  const json& list = j.is_object() && j.contains("plan") ? j["plan"] : j;
  if (!list.is_array()) throw std::invalid_argument("plan must be a JSON list of actions");
  std::vector<PlayerAction> plan;
  for (const json& a : list) plan.push_back(parse_action(a));
  return plan;
}

// The bridge state, loaded the way itb_inspect loads a recording (the
// recording loader's bridge_state path), plus what the solver tool adds:
// pilot abilities and repair skills, and the bot's can_move=false (a mech
// that has moved but not acted, after a mid-turn re-solve).
struct Loaded {
  Recording rec;
  Board board;
  TurnContext ctx;
  std::vector<std::pair<int32_t, std::string>> repair_skills;
  std::vector<std::string> warnings;
  int active_units = 0;
};

std::atomic<uint64_t> g_tmp_counter{0};

Loaded load_state(Engine& engine, const std::string& text) {
  json input = json::parse(text);
  if (input.is_object() && input.contains("data") && input["data"].is_object() &&
      input["data"].contains("bridge_state")) {
    input = input["data"]["bridge_state"];
  } else if (input.is_object() && input.contains("bridge_state")) {
    input = input["bridge_state"];
  }
  if (!input.is_object() || !input.contains("tiles") || !input.contains("units")) {
    throw std::invalid_argument("not a bridge board (needs tiles and units)");
  }
  // load_recording reads a file; hand it the state through a private
  // temporary file so the loader stays the single parser of bridge data.
  const auto tmp = std::filesystem::temp_directory_path() /
                   ("itb_engine_" + std::to_string(::getpid()) + "_" + std::to_string(g_tmp_counter++) + ".json");
  {
    std::ofstream out(tmp);
    if (!out) throw std::runtime_error("cannot write " + tmp.string());
    out << input.dump();
  }
  std::string error;
  std::optional<Recording> rec = load_recording(tmp, &engine.data(), &error);
  std::error_code ec;
  std::filesystem::remove(tmp, ec);
  if (!rec) throw std::invalid_argument("bridge state did not load: " + error);

  Loaded out;
  out.rec = std::move(*rec);
  out.board = out.rec.board;
  out.warnings = out.rec.warnings;
  for (const auto& [uid, pilot] : out.rec.pilots) {
    if (Pawn* p = out.board.find_pawn(uid)) {
      p->pilot_abilities |= engine.pilot_ability(pilot);
      out.repair_skills.emplace_back(uid, engine.repair_skill(pilot));
    }
  }
  for (const json& u : input["units"]) {
    if (!u.is_object()) continue;
    auto cm = u.find("can_move");
    if (cm == u.end() || !cm->is_boolean() || cm->get<bool>()) continue;
    if (Pawn* p = out.board.find_pawn(u.value("uid", -1))) p->moved = true;
  }
  for (const Pawn& p : out.board.pawns()) {
    if (p.controlled() && p.alive() && p.active) ++out.active_units;
  }
  out.ctx.mission = out.rec.mission;
  return out;
}

// The mission's per-frame hooks run in every action (`ctx` must outlive the
// options).
ActionOptions sim_options(const TurnContext& ctx) {
  ActionOptions o = action_options(ctx);
  o.check_legal = true;
  return o;
}

bool refused(const ActionResult& r) { return !r.ok() && r.status != ActionStatus::NoEffect; }

json phase_json(const PhaseResult& pr) {
  json events = json::array();
  for (const PhaseEvent& e : pr.events) {
    events.push_back(json{{"type", to_string(e.type)},
                          {"point", point_json(e.point)},
                          {"uid", e.uid},
                          {"amount", e.amount},
                          {"detail", e.detail}});
  }
  json emerged = json::array();
  for (Point p : pr.emerged_unknown) emerged.push_back(point_json(p));
  return json{{"environment", pr.environment},
              {"exact", pr.exact},
              {"quiescent", pr.quiescent},
              {"mission_ended", pr.mission_ended},
              {"chance_nodes", pr.chances.size()},
              {"timing_sensitive", pr.timing_sensitive()},
              {"lua_errors", pr.lua_errors},
              {"unapplied_lua_writes", pr.unapplied.size()},
              {"emerged_unknown", emerged},
              {"events", events}};
}

// Runs `plan` step by step with the default outcome of every chance node
// (no Grid Defense resist, first branch, the pawn's own death seed), the
// outcome the solver's plan follows, and records the board after every move
// and every action, after the player's turn and after the enemy phase.
json simulate(Engine& engine, const Loaded& l, const std::vector<PlayerAction>& plan,
              const ScoreOptions& score_options = {}) {
  const TierSet threat = tier_set(score_options);
  Board b = l.board;
  const ActionOptions opts = sim_options(l.ctx);
  json steps = json::array();
  int refused_at = -1;
  std::string refused_why;
  for (size_t i = 0; i < plan.size() && refused_at < 0; ++i) {
    const PlayerAction& a = plan[i];
    json step{{"index", i}, {"action", action_json(b, a)}};
    const Pawn* p = b.find_pawn(a.uid);
    step["pos_before"] = p ? point_json(p->pos) : json(nullptr);
    step["after_move"] = nullptr;
    json lua_errors = json::array();
    size_t chances = 0;
    if (a.move.valid() && p && a.move != p->pos) {
      engine.lua().seed(kLuaSeed);
      const ActionResult r = engine.move(b, a.uid, a.move, opts);
      step["move_status"] = to_string(r.status);
      for (const auto& e : r.lua_errors) lua_errors.push_back(e);
      chances += r.resolve.chances.size();
      if (refused(r)) {
        refused_at = static_cast<int>(i);
        refused_why = "move: " + std::string(to_string(r.status));
      }
      step["after_move"] = board_json(b);
    }
    if (refused_at < 0 && a.kind != PlayerAction::Kind::None) {
      engine.lua().seed(kLuaSeed);
      const ActionResult r =
          a.kind == PlayerAction::Kind::Weapon
              ? engine.fire_weapon(b, a.uid, a.weapon, a.target, a.target2, opts)
              : engine.repair(b, a.uid, a.target, a.weapon.empty() ? "Skill_Repair" : a.weapon, opts);
      step["action_status"] = to_string(r.status);
      step["fired"] = r.weapon;
      for (const auto& e : r.lua_errors) lua_errors.push_back(e);
      chances += r.resolve.chances.size();
      step["timing_sensitive"] = r.resolve.timing_sensitive();
      step["unapplied_lua_writes"] = r.unapplied.size();
      if (refused(r)) {
        refused_at = static_cast<int>(i);
        refused_why = a.weapon + ": " + std::string(to_string(r.status));
      }
    }
    if (const Pawn* q = b.find_pawn(a.uid)) {
      step["pos_after"] = point_json(q->pos);
    } else {
      step["pos_after"] = nullptr;
    }
    step["after_action"] = board_json(b);
    step["chance_nodes"] = chances;
    step["lua_errors"] = lua_errors;
    steps.push_back(std::move(step));
  }
  json out{{"start_board", board_json(l.board)}, {"steps", steps}, {"refused", refused_at}};
  if (refused_at >= 0) {
    out["refused_reason"] = refused_why;
    return out;
  }
  out["post_player_board"] = board_json(b);
  TurnContext tc = l.ctx;
  engine.lua().seed(kLuaSeed);
  const PhaseResult pr = engine.end_turn(b, tc);
  out["final_board"] = board_json(b);
  out["enemy_phase"] = phase_json(pr);
  const Score s = score_turn(l.board, b, &tc, &pr, score_options);
  out["score"] = score_json(s, threat);
  out["score_vector"] = score_vector(s, threat);
  return out;
}

json stats_json(const SolveStats& s) {
  return json{{"threads", s.threads},
              {"nodes", s.nodes},
              {"sub_actions", s.sub_actions},
              {"enemy_phases", s.enemy_phases},
              {"chance_branches", s.chance_branches},
              {"tt_hits", s.tt_hits},
              {"leaf_hits", s.leaf_hits},
              {"duplicate_children", s.duplicate_children},
              {"bound_prunes", s.bound_prunes},
              {"chance_cutoffs", s.chance_cutoffs},
              {"time_s", s.time_s},
              {"sub_action_s", s.sub_action_s},
              {"enemy_phase_s", s.enemy_phase_s},
              {"first_plan_s", s.first_plan_s},
              {"best_plan_s", s.best_plan_s},
              {"base_search_s", s.base_search_s},
              {"base_proven_components", s.base_proven_components}};
}

class PyEngine {
 public:
  PyEngine(const std::string& game_root, int threads, int difficulty) {
    root_ = game_root.empty() ? GameData::default_game_root() : std::filesystem::path(game_root);
    options_.lua.difficulty = difficulty;
    difficulty_ = difficulty;
    const auto t0 = std::chrono::steady_clock::now();
    main_ = Engine::create(root_, options_);
    ensure_threads(threads);
    load_s_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  }

  // One engine (and Lua state) per search thread; extra engines load in
  // parallel, each on its own thread.
  void ensure_threads(int threads) {
    const size_t want = static_cast<size_t>(std::max(1, threads)) - 1;
    if (helpers_.size() >= want) return;
    const size_t need = want - helpers_.size();
    std::vector<std::unique_ptr<Engine>> made(need);
    std::vector<std::string> errors(need);
    std::vector<std::thread> pool;
    for (size_t i = 0; i < need; ++i) {
      pool.emplace_back([&, i] {
        try {
          made[i] = Engine::create(root_, options_);
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

  std::string game_root() const { return root_.string(); }
  int threads() const { return 1 + static_cast<int>(helpers_.size()); }
  int difficulty() const { return difficulty_; }
  double load_seconds() const { return load_s_; }

  std::string load(const std::string& bridge) {
    std::lock_guard<std::mutex> lock(mu_);
    Loaded l = load_state(*main_, bridge);
    json out{{"mission_id", l.rec.mission_id},
             {"phase", l.rec.phase},
             {"board_busy", l.rec.board_busy},
             {"turn", l.board.turn},
             {"difficulty", l.rec.difficulty},
             {"active_units", l.active_units},
             {"warnings", l.warnings},
             {"board", board_json(l.board)}};
    return out.dump();
  }

  std::string simulate_plan(const std::string& bridge, const std::string& plan_text) {
    std::lock_guard<std::mutex> lock(mu_);
    Loaded l = load_state(*main_, bridge);
    const std::vector<PlayerAction> plan = parse_plan(plan_text);
    json out = simulate(*main_, l, plan);
    out["load_warnings"] = l.warnings;
    return out.dump();
  }

  std::string evaluate(const std::string& bridge, const std::string& plan_text, const std::string& options_text) {
    std::lock_guard<std::mutex> lock(mu_);
    Loaded l = load_state(*main_, bridge);
    const std::vector<PlayerAction> plan = parse_plan(plan_text);
    SolveOptions so = solve_options(options_text, l);
    std::string why;
    const std::optional<Score> s = evaluate_plan(*main_, l.board, l.ctx, plan, so, &why);
    json out{{"ok", s.has_value()}};
    if (s) {
      out["worst_case"] = score_json(*s, so.tiers());
      out["worst_case_vector"] = score_vector(*s, so.tiers());
    } else {
      out["refused"] = why;
    }
    return out.dump();
  }

  std::string solve(const std::string& bridge, const std::string& options_text) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto t0 = std::chrono::steady_clock::now();
    Loaded l = load_state(*main_, bridge);
    const json opt = options_text.empty() ? json::object() : json::parse(options_text);
    if (opt.contains("threads")) ensure_threads(opt["threads"].get<int>());
    SolveOptions so = solve_options(options_text, l);
    const int threads = opt.value("threads", this->threads());
    for (int i = 0; i < threads - 1 && i < static_cast<int>(helpers_.size()); ++i) {
      so.helper_engines.push_back(helpers_[static_cast<size_t>(i)].get());
    }
    json out{{"engine_version", kEngineVersion},
             {"mission_id", l.rec.mission_id},
             {"turn", l.board.turn},
             {"active_units", l.active_units},
             {"load_warnings", l.warnings}};
    if (l.active_units == 0) {
      out["plan"] = json::array();
      out["error"] = "no active unit";
      return out.dump();
    }
    SolveResult r;
    {
      py::gil_scoped_release release;
      r = solve_turn(*main_, l.board, l.ctx, so);
    }
    Board cur = l.board;
    json plan = json::array();
    for (const PlayerAction& a : r.best.actions) plan.push_back(action_json(cur, a));
    out["plan"] = plan;
    out["worst_case"] = score_json(r.best.worst_case, so.tiers());
    out["worst_case_vector"] = score_vector(r.best.worst_case, so.tiers());
    out["contingent"] = r.best.contingent;
    out["proven_optimal"] = r.proven_optimal;
    out["upper_bound"] = r.upper_bound ? score_json(*r.upper_bound, so.tiers()) : json(nullptr);
    out["upper_bound_vector"] = r.upper_bound ? score_vector(*r.upper_bound, so.tiers()) : json(nullptr);
    out["proven_components"] = r.proven_components;
    out["chance_exact"] = r.chance_exact;
    out["timed_out"] = r.timed_out;
    out["extended"] = r.extended;
    out["warnings"] = r.warnings;
    out["stats"] = stats_json(r.stats);
    if (opt.value("simulate", true)) out["simulation"] = simulate(*main_, l, r.best.actions, so.score_options());
    out["elapsed_s"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return out.dump();
  }

 private:
  static SolveOptions solve_options(const std::string& text, const Loaded& l) {
    const json opt = text.empty() ? json::object() : json::parse(text);
    SolveOptions so;
    so.time_limit_s = opt.value("time_limit", so.time_limit_s);
    so.node_limit = opt.value("node_limit", so.node_limit);
    so.max_time_s = opt.value("max_time", so.max_time_s);
    so.min_proven_tiers = opt.value("min_tiers", so.min_proven_tiers);
    so.threat_tier = opt.value("threat_tier", so.threat_tier);
    so.threat_kills = opt.value("threat_kills", so.threat_kills);
    so.objectives_first = opt.value("objectives_first", so.objectives_first);
    so.mech_grid = opt.value("mech_grid", so.mech_grid);
    so.mech_grid_turns = opt.value("mech_grid_turns", so.mech_grid_turns);
    so.mech_grid_pilot = opt.value("mech_grid_pilot", so.mech_grid_pilot);
    so.lookahead = opt.value("lookahead", so.lookahead);
    so.lookahead_options.samples = std::max(1, opt.value("lookahead_samples", so.lookahead_options.samples));
    if (opt.value("lookahead_reply", std::string("greedy")) == "idle") {
      so.lookahead_options.reply = LookaheadOptions::Reply::Idle;
    }
    so.lookahead_options.sum = opt.value("lookahead_fold", std::string("worst")) == "sum";
    so.lookahead_options.repair_skills = l.repair_skills;
    so.beam_width = opt.value("beam_width", so.beam_width);
    so.extra_death_seeds = opt.value("extra_death_seeds", so.extra_death_seeds);
    so.max_chance_leaves = opt.value("max_chance_leaves", so.max_chance_leaves);
    so.tt_max_entries = opt.value("tt_max_entries", so.tt_max_entries);
    so.repair_skills = l.repair_skills;
    return so;
  }

  std::filesystem::path root_;
  EngineOptions options_;
  int difficulty_ = 0;
  double load_s_ = 0;
  std::unique_ptr<Engine> main_;
  std::vector<std::unique_ptr<Engine>> helpers_;
  std::mutex mu_;
};

}  // namespace
}  // namespace itb::python

PYBIND11_MODULE(itb_engine, m) {
  using itb::python::PyEngine;
  m.doc() = "Into the Breach C++ engine and perfect-turn solver (engine/README.md, Python bindings)";
  m.attr("ENGINE_VERSION") = itb::python::kEngineVersion;
  m.attr("BUILD_GIT") = ITB_BUILD_GIT;
  m.def("default_game_root", [] { return itb::GameData::default_game_root().string(); },
        "ITB_GAME_DIR if set, else the build-configured default game root.");
  py::register_exception<std::invalid_argument>(m, "InvalidInput", PyExc_ValueError);
  py::class_<PyEngine>(m, "Engine")
      .def(py::init<const std::string&, int, int>(), py::arg("game_root") = "", py::arg("threads") = 1,
           py::arg("difficulty") = 0, py::call_guard<py::gil_scoped_release>(),
           "Load the game scripts (game_root: a directory containing scripts/scripts.lua; '' = "
           "default_game_root()) into `threads` engines, one per search thread.")
      .def_property_readonly("game_root", &PyEngine::game_root)
      .def_property_readonly("threads", &PyEngine::threads)
      .def_property_readonly("difficulty", &PyEngine::difficulty)
      .def_property_readonly("load_seconds", &PyEngine::load_seconds)
      .def("ensure_threads", &PyEngine::ensure_threads, py::arg("threads"),
           py::call_guard<py::gil_scoped_release>())
      .def("load", &PyEngine::load, py::arg("bridge_json"),
           "Load a bridge state (JSON text): the board as the engine sees it, plus loader warnings.")
      .def("solve", &PyEngine::solve, py::arg("bridge_json"), py::arg("options_json") = "{}",
           "Perfect-turn search. Options: time_limit, max_time, min_tiers, node_limit, threads, beam_width, "
           "extra_death_seeds, max_chance_leaves, tt_max_entries, threat_tier, threat_kills, objectives_first (default false), mech_grid (default -1: off), mech_grid_turns, mech_grid_pilot, "
           "lookahead, lookahead_samples, lookahead_reply ('greedy' | 'idle'), "
           "simulate (default true).")
      .def("simulate", &PyEngine::simulate_plan, py::arg("bridge_json"), py::arg("plan_json"),
           py::call_guard<py::gil_scoped_release>(),
           "Run a plan step by step (default chance outcomes) and return every intermediate board.")
      .def("evaluate", &PyEngine::evaluate, py::arg("bridge_json"), py::arg("plan_json"),
           py::arg("options_json") = "{}", py::call_guard<py::gil_scoped_release>(),
           "Worst case of a fixed plan over every chance outcome (evaluate_plan).");
}
