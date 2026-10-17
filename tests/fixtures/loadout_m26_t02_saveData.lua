GameData = {["save_version"] = 1, ["language"] = 1, ["network"] = 7, ["networkMax"] = 7, ["overflow"] = 16, ["seed"] = 621744850, ["new_enemies"] = 1, ["new_missions"] = 1, ["new_equip"] = 1, ["difficulty"] = 2, ["new_abilities"] = 1, ["ach_info"] = {["squad"] = "Random", ["trackers"] = {["Detritus_B_2"] = 0, ["Global_Challenge_Power"] = 0, ["Archive_A_1"] = 0, ["Archive_B_2"] = 0, ["Rust_A_2"] = 0, ["Rust_A_3"] = 0, ["Pinnacle_A_3"] = 0, ["Archive_B_1"] = 0, ["Pinnacle_B_3"] = 0, ["Detritus_B_1"] = 0, ["Pinnacle_B_1"] = 0, ["Global_Island_Mechs"] = 0, ["Global_Island_Building"] = 0, ["Squad_Mist_1"] = 0, ["Squad_Bomber_2"] = 0, ["Squad_Spiders_1"] = 0, ["Squad_Mist_2"] = 0, ["Squad_Heat_1"] = 0, ["Squad_Cataclysm_1"] = 0, ["Squad_Cataclysm_2"] = 0, ["Squad_Cataclysm_3"] = 0, },
},

["current"] = {["score"] = 19500, ["time"] = 17227074.000000, ["kills"] = 119, ["damage"] = 0, ["failures"] = 1, ["difficulty"] = 2, ["victory"] = false, ["islands"] = 2, ["squad"] = 8, 
["mechs"] = {"BottlecapMech", "JetMech", "SupermanMech", },
["colors"] = {13, 1, 11, },
["weapons"] = {"Prime_TC_Punt", "Prime_KO_Crack", "Brute_Jetmech", "Support_Missiles", "Science_TC_Control", "Science_Placer", },
["pilot0"] = {["id"] = "Pilot_Miner", ["name"] = "Silica", ["name_id"] = "Pilot_Miner_Name", ["renamed"] = false, ["skill1"] = 5, ["skill2"] = 0, ["exp"] = 50, ["level"] = 2, ["travel"] = 3, ["final"] = 2, ["starting"] = true, ["power"] = {0, 0, },
["last_end"] = 1, },
["pilot1"] = {["id"] = "Pilot_Detritus", ["name"] = "Maxim Correa", ["name_id"] = "", ["renamed"] = false, ["skill1"] = 3, ["skill2"] = 10, ["exp"] = 47, ["level"] = 1, ["travel"] = 0, ["final"] = 0, ["starting"] = true, },
["pilot2"] = {["id"] = "Pilot_Archive", ["name"] = "Nadia Berezin", ["name_id"] = "", ["renamed"] = false, ["skill1"] = 10, ["skill2"] = 6, ["exp"] = 41, ["level"] = 1, ["travel"] = 0, ["final"] = 0, ["starting"] = true, },
},
["current_squad"] = 8, }
 

