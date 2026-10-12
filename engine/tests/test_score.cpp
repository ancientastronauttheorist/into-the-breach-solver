// Stage 8: the turn score (score.hpp), mission objectives and the position
// metric (objectives.hpp).
#include <doctest/doctest.h>

#include <filesystem>
#include <optional>
#include <fstream>
#include <string>
#include <string_view>

#include "itb/engine.hpp"
#include "itb/objectives.hpp"
#include "itb/recording.hpp"
#include "itb/score.hpp"

using namespace itb;

namespace {

Pawn unit(int32_t uid, const char* type, Team team, Point pos, int hp = 1) {
  Pawn p;
  p.uid = uid;
  p.type = intern(type);
  p.team = team;
  p.pos = pos;
  p.hp = p.max_hp = static_cast<int8_t>(hp);
  return p;
}

Pawn mech(int32_t uid, Point pos, int hp = 3) {
  Pawn p = unit(uid, "PunchMech", Team::Player, pos, hp);
  p.mech = true;
  p.corpse = true;
  return p;
}

Pawn vek(int32_t uid, Point pos, int hp = 2, const char* type = "Scorpion1") {
  return unit(uid, type, Team::Enemy, pos, hp);
}

void building(Board& b, Point p, int hp = 1, const char* unique = nullptr) {
  Tile& t = b.tile(p);
  t.terrain = Terrain::Building;
  t.hp = t.max_hp = static_cast<int8_t>(hp);
  t.populated = true;
  if (unique) t.unique_building = intern(unique);
}

void kill(Board& b, int32_t uid) { b.find_pawn(uid)->hp = 0; }

// A turn of mission `id`: the board at the start, the context and the phase.
struct Turn {
  Board before;
  TurnContext ctx;
  PhaseResult phase;
  explicit Turn(const char* id = "Mission_Survive") { ctx.mission.mission_id = id; }
  ObjectiveReport eval(const Board& after) const { return evaluate_objectives(before, after, &ctx, &phase); }
  Score score(const Board& after) const { return score_turn(before, after, &ctx, &phase); }
};

// A copy: reports are usually temporaries.
std::optional<ObjectiveLine> find(const ObjectiveReport& r, std::string_view id) {
  for (const ObjectiveLine& l : r.lines) {
    if (l.id == id) return l;
  }
  return std::nullopt;
}

}  // namespace

// ---- Tiers 1, 2 and 4 -------------------------------------------------------------

TEST_CASE("score: grid and building HP are net changes over building tiles") {
  Turn t;
  building(t.before, {1, 1}, 2);
  building(t.before, {2, 2});
  t.before.tile({2, 2}).populated = false;  // unpopulated buildings count too
  building(t.before, {3, 3});
  t.before.tile({3, 3}).building_on_water = true;
  t.before.grid_power = 5;
  Board after = t.before;
  after.tile({1, 1}).hp = 1;  // damaged
  after.tile({2, 2}) = Tile{};
  after.tile({2, 2}).terrain = Terrain::Rubble;  // destroyed
  after.tile({3, 3}) = Tile{};
  after.tile({3, 3}).terrain = Terrain::Water;  // destroyed, left water
  after.grid_power = 4;
  Score s = t.score(after);
  CHECK(s[ScoreKey::GridLost] == -1);
  CHECK(s[ScoreKey::BuildingHpLost] == -3);
  // A grid gain (none happens mid-turn in shipped missions) is in the player's favour.
  after = t.before;
  after.grid_power = 6;
  CHECK(t.score(after)[ScoreKey::GridLost] == 1);
}

TEST_CASE("score: mechs and mech HP are net (repairs and revivals offset damage)") {
  Turn t;
  t.before.add_pawn(mech(0, {1, 1}, 3));
  t.before.add_pawn(mech(1, {2, 2}, 2));
  Pawn corpse = mech(2, {3, 3}, 3);
  corpse.hp = 0;
  t.before.add_pawn(corpse);
  Board after = t.before;
  after.find_pawn(0)->hp = 0;  // destroyed: stays as a corpse
  after.find_pawn(1)->hp = 3;  // repaired by 1
  Score s = t.score(after);
  CHECK(s[ScoreKey::MechsLost] == -1);
  CHECK(s[ScoreKey::MechHpLost] == -2);
  // A corpse healed back to life (and its HP) counts for the player.
  after = t.before;
  after.find_pawn(2)->hp = 1;
  s = t.score(after);
  CHECK(s[ScoreKey::MechsLost] == 1);
  CHECK(s[ScoreKey::MechHpLost] == 1);
  // A mech that fell into a chasm is lost with all its HP.
  after = t.before;
  after.find_pawn(0)->fallen = true;
  CHECK(t.score(after)[ScoreKey::MechHpLost] == -3);
}

