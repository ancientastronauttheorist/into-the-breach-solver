#include "itb/score.hpp"

#include <algorithm>
#include <sstream>

#include "itb/objectives.hpp"
#include "itb/threat.hpp"

namespace itb {
namespace {

// Structure HP over building tiles. A destroyed building (rubble, or water
// for a building on water) has left the sum with all its HP.
int building_hp(const Board& b) {
  int total = 0;
  for (int i = 0; i < kTileCount; ++i) {
    const Tile& t = b.tile(Point::from_index(i));
    if (t.is_building()) total += std::max<int>(0, t.hp);
  }
  return total;
}

bool on_board_alive(const Pawn* p) { return p && p->alive() && !p->fallen; }

// Tier 4's "Vek": every team-6 pawn that is not neutral, Vek and enemy bots
// alike. Neutral team-6 pawns are mission props (the hacked building, storm
// and shield generators, acid vats, minefield bots) scored as objectives.
bool is_enemy_unit(const Pawn& p) { return p.team == Team::Enemy && !p.neutral && !p.mech; }

// A pawn that retreated (tile_rules retreat: the minor flag set, HP 0) left
// without dying.
bool retreated(const Pawn& p, const Pawn* q) { return q && !p.minor && q->minor && q->hp <= 0 && !q->fallen; }

}  // namespace

const char* score_key_name(int key) {
  static constexpr const char* kNames[kScoreKeys] = {"grid",    "bldg_hp", "mechs", "obj_fail", "threat",
                                                     "mech_hp", "obj",     "kills", "vek_hp",   "position"};
  return key >= 0 && key < kScoreKeys ? kNames[key] : "?";
}

int components_for_tiers(int tiers, bool threat_tier) {
  int used = 0;
  for (int i = 0; i < kScoreKeys; ++i) {
    if (used >= tiers) return i;
    if (tier_used(i, threat_tier)) ++used;
  }
  return kScoreKeys;
}

int mech_grid_rate(const Board& before, const ScoreOptions& options) {
  if (!options.mech_exchange()) return -1;
  int rate = options.mech_grid;
  if (options.mech_grid_turns) rate = std::min(rate, std::max(0, before.total_turns - before.turn));
  return rate;
}

std::string Score::describe(bool threat_tier) const {
  std::ostringstream out;
  bool first = true;
  for (int i = 0; i < kScoreKeys; ++i) {
    if (!tier_used(i, threat_tier)) continue;
    out << (first ? "" : " ") << score_key_name(i) << "=" << v[static_cast<size_t>(i)];
    first = false;
  }
  return out.str();
}

// Tier rules (README "Turn score"):
//  1. grid power and building HP: net change, so a gain (none happens mid-turn
//     in shipped missions) counts in the player's favour.
//  2. mechs alive and mech HP over the mechs on the board at the start: net,
//     so a repair or a revived corpse offsets damage (Board:GetMechDamage,
//     which BONUS_MECHS reads, is max HP minus current HP).
//  3. objectives (objectives.hpp).
//  4. enemies killed and enemy HP removed over the enemies alive at the
//     start (spawns of this turn are not scored); net HP, so psion
//     regeneration counts against the player. Retreats are not kills.
//  5. position (objectives.hpp position_terms).
// With the threat tier, Threat (between stars and mech HP) is minus the
// enemies on `after` that could attack a building next turn (threat.hpp).
Score score_turn(const Board& before, const Board& after, const TurnContext* ctx, const PhaseResult* phase,
                 const ScoreOptions& options) {
  Score s;
  s[ScoreKey::GridLost] = after.grid_power - before.grid_power;
  s[ScoreKey::BuildingHpLost] = building_hp(after) - building_hp(before);

  for (const Pawn& p : before.pawns()) {
    const Pawn* q = same_pawn(p, after, phase);
    const int hp0 = on_board_alive(&p) ? p.hp : 0;
    const int hp1 = on_board_alive(q) ? q->hp : 0;
    if (p.mech && p.team == Team::Player) {
      s[ScoreKey::MechsLost] += (hp1 > 0 ? 1 : 0) - (hp0 > 0 ? 1 : 0);
      s[ScoreKey::MechHpLost] += hp1 - hp0;
    } else if (is_enemy_unit(p) && hp0 > 0 && !retreated(p, q)) {
      s[ScoreKey::VekKilled] += hp1 > 0 ? 0 : 1;
      s[ScoreKey::VekHpRemoved] += hp0 - hp1;
    }
  }

  const ObjectiveReport obj = evaluate_objectives(before, after, ctx, phase);
  s[ScoreKey::ObjectivesFailed] = -obj.failed();
  s[ScoreKey::ObjectiveProgress] = obj.progress();
  s[ScoreKey::Position] = position_terms(after).total();
  if (options.threat_kills && options.threat_tier) {
    // Both experiments: threats first, then kills, in one component.
    s[ScoreKey::Threat] = s[ScoreKey::VekKilled] - 100 * count_threats(after);
  } else if (options.threat_kills) {
    s[ScoreKey::Threat] = s[ScoreKey::VekKilled];
  } else if (options.threat_tier) {
    s[ScoreKey::Threat] = -count_threats(after);
  }
  if (options.objectives_first && options.tier_on()) {
    s[ScoreKey::Threat] += kObjectivesFirstWeight * s[ScoreKey::ObjectiveProgress];
  }
  if (options.threat_tier) {
    s[ScoreKey::Threat] -= kUnitThreatWeight * std::min(count_unit_threats(after), kMaxUnitThreats);
  }
  if (const int rate = mech_grid_rate(before, options); rate >= 0) {
    s[ScoreKey::GridLost] = mech_grid_slot(rate, s[ScoreKey::GridLost], s[ScoreKey::MechsLost], after.grid_power);
  }
  return s;
}

}  // namespace itb
