// The survival cap pre-pass (SolveOptions::survival_cap_share, README "Grid
// survival", survival cap): the certificate's proofs run before the search on
// a turn where no line surely keeps the grid.
#pragma once

#include <cstdint>

#include "itb/solver.hpp"

namespace itb::detail {

// Proves what it can of the certificate within `budget_s` seconds and
// `node_budget` nodes (0: no limit): roll budgets M = G, G - 1, ..., 1,
// then with the grid at 1 resist-first K = 1, 2, ..., each a search of
// every plan in that world (SolveOptions::cert_roll_budget,
// cert_resist_first), stopping at the first that is not proven. Sets
// `ran`, the proofs and `bound`; `proven` is left to the caller.
SurvivalCap prove_survival_cap(Engine& engine, const Board& board, const TurnContext& ctx, const SolveOptions& options,
                               double budget_s, uint64_t node_budget);

}  // namespace itb::detail