TEST_CASE("score: enemies are Vek and enemy bots, not neutral props, spawns or retreats") {
  Turn t;
  t.before.add_pawn(vek(10, {1, 1}, 2));
  Pawn bot = vek(11, {2, 2}, 1, "Snowtank1");
  bot.faction = Faction::Bots;
  t.before.add_pawn(bot);
  Pawn prop = vek(12, {3, 3}, 1, "Storm_Generator");
  prop.neutral = true;
  t.before.add_pawn(prop);
  t.before.add_pawn(vek(13, {4, 4}, 3, "Leaper1"));
  Board after = t.before;
  kill(after, 10);
  kill(after, 11);
  kill(after, 12);
  after.remove_pawn(13);  // removed body
  // A Vek emerging this turn with a reused uid is a new pawn, not uid 13.
  after.add_pawn(vek(13, {5, 5}, 3, "Leaper1"));
  t.phase.events.push_back(PhaseEvent{PhaseEventType::SpawnEmerged, {5, 5}, 13, 0, "Leaper1"});
  after.add_pawn(vek(14, {6, 6}, 2));  // emerged: not scored
  Score s = t.score(after);
  CHECK(s[ScoreKey::VekKilled] == 3);
  CHECK(s[ScoreKey::VekHpRemoved] == 6);

  // A retreat (minor flag set, HP 0) is not a kill; psion regeneration is
  // negative damage.
  Turn r;
  r.before.add_pawn(vek(20, {1, 1}, 2));
  r.before.add_pawn(vek(21, {2, 2}, 2));
  r.before.find_pawn(21)->max_hp = 3;
  after = r.before;
  after.find_pawn(20)->hp = 0;
  after.find_pawn(20)->minor = true;
  after.find_pawn(21)->hp = 3;
  s = r.score(after);
  CHECK(s[ScoreKey::VekKilled] == 0);
  CHECK(s[ScoreKey::VekHpRemoved] == -1);
}

TEST_CASE("score: tiers compare lexicographically") {
  Turn t("Mission_Tanks");
  t.before.add_pawn(unit(5, "Archive_Tank", Team::Player, {1, 1}));
  t.before.add_pawn(vek(10, {2, 2}, 2));
  Board keep_tank = t.before;
  Board kill_vek = t.before;
  kill(kill_vek, 10);
  kill(kill_vek, 5);
  CHECK(t.score(keep_tank) > t.score(kill_vek));  // an objective outranks a kill
  Board lose_mech_hp = t.before;
  lose_mech_hp.add_pawn(mech(0, {3, 3}));
  Turn m = t;
  m.before.add_pawn(mech(0, {3, 3}));
  lose_mech_hp.find_pawn(0)->hp = 2;
  kill(lose_mech_hp, 10);
  CHECK(m.score(lose_mech_hp) < m.score(m.before));  // mech HP outranks a kill

  // A star outranks mech HP (lost for good vs repaired after the mission),
  // but a mech's life outranks a star.
  Board save_tank_hurt = m.before;
  save_tank_hurt.find_pawn(0)->hp = 1;
  Board lose_tank_unhurt = m.before;
  kill(lose_tank_unhurt, 5);
  CHECK(m.score(save_tank_hurt) > m.score(lose_tank_unhurt));
  Board save_tank_lose_mech = m.before;
  kill(save_tank_lose_mech, 0);
  CHECK(m.score(save_tank_lose_mech) < m.score(lose_tank_unhurt));
}

// ---- Objectives: shared ---------------------------------------------------------------

TEST_CASE("objectives: pods break or are secured") {
  Turn t;
  t.before.tile({1, 1}).pod = PodState::Present;
  t.before.tile({2, 2}).pod = PodState::Present;
  Board after = t.before;
  after.tile({1, 1}).pod = PodState::Destroyed;
  after.tile({2, 2}).pod = PodState::Collected;
  std::optional<ObjectiveLine> l = find(t.eval(after), "pod");
  REQUIRE(l);
  CHECK(l->failed == 1);
  CHECK(l->progress == kStar);
}

