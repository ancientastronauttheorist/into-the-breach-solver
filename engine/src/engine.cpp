// Stage 6 integration: the Lua host driving the executor (engine.hpp).

#include "itb/engine.hpp"

#include <algorithm>
#include <stdexcept>
#include <variant>

#include "itb/game_data.hpp"
#include "itb/movement.hpp"
#include "engine_impl.hpp"

namespace itb {

const char* to_string(ActionStatus s) {
  switch (s) {
    case ActionStatus::Ok: return "ok";
    case ActionStatus::NoPawn: return "no pawn";
    case ActionStatus::NoWeapon: return "no weapon";
    case ActionStatus::CannotAct: return "cannot act";
    case ActionStatus::NotInArea: return "target not in area";
    case ActionStatus::NeedsSecondClick: return "needs a second click";
    case ActionStatus::NoEffect: return "no effect";
    case ActionStatus::NoUses: return "no uses left";
  }
  return "?";
}

namespace {

// SERIOUSLY_JUST_ONE: Game:ModifyPowerGrid's "exactly +1".
constexpr int kSeriouslyJustOne = 3626;

bool contains(const std::vector<Point>& area, Point p) {
  return std::find(area.begin(), area.end(), p) != area.end();
}

void note_error(ActionResult& out, const LuaCall& call) {
  if (!call.ok) out.lua_errors.push_back(call.error);
}

const bool* arg_bool(const LuaWrite& w, size_t i) {
  return i < w.args.size() ? std::get_if<bool>(&w.args[i]) : nullptr;
}
const double* arg_num(const LuaWrite& w, size_t i) {
  return i < w.args.size() ? std::get_if<double>(&w.args[i]) : nullptr;
}
const Point* arg_point(const LuaWrite& w, size_t i) {
  return i < w.args.size() ? std::get_if<Point>(&w.args[i]) : nullptr;
}
const std::string* arg_str(const LuaWrite& w, size_t i) {
  return i < w.args.size() ? std::get_if<std::string>(&w.args[i]) : nullptr;
}

// Board writes. Returns false for writes the engine does not model.
bool apply_board_write(Resolver& r, RulesContext& rules, const LuaWrite& w) {
  Board& b = r.board();
  const std::string& m = w.method;
  if (m == "AddEffect" && w.effect) {
    r.add_effect(to_engine(*w.effect));
    return true;
  }
  if (m == "DamageSpace") {
    if (w.effect && !w.effect->effect.empty()) {
      r.damage_space(to_engine(w.effect->effect.front()));
      return true;
    }
    const Point* p = arg_point(w, 0);
    const double* dmg = arg_num(w, 1);
    if (p && dmg) {
      r.damage_space(space_damage(*p, static_cast<int>(*dmg)));
      return true;
    }
    return false;
  }
  const Point* p = arg_point(w, 0);
  // Tile setters: off-board writes go to the game's dummy tile (no effect).
  if (m == "SetTerrain" || m == "SetLava" || m == "SetSmoke" || m == "SetFrozen" || m == "SetAcid" ||
      m == "SetCracked" || m == "AddShield" || m == "RemoveShield" || m == "SetItem") {
    if (!p) return false;
    if (!p->valid()) return true;
    const bool* on = arg_bool(w, 1);
    if (m == "SetTerrain") {
      const double* t = arg_num(w, 1);
      if (!t) return false;
      set_terrain(b, *p, static_cast<int>(*t), rules);
    } else if (m == "SetLava") {
      if (!on) return false;
      if (*on) {
        set_terrain(b, *p, Terrain::Lava, rules);
      } else {
        b.tile(*p).lava = false;
      }
    } else if (m == "SetSmoke") {
      if (!on) return false;
      set_tile_smoke(b, *p, *on, rules);
    } else if (m == "SetFrozen") {
      if (!on) return false;
      set_tile_frozen(b, *p, *on, rules);
    } else if (m == "SetAcid") {
      if (!on) return false;
      set_tile_acid(b, *p, *on, rules);
    } else if (m == "SetCracked") {
      if (!on) return false;
      set_cracked(b, *p, *on, rules);
    } else if (m == "AddShield" || m == "RemoveShield") {
      set_tile_shield(b, *p, m == "AddShield", rules);
    } else {  // SetItem
      const std::string* name = arg_str(w, 1);
      if (!name) return false;
      b.tile(*p).item = name->empty() ? kNoSymbol : intern(*name);
    }
    return true;
  }
  if (m == "SetCustomTile") {
    // The tile's sprite (Mission_Terraform's grass: the Terraformer clears it).
    const std::string* name = arg_str(w, 1);
    if (!p || !name) return false;
    if (p->valid()) b.tile(*p).custom_tile = name->empty() ? kNoSymbol : intern(*name);
    return true;
  }
  if (m == "SetPopulated") {
    const bool* on = arg_bool(w, 0);
    const Point* q = arg_point(w, 1);
    if (!on || !q) return false;
    if (q->valid()) b.tile(*q).populated = *on;
    return true;
  }
  if (m == "RemovePawn") {
    // No death, no death effect, no corpse.
    if (p) {
      if (!p->valid()) return true;
      for (Pawn* pawn : occupants(b, *p)) {
        if (counts_as_pawn(b, *pawn)) b.remove_pawn(pawn->uid);
      }
      return true;
    }
    if (const double* uid = arg_num(w, 0)) {
      b.remove_pawn(static_cast<int32_t>(*uid));
      return true;
    }
    return false;
  }
  return false;
}

// Pawn writes (BoardPawn mutators, stage 6 spec §3P.2).
bool apply_pawn_write(Board& b, RulesContext& rules, const LuaWrite& w) {
  Pawn* pawn = b.find_pawn(w.pawn);
  if (!pawn) return true;  // the pawn left the board in the meantime
  const std::string& m = w.method;
  const bool* on = arg_bool(w, 0);
  const double* n = arg_num(w, 0);
  if (m == "SetShield" && on) {
    set_pawn_shield(b, *pawn, *on);
  } else if (m == "SetFrozen" && on) {
    set_pawn_frozen(b, *pawn, *on);
  } else if (m == "SetAcid" && on) {
    set_pawn_acid(b, *pawn, *on);
  } else if (m == "SetBoosted" && on) {
    pawn->boosted = *on;
  } else if (m == "SetInfected" && on) {
    pawn->infected = *on;
  } else if (m == "SetPowered" && on) {
    pawn->movement.powered = *on;
  } else if (m == "SetActive" && on) {
    if (pawn->movement.powered) pawn->active = *on;
  } else if (m == "SetNeutral" && on) {
    pawn->neutral = *on;
  } else if (m == "SetMissionCritical" && on) {
    pawn->mission_critical = *on;
  } else if (m == "SetTeam" && n) {
    pawn->team = static_cast<Team>(static_cast<int>(*n));
  } else if (m == "SetMoveSpeed" && n) {
    pawn->move = static_cast<int8_t>(*n);
  } else if (m == "AddMoveBonus" && n) {
    pawn->movement.kickoff_bonus = static_cast<int8_t>(*n);  // set, not added
  } else if (m == "SetHealth" && n) {
    // HP := min(n, max); no death processing here.
    pawn->hp = static_cast<int8_t>(std::min<int>(static_cast<int>(*n), pawn->max_hp));
  } else if (m == "Kill") {
    kill_pawn(b, *pawn, rules);
  } else if (m == "FlyAway") {
    // Pawn::FlyAway: flies off and leaves the board (MapRemoval), no death.
    b.remove_pawn(pawn->uid);
  } else if (m == "ClearQueued") {
    pawn->queued.weapon = -1;
    pawn->queued.target = kInvalidPoint;
  } else if (m == "SetSpace") {
    const Point* p = arg_point(w, 0);
    if (!p) return false;
    set_space(b, *pawn, *p, false, &rules);
  } else {
    return false;
  }
  return true;
}

bool apply_game_write(Board& b, const LuaWrite& w, int difficulty) {
  if (w.method != "ModifyPowerGrid") return false;
  const double* n = arg_num(w, 0);
  if (!n) return false;
  int delta = static_cast<int>(*n);
  if (delta == kSeriouslyJustOne) {
    delta = 1;
  } else if (delta > 0 && difficulty == 3) {
    delta *= 2;  // DIFF_UNFAIR doubles grid gains
  }
  // PowerBar::Modify: no resist roll; clamped to [0, max]. The Overpower
  // Grid Defense bonus for gains past the maximum is not modelled.
  b.grid_power = std::clamp(b.grid_power + delta, 0, b.grid_power_max);
  return true;
}

// The environment's window on a player action (Engine::resolve with
// ActionOptions::mission): the per-frame hook sees the board at the frame it
// runs, appends effects to the action's stack, and its hidden choices are
// chance nodes of the action.
class ActionEnvHost final : public EnvHost {
 public:
  ActionEnvHost(Board& board, RulesContext& rules, const GameData& data, const ActionOptions& opts,
                ActionResult& out)
      : board_(board), rules_(rules), data_(data), opts_(opts), out_(out) {}

