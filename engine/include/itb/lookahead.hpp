// Next-turn lookahead: what the board a turn leaves costs on the next turn.
//
// The per-turn score (score.hpp) judges a turn by the board after its enemy
// phase. A board can look fine there and still force losses on the next
// turn: the Vek it leaves alive plan their next attacks (ai_phase.hpp) where
// the mechs cannot answer them all. The lookahead plays that planning out:
//
//   1. Start from the end of the turn's enemy phase (Engine::end_turn).
//      Spawns of a type the player cannot see emerged without a pawn; with
//      LookaheadOptions::sample_spawns each gets a pawn of an enemy type the
//      player can see on the turn's board (a sample of the hidden type).
//   2. The AI plans (Engine::ai_phase) with a sampled seed (the live seed is
//      hidden): every enemy moves and queues its attack. The next player turn
//      starts (as deploy.cpp does for turn 1).
//   3. Reply: how much the player loses on that next turn, under a fixed
//      reply policy (LookaheadOptions::reply):
//        - Idle: no action at all (an upper bound on what is forced);
//        - Greedy: unit by unit, the unit turn (move, then weapon or repair)
//          that lowers the loss most, until no unit turn helps (an
//          achievable reply, so again an upper bound on what is forced);
//        - Search: solve_turn with a node budget (plain tiers; about 3.6x
//          the greedy reply's cost, closer to a real solve).
//      The loss is the next turn's grid power lost, then mechs lost, then
//      building HP lost (score_turn's plain tiers), packed into one value.
//   4. The worst loss over the sampled seeds (chance is taken worst case, as
//      everywhere in the solver); LookaheadOptions::fold can instead take
//      their sum, or the worst with ties broken by the sum (WorstSum).
//
// The result is a function of the end board alone (and fixed options), so
// the solver can score it as one more tier (ScoreKey::NextTurn) and keep its
// memo tables and proofs: they hold with respect to this estimate.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "itb/board.hpp"
#include "itb/enemy_phase.hpp"
#include "itb/score.hpp"