TEST_CASE("objectives: asset and critical buildings fail when damaged") {
  Turn t("Mission_Power");
  building(t.before, {1, 1}, 1, "Str_Power");
  building(t.before, {2, 2}, 2, "Mission_Power");
  building(t.before, {3, 3}, 1, "Mission_Power");
  Board after = t.before;
  after.tile({2, 2}).hp = 1;  // damaged, still standing: IsDamaged
  const ObjectiveReport r = t.eval(after);
  CHECK(find(r, "BONUS_ASSET Str_Power")->failed == 0);
  CHECK(r.failed() == 1);
  after.tile({1, 1}).terrain = Terrain::Rubble;
  after.tile({1, 1}).hp = 0;
  CHECK(t.eval(after).failed() == 2);
  // The asset is an active bonus even without the bridge's id list.
  const std::vector<BonusId> b = active_bonuses(t.before, t.ctx.mission);
  CHECK(std::find(b.begin(), b.end(), BonusId::Asset) != b.end());
}

// ---- Bonus objectives -----------------------------------------------------------------

TEST_CASE("objectives: BONUS_KILL_FIVE counts EVENT_ENEMY_KILLED deaths toward the target") {
  Turn t;
  ObjectiveData& o = t.ctx.mission.objectives;
  o.kill_target = 5;
  o.kills_done = 3;
  t.before.add_pawn(vek(10, {1, 1}));
  t.before.add_pawn(vek(11, {2, 2}));
  Pawn minor = vek(12, {3, 3}, 1, "WebbEgg1");
  minor.minor = true;  // minor deaths raise another event
  t.before.add_pawn(minor);
  Pawn bot = vek(13, {4, 4}, 1, "Snowtank1");
  bot.faction = Faction::Bots;  // bots are team 6 too
  t.before.add_pawn(bot);
  Board after = t.before;
  kill(after, 10);
  kill(after, 12);
  kill(after, 13);
  ObjectiveReport r = t.eval(after);
  CHECK(r.enemy_kills == 2);
  std::optional<ObjectiveLine> l = find(r, "BONUS_KILL_FIVE");
  REQUIRE(l);
  CHECK(l->progress == 2 * kStar / 5);
  CHECK(l->failed == 0);
  // Kills beyond the target add nothing; the end decides.
  kill(after, 11);
  t.phase.mission_ended = true;
  l = find(t.eval(after), "BONUS_KILL_FIVE");
  CHECK(l->progress == 2 * kStar / 5);
  CHECK(l->failed == 0);
  o.kills_done = 0;
  CHECK(find(t.eval(after), "BONUS_KILL_FIVE")->failed == 1);
}

TEST_CASE("objectives: BONUS_PACIFIST fails when the kill limit is crossed") {
  Turn t;
  t.ctx.mission.objectives.kill_limit = 4;
  t.ctx.mission.objectives.kills_done = 3;
  t.before.add_pawn(vek(10, {1, 1}));
  t.before.add_pawn(vek(11, {2, 2}));
  Board after = t.before;
  kill(after, 10);
  CHECK(find(t.eval(after), "BONUS_PACIFIST")->failed == 0);
  kill(after, 11);
  CHECK(find(t.eval(after), "BONUS_PACIFIST")->failed == 1);
  t.ctx.mission.objectives.kills_done = 5;  // already failed earlier
  CHECK(find(t.eval(after), "BONUS_PACIFIST")->failed == 0);
}

TEST_CASE("objectives: BONUS_GRID fails when grid damage since deployment reaches 3") {
  Turn t;
  ObjectiveData& o = t.ctx.mission.objectives;
  o.bonus_known = true;
  o.bonus = {static_cast<int>(BonusId::Grid)};
  o.power_start = 7;
  t.before.grid_power = 5;
  Board after = t.before;
  after.grid_power = 4;
  std::optional<ObjectiveLine> l = find(t.eval(after), "BONUS_GRID");
  REQUIRE(l);
  CHECK(l->failed == 1);
  CHECK(l->exact);
  after.grid_power = 5;
  CHECK(find(t.eval(after), "BONUS_GRID")->failed == 0);
}

TEST_CASE("objectives: BONUS_MECHS judges mech damage (max HP - HP) at the end") {
  Turn t;
  t.ctx.mission.objectives.bonus_known = true;
  t.ctx.mission.objectives.bonus = {static_cast<int>(BonusId::Mechs)};
  t.before.add_pawn(mech(0, {1, 1}, 3));
  t.before.add_pawn(mech(1, {2, 2}, 3));
  Board after = t.before;
  after.find_pawn(0)->hp = 0;  // 3
  after.find_pawn(1)->hp = 2;  // 1
  CHECK(find(t.eval(after), "BONUS_MECHS")->failed == 0);  // not decided yet
  t.phase.mission_ended = true;
  CHECK(find(t.eval(after), "BONUS_MECHS")->failed == 1);
  after.find_pawn(1)->hp = 3;  // repaired
  CHECK(find(t.eval(after), "BONUS_MECHS")->failed == 0);
}

