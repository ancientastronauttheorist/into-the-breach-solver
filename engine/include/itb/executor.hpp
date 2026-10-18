// Stages 3 and 4: push and death resolution, and the SkillEffect executor.
//
// The game resolves an effect in real time: entries are applied in chunks
// separated by delays, projectiles fly, pushes take 0.4 s, explosions and
// death animations keep the board busy, death effects and corpse explosions
// queue up behind. This executor replays that as an exact frame-clock
// simulation: an integer frame counter with a fixed frame rate (default 60
// fps), the game's six per-frame phases in order (weapon animations, tiles,
// pawns, stacked effects, quiescent work, render), and every timer converted
// to the frame on which the game's own float32 accumulation crosses its
// threshold (timing.hpp). Frames in which nothing can change are skipped, so
// the cost follows the number of events, not the elapsed time.
//
// At a fixed frame rate the result is deterministic. The few outcomes that
// depend on the frame rate (two interacting events within one frame of each
// other) are reported as TimingFlags so a solver can treat them as uncertain.
//
// Lua is not part of this stage. The weapon's GetSkillEffect result comes in
// as a SkillEffect; Pawn:GetDeathEffect, sScript and the spider-egg choice go
// through the hooks in ResolveContext.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "itb/board.hpp"
#include "itb/space_damage.hpp"
#include "itb/tile_rules.hpp"
#include "itb/timing.hpp"

namespace itb {

class GameData;
class Resolver;

// What the executor needs to know about the skill that produced an effect.
struct WeaponInfo {
  Symbol name = kNoSymbol;       // the shot's name (last-shot record, train shots)
  Symbol explosion = kNoSymbol;  // Lua Explosion: fills entries with no sAnimation
  bool move_skill = false;       // the Move skill: never boosted, consumes nothing
  // The effect already went through prepare_effect and check_alterations
  // (Stage 6 computes effects at targeting time). Otherwise the executor does
  // both when it fires, which is what the game does for queued attacks.
  bool prepared = false;
  // Skill::FireQueued: apply the queued list (q_effect) instead of the instant one.
  bool queued = false;
};

// ---- Skill-side preparation (Skill::PrepareEffect, Skill::CheckAlterations) --

// Stamps origin, target and team; fills empty sAnimations with `explosion`,
// unset projectile sources with `origin` and every entry's owner team.
void prepare_effect(SkillEffect& effect, Point origin, Point target, Team team, Symbol explosion);
// Pawn::IsBoosted: Boosted status, the Boost psion, or an Arrogant pilot at
// full health.
bool is_boosted(const Board& board, const Pawn& pawn);
// Vek Hormones then Boost, baked into the entries from the board as it is now.
void check_alterations(const Board& board, std::vector<SpaceDamage>& list, const Pawn* owner,
                       bool move_skill);

// ---- Results ---------------------------------------------------------------

enum class TimingKind : uint8_t {
  DyingBlocker,    // a push ending within a frame of a dying body's 50% mark
  DelayVsPush,     // a delayed chunk within a frame of a push it interacts with
  ImpactVsPush,    // a projectile impact within a frame of a push it interacts with
  GateVsAnimation, // FULL_DELAY released within a frame of both a push and an animation ending
};

struct TimingFlag {
  TimingKind kind;
  int64_t frame = 0;
  Point point = kInvalidPoint;
  int32_t uid = -1;
};

enum class ResolveEventType : uint8_t {
  ChunkApplied,      // a stack entry (or the fired effect) was applied
  Impact,            // a projectile / artillery / dropper hit `point`
  PushStarted,       // pawn `uid` starts a push (amount = direction)
  PushMoved,         // pawn `uid` moved to `point`
  PushBlocked,       // pawn `uid` bumped into `point`
  LungeHit,          // melee entry of pawn `uid` applied
  DeathProcessed,    // pawn `uid` died (ProcessDeath)
  DeathEffect,       // pawn `uid`'s death effect was stacked
  CorpseExploded,    // pawn `uid`'s explosion was stacked
  PawnRemoved,       // pawn `uid` left the board
  TimingSensitive,   // see ResolveResult::timing
  PilotLevelUp,      // pawn `uid`'s pilot reached level `amount` (itb/pilot_xp.hpp)
  PawnUnderground,   // burrower `uid` finished its dive from `point` (off the board)
  VoidShock,         // Void Shocker took `amount` HP from pawn `uid` (its shot hit nothing)
};

struct ResolveEvent {
  ResolveEventType type;
  int64_t frame = 0;
  Point point = kInvalidPoint;
  int32_t uid = -1;
  int amount = 0;
};

// A random branch taken during resolution.
enum class ChanceKind : uint8_t {
  GridDefense,  // a populated building rolled to resist (outcome 1 = resisted)
  SpiderEgg,    // a spider psion egg picked a tile among `options`
  // A Lua hook (death effect, script) drew random numbers: its result is
  // one sample. `amount` = the seed used, `outcome` = numbers drawn.
  LuaRandom,
  // Stage 7. The order of environment strikes the game draws at random
  // (Lightning) or that the recording does not pin down (Seismic path
  // direction): `outcome` indexes the permutations of the strikes that can
  // interact, `options` = how many (1 = order irrelevant).
  EnvOrder,
  // A hidden environment parameter the recording lacks (e.g. the wind
  // direction of old recordings): `outcome` of `options`.
  EnvChoice,
  // A mission hook's random pick (Mission_Reactivation thaw, Final Cave
  // bomb drop): `outcome` of `options`.
  MissionRandom,
  // BoardPlayer::UpdateXP: which mechs get the +1s of an XP split's
  // remainder, only when that decides a level-up (itb/pilot_xp.hpp).
  // `amount` = the XP split, `point` = a mech that may level up; outcome 0 =
  // the fewest level-ups.
  XpSplit,
};

struct ChanceRecord {
  ChanceKind kind;
  int64_t frame = 0;
  Point point = kInvalidPoint;
  int amount = 0;   // grid power at stake
  int outcome = 0;  // resisted / chosen index
  int options = 0;
};

struct ResolveResult {
  int64_t start_frame = 0;
  int64_t end_frame = 0;
  bool quiescent = true;  // false: gave up after ResolveConfig::max_frames
  std::vector<TimingFlag> timing;
  std::vector<ChanceRecord> chances;

