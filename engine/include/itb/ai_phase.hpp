// The enemy AI's planning (state 2 of the enemy phase): every enemy moves and
// queues its attack, one pawn at a time. Behaviour from AiController /
// AiPlanner (build 21601364) and the shipped Lua scoring (global.lua
// ScorePositioning, Skill:GetTargetScore, Skill:ScoreList).
//
// Engine::ai_phase(board) runs it on the board Engine::end_turn leaves (after
// the spawns), or on a freshly deployed board for the AI phase before the
// first player turn:
//
//   1. AiController::StartTurn: every enemy-team or neutral pawn is made
//      active (and gets Pawn::StartTurn's movement reset).
//   2. NextPawn: the first active pawn in board-list order plans, moves and
//      queues; it is then made inactive. A frozen pawn is skipped.
//   3. Planning (AiPlanner): the candidate tiles are the reachable tiles
//      (GetReachablePoints from the move origin, minus blocked tiles) plus the
//      pawn's own tile; a tile is scored if it is enterable for the pawn's
//      profile, or is its own tile. Scoring a tile (ScoreLocation) puts the
//      pawn there and takes:
//        - position score: Lua ScorePositioning (0 for a pawn that cannot
//          move); an injured 1-HP pawn moving elsewhere has it capped at 0;
//        - target score: the best of SkillManager::GetTargetScore over the
//          weapon's target area from that tile (none when the pawn could not
//          act there: smoke, water). GetTargetScore is the Lua
//          Skill:GetTargetScore, minus 5 for the target this pawn shot at
//          last (a positive Lua score stays at least 1).
//          Ties for the best positive score are broken at random.
//      Once a non-edge tile has a positive target score and a non-negative
//      position score, edge tiles are no longer scored.
//      The plan (SelectPlan) is the best by: a positive position score
//      beats a negative one, then target score, then position score; plans
//      with a position score below -10 (and edge tiles, as above) are out.
//      Ties are broken at random. With chance 1/4 the AI instead takes a plan
//      from the group that was best before the last improvement (drawn at
//      random among those off the edge with a positive target score and a
//      non-negative position score), when there is one.
//   4. The pawn moves there (Pawn::ManualMove) and fires its weapon at the
//      target (SkillManager::FireWeapon): a queued weapon telegraphs its shot,
//      and the instant part of the effect (webs, eggs, smoke...) resolves now.
//
// Randomness: every draw comes from the C library's rand(), reseeded with
// the AiController seed before each pawn plans (the save's "aiSeed"); the
// next pawn's seed is unique_rand(seed) drawn after the planning. On macOS
// (the live game) rand() is the Park-Miller minimal standard generator, whose
// state is its last output, so the whole phase is one stream from the saved
// seed (checked on the live saves: the next turn's aiSeed is the draw right
// after the phase's last one). The live seed is hidden, so callers either
// replay a recorded seed (validation) or sample seeds (each one an outcome
// the game could produce). Lua code that draws during planning would share
// the stream; no shipped scoring script does, and the Lua host keeps its own
// (glibc) stream: AiPawnResult::lua_draws counts such draws.
//
// Not modelled: SetPlanPoints' extra Lua calls (GetTargetArea /
// GetSkillEffect before each score; no shipped enemy weapon draws random
// numbers there), SkillManager priority targets (tutorial only), the
// "IsFast" timing shortcut (timing only).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "itb/board.hpp"
#include "itb/engine.hpp"

namespace itb {

// One scored location (AiPlan): where to stand, what to hit and the scores.
struct AiPlan {
  Point loc = kInvalidPoint;
  Point target = kInvalidPoint;  // invalid: no attack
  int target_score = 0;
  int position_score = 0;

  bool operator==(const AiPlan&) const = default;
};

// AiPlan::operator>= : a positive position score beats a negative one,
// then target score, then position score (equal plans compare >=).
bool ai_plan_at_least(const AiPlan& a, const AiPlan& b);

// Where the AI phase's random draws come from. Kind names the native draw.
enum class AiDraw : uint8_t {
  TargetTie,  // ScoreLocation: one of the tied best targets
  PlanTie,    // SelectPlan: one of the tied best plans
  Deviate,    // SelectPlan: random_bool(4), 1 = take a previous-best plan
  DeviatePick // SelectPlan: random_removal from the previous-best group
};

struct AiPawnResult {
  int32_t uid = -1;
  Symbol type = kNoSymbol;
  Point from = kInvalidPoint;
  AiPlan plan;               // the chosen plan
  Point end = kInvalidPoint; // where the pawn stood after its move
  bool queued = false;       // it telegraphed a shot
  bool deviated = false;     // the 1-in-4 pick from the previous-best group
  int best_ties = 0;         // plans tied for best
  int previous_best = 0;     // plans in the previous-best group
  int lua_draws = 0;         // random numbers Lua drew while it planned
  std::vector<AiPlan> plans; // every scored location (AiPhaseOptions::keep_plans)
};

struct AiPhaseResult {
  std::vector<AiPawnResult> pawns;  // in planning order
  uint32_t seed = 0;                // the AiController seed after the phase
  std::vector<std::string> lua_errors;
  std::vector<LuaWrite> unapplied;
  // Every native draw: (kind, options, pick). Only draws with options > 1
  // decided anything.
  struct Draw {
    AiDraw kind;
    int options;
    int pick;
  };
  std::vector<Draw> draws;
};

// The libc rand() the game was built against. The live game is the macOS
// build (Park-Miller minimal standard rand); the decompiled Linux build uses
// glibc's (the Lua host's stream).
enum class AiRng : uint8_t { MacOS, Glibc };

struct AiPhaseOptions {
  // The AiController seed (save "aiSeed").
  uint32_t seed = 0;
  AiRng rng = AiRng::MacOS;
  // Optional override for the native draws: return a pick in [0, options).
  // Set, it replaces the rand() stream for those draws (Lua's own draws still
  // use the stream).
  std::function<int(AiDraw kind, int options)> draw;
  // The target each pawn fired at last (save "targetHistory",
  // SkillManager +0x70), by uid. Absent: none (a pawn that never fired).
  std::unordered_map<int32_t, Point> last_target;
  // Chance hooks, mission and logs for the moves and instant effects.
  ActionOptions actions;
  bool keep_plans = false;
};

}  // namespace itb