TEST_CASE("objectives: BONUS_BLOCK counts blocked spawns") {
  Turn t;
  ObjectiveData& o = t.ctx.mission.objectives;
  o.bonus_known = true;
  o.bonus = {static_cast<int>(BonusId::Block)};
  o.blocked_spawns = 2;
  t.phase.events.push_back(PhaseEvent{PhaseEventType::SpawnBlocked, {1, 1}, 0, 0, ""});
  t.phase.events.push_back(PhaseEvent{PhaseEventType::SpawnBlocked, {2, 2}, 0, 0, ""});
  std::optional<ObjectiveLine> l = find(t.eval(t.before), "BONUS_BLOCK");
  REQUIRE(l);
  CHECK(l->progress == kStar / 3);  // only the third block counts
  o.blocked_spawns = 0;
  t.phase.events.clear();
  t.phase.mission_ended = true;
  CHECK(find(t.eval(t.before), "BONUS_BLOCK")->failed == 1);
}

TEST_CASE("objectives: BONUS_DEBRIS, BONUS_SELFDAMAGE and BONUS_KILL") {
  Turn t;
  t.before.add_pawn(vek(10, {1, 1}, 1, "BonusDebris"));
  t.before.add_pawn(vek(11, {2, 2}, 1, "BonusDebris"));
  Pawn m = mech(0, {3, 3});
  m.infected = true;
  t.before.add_pawn(m);
  Board after = t.before;
  kill(after, 10);
  after.find_pawn(0)->infected = false;
  ObjectiveReport r = t.eval(after);
  CHECK(find(r, "BONUS_DEBRIS")->progress == kStar / 2);
  CHECK(find(r, "BONUS_SELFDAMAGE")->progress == kStar / 3);
  CHECK(r.failed() == 0);
  t.phase.mission_ended = true;
  r = t.eval(after);
  CHECK(find(r, "BONUS_DEBRIS")->failed == 1);  // one sack left
  CHECK(find(r, "BONUS_SELFDAMAGE")->failed == 0);

  Turn k;
  k.ctx.mission.objectives.bonus_known = true;
  k.ctx.mission.objectives.bonus = {static_cast<int>(BonusId::Kill)};
  k.before.add_pawn(vek(10, {1, 1}));
  k.phase.mission_ended = true;
  CHECK(find(k.eval(k.before), "BONUS_KILL")->failed == 1);
}

// ---- Mission objectives -----------------------------------------------------------------

TEST_CASE("objectives: protected units, 1 star each") {
  struct Case {
    const char* mission;
    const char* type;
    Team team;
  };
  for (const Case& c : {Case{"Mission_Tanks", "Archive_Tank", Team::Player},
                        Case{"Mission_Civilians", "VIP_Truck", Team::Player},
                        Case{"Mission_Bomb", "ProtoBomb", Team::Player},
                        Case{"Mission_BotDefense", "Snowmine1", Team::Player},
                        Case{"Mission_Artillery", "ArchiveArtillery", Team::Player},
                        Case{"Mission_Filler", "Filler_Pawn", Team::Player},
                        Case{"Mission_Final_Cave", "BigBomb", Team::Player},
                        Case{"Mission_Volatile", "GlowingScorpion", Team::Enemy}}) {
    CAPTURE(c.mission);
    Turn t(c.mission);
    t.before.add_pawn(unit(5, c.type, c.team, {1, 1}, 2));
    t.before.add_pawn(unit(6, c.type, c.team, {5, 5}, 2));
    Board after = t.before;
    kill(after, 5);
    after.remove_pawn(6);  // removed bodies are lost too
    std::optional<ObjectiveLine> l = find(t.eval(after), c.mission);
    REQUIRE(l);
    CHECK(l->failed == 2);
  }
  // The Volatile Vek retreating is not a death.
  Turn v("Mission_Volatile");
  v.before.add_pawn(unit(5, "GlowingScorpion", Team::Enemy, {1, 1}, 4));
  Board after = v.before;
  after.find_pawn(5)->hp = 0;
  after.find_pawn(5)->minor = true;
  CHECK(find(v.eval(after), "Mission_Volatile")->failed == 0);
}

TEST_CASE("objectives: the train loses a star when stopped and one when the wreck dies") {
  Turn t("Mission_Train");
  t.before.add_pawn(unit(5, "Train_Pawn", Team::Player, {4, 6}));
  Board after = t.before;
  kill(after, 5);
  after.add_pawn(unit(9, "Train_Damaged", Team::Player, {4, 6}));
  CHECK(find(t.eval(after), "Mission_Train")->failed == 1);
  kill(after, 9);
  CHECK(find(t.eval(after), "Mission_Train")->failed == 2);
  Turn w("Mission_Armored_Train");
  w.before.add_pawn(unit(9, "Train_Armored_Damaged", Team::Player, {4, 6}));
  after = w.before;
  kill(after, 9);
  CHECK(find(w.eval(after), "Mission_Armored_Train")->failed == 1);
}