  bool timing_sensitive() const { return !timing.empty(); }
};

// ---- Context -----------------------------------------------------------------

// Animation timelines (tile and death animations) for one game data, frame
// clock and set of durations, built by the executor on first use. A cache
// that outlives one resolution (ResolveContext::timelines) saves rebuilding
// them every time; the executor only uses it when all three match. One user
// at a time (not thread-safe).
struct TimelineCache {
  bool ready = false;
  const GameData* data = nullptr;
  FrameClock clock;
  Durations durations;
  std::map<Symbol, AnimTimeline> anims;   // by animation name
  std::map<Symbol, AnimTimeline> deaths;  // by pawn type
  AnimTimeline default_death;
};

struct ResolveConfig {
  double fps = 60.0;     // steady frame rate the game runs at
  int speed_level = 0;   // CFPS speed level
  int64_t max_frames = 60 * 120;  // per resolve call
};

struct ResolveContext {
  // Pawn definitions (spawns, death animations) and animation lengths.
  // Without it every death animation is the default one and sAnimations
  // never keep a tile busy.
  const GameData* data = nullptr;
  // Stage 2 context: Grid Defense rolls, the event log, next uid. Its `frame`
  // and `run_script` are managed by the executor.
  RulesContext rules;
  ResolveConfig config;
  // Durations; defaults (with Values from `data`) when left unset.
  std::optional<Durations> durations;