RegionData = {
["sector"] = 2, ["island"] = 2, ["secret"] = true, 
["island0"] = {["corporation"] = "Corp_Grass", ["id"] = 0, ["secured"] = true, },
["island1"] = {["corporation"] = "Corp_Desert", ["id"] = 1, ["secured"] = true, },
["island2"] = {["corporation"] = "Corp_Snow", ["id"] = 2, ["secured"] = false, },
["island3"] = {["corporation"] = "Corp_Factory", ["id"] = 3, ["secured"] = false, },

["turn"] = 3, ["iTower"] = 0, ["quest_tracker"] = 0, ["quest_id"] = 0, ["podRewards"] = {},

["region2"] = {["mission"] = "Mission1", ["player"] = {["battle_type"] = 0, ["iCurrentTurn"] = 2, ["iTeamTurn"] = 1, ["iState"] = 0, ["sMission"] = "Mission1", ["iMissionType"] = 0, ["sBriefingMessage"] = "Mission_Shields_Briefing_CEO_Snow2", ["podReward"] = CreateEffect({}), ["secret"] = false, ["spawn_needed"] = false, ["env_time"] = 1000, ["actions"] = 0, ["iUndoTurn"] = 1, ["aiState"] = 3, ["aiDelay"] = 0.000000, ["aiSeed"] = 1141835894, ["victory"] = 2, ["undo_pawns"] = {},

["map_data"] = {["version"] = 7, ["dimensions"] = Point( 8, 8 ), ["name"] = "snow4", ["enemy_kills"] = 3, 
["map"] = {{["loc"] = Point( 0, 0 ), ["terrain"] = 4, },
{["loc"] = Point( 0, 1 ), ["terrain"] = 4, },
{["loc"] = Point( 0, 3 ), ["terrain"] = 0, ["cracked"] = true, },
{["loc"] = Point( 1, 0 ), ["terrain"] = 4, },
{["loc"] = Point( 1, 3 ), ["terrain"] = 0, ["cracked"] = true, },
{["loc"] = Point( 1, 5 ), ["terrain"] = 6, },
},
["pod"] = Point(4,1), ["spawns"] = {"Bouncer1", "Scarab1", },
["spawn_ids"] = {4452, 4453, },
["spawn_points"] = {Point(6,5), Point(7,4), },
["zones"] = {},
["tags"] = {"generic", "snow", "water", },

["pawn1"] = {["type"] = "BottlecapMech", ["name"] = "", ["id"] = 0, ["mech"] = true, ["offset"] = 13, 
["reactor"] = {["iNormalPower"] = 0, ["iUsedPower"] = 0, ["iBonusPower"] = 0, ["iUsedBonus"] = 0, ["iUndoPower"] = 0, ["iUsedUndo"] = 0, },
["movePower"] = {0, },
["healthPower"] = {0, },
["primary"] = "Prime_TC_Punt", ["primary_power"] = {},
["primary_power_class"] = false, ["primary_mod1"] = {0, },
["primary_mod2"] = {0, 0, },
["primary_damaged"] = false, ["primary_starting"] = true, ["primary_uses"] = 1, ["secondary"] = "Prime_KO_Crack", ["secondary_power"] = {},
["secondary_power_class"] = false, ["secondary_mod1"] = {0, 0, },
["secondary_mod2"] = {0, },
["secondary_damaged"] = false, ["secondary_starting"] = true, ["secondary_uses"] = 0, ["pilot"] = {["id"] = "Pilot_Miner", ["name"] = "Silica", ["name_id"] = "Pilot_Miner_Name", ["renamed"] = false, ["skill1"] = 5, ["skill2"] = 0, ["exp"] = 50, ["level"] = 2, ["travel"] = 3, ["final"] = 2, ["starting"] = true, ["power"] = {0, 0, },
["last_end"] = 1, },
["iTeamId"] = 1, ["timebonus"] = false, ["iFaction"] = 0, ["iKills"] = 1, ["is_corpse"] = true, ["health"] = 6, ["max_health"] = 6, ["undo_state"] = {["health"] = 5, ["max_health"] = 5, },
["undo_ready"] = false, ["undo_point"] = Point(-1,-1), ["iMissionDamage"] = 0, ["location"] = Point(4,3), ["last_location"] = Point(4,2), ["bActive"] = true, ["iCurrentWeapon"] = 0, ["iTurnCount"] = 2, ["iTurnsRemaining"] = 3, ["undoPosition"] = Point(-1,-1), ["undoReady"] = false, ["iKillCount"] = 6, ["iOwner"] = 0, ["piTarget"] = Point(3,3), ["piOrigin"] = Point(4,3), ["piQueuedShot"] = Point(-1,-1), ["iQueuedSkill"] = -1, ["priorityTarget"] = Point(-1,-1), ["targetHistory"] = Point(3,3), },

["pawn2"] = {["type"] = "JetMech", ["name"] = "", ["id"] = 1, ["mech"] = true, ["offset"] = 1, 
["reactor"] = {["iNormalPower"] = 0, ["iUsedPower"] = 2, ["iBonusPower"] = 0, ["iUsedBonus"] = 1, ["iUndoPower"] = 0, ["iUsedUndo"] = 0, },
["movePower"] = {2, },
["healthPower"] = {1, },
["primary"] = "Brute_Jetmech", ["primary_power"] = {},
["primary_power_class"] = false, ["primary_mod1"] = {0, 0, },
["primary_mod2"] = {0, 0, },
["primary_damaged"] = false, ["primary_starting"] = true, ["primary_uses"] = 1, ["secondary"] = "Support_Missiles", ["secondary_power"] = {1, },
["secondary_power_class"] = false, ["secondary_mod1"] = {0, 0, },
["secondary_mod2"] = {0, },
["secondary_damaged"] = false, ["secondary_starting"] = true, ["secondary_uses"] = 0, ["pilot"] = {["id"] = "Pilot_Detritus", ["name"] = "Maxim Correa", ["name_id"] = "", ["renamed"] = false, ["skill1"] = 3, ["skill2"] = 10, ["exp"] = 47, ["level"] = 1, ["travel"] = 0, ["final"] = 0, ["starting"] = true, },
["iTeamId"] = 1, ["timebonus"] = false, ["iFaction"] = 0, ["iKills"] = 1, ["is_corpse"] = true, ["health"] = 4, ["max_health"] = 4, ["undo_state"] = {["health"] = 5, ["max_health"] = 5, },
["undo_ready"] = false, ["undo_point"] = Point(-1,-1), ["iMissionDamage"] = 0, ["location"] = Point(4,4), ["last_location"] = Point(3,4), ["bActive"] = true, ["iCurrentWeapon"] = 0, ["iTurnCount"] = 2, ["iTurnsRemaining"] = 3, ["undoPosition"] = Point(-1,-1), ["undoReady"] = false, ["iKillCount"] = 0, ["iOwner"] = 1, ["piTarget"] = Point(2,3), ["piOrigin"] = Point(4,4), ["piQueuedShot"] = Point(-1,-1), ["iQueuedSkill"] = -1, ["priorityTarget"] = Point(-1,-1), ["targetHistory"] = Point(2,3), },

["pawn3"] = {["type"] = "SupermanMech", ["name"] = "", ["id"] = 2, ["mech"] = true, ["offset"] = 11, 
["reactor"] = {["iNormalPower"] = 0, ["iUsedPower"] = 1, ["iBonusPower"] = 0, ["iUsedBonus"] = 0, ["iUndoPower"] = 0, ["iUsedUndo"] = 0, },
["movePower"] = {0, },
["healthPower"] = {1, },
["primary"] = "Science_TC_Control", ["primary_power"] = {},
["primary_power_class"] = false, ["primary_mod1"] = {0, },
["primary_mod2"] = {0, 0, },
["primary_damaged"] = false, ["primary_starting"] = true, ["primary_uses"] = 1, ["secondary"] = "Science_Placer", ["secondary_power"] = {},
["secondary_power_class"] = false, ["secondary_mod1"] = {0, },
["secondary_mod2"] = {0, 0, },
["secondary_damaged"] = false, ["secondary_starting"] = true, ["secondary_uses"] = 1, ["pilot"] = {["id"] = "Pilot_Archive", ["name"] = "Nadia Berezin", ["name_id"] = "", ["renamed"] = false, ["skill1"] = 10, ["skill2"] = 6, ["exp"] = 41, ["level"] = 1, ["travel"] = 0, ["final"] = 0, ["starting"] = true, },
["iTeamId"] = 1, ["timebonus"] = false, ["iFaction"] = 0, ["iKills"] = 0, ["is_corpse"] = true, ["health"] = 4, ["max_health"] = 4, ["undo_state"] = {["health"] = 5, ["max_health"] = 5, },
["undo_ready"] = false, ["undo_point"] = Point(-1,-1), ["iMissionDamage"] = 0, ["location"] = Point(3,6), ["last_location"] = Point(2,6), ["bActive"] = true, ["iCurrentWeapon"] = 0, ["iTurnCount"] = 2, ["iTurnsRemaining"] = 3, ["undoPosition"] = Point(-1,-1), ["undoReady"] = false, ["iKillCount"] = 1, ["iOwner"] = 2, ["piTarget"] = Point(5,5), ["piOrigin"] = Point(3,6), ["piQueuedShot"] = Point(-1,-1), ["iQueuedSkill"] = -1, ["priorityTarget"] = Point(-1,-1), ["targetHistory"] = Point(5,5), },

["pawn4"] = {["type"] = "Bouncer1", ["name"] = "", ["id"] = 4211, ["mech"] = false, ["offset"] = 0, ["primary"] = "BouncerAtk1", ["primary_uses"] = 1, ["iTeamId"] = 6, ["timebonus"] = false, ["iFaction"] = 0, ["iKills"] = 0, ["is_corpse"] = false, ["health"] = 2, ["max_health"] = 3, ["undo_state"] = {["health"] = 5, ["max_health"] = 5, },
["undo_ready"] = false, ["undo_point"] = Point(-1,-1), ["iMissionDamage"] = 0, ["location"] = Point(4,5), ["last_location"] = Point(5,5), ["iCurrentWeapon"] = 1, ["iTurnCount"] = 2, ["iTurnsRemaining"] = 4, ["undoPosition"] = Point(-1,-1), ["undoReady"] = false, ["iKillCount"] = 0, ["iOwner"] = 4211, ["piTarget"] = Point(4,4), ["piOrigin"] = Point(4,5), ["piQueuedShot"] = Point(4,4), ["iQueuedSkill"] = 1, ["priorityTarget"] = Point(-1,-1), ["targetHistory"] = Point(4,4), },

["blocked_points"] = {},
["blocked_type"] = {},
},

},
["state"] = 1, ["name"] = "Cold Storage", },

["iBattleRegion"] = 2, }
 

 

SquadData = {
["money"] = 5, ["cores"] = 3, ["bIsFavor"] = false, ["repairs"] = 0, ["RewardClaimed"] = false, 
["skip_pawns"] = true, 

["storage_size"] = 4, ["storage_3"] = {["weapon"] = "Passive_Leech", },
["island_store_count"] = 2, }
 