TEST_CASE("objectives: satellites launch (progress) or are destroyed (failed)") {
  Turn t("Mission_Satellite");
  t.before.add_pawn(unit(5, "SatelliteRocket", Team::Player, {1, 1}, 2));
  t.before.add_pawn(unit(6, "SatelliteRocket", Team::Player, {5, 5}, 2));
  Board after = t.before;
  after.remove_pawn(5);  // FlyAway
  kill(after, 6);        // corpse stays
  std::optional<ObjectiveLine> l = find(t.eval(after), "Mission_Satellite");
  REQUIRE(l);
  CHECK(l->progress == kStar);
  CHECK(l->failed == 1);
}

TEST_CASE("objectives: destroy targets (dam, generators, hacked tower, bosses)") {
  struct Case {
    const char* mission;
    const char* type;
    const char* line;
  };
  for (const Case& c : {Case{"Mission_Dam", "Dam_Pawn", "Mission_Dam"},
                        Case{"Mission_Shields", "Shield_Building", "Mission_Shields"},
                        Case{"Mission_AcidStorm", "Storm_Generator", "Mission_AcidStorm"},
                        Case{"Mission_Hacking", "Hacked_Building", "Mission_Hacking tower"},
                        Case{"Mission_ScorpionBoss", "ScorpionBoss", "Mission_ScorpionBoss"},
                        Case{"Mission_BotBoss", "BotBoss2", "Mission_BotBoss"},
                        Case{"Mission_JellyBoss", "Jelly_Boss", "Mission_JellyBoss"}}) {
    CAPTURE(c.mission);
    Turn t(c.mission);
    t.before.add_pawn(unit(5, c.type, Team::Enemy, {1, 1}, 3));
    Board after = t.before;
    kill(after, 5);
    std::optional<ObjectiveLine> l = find(t.eval(after), c.line);
    REQUIRE(l);
    CHECK(l->progress == kStar);
    CHECK(l->failed == 0);
    t.phase.mission_ended = true;
    l = find(t.eval(t.before), c.line);
    CHECK(l->failed == 1);  // still standing when the mission ends
    CHECK(l->progress == 0);
  }
}

TEST_CASE("objectives: Mission_Hacking's bot, and its conversion is not a loss") {
  Turn t("Mission_Hacking");
  Pawn bot = unit(7, "Snowtank1", Team::Enemy, {2, 2});
  bot.shield = true;
  t.before.add_pawn(bot);
  Pawn tower = unit(8, "Hacked_Building", Team::Enemy, {4, 4});
  tower.minor = true;
  tower.neutral = true;
  t.before.add_pawn(tower);
  Board after = t.before;
  kill(after, 8);
  after.remove_pawn(7);
  after.add_pawn(unit(9, "Snowtank1_Player", Team::Player, {2, 2}));
  ObjectiveReport r = t.eval(after);
  CHECK(find(r, "Mission_Hacking bot")->failed == 0);
  CHECK(find(r, "Mission_Hacking tower")->progress == kStar);
  CHECK(r.enemy_kills == 0);  // the tower is minor in game; the bot left
  after = t.before;
  kill(after, 7);
  CHECK(find(t.eval(after), "Mission_Hacking bot")->failed == 1);
}

TEST_CASE("objectives: Mission_Disposal and Mission_Force mountains") {
  Turn d("Mission_Disposal");
  d.before.add_pawn(unit(5, "Disposal_Unit", Team::Player, {1, 1}, 2));
  d.before.tile({3, 3}).terrain = Terrain::Mountain;
  d.before.tile({4, 4}).terrain = Terrain::Mountain;
  Board after = d.before;
  after.tile({3, 3}).terrain = Terrain::Rubble;
  ObjectiveReport r = d.eval(after);
  CHECK(find(r, "Mission_Disposal unit")->failed == 0);
  CHECK(find(r, "Mission_Disposal mountains")->progress == kStar / 2);
  d.phase.mission_ended = true;
  CHECK(find(d.eval(after), "Mission_Disposal mountains")->failed == 1);

  Turn f("Mission_Force");
  f.ctx.mission.objectives.mountains_done = 1;
  f.ctx.mission.objectives.mountain_target = 2;
  f.before.tile({3, 3}).terrain = Terrain::Mountain;
  f.before.tile({4, 4}).terrain = Terrain::Mountain;
  after = f.before;
  after.tile({3, 3}).terrain = Terrain::Rubble;
  after.tile({4, 4}).terrain = Terrain::Rubble;
  r = f.eval(after);
  CHECK(r.mountains_destroyed == 2);
  CHECK(find(r, "Mission_Force")->progress == kStar / 2);  // only one was still needed
}

