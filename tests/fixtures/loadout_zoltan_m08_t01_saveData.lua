GameData = {["save_version"] = 1, ["language"] = 1, ["network"] = 6, ["networkMax"] = 7, ["overflow"] = 0, ["seed"] = 1258217463, ["new_enemies"] = 1, ["new_missions"] = 1, ["new_equip"] = 1, ["difficulty"] = 3, ["new_abilities"] = 1, ["current"] = {["score"] = 7323, ["time"] = 1731739.625000, ["kills"] = 33, ["damage"] = 0, ["failures"] = 1, ["difficulty"] = 3, ["victory"] = false, ["squad"] = 8, 
["mechs"] = {"NeedleMech", "RocketMech", "FourwayMech", },
["colors"] = {11, 1, 10, },
["weapons"] = {"Brute_KickBack", "", "Ranged_Rocket", "Passive_Electric", "Science_MassShift", "", },
["pilot0"] = {["id"] = "Pilot_Pinnacle", ["name"] = "Calypso", ["name_id"] = "", ["renamed"] = false, ["skill1"] = 4, ["skill2"] = 8, ["exp"] = 50, ["level"] = 2, ["travel"] = 6, ["final"] = 3, ["starting"] = true, ["last_end"] = 2, },
["pilot1"] = {["id"] = "Pilot_Rust", ["name"] = "Leo Kirk", ["name_id"] = "", ["renamed"] = false, ["skill1"] = 0, ["skill2"] = 1, ["exp"] = 19, ["level"] = 1, ["travel"] = 0, ["final"] = 0, ["starting"] = true, },
["pilot2"] = {["id"] = "Pilot_Artificial", ["name"] = "A.I. Unit", ["name_id"] = "Pilot_Artificial_Name", ["renamed"] = false, ["skill1"] = 12, ["skill2"] = 6, ["exp"] = 0, ["level"] = 0, ["travel"] = 0, ["final"] = 0, ["starting"] = false, },
},
["current_squad"] = 8, }
 