namespace itb {

class Engine;
struct AiPhaseResult;

struct LookaheadOptions {
  enum class Reply : uint8_t { Idle, Greedy, Search };
  int samples = 8;            // AI seeds per end board (worst case over them)
  uint32_t seed = 7;          // base of the sampled AI seeds (deploy_sample_seed)
  bool sample_spawns = true;  // unknown spawns emerge as a visible enemy type
  Reply reply = Reply::Greedy;
  uint64_t search_nodes = 200;  // Reply::Search: solve_turn's node budget
  // Greedy: rounds of "best single unit turn" (at most one per unit).
  int greedy_rounds = 8;
  // SolveOptions::lookahead: this share of the time (or node) budget first
  // goes to a search without the lookahead, whose plan, scored with it,
  // seeds the incumbent. The lookahead search then only replaces it with a
  // better plan under the full order, so the tiers above NextTurn are never
  // worse than that search's (the lookahead slows the search down). 0: off.
  double base_share = 0.25;
  // ... extended (adaptive budget) up to this share of the whole budget while
  // the tiers above NextTurn (grid, buildings, mechs, stars) are unproven.
  double base_max_share = 0.75;
  // How the samples combine into the tier (lookahead_fold): Worst = the
  // worst sample (chance taken worst case), Sum = their sum (K times the
  // mean loss), WorstSum = the worst sample, ties broken by the sum.
  enum class Fold : uint8_t { Worst, Sum, WorstSum };
  Fold fold = Fold::Worst;
  // Plan the environment's next-turn hazards (next_turn_mission): the next
  // turn's enemy phase strikes the tiles the environment picks for it (its
  // random picks sampled from the sample's AI seed), and the greedy reply
  // also answers them (moves off, pushes onto them). Off: the next turn runs
  // with this turn's mission data (its hazards strike this turn's tiles
  // again).
  bool plan_env = false;
  // Repair skill per unit uid (as SolveOptions::repair_skills).
  std::vector<std::pair<int32_t, std::string>> repair_skills;
};

// The next turn's losses under the reply (plain tiers, net, >= 0).
struct NextTurnLoss {
  int grid = 0;
  int mechs = 0;
  int building_hp = 0;
  bool mission_over = false;  // no next turn (the mission ended)
};

// Packing into ScoreKey::NextTurn: grid first, then mechs, then building HP,
// each clamped (grid and mechs to 15, building HP to 63), negated.
inline constexpr int32_t kNextTurnMech = 64;
inline constexpr int32_t kNextTurnGrid = 16 * kNextTurnMech;
inline int32_t next_turn_value(const NextTurnLoss& l) {
  auto clamp = [](int v, int hi) { return v < 0 ? 0 : (v > hi ? hi : v); };
  return -(kNextTurnGrid * clamp(l.grid, 15) + kNextTurnMech * clamp(l.mechs, 15) + clamp(l.building_hp, 63));
}
// The lowest value next_turn_value can take.
inline constexpr int32_t kNextTurnFloor = -(kNextTurnGrid * 15 + kNextTurnMech * 15 + 63);
// Folding sample values into the tier (LookaheadOptions::fold). The fold of
// no sample is 0 (the tier's cap), and each sample can only lower it: the
// solver relies on both (NextTurn at its cap is an upper bound, and its
// samples run one at a time, cut as soon as the value so far is <= alpha).
// The fold is a function of the set of sample values (their order does not
// matter), so cached samples resume a value exactly.
//
// WorstSum packs (worst, sum) into one int32, lexicographically:
// -(kWorstSumScale * W + min(S, kWorstSumScale - 1)) with W = -worst and
// S = -sum (both >= 0). W <= -kNextTurnFloor = 16383 and the sum part is
// below kWorstSumScale = 2^17, so the value stays >= -(2^31 - 1) (no
// overflow) and a worse worst always ranks below any sum. With up to 8
// samples the sum part never saturates (8 x 16383 < 2^17 - 1): exact
// lexicographic order; with more samples a sum past 2^17 - 1 saturates
// (ties among very bad sums), still monotone and still worst first.
inline constexpr int32_t kWorstSumScale = 1 << 17;
inline constexpr int32_t worst_sum_pack(int32_t worst_loss, int32_t sum_loss) {
  return -(kWorstSumScale * worst_loss + (sum_loss < kWorstSumScale - 1 ? sum_loss : kWorstSumScale - 1));
}
inline int32_t lookahead_fold(int32_t acc, int32_t v, LookaheadOptions::Fold fold) {
  switch (fold) {
    case LookaheadOptions::Fold::Worst:
      return v < acc ? v : acc;
    case LookaheadOptions::Fold::Sum:
      return acc + v;
    case LookaheadOptions::Fold::WorstSum: {
      const int32_t q = -acc;  // >= 0: kWorstSumScale * W + S
      const int32_t w = q / kWorstSumScale, s = q % kWorstSumScale, l = -v;
      return worst_sum_pack(l > w ? l : w, s + l);
    }
  }
  return acc;
}
// The worst sample's next_turn_value inside a folded value (Worst and
// WorstSum; for Sum, the value itself).
inline int32_t lookahead_fold_worst(int32_t value, LookaheadOptions::Fold fold) {
  return fold == LookaheadOptions::Fold::WorstSum ? -((-value) / kWorstSumScale) : value;
}
// The fold's name as the tools spell it ("worst", "sum", "worst_sum"), and
// back (std::nullopt for anything else).
const char* lookahead_fold_name(LookaheadOptions::Fold fold);
std::optional<LookaheadOptions::Fold> lookahead_fold_of(const std::string& name);
// The reply's name ("idle", "greedy", "search"), and back.
const char* lookahead_reply_name(LookaheadOptions::Reply reply);
std::optional<LookaheadOptions::Reply> lookahead_reply_of(const std::string& name);

// Step 1-2: the board the player faces at the start of the next turn.
// `root` is the board at the start of this turn, `after` the board at the
// end of this turn's enemy phase and `phase` that phase's result.
// `ai`, if given, gets the AI phase's result.
Board next_turn_start(Engine& engine, const Board& root, const Board& after, const PhaseResult* phase,
                      const TurnContext& ctx, uint32_t ai_seed, const LookaheadOptions& options,
                      AiPhaseResult* ai = nullptr);

// The next turn's mission data: `ctx.mission` with, under
// LookaheadOptions::plan_env, the environment's next-turn hazards planned on
// `after` (environment.hpp plan_next_environment; random picks drawn from
// `ai_seed`). Without plan_env: `ctx.mission` unchanged.
MissionData next_turn_mission(const Board& root, const Board& after, const TurnContext& ctx, uint32_t ai_seed,
                              const LookaheadOptions& options);

// Step 3 on a next-turn start board. `plan`, if given, gets the reply's
// actions (Greedy, Search).
NextTurnLoss reply_loss(Engine& engine, const Board& start, const TurnContext& ctx,
                        const LookaheadOptions& options, std::vector<PlayerAction>* plan = nullptr);

// Steps 1-3 for sample k (its AI seed is lookahead_seed(options, k)).
// Without a next turn (the mission ended, or `root` is its last turn):
// mission_over and no loss.
NextTurnLoss lookahead_sample(Engine& engine, const Board& root, const Board& after, const PhaseResult* phase,
                              const TurnContext& ctx, const LookaheadOptions& options, int k);

// Steps 1-4: the worst next-turn loss over the sampled seeds. `per_sample`,
// if given, gets each sample's loss. The solver runs the samples one at a
// time instead (lookahead_sample), stopping as soon as the worst so far
// decides the comparison at hand.
NextTurnLoss lookahead_loss(Engine& engine, const Board& root, const Board& after, const PhaseResult* phase,
                            const TurnContext& ctx, const LookaheadOptions& options,
                            std::vector<NextTurnLoss>* per_sample = nullptr);

// The loss of ending `start`'s turn at once (no player action), default
// chance outcomes.
NextTurnLoss idle_loss(Engine& engine, const Board& start, const TurnContext& ctx);

// The plain losses of a turn from `before` (turn start) to `after` (end of
// its enemy phase `pr`): score_turn's grid, mechs and building HP tiers.
NextTurnLoss turn_loss(const Board& before, const Board& after, const TurnContext& ctx, const PhaseResult& pr);

// The NextTurn tier itself: the samples' next_turn_value folded
// (LookaheadOptions::fold), 0 without a next turn.
int32_t lookahead_value(Engine& engine, const Board& root, const Board& after, const PhaseResult* phase,
                        const TurnContext& ctx, const LookaheadOptions& options,
                        std::vector<NextTurnLoss>* per_sample = nullptr);
// The lowest value lookahead_value can take with these options.
inline int32_t lookahead_floor(const LookaheadOptions& o) {
  const int32_t k = o.samples < 1 ? 1 : o.samples;
  switch (o.fold) {
    case LookaheadOptions::Fold::Worst:
      return kNextTurnFloor;
    case LookaheadOptions::Fold::Sum:
      return kNextTurnFloor * k;
    case LookaheadOptions::Fold::WorstSum:
    {
      const int64_t sum = static_cast<int64_t>(-kNextTurnFloor) * k;  // clamped by worst_sum_pack
      return worst_sum_pack(-kNextTurnFloor, sum > kWorstSumScale ? kWorstSumScale : static_cast<int32_t>(sum));
    }
  }
  return kNextTurnFloor;
}

// The AI seed of lookahead sample k.
uint32_t lookahead_seed(const LookaheadOptions& options, int k);

}  // namespace itb