TEST_CASE("objectives: Mission_Terraform") {
  Turn t("Mission_Terraform");
  t.before.add_pawn(unit(5, "Terraformer", Team::Player, {1, 1}, 2));
  Board after = t.before;
  ObjectiveReport r = t.eval(after);
  CHECK_FALSE(find(r, "Mission_Terraform grass")->exact);  // grass tiles not recorded
  t.ctx.mission.objectives.grass_known = true;
  t.ctx.mission.objectives.grass = {{2, 2}, {2, 3}};
  after.tile({2, 2}).terrain = Terrain::Sand;
  kill(after, 5);
  r = t.eval(after);
  CHECK(find(r, "Mission_Terraform unit")->failed == 1);
  CHECK(find(r, "Mission_Terraform grass")->progress == kStar / 2);
}

TEST_CASE("objectives: Mission_AcidTank counts acid kills, 2 stars") {
  Turn t("Mission_AcidTank");
  t.ctx.mission.objectives.kills_done = 0;
  t.ctx.mission.objectives.kill_target = 4;
  Pawn a = vek(10, {1, 1});
  a.acid = true;
  t.before.add_pawn(a);
  t.before.add_pawn(vek(11, {2, 2}));
  t.before.acid_destroyed = 5;  // a running count: the turn's share is after - before
  Board after = t.before;
  // Deaths are counted as they are processed (Board::acid_destroyed, see
  // the executor and enemy phase tests), not read off the pawns.
  kill(after, 10);
  kill(after, 11);
  ++after.acid_destroyed;
  ObjectiveReport r = t.eval(after);
  CHECK(r.acid_kills == 1);
  CHECK(find(r, "Mission_AcidTank")->progress == kStar);  // the first acid kill earns a star
  CHECK_FALSE(find(r, "BONUS_KILL_FIVE").has_value());         // the acid goal is not a kill bonus
  t.phase.mission_ended = true;
  CHECK(find(t.eval(after), "Mission_AcidTank")->failed == 1);
  // The fourth earns the second star. Pawns created during the turn (a split
  // blob, a spawn) count too, so 2 enemies at the start can make 4 kills and
  // the progress cap must allow it.
  after.acid_destroyed += 3;
  r = t.eval(after);
  CHECK(r.acid_kills == 4);
  CHECK(find(r, "Mission_AcidTank")->failed == 0);
  CHECK(find(r, "Mission_AcidTank")->progress == 2 * kStar);
  CHECK(objective_progress_cap(t.before, &t.ctx) >= 2 * kStar);
}

TEST_CASE("objectives: Mission_Barrels and Mission_BoomBots") {
  Turn b("Mission_Barrels");
  for (int i = 0; i < 2; ++i) {
    Pawn v = unit(10 + i, "AcidVat", Team::Enemy, {1 + i, 1}, 2);
    v.neutral = true;
    v.minor = true;
    b.before.add_pawn(v);
  }
  Board after = b.before;
  kill(after, 10);
  CHECK(find(b.eval(after), "Mission_Barrels")->progress == kStar);
  b.phase.mission_ended = true;
  CHECK(find(b.eval(after), "Mission_Barrels")->failed == 1);

  Turn m("Mission_BoomBots");
  for (int i = 0; i < 3; ++i) m.before.add_pawn(unit(20 + i, "Snowtank1_Boom", Team::Enemy, {i, 4}));
  after = m.before;  // one already destroyed
  kill(after, 20);   // the second: 1 star
  ObjectiveReport r = m.eval(after);
  CHECK(find(r, "Mission_BoomBots")->progress == kStar / 2);
  m.phase.mission_ended = true;
  CHECK(find(m.eval(after), "Mission_BoomBots")->failed == 1);
}

TEST_CASE("objectives: Mission_ForestFire counts fires on the board") {
  Turn t("Mission_ForestFire");
  for (int i = 0; i < 3; ++i) t.before.tile({i, 0}).fire = FireState::Burning;
  Board after = t.before;
  after.tile({5, 5}).fire = FireState::BurningForest;
  CHECK(find(t.eval(after), "Mission_ForestFire")->progress == kStar / 4);
  after.tile({0, 0}).fire = FireState::None;
  after.tile({1, 0}).fire = FireState::None;
  CHECK(find(t.eval(after), "Mission_ForestFire")->progress == -kStar / 4);  // put out
  t.phase.mission_ended = true;
  CHECK(find(t.eval(after), "Mission_ForestFire")->failed == 2);  // 2 fires: no star
}