RegionData = {
["sector"] = 0, ["island"] = 0, ["secret"] = false, 
["island0"] = {["corporation"] = "Corp_Grass", ["id"] = 0, ["secured"] = false, },
["island1"] = {["corporation"] = "Corp_Desert", ["id"] = 1, ["secured"] = false, },
["island2"] = {["corporation"] = "Corp_Snow", ["id"] = 2, ["secured"] = false, },
["island3"] = {["corporation"] = "Corp_Factory", ["id"] = 3, ["secured"] = false, },

["turn"] = 4, ["iTower"] = 1, ["quest_tracker"] = 1, ["quest_id"] = 0, ["podRewards"] = {},


["region1"] = {["mission"] = "Mission8", ["player"] = {["battle_type"] = 1, ["iCurrentTurn"] = 1, ["iTeamTurn"] = 1, ["iState"] = 0, ["sMission"] = "Mission8", ["iMissionType"] = 0, ["sBriefingMessage"] = "Mission_BossGeneric_Briefing_CEO_Grass_1", ["podReward"] = CreateEffect({}), ["secret"] = false, ["spawn_needed"] = false, ["env_time"] = 1000, ["actions"] = 0, ["iUndoTurn"] = 1, ["aiState"] = 3, ["aiDelay"] = 0.000000, ["aiSeed"] = 763869802, ["victory"] = 2, ["map_data"] = {["version"] = 7, ["dimensions"] = Point( 8, 8 ), ["name"] = "mix1", ["enemy_kills"] = 0, 
["map"] = {{["loc"] = Point( 0, 0 ), ["terrain"] = 3, },
{["loc"] = Point( 0, 1 ), ["terrain"] = 3, },
{["loc"] = Point( 0, 3 ), ["terrain"] = 6, },
{["loc"] = Point( 0, 4 ), ["terrain"] = 6, },
},
["pod"] = Point(5,1), ["spawns"] = {"Scarab2", },
["spawn_ids"] = {11268, },
["spawn_points"] = {Point(7,5), },
["zones"] = {["satellite"] = {Point( 1, 6 ), Point( 1, 1 ), Point( 6, 1 ), Point( 6, 6 ), },
},
["tags"] = {"generic", "grass", "snow", "satellite", },


["pawn1"] = {["type"] = "NeedleMech", ["name"] = "", ["id"] = 0, ["mech"] = true, ["offset"] = 11, 
["reactor"] = {["iNormalPower"] = 0, ["iUsedPower"] = 0, ["iBonusPower"] = 0, ["iUsedBonus"] = 0, ["iUndoPower"] = 0, ["iUsedUndo"] = 0, },
["movePower"] = {0, },
["healthPower"] = {0, },
["primary"] = "Brute_KickBack", ["primary_power"] = {},
["primary_power_class"] = false, ["primary_mod1"] = {0, },
["primary_mod2"] = {0, 0, 0, },
["primary_damaged"] = false, ["primary_starting"] = true, ["primary_uses"] = 1, ["pilot"] = {["id"] = "Pilot_Pinnacle", ["name"] = "Calypso", ["name_id"] = "", ["renamed"] = false, ["skill1"] = 4, ["skill2"] = 8, ["exp"] = 50, ["level"] = 2, ["travel"] = 6, ["final"] = 3, ["starting"] = true, ["last_end"] = 2, },
["iTeamId"] = 1, ["timebonus"] = false, ["iFaction"] = 0, ["iKills"] = 0, ["is_corpse"] = true, ["bBoosted"] = true, ["health"] = 5, ["max_health"] = 5, ["undo_state"] = {["health"] = 5, ["max_health"] = 5, },
["undo_ready"] = false, ["undo_point"] = Point(-1,-1), ["iMissionDamage"] = 0, ["location"] = Point(3,5), ["last_location"] = Point(3,2), ["bActive"] = true, ["iCurrentWeapon"] = 0, ["iTurnCount"] = 1, ["iTurnsRemaining"] = 4, ["undoPosition"] = Point(-1,-1), ["undoReady"] = false, ["iKillCount"] = 0, ["iOwner"] = 0, ["piTarget"] = Point(3,3), ["piOrigin"] = Point(3,3), ["piQueuedShot"] = Point(-1,-1), ["iQueuedSkill"] = -1, ["priorityTarget"] = Point(-1,-1), ["targetHistory"] = Point(3,3), },


["pawn2"] = {["type"] = "RocketMech", ["name"] = "", ["id"] = 1, ["mech"] = true, ["offset"] = 1, 
["reactor"] = {["iNormalPower"] = 0, ["iUsedPower"] = 0, ["iBonusPower"] = 0, ["iUsedBonus"] = 0, ["iUndoPower"] = 0, ["iUsedUndo"] = 0, },
["movePower"] = {0, },
["healthPower"] = {0, },
["primary"] = "Ranged_Rocket", ["primary_power"] = {},
["primary_power_class"] = false, ["primary_mod1"] = {0, 0, },
["primary_mod2"] = {0, 0, },
["primary_damaged"] = false, ["primary_starting"] = true, ["primary_uses"] = 1, ["secondary"] = "Passive_Electric", ["secondary_power"] = {},
["secondary_power_class"] = false, ["secondary_mod1"] = {0, 0, 0, },
["secondary_mod2"] = {0, },
["secondary_damaged"] = false, ["secondary_starting"] = true, ["secondary_uses"] = 1, ["pilot"] = {["id"] = "Pilot_Rust", ["name"] = "Leo Kirk", ["name_id"] = "", ["renamed"] = false, ["skill1"] = 0, ["skill2"] = 1, ["exp"] = 19, ["level"] = 1, ["travel"] = 0, ["final"] = 0, ["starting"] = true, },
["iTeamId"] = 1, ["timebonus"] = false, ["iFaction"] = 0, ["iKills"] = 0, ["is_corpse"] = true, ["health"] = 5, ["max_health"] = 5, ["undo_state"] = {["health"] = 5, ["max_health"] = 5, },
["undo_ready"] = false, ["undo_point"] = Point(-1,-1), ["iMissionDamage"] = 0, ["location"] = Point(3,6), ["last_location"] = Point(3,5), ["bActive"] = true, ["iCurrentWeapon"] = 0, ["iTurnCount"] = 1, ["iTurnsRemaining"] = 4, ["undoPosition"] = Point(-1,-1), ["undoReady"] = false, ["iKillCount"] = 0, ["iOwner"] = 1, ["piTarget"] = Point(7,3), ["piOrigin"] = Point(7,3), ["piQueuedShot"] = Point(-1,-1), ["iQueuedSkill"] = -1, ["priorityTarget"] = Point(-1,-1), ["targetHistory"] = Point(7,3), },


["pawn3"] = {["type"] = "FourwayMech", ["name"] = "", ["id"] = 2, ["mech"] = true, ["offset"] = 10, 
["reactor"] = {["iNormalPower"] = 0, ["iUsedPower"] = 0, ["iBonusPower"] = 0, ["iUsedBonus"] = 0, ["iUndoPower"] = 0, ["iUsedUndo"] = 0, },
["movePower"] = {0, },
["healthPower"] = {0, },
["primary"] = "Science_MassShift", ["primary_power"] = {},
["primary_power_class"] = false, ["primary_mod1"] = {0, },
["primary_mod2"] = {0, 0, },
["primary_damaged"] = false, ["primary_starting"] = true, ["primary_uses"] = 1, ["iTeamId"] = 1, ["timebonus"] = false, ["iFaction"] = 0, ["iKills"] = 0, ["is_corpse"] = true, ["health"] = 2, ["max_health"] = 2, ["undo_state"] = {["health"] = 5, ["max_health"] = 5, },
["undo_ready"] = false, ["undo_point"] = Point(-1,-1), ["iMissionDamage"] = 0, ["location"] = Point(2,4), ["last_location"] = Point(2,3), ["bActive"] = true, ["iCurrentWeapon"] = 0, ["iTurnCount"] = 1, ["iTurnsRemaining"] = 4, ["undoPosition"] = Point(-1,-1), ["undoReady"] = false, ["iKillCount"] = 0, ["iOwner"] = 2, ["piTarget"] = Point(5,6), ["piOrigin"] = Point(5,5), ["piQueuedShot"] = Point(-1,-1), ["iQueuedSkill"] = -1, ["priorityTarget"] = Point(-1,-1), ["targetHistory"] = Point(5,6), },


["pawn4"] = {["type"] = "BouncerBoss", ["name"] = "", ["id"] = 11263, ["mech"] = false, ["offset"] = 2, ["primary"] = "BouncerAtkB", ["primary_uses"] = 1, ["iTeamId"] = 6, ["timebonus"] = false, ["iFaction"] = 0, ["iKills"] = 0, ["is_corpse"] = false, ["health"] = 4, ["max_health"] = 4, ["undo_state"] = {["health"] = 5, ["max_health"] = 5, },
["undo_ready"] = false, ["undo_point"] = Point(-1,-1), ["iMissionDamage"] = 0, ["location"] = Point(4,5), ["last_location"] = Point(5,5), ["iCurrentWeapon"] = 1, ["iTurnCount"] = 1, ["iTurnsRemaining"] = 5, ["undoPosition"] = Point(-1,-1), ["undoReady"] = false, ["iKillCount"] = 0, ["iMutation"] = 11, ["iOwner"] = 11263, ["piTarget"] = Point(3,5), ["piOrigin"] = Point(4,5), ["piQueuedShot"] = Point(3,5), ["iQueuedSkill"] = 1, ["priorityTarget"] = Point(-1,-1), ["targetHistory"] = Point(3,5), },


["pawn_count"] = 8, ["blocked_points"] = {},
["blocked_type"] = {},
},


},
["state"] = 1, ["name"] = "Corporate HQ", },

["iBattleRegion"] = 1, }
 
SquadData = {
["money"] = 5, ["cores"] = 1, ["bIsFavor"] = false, ["repairs"] = 0, ["CorpReward"] = {CreateEffect({weapon = "Prime_Leap",}), CreateEffect({skill1 = "Regen",skill2 = "Pain",pilot = "Pilot_Warrior",}), CreateEffect({power = 2,}), },
["RewardClaimed"] = false, 
["skip_pawns"] = true, 

["storage_size"] = 3, ["CorpStore"] = {CreateEffect({weapon = "Science_TC_Control",money = -2,}), CreateEffect({weapon = "Passive_Burrows",money = -2,}), CreateEffect({stock = 0,}), CreateEffect({stock = 0,}), CreateEffect({money = -3,stock = -1,cores = 1,}), CreateEffect({money = -1,power = 1,stock = -1,}), },
["island_store_count"] = 0, ["store_undo_size"] = 0, }
 