  // Lua Pawn:GetDeathEffect(tile) for `pawn` (BlobBoss, AcidVat, Burnbug...).
  // Return the raw entries; the executor prepares and stacks them. Called
  // with the pawn's RNG reseeded natively; the hook owns any randomness.
  std::function<SkillEffect(Resolver&, const Pawn& pawn, Point tile)> death_effect;
  // sScript entries (Lua, run before the entry's hit). The script can call
  // back into Resolver::add_effect (Board:AddEffect) and read the board.
  std::function<void(Resolver&, const std::string& script, Point loc)> run_script;
  // A spider psion egg landing: pick an index into `tiles` (free neighbours).
  // Default: 0.
  std::function<int(Resolver&, const Pawn& pawn, const std::vector<Point>& tiles)> spider_egg;
  // Other hidden choices of the resolution (ChanceKind::XpSplit): an
  // outcome index in [0, node.options). Default: 0.
  std::function<int(const ChanceRecord& node)> choose;

  // Called at the end of every simulated frame (after the quiescent work):
  // the mission's per-frame Lua (Mission:BaseUpdate / UpdateMission), which
  // may edit the board and append effects. Frames skipped because nothing
  // could change are not reported.
  std::function<void(Resolver&)> frame_hook;

  // Optional log of what happened, frame by frame.
  std::vector<ResolveEvent>* log = nullptr;

  // Optional timeline cache shared by resolutions (see TimelineCache).
  TimelineCache* timelines = nullptr;
};

// ---- The executor ---------------------------------------------------------------

namespace detail {
class Simulation;
}

// Resolves effects on a board, keeping the frame clock and the game's
// transient state (stacked effects, flights, trackers, death bookkeeping)
// between calls. Board stays plain data: everything transient lives here.
class Resolver {
 public:
  Resolver(Board& board, ResolveContext& ctx);
  ~Resolver();
  Resolver(const Resolver&) = delete;
  Resolver& operator=(const Resolver&) = delete;

  // Fires `effect` (Board::ApplyEffect from player input or the enemy
  // driver) and runs frames until the board is idle again: no projectile,
  // tile animation, busy pawn, stacked effect, pending death or body left.
  ResolveResult resolve(const SkillEffect& effect, const WeaponInfo& weapon);
  // Runs frames until idle without firing anything (e.g. after edits).
  ResolveResult settle();

  Board& board();
  int64_t frame() const;
  const FrameClock& clock() const;

  // For scripts: Board:AddEffect (append, FULL_DELAY) and Board:AddDelay.
  void add_effect(SkillEffect effect);
  void add_delay(float seconds);
  // Board:DamageSpace(SD): applied at once, as if it were an entry of the
  // effect being applied (pushes start from it, deaths are noted).
  void damage_space(const SpaceDamage& sd);
  // Board:Bounce(p, amount) (the script of SkillEffect:AddBounce): the tile
  // bounces for 0.5 s, and a bouncing tile is busy (code 2), so FULL_DELAY
  // chunks, death effects and the quiescent-frame work (kill credit, corpse
  // detonation, the end of the last shot) wait for it.
  void bounce(Point p, int amount);
  // Records a chance node of a hook into the result of the call in progress.
  void add_chance(const ChanceRecord& chance);

  // Runs `edit` on the board as part of a frame (the enemy driver's own
  // board changes: status ticks, blocked spawns, mission hooks): deaths it
  // causes are noted and pushes it starts are run, then frames run until the
  // board is idle again. Grid Defense rolls inside `edit` are logged.
  ResolveResult apply(const std::function<void()>& edit);
  // Board::UpdateLeaders now (it also runs at every quiescent frame).
  void update_leaders();
  // Board::IsBusy(): something is still resolving.
  bool busy() const;

 private:
  std::unique_ptr<detail::Simulation> sim_;
};

// One effect on `board`, from an idle board to an idle board.
ResolveResult resolve_effect(Board& board, const SkillEffect& effect, const WeaponInfo& weapon,
                             ResolveContext& ctx);

struct FiredEffect {
  SkillEffect effect;
  WeaponInfo weapon;
};
// Several effects in turn, each fired once the previous one is idle, on one
// frame clock. The result spans all of them.
ResolveResult resolve_effects(Board& board, const std::vector<FiredEffect>& effects,
                              ResolveContext& ctx);

}  // namespace itb