TEST_CASE("objectives: Mission_Repair counts platforms used by player units") {
  Turn t("Mission_Repair");
  t.ctx.mission.objectives.repairs_done = 2;
  t.ctx.mission.objectives.repair_target = 3;
  t.before.tile({1, 1}).item = intern("Item_Repair_Mine");
  t.before.tile({2, 2}).item = intern("Item_Repair_Mine");
  t.before.add_pawn(mech(0, {0, 0}));
  t.before.add_pawn(vek(10, {3, 3}));
  Board after = t.before;
  after.tile({1, 1}).item = kNoSymbol;
  after.find_pawn(0)->pos = {1, 1};
  after.tile({2, 2}).item = kNoSymbol;
  after.find_pawn(10)->pos = {2, 2};  // an enemy on a platform does not count
  ObjectiveReport r = t.eval(after);
  CHECK(r.repairs_used == 1);
  CHECK(find(r, "Mission_Repair")->progress == kStar / 3);
}

TEST_CASE("objectives: Mission_FreezeBldg counts thawed buildings") {
  Turn t("Mission_FreezeBldg");
  t.ctx.mission.objectives.freeze_target = 5;
  for (int i = 0; i < 6; ++i) {
    building(t.before, {i, 0});
    t.before.tile({i, 0}).frozen = i >= 2;  // 2 thawed already
    t.ctx.mission.objectives.freeze_buildings.push_back({i, 0});
  }
  Board after = t.before;
  after.tile({2, 0}).frozen = false;                   // thawed
  after.tile({3, 0}) = Tile{};                         // destroyed counts as thawed
  after.tile({3, 0}).terrain = Terrain::Rubble;
  ObjectiveReport r = t.eval(after);
  CHECK(find(r, "Mission_FreezeBldg")->progress == 2 * kStar / 5);
  t.phase.mission_ended = true;
  CHECK(find(t.eval(after), "Mission_FreezeBldg")->failed == 1);  // 4 of 5
}

TEST_CASE("objectives: Mission_FreezeBots wants both bots frozen and alive") {
  Turn t("Mission_FreezeBots");
  t.before.add_pawn(unit(5, "Snowtank1", Team::Enemy, {1, 1}));
  t.before.add_pawn(unit(6, "Snowlaser1", Team::Enemy, {3, 3}));
  Board after = t.before;
  after.find_pawn(5)->frozen = true;
  CHECK(find(t.eval(after), "Mission_FreezeBots")->progress == kStar);
  kill(after, 6);
  CHECK(find(t.eval(after), "Mission_FreezeBots")->failed == 1);
  after.find_pawn(6)->hp = 1;
  t.phase.mission_ended = true;
  CHECK(find(t.eval(after), "Mission_FreezeBots")->failed == 1);  // one bot not frozen at the end
}

TEST_CASE("objectives: Mission_BlobBoss estimates dead blobs; Mission_Missiles is not modelled") {
  Turn t("Mission_BlobBoss");
  t.before.add_pawn(unit(5, "BlobBossMed", Team::Enemy, {1, 1}, 2));
  t.before.add_pawn(unit(6, "BlobBossMed", Team::Enemy, {3, 3}, 2));  // 1 dead so far
  Board after = t.before;
  kill(after, 5);
  after.remove_pawn(5);
  after.add_pawn(unit(7, "BlobBossSmall", Team::Enemy, {1, 2}));
  after.add_pawn(unit(8, "BlobBossSmall", Team::Enemy, {1, 0}));
  std::optional<ObjectiveLine> l = find(t.eval(after), "Mission_BlobBoss");
  REQUIRE(l);
  CHECK(l->progress == kStar / 5);
  CHECK_FALSE(l->exact);

  Turn m("Mission_Missiles");
  CHECK_FALSE(find(m.eval(m.before), "Mission_Missiles")->exact);
}