  void attach(Resolver& r) { r_ = &r; }

  Board& board() override { return r_ ? r_->board() : board_; }
  const GameData& data() const override { return data_; }
  RulesContext& rules() override { return rules_; }
  void add_effect(SkillEffect effect) override {
    if (r_) r_->add_effect(std::move(effect));
  }
  int choose(ChanceKind kind, int options, Point where, int amount) override {
    if (options <= 1) return 0;
    ChanceRecord node{kind, r_ ? r_->frame() : 0, where, amount, 0, options};
    const int pick = opts_.choose ? opts_.choose(node) : 0;
    node.outcome = std::clamp(pick, 0, options - 1);
    if (r_) r_->add_chance(node);
    return node.outcome;
  }
  void note(PhaseEventType type, std::string detail, Point where, int32_t uid, int amount) override {
    out_.mission_events.push_back(PhaseEvent{type, where, uid, amount, std::move(detail)});
  }
  int32_t new_uid() override { return board().next_uid++; }
  bool busy() const override { return r_ && r_->busy(); }
  int turn() const override { return board_.turn; }

 private:
  Board& board_;
  Resolver* r_ = nullptr;
  RulesContext& rules_;
  const GameData& data_;
  const ActionOptions& opts_;
  ActionResult& out_;
};

}  // namespace

ActionOptions action_options(const TurnContext& ctx) {
  ActionOptions o;
  o.grid_resist = ctx.grid_resist;
  o.death_seed = ctx.death_seed;
  o.spider_egg = ctx.spider_egg;
  o.choose = ctx.choose;
  o.mission = &ctx.mission;
  o.events = ctx.events;
  o.log = ctx.log;
  return o;
}

Engine::Engine() = default;
Engine::~Engine() = default;

std::unique_ptr<Engine> Engine::create(const std::filesystem::path& game_root,
                                       const EngineOptions& options) {
  std::unique_ptr<Engine> e(new Engine());
  e->impl_ = std::make_unique<Impl>();
  e->impl_->options = options;
  e->impl_->data = GameData::load(game_root);
  e->impl_->host = LuaHost::create(game_root, options.lua);
  return e;
}

const GameData& Engine::data() const { return impl_->data; }
LuaHost& Engine::lua() { return *impl_->host; }
const EngineOptions& Engine::options() const { return impl_->options; }

void Engine::apply_writes(Resolver& r, RulesContext& rules, const std::vector<LuaWrite>& writes,
                          std::vector<LuaWrite>& unapplied) {
  for (const LuaWrite& w : writes) {
    bool done = false;
    if (w.object == "Board") {
      done = apply_board_write(r, rules, w);
    } else if (w.object == "Pawn") {
      done = apply_pawn_write(r.board(), rules, w);
    } else if (w.object == "Game") {
      done = apply_game_write(r.board(), w, impl_->options.lua.difficulty);
    }
    if (!done) unapplied.push_back(w);
  }
}

uint32_t Engine::pilot_ability(std::string_view pilot_id) {
  static const std::pair<const char*, PilotAbility> kNames[] = {
      {"Armored", kPilotArmored},          {"Thick", kPilotThick},
      {"Rock_Skill", kPilotRockSkill},     {"Retaliation", kPilotRetaliation},
      {"Flying", kPilotFlying},            {"Disable_Immunity", kPilotDisableImmunity},
      {"Freeze_Walk", kPilotFreezeWalk},   {"Pain_Immunity", kPilotPainImmunity},
      {"Road_Runner", kPilotRoadRunner},   {"Shifty", kPilotShifty},
      {"Post_Move", kPilotPostMove},       {"Double_Shot", kPilotDoubleShot},
      {"Youth_Move", kPilotYouthMove},     {"Arrogant_Boost", kPilotArrogantBoost},
      {"Regen", kPilotRegen},              {"Zoltan_Skill", kPilotZoltan},
      {"Extra_XP", kPilotExtraXp},         {"KO_Boost", kPilotKoBoost},
  };
  const std::optional<std::string> skill = impl_->host->lua_string(pilot_id, "Skill");
  if (!skill) return kPilotNone;
  for (const auto& [name, bit] : kNames) {
    if (*skill == name) return bit;
  }
  return kPilotNone;
}

std::string Engine::repair_skill(std::string_view pilot_id) {
  const std::optional<std::string> skill = impl_->host->lua_string(pilot_id, "Skill");
  if (skill == "Power_Repair") return "Skill_Repair_Power";
  if (skill == "Mantis_Skill") return "Skill_Repair_Punch";
  return "Skill_Repair";
}

void Engine::Impl::wire(Engine& engine, ResolveContext& ctx, const ActionOptions& opts,
                        ActionResult& diag) {
  ctx.data = &data;
  ctx.config = options.config;
  ctx.durations = options.durations;
  ctx.timelines = &timelines;
  ctx.rules.data = &data;
  ctx.rules.grid_resist = opts.grid_resist;
  ctx.rules.events = opts.events;
  ctx.log = opts.log;
  ctx.spider_egg = opts.spider_egg;
  ctx.choose = opts.choose;
  LuaHost* lua = host.get();
  RulesContext* rules = &ctx.rules;
  ActionResult* out = &diag;
  Engine* e = &engine;
  // Nested Lua reads the board under resolution (Resolver::board), i.e. the
  // state at the frame the script or death effect runs.
  ctx.run_script = [this, lua, rules, out, e](Resolver& r, const std::string& script, Point) {
    const Pawn* sel = r.board().find_pawn(selected);
    const LuaCall call = lua->run_script(r.board(), script, sel);
    note_error(*out, call);
    e->apply_writes(r, *rules, call.writes, out->unapplied);
  };
  auto death_seed = opts.death_seed;
  ctx.death_effect = [this, lua, rules, out, e, death_seed](Resolver& r, const Pawn& pawn, Point tile) {
    const uint32_t seed = death_seed ? death_seed(pawn) : static_cast<uint32_t>(pawn.uid + 1);
    const Pawn* sel = r.board().find_pawn(selected);
    const uint64_t before = lua->rand_draws();
    LuaCall call;
    SkillEffect fx = lua->death_effect(r.board(), pawn, seed, sel, &call);
    const uint64_t drawn = lua->rand_draws() - before;
    if (drawn > 0) {
      r.add_chance(ChanceRecord{ChanceKind::LuaRandom, r.frame(), tile, static_cast<int>(seed),
                                static_cast<int>(drawn), 0});
    }
    note_error(*out, call);
    e->apply_writes(r, *rules, call.writes, out->unapplied);
    return fx;
  };
}

ResolveResult Engine::resolve(Board& board, const SkillEffect& effect, const WeaponInfo& weapon,
                              const ActionOptions& opts, ActionResult* out) {
  ActionResult local;
  ActionResult& diag = out ? *out : local;
  ResolveContext ctx;
  impl_->wire(*this, ctx, opts, diag);
  // The mission's per-frame hook (Mission:BaseUpdate), as in the enemy
  // phase; missions without one take no hook at all.
  if (!opts.mission || !native_environment_has_update(*opts.mission)) {
    return resolve_effect(board, effect, weapon, ctx);
  }
  std::unique_ptr<Environment> env = make_native_environment(*opts.mission);
  env->bind(board);
  ActionEnvHost host(board, ctx.rules, impl_->data, opts, diag);
  ctx.frame_hook = [&env, &host](Resolver& r) {
    host.attach(r);
    env->update(host);
  };
  return resolve_effect(board, effect, weapon, ctx);
}

// Skill::ComputeAffectedPoints, then FireInstant and the Pawn::FireWeapon
// bookkeeping.
static ActionResult fire_skill(Engine& engine, Engine::Impl& im, Board& board, int32_t uid,
                               int slot, const std::string& weapon, Point target,
                               std::optional<Point> target2, bool move_skill,
                               const ActionOptions& opts) {
  ActionResult out;
  out.weapon = weapon;
  LuaHost& host = *im.host;
  Pawn* pawn = board.find_pawn(uid);
  if (!pawn) {
    out.status = ActionStatus::NoPawn;
    return out;
  }
  out.end = pawn->pos;
  if (!host.lua_string(weapon, "Name")) {  // every Skill table inherits one
    out.status = ActionStatus::NoWeapon;
    return out;
  }
  if (opts.check_legal) {
    const bool can = move_skill ? can_move(*pawn)
                                : pawn->alive() && pawn->active && pawn->movement.powered && !pawn->frozen;
    if (!can) {
      out.status = ActionStatus::CannotAct;
      return out;
    }
    if (!move_skill && slot >= 0 && slot < kMaxWeapons && pawn->uses[static_cast<size_t>(slot)] == 0) {
      out.status = ActionStatus::NoUses;
      return out;
    }
  }
  im.selected = uid;  // the shooter is selected (Lua `Pawn`)
  const Point origin = pawn->pos;
  std::vector<Point> computed;
  const std::vector<Point>* area = opts.known_area;
  if (area) {
    host.select(pawn);  // as the target-area call would
  } else {
    LuaCall call;
    computed = host.target_area(board, *pawn, weapon, origin, &call);
    note_error(out, call);
    area = &computed;
  }
  if (opts.check_legal && !contains(*area, target)) {
    out.status = ActionStatus::NotInArea;
    return out;
  }

  LuaSkillEffect raw;
  Point final_target = target;
  bool two_click = false;
  if (!move_skill) {
    LuaCall c;
    two_click = host.is_two_click(weapon, &c) && !host.two_click_exception(board, *pawn, weapon, origin, target, &c);
    note_error(out, c);
  }
  if (two_click) {
    if (!target2) {
      out.status = ActionStatus::NeedsSecondClick;
      return out;
    }
    LuaCall c;
    const Point first = host.translate_first_click(board, *pawn, weapon, origin, target, &c);
    const std::vector<Point> area2 = host.second_target_area(board, *pawn, weapon, origin, first, &c);
    note_error(out, c);
    if (opts.check_legal && !contains(area2, *target2)) {
      out.status = ActionStatus::NotInArea;
      return out;
    }
    final_target = *target2;
    raw = host.final_effect_raw(board, *pawn, weapon, origin, first, *target2, &c);
    note_error(out, c);
  } else {
    LuaCall c;
    raw = host.skill_effect_raw(board, *pawn, weapon, origin, target, &c);
    note_error(out, c);
  }

  // PrepareEffect, owner, CheckAlterations on both lists.
  SkillEffect fx = to_engine(raw);
  const std::optional<std::string> explosion = host.lua_string(weapon, "Explosion");
  const Symbol explosion_sym = explosion && !explosion->empty() ? intern(*explosion) : kNoSymbol;
  prepare_effect(fx, origin, final_target, pawn->team, explosion_sym);
  fx.owner = uid;
  check_alterations(board, fx.effect, pawn, move_skill);
  check_alterations(board, fx.q_effect, pawn, move_skill);

  if (fx.effect.empty() && fx.q_effect.empty()) {
    out.status = ActionStatus::NoEffect;
    out.effect = std::move(fx);
    return out;
  }
  // A skill with a queued part telegraphs it (SetQueuedShot).
  if (!fx.q_effect.empty() && slot >= 0) {
    pawn->queued = QueuedShot{static_cast<int8_t>(slot), origin, final_target};
  }
  if (move_skill && pawn->team == Team::Player) pawn->movement.undo_ready = true;

  WeaponInfo info;
  info.name = intern(weapon);
  info.explosion = explosion_sym;
  info.move_skill = move_skill;
  info.prepared = true;
  out.effect = fx;
  if (!fx.effect.empty()) out.resolve = engine.resolve(board, fx, info, opts, &out);
  if (Pawn* p = board.find_pawn(uid)) {
    // The executor spent the Boost with the first chunk (Pawn::FireWeapon);
    // one the pawn holds now came from the resolution itself (a kill with
    // KO_Boost, an Opener / Closer level-up) and stays.
    const bool boost_gained = !fx.effect.empty() && !move_skill && p->boosted;
    on_skill_fired(board, *p, move_skill);
    if (boost_gained) p->boosted = true;
    // Skill::FireInstant uses a limited charge.
    if (!move_skill && slot >= 0 && slot < kMaxWeapons && p->uses[static_cast<size_t>(slot)] > 0) {
      --p->uses[static_cast<size_t>(slot)];
    }
    out.end = p->pos;
  } else {
    out.end = kInvalidPoint;
  }
  return out;
}

ActionResult Engine::move(Board& board, int32_t uid, Point dest, const ActionOptions& opts) {
  return fire_skill(*this, *impl_, board, uid, -1, "Move", dest, std::nullopt, true, opts);
}

ActionResult Engine::fire_weapon(Board& board, int32_t uid, int slot, Point target,
                                 std::optional<Point> target2, const ActionOptions& opts) {
  const Pawn* pawn = board.find_pawn(uid);
  if (!pawn) {
    ActionResult out;
    out.status = ActionStatus::NoPawn;
    return out;
  }
  if (slot < 0 || slot >= kMaxWeapons || pawn->weapons[static_cast<size_t>(slot)] == kNoSymbol) {
    ActionResult out;
    out.status = ActionStatus::NoWeapon;
    out.end = pawn->pos;
    return out;
  }
  const std::string weapon(symbol_name(pawn->weapons[static_cast<size_t>(slot)]));
  return fire_skill(*this, *impl_, board, uid, slot, weapon, target, target2, false, opts);
}

ActionResult Engine::fire_weapon(Board& board, int32_t uid, std::string_view weapon, Point target,
                                 std::optional<Point> target2, const ActionOptions& opts) {
  int slot = -1;
  if (const Pawn* pawn = board.find_pawn(uid)) {
    for (int i = 0; i < kMaxWeapons; ++i) {
      if (pawn->weapons[static_cast<size_t>(i)] != kNoSymbol &&
          symbol_name(pawn->weapons[static_cast<size_t>(i)]) == weapon) {
        slot = i;
        break;
      }
    }
  }
  return fire_skill(*this, *impl_, board, uid, slot, std::string(weapon), target, target2, false,
                    opts);
}

ActionResult Engine::repair(Board& board, int32_t uid, Point target, std::string_view skill,
                            const ActionOptions& opts) {
  if (!target.valid()) {
    if (const Pawn* pawn = board.find_pawn(uid)) target = pawn->pos;
  }
  return fire_skill(*this, *impl_, board, uid, -1, std::string(skill), target, std::nullopt, false,
                    opts);
}

ActionResult Engine::Impl::queued_effect(Board& board, int32_t uid, WeaponInfo& info) {
  ActionResult out;
  Pawn* pawn = board.find_pawn(uid);
  if (!pawn) {
    out.status = ActionStatus::NoPawn;
    return out;
  }
  out.end = pawn->pos;
  const QueuedShot shot = pawn->queued;
  if (!shot.active() || shot.weapon >= kMaxWeapons ||
      pawn->weapons[static_cast<size_t>(shot.weapon)] == kNoSymbol) {
    // Nothing can fire it; the shot is used up all the same (else the enemy
    // phase would pick this shooter again and again).
    pawn->queued = QueuedShot{};
    out.status = ActionStatus::NoWeapon;
    return out;
  }
  out.weapon = std::string(symbol_name(pawn->weapons[static_cast<size_t>(shot.weapon)]));
  // SkillManager::FireQueued: the stored shot is used up, the shooter is
  // selected, then the effect is recomputed from its current tile.
  pawn->queued = QueuedShot{};
  selected = uid;
  LuaCall call;
  const LuaSkillEffect raw = host->queued_effect_raw(board, *pawn, out.weapon, shot.target, &call);
  note_error(out, call);
  SkillEffect fx = to_engine(raw);
  const std::optional<std::string> explosion = host->lua_string(out.weapon, "Explosion");
  const Symbol explosion_sym = explosion && !explosion->empty() ? intern(*explosion) : kNoSymbol;
  prepare_effect(fx, pawn->pos, shot.target, pawn->team, explosion_sym);
  fx.owner = uid;
  check_alterations(board, fx.effect, pawn, false);
  out.effect = fx;
  if (fx.effect.empty()) {
    out.status = ActionStatus::NoEffect;  // fizzled: target outside the area
    return out;
  }
  info = WeaponInfo{};
  info.name = intern(out.weapon);
  info.explosion = explosion_sym;
  info.prepared = true;
  return out;
}

ActionResult Engine::fire_queued(Board& board, int32_t uid, const ActionOptions& opts) {
  WeaponInfo info;
  ActionResult out = impl_->queued_effect(board, uid, info);
  if (!out.ok()) return out;
  out.resolve = resolve(board, out.effect, info, opts, &out);
  const Pawn* p = board.find_pawn(uid);
  out.end = p ? p->pos : kInvalidPoint;
  return out;
}

}  // namespace itb