TEST_CASE("objectives: active bonuses inferred without the bridge's list") {
  Turn t;
  t.ctx.mission.objectives.kill_target = 7;
  t.ctx.mission.objectives.kill_limit = 5;
  Pawn m = mech(0, {1, 1});
  m.infected = true;
  t.before.add_pawn(m);
  t.before.add_pawn(vek(10, {2, 2}, 1, "BonusDebris"));
  const std::vector<BonusId> b = active_bonuses(t.before, t.ctx.mission);
  CHECK(b == std::vector<BonusId>{BonusId::KillFive, BonusId::Pacifist, BonusId::Debris, BonusId::SelfDamage});
  t.ctx.mission.objectives.bonus_known = true;
  t.ctx.mission.objectives.bonus = {5, 1};
  CHECK(active_bonuses(t.before, t.ctx.mission) == std::vector<BonusId>{BonusId::Block, BonusId::Asset});
}

TEST_CASE("objectives: without a phase result the mission ends on its last turn") {
  Board before;
  before.turn = 4;
  before.total_turns = 4;
  TurnContext ctx;
  ctx.mission.mission_id = "Mission_Dam";
  before.add_pawn(unit(5, "Dam_Pawn", Team::None, {1, 1}, 2));
  const ObjectiveReport r = evaluate_objectives(before, before, &ctx, nullptr);
  CHECK(r.mission_ends);
  CHECK(r.failed() == 1);
  CHECK(score_turn(before, before, &ctx, nullptr)[ScoreKey::ObjectivesFailed] == -1);
}

// ---- Position ----------------------------------------------------------------------------

TEST_CASE("position: mech hazards, threatened buildings, enemy debuffs") {
  Board b;
  Pawn m = mech(0, {1, 1}, 1);  // at 1 HP
  m.fire = true;
  m.acid = true;
  b.add_pawn(m);
  Pawn m2 = mech(1, {5, 5});
  m2.frozen = true;
  b.add_pawn(m2);
  b.tile({5, 5}).smoke = true;
  building(b, {3, 4});
  building(b, {4, 3}, 1, "Str_Power");
  b.add_pawn(vek(10, {3, 3}));  // next to both buildings
  Pawn burning = vek(11, {6, 1});
  burning.fire = true;
  b.add_pawn(burning);
  b.add_pawn(unit(12, "Archive_Tank", Team::Player, {7, 1}));  // next to the burning Vek
  const PositionTerms t = position_terms(b);
  CHECK(t.mech_fire == 1);
  CHECK(t.mech_acid == 1);
  CHECK(t.mech_frozen == 1);
  CHECK(t.mech_smoke == 1);
  CHECK(t.mech_fragile == 1);
  CHECK(t.building_threat == 3);
  CHECK(t.unit_threat == 1);
  CHECK(t.enemy_fire == 1);
  CHECK(t.total() == -(3 + 2 + 2 + 1 + 2) - (3 + 1) + 1);
  // Fire immunity (Flame Shielding) removes the fire term.
  b.passives |= kPassiveFlameImmune;
  CHECK(position_terms(b).mech_fire == 0);
}

// ---- Recording loader ---------------------------------------------------------------------

TEST_CASE("recordings: objective fields load into MissionData::objectives") {
  const auto path = std::filesystem::temp_directory_path() / "itb_recording_objectives.json";
  {
    std::ofstream out(path);
    out << R"({"tiles": [{"x": 1, "y": 1, "terrain": "building", "terrain_id": 1, "building_hp": 1,
                          "unique_building": true, "objective_name": "Str_Power"}],
      "units": [], "turn": 1, "grid_power": 6, "mission_id": "Mission_FreezeBldg",
      "bonus_objective_ids": [6, 1], "mission_kills_done": 2, "mission_kill_target": 5,
      "mission_kill_limit": 4, "repair_platform_target": 3, "repair_platforms_used": 1,
      "mission_mountain_target": 2, "mission_mountains_destroyed": 1,
      "freeze_building_target": 5, "freeze_building_tiles": [[2, 0], [2, 3]]})";
  }
  std::string error;
  auto rec = load_recording(path, nullptr, &error);
  std::filesystem::remove(path);
  REQUIRE_MESSAGE(rec.has_value(), error);
  const ObjectiveData& o = rec->mission.objectives;
  CHECK(o.bonus_known);
  CHECK(o.bonus == std::vector<int>{6, 1});
  CHECK(o.kills_done == 2);
  CHECK(o.kill_target == 5);
  CHECK(o.kill_limit == 4);
  CHECK(o.power_start == 6);  // turn 1: the grid at deployment
  CHECK(o.repair_target == 3);
  CHECK(o.repairs_done == 1);
  CHECK(o.mountain_target == 2);
  CHECK(o.mountains_done == 1);
  CHECK(o.freeze_target == 5);
  CHECK(o.freeze_buildings == std::vector<Point>{{2, 0}, {2, 3}});
  CHECK(symbol_name(rec->board.tile({1, 1}).unique_building) == "Str_Power");
}
