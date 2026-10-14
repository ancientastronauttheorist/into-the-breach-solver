-- Offline checks of the Lua bridge (src/bridge/modloader.lua) in the game's
-- Lua version (5.1), against game_mock.lua. Run by itb_tests
-- (test_bridge.cpp) or by hand:
--
--   engine/build/itb_lua51 engine/tests/bridge/bridge_harness.lua \
--       src/bridge/modloader.lua engine/tests/bridge/game_mock.lua <empty dir>
--
-- It loads the real bridge with the game API mocked strictly (luabind
-- arity and types), then checks the pure helpers, a full state dump (old
-- fields still there, new fields right), the hooks the bridge installs, and
-- the commands, including the SCENARIO coroutine with a board that stays
-- busy across frames. Raises with every failed check.

local MODLOADER = HARNESS_MODLOADER or (arg and arg[1])
local MOCK_FILE = HARNESS_MOCK or (arg and arg[2])
local DIR = HARNESS_DIR or (arg and arg[3])
-- Optional: the game's script directory (builds/<build>/scripts or the
-- build root). With it, the final-mission and environment sections run the
-- game's own env_volcano.lua, mission_final.lua, mission_lightning.lua and
-- the Mission / Env_Attack methods from missions.lua / environments.lua.
local GAME_DIR = HARNESS_GAME_DIR or (arg and arg[4])
if GAME_DIR == "" then GAME_DIR = nil end
assert(MODLOADER and MOCK_FILE and DIR, "usage: bridge_harness.lua <modloader.lua> <game_mock.lua> <dir>")

local failures = {}
local checks = 0
local function check(cond, msg)
    checks = checks + 1
    if not cond then failures[#failures + 1] = msg end
end
local function eq(a, b, msg)
    check(a == b, msg .. ": expected " .. tostring(b) .. ", got " .. tostring(a))
end

---------------------------------------------------------------- sandbox
-- The bridge reads its directories from the environment and writes
-- /tmp files by default: point everything at DIR. No shell commands.
local real_getenv = os.getenv
os.getenv = function(name)
    if name == "ITB_BRIDGE_DIR" or name == "ITB_SAVE_DIR" then return DIR end
    return real_getenv(name)
end
local real_execute = os.execute
os.execute = function() return 0 end
local CLOCK = 1000
os.clock = function() return CLOCK end

local function read_file(path)
    local f = io.open(path, "r")
    if not f then return nil end
    local s = f:read("*a")
    f:close()
    return s
end
local function write_file(path, text)
    local f = assert(io.open(path, "w"))
    f:write(text)
    f:close()
end

---------------------------------------------------------------- game data
local MOCK = dofile(MOCK_FILE)

-- Pawn and weapon tables (plain Lua, like the game's scripts).
PunchMech = {Health = 3, MoveSpeed = 3, SkillList = {"Prime_Punchmech"}, Massive = true,
             Corpse = true, DefaultTeam = 1}
LimitMech = {Health = 3, MoveSpeed = 3, SkillList = {"Ranged_Limited"}, Corpse = true}
Firefly1 = {Health = 3, MoveSpeed = 2, SkillList = {"FireflyAtk1"}}
Scorpion1 = {Health = 3, MoveSpeed = 3, SkillList = {"ScorpionAtk1"}}
Jelly_Health1 = {Health = 2, MoveSpeed = 2, SkillList = {}, Leader = 1, Flying = true}
Train_Pawn = {Health = 1, MoveSpeed = 0, SkillList = {"Train_Move"}, DefaultTeam = 1,
              ExtraSpaces = {Point(0, 1)}, Pushable = false}
Prime_Punchmech = {Damage = 2}
Prime_Punchmech_B = {Damage = 3}
Ranged_Limited = {Damage = 2, Limited = 2}
Passive_Electric = {Passive = "Electric_Smoke"}
Passive_Electric_A = {Passive = "Electric_Smoke_A"}
FireflyAtk1 = {Damage = 1}
ScorpionAtk1 = {Damage = 1}
Train_Move = {Damage = 0}

-- Leap weapons with the game's target-area rule (Brute_Jetmech in
-- weapons_brute.lua, Leap_Attack in weapons_base.lua): cardinal tiles
-- MinMove..Range away that are not blocked for the path profile of the
-- global `Pawn`, the last selected pawn. MockLeap: the mock moves the
-- shooter to the target. The bridge must leave these functions alone.
local function cardinal_area(point, min_k, max_k)
    local ret = PointList()
    for i = DIR_START, DIR_END do
        for k = min_k, max_k do
            local d = DIR_VECTORS[i]
            local curr = Point(point.x + d.x * k, point.y + d.y * k)
            if not Board:IsBlocked(curr, Pawn:GetPathProf()) then ret:push_back(curr) end
        end
    end
    return ret
end
JetMech = {Health = 2, MoveSpeed = 4, SkillList = {"Brute_Jetmech"}, Flying = true, Corpse = true,
           DefaultTeam = 1}
Brute_Jetmech = {MinMove = 2, Range = 2, Damage = 1, Smoke = 1, MockLeap = true}
function Brute_Jetmech:GetTargetArea(point) return cardinal_area(point, self.MinMove, self.Range) end
Brute_Bombrun = {MinMove = 2, Range = 8, Damage = 1, Smoke = 0, MockLeap = true,
                 GetTargetArea = Brute_Jetmech.GetTargetArea}
Support_Smoke = {MinMove = 2, Range = 2, Damage = 0, Smoke = 1, MockLeap = true,
                 GetTargetArea = Brute_Jetmech.GetTargetArea}
Prime_Leap = {Range = 7, Damage = 1, MockLeap = true}
function Prime_Leap:GetTargetArea(point) return cardinal_area(point, 1, self.Range) end
Support_Boosters = {Range = 2, Damage = 0, MockLeap = true, GetTargetArea = Prime_Leap.GetTargetArea}
-- A two-click weapon (first click: a target; second: where to).
Science_TC_SwapOther = {TwoClick = true}
SwapMech = {Health = 3, MoveSpeed = 3, SkillList = {"Science_TC_SwapOther"}, Corpse = true, DefaultTeam = 1}
local ORIGINAL_TARGET_AREAS = {}
for _, name in ipairs({"Brute_Jetmech", "Brute_Bombrun", "Support_Smoke", "Prime_Leap", "Support_Boosters"}) do
    ORIGINAL_TARGET_AREAS[name] = _G[name].GetTargetArea
end

-- Board: mechs 0 and 1, a train (main tile (4,4), extra (4,5)), two Vek, a
-- psion; buildings, ice, lava, a custom tile; a lightning mission.
local mech0 = MOCK.add_pawn{id = 0, type = "PunchMech", x = 2, y = 2, team = 1, mech = true, shots = 1}
local mech1 = MOCK.add_pawn{id = 1, type = "LimitMech", x = 3, y = 3, team = 1, mech = true, shots = 2}
local vek_a = MOCK.add_pawn{id = 50, type = "Firefly1", x = 5, y = 5, team = 6, selected_weapon = 1}
local vek_b = MOCK.add_pawn{id = 51, type = "Scorpion1", x = 6, y = 2, team = 6, selected_weapon = 1}
local psion = MOCK.add_pawn{id = 52, type = "Jelly_Health1", x = 7, y = 7, team = 6}
local train = MOCK.add_pawn{id = 60, type = "Train_Pawn", x = 4, y = 4, team = 1}
MOCK.tiles[0][0].terrain = TERRAIN_BUILDING
MOCK.tiles[0][0].hp, MOCK.tiles[0][0].max, MOCK.tiles[0][0].populated = 1, 1, true
MOCK.tiles[1][0].terrain = TERRAIN_BUILDING
MOCK.tiles[1][0].hp, MOCK.tiles[1][0].max, MOCK.tiles[1][0].populated = 2, 2, false
MOCK.tiles[6][6].terrain = TERRAIN_ICE
MOCK.tiles[6][6].hp, MOCK.tiles[6][6].max = 1, 2
MOCK.tiles[7][0].terrain, MOCK.tiles[7][0].lava = TERRAIN_WATER, true
MOCK.tiles[0][7].custom = "conveyor1.png"
MOCK.tiles[1][1].env, MOCK.tiles[5][2].env = true, true
MOCK.tiles[1][5].targeted = true
MOCK.zones.enemy = {{6, 0}, {6, 1}, {7, 1}}
MOCK.zones.deployment = {{1, 3}, {2, 3}}
MOCK.spawns = {{type = "Firefly1", x = 6, y = 3, id = 70}, {type = "Scorpion1", x = 5, y = 1, id = 71}}

local M = Mission_Lightning:new()
M.ID = "Mission_Lightning"
M.TurnLimit = 5
M.BonusObjs = {5, 6}
M.PowerStart = 6
M.BlockedSpawns = 1
M.KilledVek = 2
M.Spawner = {num_spawns = 5, pawn_counts = {Firefly = 2}}
M.LiveEnvironment = Env_Lightning:new({Locations = {Point(1, 1), Point(5, 2)}, Planned = {Point(1, 1), Point(5, 2)}})
GAME = {Missions = {[0] = Mission:new{ID = "Mission_Final"}, [3] = M}}

-- Save: the active region is 3; region 1 is a decoy whose pawn 0 has
-- other weapons.
local function pawn_block(n, body) return '["pawn' .. n .. '"] = {' .. body .. '},\n' end
local region3 = '["region3"] = {["mission"] = "Mission3", ["player"] = {["iCurrentTurn"] = 1, '
    .. '["iTeamTurn"] = 1, ["map_data"] = {\n'
    .. pawn_block(1, '["type"] = "PunchMech", ["id"] = 0, ["mech"] = true, '
        .. '["primary"] = "Prime_Punchmech", ["primary_power"] = {}, ["primary_mod1"] = {0, 0, }, '
        .. '["primary_mod2"] = {1, 1, 1, }, ["primary_uses"] = 1, ["secondary"] = "Passive_Electric", '
        .. '["secondary_power"] = {1, }, ["secondary_mod1"] = {1, }, ["secondary_mod2"] = {}, '
        .. '["secondary_uses"] = 1, ["pilot"] = {["id"] = "Pilot_Original", ["exp"] = 17, ["level"] = 1, '
        .. '["skill1"] = 7, ["skill2"] = 12, }, ["iMutation"] = 0, ["iQueuedSkill"] = -1, '
        .. '["health"] = 3, ["max_health"] = 5')
    .. pawn_block(2, '["type"] = "LimitMech", ["id"] = 1, ["mech"] = true, '
        .. '["primary"] = "Ranged_Limited", ["primary_mod1"] = {0, }, ["primary_mod2"] = {0, }, '
        .. '["primary_uses"] = 2, ["primary_damaged"] = false, '
        .. '["pilot"] = {["id"] = "Pilot_Rock", ["exp"] = 3, ["level"] = 0, ["skill1"] = 1, ["skill2"] = 2, }, '
        .. '["iQueuedSkill"] = -1')
    .. pawn_block(3, '["type"] = "Firefly1", ["id"] = 50, ["mech"] = false, ["iMutation"] = 1, '
        .. '["piTarget"] = Point(5,4), ["piOrigin"] = Point(5,5), ["piQueuedShot"] = Point(5,4), ["iQueuedSkill"] = 1')
    .. pawn_block(4, '["type"] = "Scorpion1", ["id"] = 51, ["mech"] = false, ["iMutation"] = 1, '
        .. '["piTarget"] = Point(6,3), ["piOrigin"] = Point(6,2), ["piQueuedShot"] = Point(-1,-1), ["iQueuedSkill"] = 1')
    .. pawn_block(5, '["type"] = "Train_Pawn", ["id"] = 60, ["mech"] = false, '
        .. '["piTarget"] = Point(4,3), ["piOrigin"] = Point(4,4), ["piQueuedShot"] = Point(4,3), ["iQueuedSkill"] = 1')
    .. '["pawn_count"] = 5, ["blocked_points"] = {Point(1,1), Point(5,2), },\n'
    .. '["blocked_type"] = {1, 1, },\n'
    .. '["spawns"] = {"Firefly1", "Scorpion1", },\n'
    .. '["spawn_ids"] = {70, 71, },\n'
    .. '["spawn_points"] = {Point(6,3), Point(5,1), },\n'
    .. '}, }, }, \n'
local region1 = '["region1"] = {["mission"] = "Mission1", ["player"] = {["iCurrentTurn"] = 3, ["map_data"] = {\n'
    .. pawn_block(1, '["type"] = "PunchMech", ["id"] = 0, ["primary"] = "Decoy_Gun", ["primary_uses"] = 1')
    .. '["spawns"] = {"Decoy1", },\n["spawn_ids"] = {9, },\n["spawn_points"] = {Point(0,0), },\n'
    .. '}, }, }, \n'
local save_dir = DIR .. "/profile_Alpha"
if not io.open(save_dir .. "/.probe", "w") then
    real_execute('mkdir -p "' .. save_dir .. '"')
end
write_file(save_dir .. "/saveData.lua",
    'GameData = {["network"] = 5, ["networkMax"] = 7, ["overflow"] = 12, ["difficulty"] = 1, ["seed"] = 42, '
    .. '["weapons"] = {"Prime_Punchmech_B", "Passive_Electric_A", "Ranged_Limited", "", "", "", }, }\n'
    .. 'RegionData = {\n' .. region1 .. region3 .. '["iBattleRegion"] = 3, }\n')

---------------------------------------------------------------- load
local chunk, load_err = loadfile(MODLOADER)
assert(chunk, "modloader.lua does not compile: " .. tostring(load_err))
chunk()
local X = _ITB_BRIDGE_EXT
assert(type(X) == "table", "bridge extension table missing")

eq(#MOCK.mismatches, 0, "strict-call mismatches while loading")
-- The bridge observes and commands; it never rewrites the game's scripts.
for name, fn in pairs(ORIGINAL_TARGET_AREAS) do
    check(rawequal(_G[name].GetTargetArea, fn), name .. ".GetTargetArea untouched by the bridge")
end
eq(_ITB_BRIDGE_SAFE_JET_TARGET_AREA, nil, "no target-area patch installed")
eq(_ITB_BRIDGE_SAFE_LEAP_TARGET_AREA, nil, "no leap target-area patch installed")
eq(X.exec_mode, "native", "native execution by default")
check(Mission.BaseUpdate ~= nil and Mission.BaseNextTurn ~= nil, "Mission hooks present")
check(_ITB_BRIDGE_ORIGINALS.BaseNextTurn ~= Mission.BaseNextTurn, "BaseNextTurn wrapped")
check(_ITB_BRIDGE_ORIGINALS.ApplyEnvironmentEffect ~= Mission.ApplyEnvironmentEffect, "ApplyEnvironmentEffect wrapped")
check(_ITB_BRIDGE_ORIGINALS.PlanEnvironment ~= Mission.PlanEnvironment, "PlanEnvironment wrapped")

---------------------------------------------------------------- helpers
do
    local v = X.json_decode('{"a": [1, 2.5, -3e2, true, false, null, "x\\"y\\n\\u0041"], "b": {}, "c": {"d": null}}')
    eq(v.a[1], 1, "json int")
    eq(v.a[2], 2.5, "json float")
    eq(v.a[3], -300, "json exponent")
    eq(v.a[4], true, "json true")
    eq(v.a[5], false, "json false")
    check(rawequal(v.a[6], X.JSON_NULL), "json null kept in arrays")
    eq(v.a[7], 'x"y\nA', "json escapes")
    eq(next(v.b), nil, "json empty object")
    eq(v.c.d, nil, "json null dropped from objects")
    for _, bad in ipairs({'{"a": }', '[1, 2', '{"a" 1}', '"abc', '{} x'}) do
        check(not pcall(X.json_decode, bad), "json rejects " .. bad)
    end

    local cyc = {name = "c"}
    cyc.self = cyc
    local ctx = X.new_dump_ctx()
    local d = X.dump_value({p = Point(3, 4), list = {Point(1, 2), Point(3, 4)}, f = function() end,
                           nan = 0 / 0, inf = math.huge, cyc = cyc, deep = {{{{{1}}}}},
                           mixed = {1, 2, x = 3}, [7] = "seven"}, 0, ctx)
    eq(d.p.x, 3, "dump Point x")
    eq(d.list[2].y, 4, "dump Point array")
    eq(d.f, nil, "dump skips functions")
    eq(d.nan, "nan", "dump NaN as string")
    eq(d.inf, "inf", "dump inf as string")
    eq(d.cyc.self, "<cycle>", "dump cycle")
    eq(d.deep[1][1][1], "<depth>", "dump depth limit")
    eq(d.mixed.x, 3, "mixed table becomes object")
    eq(d.mixed["1"], 1, "mixed table keeps array part")
    eq(d["7"], "seven", "numeric key as string")
    check(ctx.truncated, "depth limit flagged")

    local save = X.parse_save(read_file(save_dir .. "/saveData.lua"))
    eq(save.battle_region, 3, "save battle region")
    eq(save.mission, "Mission3", "save region mission")
    eq(save.turn, 1, "save region turn")
    eq(#save.spawns, 2, "save spawn count")
    eq(save.spawns[1].type, "Firefly1", "save spawn 1 type")
    eq(save.spawns[2].x, 5, "save spawn 2 x")
    eq(save.spawns[2].uid, 71, "save spawn 2 uid")
    eq(save.pawns[0].weapons[1].base, "Prime_Punchmech", "save uses the active region's pawn 0")
    eq(save.pawns[0].pilot.xp, 17, "save pilot xp")
    eq(save.pawns[51].queued_target[1], 6, "save piTarget")
    eq(save.spawn_blocks[2][3], 1, "save blocked spawn type")

    eq(X.exact_weapon_id({base = "Prime_Punchmech", mod1 = {0, 0}, mod2 = {1, 1, 1}}), "Prime_Punchmech_B", "exact id _B")
    eq(X.exact_weapon_id({base = "Passive_Electric", mod1 = {1}, mod2 = {}}), "Passive_Electric_A", "exact id passive _A")
    eq(X.exact_weapon_id({base = "Prime_Punchmech", mod1 = {}, mod2 = {}}), "Prime_Punchmech", "exact id base")

    -- One limited weapon (2 saved) + one passive: 2 shots left reads as
    -- 2 (passive not a skill) or 1 (passive a skill): ambiguous until
    -- calibrated.
    local ws = {{slot = 0, base = "Ranged_Limited", mod1 = {}, mod2 = {}, uses = 2},
                {slot = 1, base = "Passive_Electric", mod1 = {}, mod2 = {}, uses = 1}}
    local slots = X.weapon_slots(ws, 2)
    check(slots[1].uses_ambiguous, "limited + passive uncalibrated is ambiguous")
    slots = X.weapon_slots(ws, 2, 1)
    eq(slots[1].uses, 1, "calibrated limited uses")
    eq(slots[1].uses_basis, "calibrated", "calibrated basis")
    slots = X.weapon_slots({ws[1]}, 1)
    eq(slots[1].uses, 1, "single limited weapon")
    eq(slots[1].limited, 2, "limited count from the weapon table")
end

---------------------------------------------------------------- dump
local function load_state(path)
    local text = read_file(path or (DIR .. "/itb_state.json"))
    assert(text, "no state file at " .. tostring(path))
    local ok, st = pcall(X.json_decode, text)
    assert(ok, "state is not valid JSON: " .. tostring(st))
    return st
end
local function unit(st, uid, extra)
    for _, u in ipairs(st.units) do
        if u.uid == uid and (u.is_extra_tile == true) == (extra == true) then return u end
    end
    return nil
end

_ITB_CURRENT_MISSION = M
X._dump_state()
eq(#MOCK.mismatches, 0, "strict-call mismatches during the dump")
local st = load_state()
do
    -- Old fields.
    eq(#st.tiles, 64, "64 tiles")
    eq(st.phase, "combat_player", "phase")
    eq(st.grid_power, 5, "grid from the save")
    eq(st.grid_overflow, 12, "Grid Defense overpower bonus from the save")
    eq(st.mission_id, "Mission_Lightning", "mission id")
    eq(st.total_turns, 5, "TurnLimit")
    check(unit(st, 60, true) ~= nil, "train extra tile still emitted")
    eq(#st.attack_order, 2, "attack_order: two Vek")
    eq(#st.environment_danger_v2, 2, "env danger")
    eq(#st.spawning_tiles, 2, "spawning tiles")
    -- New fields.
    eq(#st.bridge_errors, 0, "bridge_errors empty (" .. (st.bridge_errors[1] and st.bridge_errors[1].where .. ": " .. st.bridge_errors[1].error or "") .. ")")
    eq(st.bridge_ext_version, 2, "ext version")
    eq(st.exec_mode, "native", "exec mode in the state")
    eq(st.bridge_debug, false, "debug off")
    local ms = st.mission_state
    check(ms ~= nil, "mission_state present")
    if ms then
        eq(ms.key, 3, "mission key")
        eq(ms.native_key, "Mission3", "native key")
        eq(ms.class_chain[1], "Mission_Lightning", "mission class")
        eq(ms.class_chain[2], "Mission", "mission base class")
        eq(ms.env_class_chain[1], "Env_Lightning", "env class")
        eq(ms.env_class_chain[2], "Env_Attack", "env base class")
        eq(ms.turn_limit, 5, "turn limit")
        eq(ms.instance.BlockedSpawns, 1, "instance field")
        eq(ms.instance.LiveEnvironment, nil, "LiveEnvironment not inside the mission dump")
        eq(ms.instance.Spawner.num_spawns, 5, "nested instance table")
        eq(ms.env_instance.Locations[2].x, 5, "env Locations as points")
        eq(ms.sector, 2, "sector")
    end
    eq(#st.spawn_queue, 2, "spawn queue")
    eq(st.spawn_queue[1].type, "Firefly1", "spawn queue order")
    eq(st.spawn_queue_source, "save", "spawn queue source")
    eq(st.spawn_queue_matches_markers, true, "queue matches markers")
    eq(st.mission_power_start, 6, "PowerStart")
    eq(st.mission_blocked_spawns, 1, "BlockedSpawns with BONUS_BLOCK")
    eq(st.bonus_objective_ids[2], 6, "bonus ids")
    eq(st.zones.enemy[3][1], 7, "zone points")
    local m0 = unit(st, 0)
    eq(m0.weapons_exact[1], "Prime_Punchmech_B", "mech 0 exact primary")
    eq(m0.weapons_exact[2], "Passive_Electric_A", "mech 0 exact passive")
    eq(m0.weapons[1], "Prime_Punchmech", "old weapons field untouched")
    eq(m0.pilot.xp, 17, "pilot xp")
    eq(m0.pilot.skill2, 12, "pilot skill2")
    eq(m0.pilot_skills[1], "skill1=7", "old pilot_skills untouched")
    -- Pawn:GetMaxHealth is unbound: max_hp comes from the save's
    -- max_health (bonuses included), else the type's Health.
    eq(m0.max_hp, 5, "mech max_hp from the save's max_health")
    eq(unit(st, 1).max_hp, 3, "no save max_health: the type's Health")
    eq(st.stable, true, "idle board: stable dump")
    eq(st.board_busy, false, "idle board: not busy")
    eq(st.busy_state, 0, "idle board: busy_state 0")
    check(type(st.dump_seq) == "number", "dump_seq")
    local m1 = unit(st, 1)
    eq(m1.shots_remaining, 2, "shots remaining")
    eq(m1.weapon_slots[1].uses, 2, "limited uses (calibrated at turn start)")
    eq(m1.weapon_slots[1].uses_basis, "calibrated", "uses basis")
    local t = unit(st, 60)
    eq(t.queued_any.target[2], 3, "train queued shot")
    eq(t.extra_spaces[1][2], 1, "train extra space")
    eq(t.has_queued_attack, nil, "train has no old-style queued fields")
    local p = unit(st, 52)
    eq(p.traits.leader, 1, "psion Leader trait")
    eq(p.traits.flying, true, "flying trait")
    eq(unit(st, 50).mutation, 1, "mutation from the save")
    eq(st.attack_order_all[1], 50, "attack_order_all: Vek first")
    eq(st.attack_order_all[3], 60, "attack_order_all: train last")
    local by = {}
    for _, tl in ipairs(st.tiles) do by[tl.x .. "," .. tl.y] = tl end
    eq(by["6,6"].ice_hp, 1, "cracked ice hp")
    eq(by["0,7"].custom_tile, "conveyor1.png", "custom tile")
    eq(by["0,0"].populated, true, "populated building")
    eq(by["1,0"].populated, false, "unpopulated building")
    eq(by["7,0"].terrain, "lava", "lava tile")
end

-- The second mech fires its limited weapon: the save stays, live uses drop.
MOCK.data(mech1).shots = 1
MOCK.data(mech1).active = false
X._dump_state()
st = load_state()
eq(unit(st, 1).weapon_slots[1].uses, 1, "limited uses after a shot")

---------------------------------------------------------------- hooks
do
    MOCK.hook_calls = {}
    local r1 = Mission.ApplyEnvironmentEffect(M)
    local r2 = Mission.ApplyEnvironmentEffect(M)
    eq(r1, true, "env step 1 result passed through")
    eq(r2, false, "env step 2 result passed through")
    eq(#MOCK.hook_calls, 2, "original ApplyEnvironmentEffect called once per step")
    X._dump_state()
    st = load_state()
    eq(#st.env_strike_log, 2, "two env steps logged")
    eq(st.env_strike_log[1].current_attack.x, 5, "first strike (5,2)")
    eq(st.env_strike_log[2].current_attack.x, 1, "second strike (1,1)")
    eq(st.env_strike_log[1].returned, true, "step 1 returned true")

    -- Debug captures around the enemy phase.
    write_file(X.DEBUG_FLAG_FILE, "1")
    X._debug_at = nil
    MOCK.team = 6
    MOCK.hook_calls = {}
    Mission.BaseNextTurn(M)
    eq(MOCK.hook_calls[1], "BaseNextTurn", "original BaseNextTurn ran")
    check(read_file(X.PRE_SPAWN_FILE) ~= nil, "pre-spawn capture written")
    local ret = Mission.PlanEnvironment(M)
    eq(ret, "plan-result", "PlanEnvironment result passed through")
    check(read_file(X.POST_SPAWN_FILE) ~= nil, "post-spawn capture written")
    local pre = load_state(X.PRE_SPAWN_FILE)
    eq(pre.phase, "combat_enemy", "pre-spawn capture in the enemy phase")
    os.remove(X.POST_SPAWN_FILE)
    Mission.PlanEnvironment(M)
    eq(read_file(X.POST_SPAWN_FILE), nil, "only the first PlanEnvironment is captured")
    MOCK.team = 1

    -- Per-frame log: HP change and a selected shooter.
    _ITB_BRIDGE_PHASE_LOG = nil
    Mission.BaseUpdate(M)
    MOCK.data(vek_a).hp = 2
    MOCK.data(vek_b).selected = true
    Mission.BaseUpdate(M)
    local log = _ITB_BRIDGE_PHASE_LOG
    local kinds = {}
    for _, e in ipairs(log.entries) do kinds[e.kind] = (kinds[e.kind] or 0) + 1 end
    eq(kinds.hp, 1, "hp change logged")
    eq(kinds.selected, 1, "shooter selection logged")
    MOCK.data(vek_b).selected = false
    os.remove(X.DEBUG_FLAG_FILE)
    X._debug_at = nil
end

-- Grid Defense base: the difficulty the power bar was built under.
do
    eq(st.grid_defense_base, nil, "no Grid Defense base before a game is built")
    local real_difficulty, real_game_data = GetDifficulty, rawget(_G, "GameData")
    -- Continue after a restart: Game::LoadGame runs the save (a new GameData
    -- table, then CreateEffect) under the settings file's Hard; then
    -- GameMap::LoadGame sets the save's Unfair.
    GetDifficulty = function() return 2 end
    GameData = {overflow = 0}
    local e = CreateEffect({cores = 1})
    eq(e.effect.cores, 1, "CreateEffect result passed through")
    GetDifficulty = function() return 3 end
    CreateEffect({})
    X._dump_state()
    st = load_state()
    eq(st.grid_defense_base, 15, "continued under Hard settings: base 15")
    eq(st.grid_base_difficulty, 2, "difficulty behind the base")
    -- Reset Turn: a new map and the undo save under the save's difficulty.
    GameData = {overflow = 0}
    CreateEffect({})
    X._dump_state()
    st = load_state()
    eq(st.grid_defense_base, 0, "Reset Turn under Unfair: no base")
    -- A new run chosen on Hard.
    GetDifficulty = function() return 2 end
    local before = MOCK_NEW_GAMES or 0
    startNewGame()
    eq(MOCK_NEW_GAMES, before + 1, "original startNewGame ran")
    X._dump_state()
    st = load_state()
    eq(st.grid_defense_base, 15, "new run on Hard: base 15")
    GetDifficulty, GameData = real_difficulty, real_game_data
end

---------------------------------------------------------------- commands
local CUR_M = M  -- the mission BaseUpdate runs for (the final section swaps it)
local function run_command(cmd, max_frames)
    os.remove(DIR .. "/itb_ack.txt")
    write_file(DIR .. "/itb_cmd.txt", "#7 " .. cmd)
    for _ = 1, (max_frames or 200) do
        CLOCK = CLOCK + 1
        Mission.BaseUpdate(CUR_M)
        local ack = read_file(DIR .. "/itb_ack.txt")
        if ack and read_file(DIR .. "/itb_cmd.txt") == nil then
            -- The coroutine may still be running if the ack came early;
            -- run frames until it is done.
            for _ = 1, 20 do
                CLOCK = CLOCK + 1
                Mission.BaseUpdate(CUR_M)
            end
            return ack
        end
    end
    return nil
end

do
    local ack = run_command("SNAPSHOT probe one")
    eq(ack, "#7 OK SNAPSHOT " .. X.SNAPSHOT_PREFIX .. "probe.json", "SNAPSHOT ack")
    check(read_file(X.SNAPSHOT_PREFIX .. "probe.json") ~= nil, "snapshot file")

    ack = run_command("SCENARIO {\"name\": \"x\"}")
    check(ack and string.find(ack, "ERROR: SCENARIO disabled", 1, true) ~= nil, "SCENARIO needs the debug flag")

    -- Old command path unchanged.
    MOCK.data(mech0).active = true
    ack = run_command("SKIP 0")
    eq(ack, "#7 OK SKIP 0", "SKIP ack")
    eq(MOCK.data(mech0).active, false, "SKIP deactivates")
    MOCK.data(mech0).active = true

    write_file(X.DEBUG_FLAG_FILE, "1")
    X._debug_at = nil
    ack = run_command("DEBUG_STATUS")
    check(ack and string.find(ack, '"debug":true', 1, true) ~= nil, "DEBUG_STATUS")

    local scenario = [[{
      "name": "vek order",
      "clear_pawns": "all",
      "clear_spawns": true,
      "tiles": [
        {"x": 4, "y": 1, "terrain": "building", "hp": 1, "populated": true},
        {"x": 2, "y": 6, "fire": true},
        {"x": 3, "y": 6, "smoke": true, "acid": true},
        {"x": 0, "y": 4, "terrain": "water"},
        {"x": 5, "y": 6, "terrain": "ice", "hp": 1}
      ],
      "pawns": [
        {"uid": 0, "x": 4, "y": 2, "hp": 2, "shield": true},
        {"type": "Firefly1", "team": 6, "x": 4, "y": 5, "fire": true, "queue": {"x": 4, "y": 4}},
        {"type": "Scorpion1", "team": 6, "x": 5, "y": 2, "acid": true, "queue": {"x": 4, "y": 2}},
        {"type": "Firefly1", "team": 6, "x": 1, "y": 2, "frozen": true}
      ],
      "spawns": [{"type": "Scorpion1", "x": 6, "y": 6}]
    }]]
    write_file(DIR .. "/scenario.json", scenario)
    MOCK.calls = {}
    MOCK.busy_after_mutation = 3
    ack = run_command("SCENARIO @" .. DIR .. "/scenario.json", 400)
    check(ack ~= nil, "SCENARIO acked")
    ack = ack or ""
    check(string.find(ack, "#7 OK SCENARIO ", 1, true) == 1, "SCENARIO ok: " .. ack)
    local report = X.json_decode(string.sub(ack, string.len("#7 OK SCENARIO ") + 1))
    eq(#report.errors, 0, "SCENARIO errors (" .. tostring(report.errors[1]) .. ")")
    eq(#report.created, 3, "three pawns created")
    eq(#MOCK.mismatches, 0, "strict-call mismatches during SCENARIO (" .. tostring(MOCK.mismatches[1]) .. ")")

    -- The board.
    eq(MOCK.find_pawn(50), nil, "old Vek removed")
    eq(MOCK.find_pawn(60), nil, "train removed (clear_pawns all)")
    check(MOCK.find_pawn(1) ~= nil, "mechs kept")
    local d0 = MOCK.data(mech0)
    eq(d0.x * 10 + d0.y, 42, "mech moved to (4,2)")
    eq(d0.hp, 2, "mech hp")
    eq(d0.shield, true, "mech shield")
    eq(MOCK.tiles[4][1].terrain, TERRAIN_BUILDING, "building placed")
    eq(MOCK.tiles[2][6].fire, true, "tile fire")
    eq(MOCK.tiles[0][4].terrain, TERRAIN_WATER, "water")
    eq(MOCK.tiles[5][6].hp, 1, "cracked ice")
    local ff = MOCK.pawn_at(Point(4, 5))
    check(ff ~= nil and MOCK.data(ff).fire == true, "burning Firefly")
    eq(MOCK.tiles[4][5].fire, false, "its tile does not burn")
    local fz = MOCK.pawn_at(Point(1, 2))
    check(fz ~= nil and MOCK.data(fz).frozen == true, "frozen Firefly")
    eq(#MOCK.spawns, 1, "old spawns cleared, one queued")
    eq(MOCK.spawns[1].x, 6, "queued spawn")

    -- The dump that follows is the engine's input.
    st = load_state()
    eq(#st.bridge_errors, 0, "bridge_errors after SCENARIO")
    local created = report.created
    local u1 = unit(st, created[1].uid)
    eq(u1.has_queued_attack, true, "scenario queue overlay")
    eq(u1.queued_target[2], 4, "scenario queued target")
    eq(u1.queued_source, "scenario", "queued source")
    eq(st.attack_order[1], created[1].uid, "attack order = creation order (1)")
    eq(st.attack_order[2], created[2].uid, "attack order = creation order (2)")
    eq(#st.attack_order, 2, "frozen pawn has no queued attack")
    eq(st.spawn_queue_source, "scenario", "spawn queue from the scenario")
    eq(#st.spawn_queue, 1, "spawn queue length")
    eq(st.spawn_queue[1].type, "Scorpion1", "spawn type")
    eq(st.spawn_queue_matches_markers, true, "scenario queue matches markers")
    eq(st.scenario.name, "vek order", "scenario echo")
    check(st.phase_log ~= nil, "phase log exported with the debug flag")
    check(read_file(X.SNAPSHOT_PREFIX .. "scenario_vek_order.json") ~= nil, "scenario snapshot file")

    ack = run_command("PHASE_LOG")
    check(ack and string.find(ack, "OK PHASE_LOG", 1, true) ~= nil, "PHASE_LOG ack")
    check(read_file(X.PHASE_LOG_FILE) ~= nil, "phase log file")
    ack = run_command("SCENARIO_RESET")
    eq(ack, "#7 OK SCENARIO_RESET", "SCENARIO_RESET")
    eq(_ITB_BRIDGE_SCENARIO, nil, "scenario ledger cleared")

    ack = run_command("SCENARIO {\"pawns\": [{\"type\": \"Firefly1\", \"x\": 4, \"y\": 2}]}")
    check(ack and string.find(ack, "OK SCENARIO", 1, true) ~= nil, "SCENARIO inline JSON")
    report = X.json_decode(string.sub(ack or "", string.len("#7 OK SCENARIO ") + 1))
    check(report.errors and string.find(report.errors[1] or "", "occupied", 1, true) ~= nil,
          "placing on an occupied tile is reported")
    ack = run_command("SCENARIO {bad json")
    check(ack and string.find(ack, "ERROR: SCENARIO payload", 1, true) ~= nil, "bad JSON rejected")
    MOCK.team = 6
    ack = run_command("SCENARIO {}")
    check(ack and string.find(ack, "not the player turn", 1, true) ~= nil, "SCENARIO only in the player turn")
    MOCK.team = 1
    os.remove(X.DEBUG_FLAG_FILE)
end

---------------------------------------------------------------- live findings
-- moved flag, MOVE_NATIVE, FIRE, re-added pawns, weapons_add, injured,
-- tiles_after, the drop zone, saveData vs undoSave, powered weapons, tile
-- changes in the phase log.
do
    write_file(X.DEBUG_FLAG_FILE, "1")
    X._debug_at = nil
    MOCK.team = 1
    MOCK.data(mech0).active = true
    MOCK.data(mech1).active = true
    Mission.BaseNextTurn(M)  -- the player's turn begins: positions recorded
    local ack = run_command("MOVE_NATIVE 0 4 3")
    check(ack and string.find(ack, "[FireWeapon[0]] at 4,3", 1, true) ~= nil, "MOVE_NATIVE through the Move skill: " .. tostring(ack))
    eq(MOCK.fired[#MOCK.fired].slot, 0, "MOVE_NATIVE fires slot 0")
    eq(MOCK.fired[#MOCK.fired].selected, 0, "MOVE_NATIVE selects the pawn (global Pawn) first")
    MOCK.data(mech1).active = false
    -- Native mode: the game refused the move, so the bridge refuses too.
    local before1 = {x = MOCK.data(mech1).x, y = MOCK.data(mech1).y}
    ack = run_command("MOVE_NATIVE 1 3 4")
    check(ack and string.find(ack, "ERROR: MOVE_NATIVE FireWeapon[0] returned 0", 1, true) ~= nil,
          "MOVE_NATIVE refused by the game: error, no fallback: " .. tostring(ack))
    check(MOCK.data(mech1).x == before1.x and MOCK.data(mech1).y == before1.y, "refused MOVE_NATIVE: the pawn did not move")
    -- Legacy mode (the old bot's opt-in) keeps the Pawn:Move fallback.
    eq(run_command("EXEC_MODE legacy"), "#7 OK EXEC_MODE legacy", "EXEC_MODE legacy")
    eq(load_state().exec_mode, "legacy", "state reports the exec mode")
    ack = run_command("MOVE_NATIVE 1 3 4")
    check(ack and string.find(ack, "[Move] at 3,4", 1, true) ~= nil, "legacy MOVE_NATIVE falls back to Pawn:Move: " .. tostring(ack))
    eq(run_command("EXEC_MODE native"), "#7 OK EXEC_MODE native", "EXEC_MODE native")
    eq(run_command("EXEC_MODE"), "#7 OK EXEC_MODE native", "EXEC_MODE query")
    check(string.find(run_command("EXEC_MODE fast") or "", "ERROR", 1, true) ~= nil, "EXEC_MODE rejects unknown modes")
    st = load_state()
    eq(unit(st, 0).moved, true, "moved after MOVE_NATIVE")
    eq(unit(st, 0).moved_source, "turn_start", "moved from the turn-start positions")
    eq(unit(st, 1).moved, true, "moved by position")
    MOCK.data(mech1).x, MOCK.data(mech1).y = 3, 3
    st = load_state((function() X._dump_state() return nil end)())
    eq(unit(st, 1).moved, false, "back on its turn-start tile and no undo: not moved")

    ack = run_command("FIRE 1 1 3 5")
    check(ack and string.find(ack, "OK FIRE 1 slot=1 at 3,5 ret=1", 1, true) ~= nil, "FIRE: " .. tostring(ack))

    local spec = [[{"name": "readd", "clear_pawns": "all",
      "pawns": [
        {"type": "Firefly1", "team": 6, "x": 6, "y": 1, "queue": {"x": 6, "y": 2}},
        {"type": "Scorpion1", "team": 6, "x": 6, "y": 4, "queue": {"x": 6, "y": 5}},
        {"ref": 1, "readd": true},
        {"type": "PunchMech", "team": 1, "x": 0, "y": 5, "weapons_add": ["Ranged_Limited"], "injured": true}],
      "tiles_after": [{"x": 0, "y": 5, "fire": true}]}]]
    ack = run_command("SCENARIO " .. spec, 400) or ""
    check(string.find(ack, "OK SCENARIO", 1, true) ~= nil, "SCENARIO readd: " .. ack)
    local report = X.json_decode(string.sub(ack, string.len("#7 OK SCENARIO ") + 1))
    eq(#report.errors, 0, "readd scenario errors (" .. tostring(report.errors[1]) .. ")")
    st = load_state()
    eq(st.attack_order[1], report.created[2].uid, "re-added pawn attacks last (1)")
    eq(st.attack_order[2], report.created[1].uid, "re-added pawn attacks last (2)")
    local helper = MOCK.find_pawn(report.created[3].uid)
    eq(MOCK.data(helper).added[1], "Ranged_Limited", "weapons_add")
    eq(MOCK.data(helper).injured, true, "injured via iInjure")
    eq(MOCK.tiles[0][5].fire, true, "tiles_after")

    -- Drop zone during deployment (turn 0).
    MOCK.turn = 0
    X._dump_state()
    st = load_state()
    eq(st.deploying, true, "deploying at turn 0")
    eq(st.drop_zone_source, "zone+columns", "two zone tiles: whole columns added")
    check(#st.drop_zone > 3, "drop zone has more than 3 tiles")
    MOCK.zones.deployment = {}
    X._dump_state()
    st = load_state()
    eq(st.drop_zone_source, "default", "no zone: default columns 1-3")
    check(st.deployment_zone ~= nil and #st.deployment_zone > 0, "deployment_zone filled from the drop zone")
    for _, p in ipairs(st.drop_zone) do
        check(p[1] >= 1 and p[1] <= 3 and p[2] >= 1 and p[2] <= 6, "default drop zone tile in x 1-3, y 1-6")
    end
    MOCK.turn = 1

    -- undoSave.lua holding a later turn wins; powered weapons.
    Prime_Punchmech_A = {Damage = 2}
    Passive_Electric.PowerCost = 1
    Ranged_Limited.PowerCost = 1
    local undo = read_file(save_dir .. "/saveData.lua")
    undo = string.gsub(undo, '%["iCurrentTurn"%] = 1', '["iCurrentTurn"] = 2', 1)
    undo = string.gsub(undo, '%["primary_mod1"%] = {0, 0, }, %["primary_mod2"%] = {1, 1, 1, }',
                       '["primary_mod1"] = {1, 1, }, ["primary_mod2"] = {0, 0, 0, }', 1)
    write_file(save_dir .. "/undoSave.lua", undo)
    X._dump_state()
    st = load_state()
    eq(st.save_source, "undoSave.lua", "the later save is used")
    eq(unit(st, 0).weapons_exact[1], "Prime_Punchmech_A", "upgrade from undoSave")
    eq(unit(st, 0).weapon_slots[2].powered, true, "powered passive (1 core, cost 1)")
    eq(unit(st, 1).weapon_slots[1].powered, false, "unpowered weapon (0 cores, cost 1)")
    os.remove(save_dir .. "/undoSave.lua")

    -- Tile changes in the phase log.
    Mission.BaseUpdate(M)
    MOCK.tiles[0][0].hp = 0
    MOCK.tiles[0][0].terrain = TERRAIN_RUBBLE
    Mission.BaseUpdate(M)
    local found = false
    for _, e in ipairs(_ITB_BRIDGE_PHASE_LOG.entries) do
        if e.kind == "tile" and e.x == 0 and e.y == 0 then found = true end
    end
    check(found, "building damage logged per frame")
    os.remove(X.DEBUG_FLAG_FILE)
    X._debug_at = nil
end

---------------------------------------------------------------- settled dumps
-- Live 2026-10-09 (Mission_Final_Cave turns 1 and 6): ATTACK with
-- Ranged_Ignite acked at the 2 s fast-mode cap while its side pushes were
-- still running; the dump showed the burning target but the pushed Vek on
-- their old tiles, and the next dump had them moved ("flip-flops").
do
    local real_time = os.time
    local WALL = 100000
    os.time = function() return WALL end
    -- One frame = one wall second here, so the waits' budgets are frames.
    -- dt advances os.clock: 0.25 polls commands, 0 neither polls nor
    -- reaches the 5 s periodic dump.
    local function frame(dt)
        WALL = WALL + 1
        CLOCK = CLOCK + (dt or 0.25)
        Mission.BaseUpdate(CUR_M)
    end
    local function attack(cmd, max_frames)
        os.remove(DIR .. "/itb_ack.txt")
        write_file(DIR .. "/itb_cmd.txt", "#7 " .. cmd)
        for i = 1, max_frames do
            frame()
            local ack = read_file(DIR .. "/itb_ack.txt")
            if ack then return ack, i end
        end
        return nil, max_frames
    end
    MOCK.busy, MOCK.busy_after_mutation = 0, 0
    frame(10)  -- the periodic dump, out of the way

    -- A dump while busy is marked, and replaced once the board is idle.
    MOCK.busy = 3
    X._dump_state()
    st = load_state()
    eq(st.board_busy, true, "busy dump: board_busy")
    eq(st.busy_state, 6, "busy dump: busy_state")
    eq(st.stable, false, "busy dump: not stable")
    local seq = st.dump_seq
    frame(0)
    eq(load_state().dump_seq, seq, "still busy: no re-dump")
    MOCK.busy = 0
    frame(0)
    st = load_state()
    eq(st.dump_seq, seq + 1, "idle again: re-dumped")
    eq(st.stable, true, "re-dump is stable")
    frame(0)
    eq(load_state().dump_seq, seq + 1, "stable: no further re-dumps")

    -- ATTACK waits for the weapon's effects beyond the old 2 s cap.
    eq(X.cmd_waiting, false, "no command waiting")
    MOCK.data(mech0).active = true
    MOCK.fire_busy = 6
    local ack, n = attack("ATTACK 0 0 2 3", 40)
    check(ack ~= nil and string.find(ack, "OK ATTACK 0", 1, true) ~= nil, "ATTACK acked: " .. tostring(ack))
    check(n >= 6, "ATTACK acked only once the effects resolved (frame " .. n .. ")")
    st = load_state()
    eq(MOCK.busy, 0, "board idle at the ack")
    eq(st.stable, true, "post-ATTACK dump is stable")
    eq(st.command_waiting, false, "post-ATTACK dump: no command waiting")

    -- An effect longer than the 15 s budget: an error ack (never "OK"
    -- before the game is idle), the dump says it is not settled, and a
    -- settled dump follows when the board idles.
    MOCK.data(mech0).active = true
    MOCK.fire_busy = 1000
    ack, n = attack("ATTACK 0 0 2 3", 40)
    check(ack ~= nil and string.find(ack, "ERROR: TIMEOUT: ATTACK was sent", 1, true) ~= nil, "slow ATTACK: timeout error: " .. tostring(ack))
    check(n >= 15 and n < 40, "slow ATTACK acked at the 15 s budget (frame " .. n .. ")")
    st = load_state()
    eq(st.stable, false, "dump after a timed-out wait is not stable")
    eq(st.board_busy, true, "dump after a timed-out wait: board busy")
    seq = st.dump_seq
    MOCK.busy = 0
    frame()
    st = load_state()
    eq(st.stable, true, "settled dump once idle")
    check(st.dump_seq > seq, "settled dump is a new dump")

    -- A periodic dump taken while a command waits says so.
    MOCK.data(mech0).active = true
    MOCK.fire_busy = 4
    os.remove(DIR .. "/itb_ack.txt")
    write_file(DIR .. "/itb_cmd.txt", "#7 ATTACK 0 0 2 3")
    frame()
    eq(X.cmd_waiting, true, "ATTACK waiting for its effects")
    X._dump_state()
    st = load_state()
    eq(st.command_waiting, true, "dump during the wait: command_waiting")
    eq(st.stable, false, "dump during the wait: not stable")
    for _ = 1, 10 do frame() end
    check(read_file(DIR .. "/itb_ack.txt") ~= nil, "ATTACK acked after the wait")
    eq(load_state().stable, true, "final dump stable")

    MOCK.fire_busy = nil
    MOCK.busy, MOCK.busy_after_mutation = 0, 2
    os.time = real_time
end

---------------------------------------------------------------- native execution
-- Live 2026-10-09, Mission_Barrels turn 4: MOVE_NATIVE, then ATTACK with
-- Aerial Bombs (4,4) -> water (2,4). The bridge never selected the Jet, so
-- the global `Pawn` was the last pawn the game selected (a ground Vek the
-- AI planned); the game's Brute_Jetmech:GetTargetArea dropped the water
-- tile for that path profile, Pawn:FireWeapon returned 0 and the Jet
-- stayed, yet the bridge acked OK (it only rejected `false`) after smoking
-- the transit tile itself (its TRANSIT emulation). Native mode: the actor
-- is selected first, FireWeapon must return 1, nothing is emulated, actions
-- are refused while the game is busy, and acks wait for the board and the
-- actor to be idle.
do
    local real_time = os.time
    local WALL = 200000
    os.time = function() return WALL end
    local function frame()
        WALL = WALL + 1
        CLOCK = CLOCK + 0.25
        Mission.BaseUpdate(CUR_M)
    end
    local function cmd(text, max_frames)
        os.remove(DIR .. "/itb_ack.txt")
        write_file(DIR .. "/itb_cmd.txt", "#7 " .. text)
        for i = 1, max_frames or 40 do
            frame()
            local ack = read_file(DIR .. "/itb_ack.txt")
            if ack then
                for _ = 1, 3 do frame() end  -- let the command coroutine finish
                return ack, i
            end
        end
        return nil, max_frames or 40
    end
    local function only_fire_calls(what)
        for _, c in ipairs(MOCK.calls) do
            check(c == "Pawn:FireWeapon", what .. ": the bridge changed the game itself (" .. c .. ")")
        end
        MOCK.calls = {}
    end
    MOCK.busy, MOCK.busy_after_mutation = 0, 0
    MOCK.team = 1
    for _, t in ipairs({{0, 6}, {0, 4}, {1, 4}, {2, 4}, {2, 6}, {0, 1}, {1, 1}, {2, 1}}) do
        check(MOCK.pawn_at(Point(t[1], t[2])) == nil, "native section: tile " .. t[1] .. "," .. t[2] .. " free")
    end
    local jet = MOCK.add_pawn{id = 7, type = "JetMech", x = 0, y = 6, team = 1, mech = true}
    local fly = MOCK.add_pawn{id = 81, type = "Firefly1", x = 1, y = 4, team = 6, hp = 2}
    MOCK.tiles[2][4].terrain = TERRAIN_WATER
    SetPawn(fly)  -- the AI's last selection

    local ack = cmd("MOVE_NATIVE 7 0 4")
    check(ack and string.find(ack, "OK MOVE_NATIVE 7 to 0,4 [FireWeapon[0]] at 0,4", 1, true) ~= nil,
          "jet MOVE_NATIVE: " .. tostring(ack))
    eq(MOCK.fired[#MOCK.fired].selected, 7, "MOVE_NATIVE fired with the jet selected")

    -- The live failure, in the mock: the jet fired without being selected.
    SetPawn(fly)
    eq(MOCK.find_pawn(7):FireWeapon(Point(2, 4), 1), 0,
       "unselected jet: water landing outside the target area (live 2026-10-09)")
    eq(MOCK.data(jet).x, 0, "unselected jet did not leap")
    MOCK.calls = {}

    -- ATTACK selects the jet, fires natively, waits for the leap.
    MOCK.pawn_busy_after_fire = 4
    local n
    ack, n = cmd("ATTACK 7 0 2 4")
    MOCK.pawn_busy_after_fire = nil
    eq(ack, "#7 OK ATTACK 7 slot=0 at 2,4 [FireWeapon[1](Brute_Jetmech)]", "jet ATTACK onto water")
    local f = MOCK.fired[#MOCK.fired]
    check(f.slot == 1 and f.selected == 7 and f.ret == 1, "ATTACK: FireWeapon slot 1 with the jet selected, returned 1")
    check(MOCK.data(jet).x == 2 and MOCK.data(jet).y == 4, "the jet leapt to 2,4")
    check(n >= 5, "ATTACK acked once the jet was idle (frame " .. n .. ")")
    only_fire_calls("native ATTACK")
    eq(MOCK.tiles[1][4].smoke, false, "no bridge smoke on the transit tile")
    eq(MOCK.data(fly).hp, 2, "no bridge damage on the transit tile")
    check(string.find(read_file(DIR .. "/itb_bridge.log") or "", "TRANSIT", 1, true) == nil, "no TRANSIT emulation")
    eq(load_state().stable, true, "post-ATTACK dump stable")

    -- The game refuses (FireWeapon 0): an error ack, never OK.
    MOCK.data(jet).active = true
    ack = cmd("ATTACK 7 0 2 3")  -- 1 tile: under MinMove
    check(ack and string.find(ack, "ERROR: FireWeapon[1](Brute_Jetmech) returned 0", 1, true) ~= nil,
          "target outside the area: error ack: " .. tostring(ack))
    MOCK.data(jet).frozen = true
    ack = cmd("ATTACK 7 0 2 6")
    check(ack and string.find(ack, "returned 0", 1, true) ~= nil, "frozen: error ack: " .. tostring(ack))
    MOCK.data(jet).frozen = false
    MOCK.calls = {}

    -- Busy: refused before anything is sent; a short tail is waited out.
    local fired_before = #MOCK.fired
    MOCK.data(jet).busy = true
    ack = cmd("ATTACK 7 0 2 6")
    check(ack and string.find(ack, "ERROR: ATTACK BUSY: board busy_state=0, pawn busy=true", 1, true) ~= nil,
          "busy pawn: ATTACK refused: " .. tostring(ack))
    ack = cmd("MOVE_NATIVE 7 2 5")
    check(ack and string.find(ack, "ERROR: MOVE_NATIVE BUSY", 1, true) ~= nil, "busy pawn: MOVE_NATIVE refused: " .. tostring(ack))
    ack = cmd("REPAIR 7")
    check(ack and string.find(ack, "ERROR: REPAIR BUSY", 1, true) ~= nil, "busy pawn: REPAIR refused: " .. tostring(ack))
    MOCK.data(jet).busy = nil
    MOCK.busy = 1000
    ack = cmd("ATTACK 7 0 2 6")
    check(ack and string.find(ack, "ERROR: ATTACK BUSY: board busy_state=6", 1, true) ~= nil, "busy board: ATTACK refused: " .. tostring(ack))
    MOCK.busy = 0
    eq(#MOCK.fired, fired_before, "nothing fired while busy")
    MOCK.data(jet).busy_polls = 1
    ack = cmd("ATTACK 7 0 2 6")
    eq(ack, "#7 OK ATTACK 7 slot=0 at 2,6 [FireWeapon[1](Brute_Jetmech)]", "a busy tail is waited out")
    MOCK.calls = {}

    -- REPAIR: the pawn's repair skill (native slot 50), no HP edits.
    MOCK.data(jet).active = true
    ack = cmd("REPAIR 7")
    eq(ack, "#7 OK REPAIR 7 [FireWeapon[50]]", "native REPAIR")
    f = MOCK.fired[#MOCK.fired]
    check(f.slot == 50 and f.selected == 7, "REPAIR: FireWeapon slot 50 with the pawn selected")
    only_fire_calls("native REPAIR")

    -- Two-click weapons: two FireWeapon calls on the same slot (2, then 1).
    local swap = MOCK.add_pawn{id = 8, type = "SwapMech", x = 0, y = 1, team = 1, mech = true}
    fired_before = #MOCK.fired
    ack = cmd("ATTACK 8 0 1 1")
    check(ack and string.find(ack, "use TWO_CLICK_ATTACK; nothing fired", 1, true) ~= nil, "ATTACK with a two-click weapon refused: " .. tostring(ack))
    ack = cmd("TWO_CLICK_ATTACK 7 0 2 4 2 3")
    check(ack and string.find(ack, "use ATTACK; nothing fired", 1, true) ~= nil, "TWO_CLICK_ATTACK with a one-click weapon refused: " .. tostring(ack))
    eq(#MOCK.fired, fired_before, "refused shots fired nothing")
    ack = cmd("TWO_CLICK_ATTACK 8 0 1 1 2 1")
    eq(ack, "#7 OK TWO_CLICK_ATTACK 8 slot=0 at 1,1 and 2,1 [FireWeapon[1](Science_TC_SwapOther) x2]", "native two-click")
    local f1, f2 = MOCK.fired[#MOCK.fired - 1], MOCK.fired[#MOCK.fired]
    check(f1.slot == 1 and f1.ret == 2 and f1.x == 1 and f1.y == 1, "first click: slot 1 returned 2")
    check(f2.slot == 1 and f2.ret == 1 and f2.x == 2 and f2.y == 1 and f2.selected == 8, "second click: slot 1 returned 1")
    only_fire_calls("native TWO_CLICK_ATTACK")

    -- Turn start: no bridge re-activation in native mode (the game does it).
    MOCK.data(jet).active = false
    Mission.NextTurn(CUR_M)
    eq(MOCK.data(jet).active, false, "native: NextTurn leaves activation to the game")

    -- The old emulations exist only behind EXEC_MODE legacy.
    eq(cmd("EXEC_MODE legacy"), "#7 OK EXEC_MODE legacy", "legacy on")
    Mission.NextTurn(CUR_M)
    eq(MOCK.data(jet).active, true, "legacy: NextTurn re-activates")
    MOCK.data(jet).x, MOCK.data(jet).y = 0, 4
    MOCK.calls = {}
    ack = cmd("ATTACK 7 0 2 4")
    check(ack and string.find(ack, "OK ATTACK 7", 1, true) ~= nil, "legacy ATTACK: " .. tostring(ack))
    check(string.find(read_file(DIR .. "/itb_bridge.log") or "", "TRANSIT: Brute_Jetmech", 1, true) ~= nil,
          "legacy keeps its TRANSIT emulation")
    local emulated = false
    for _, c in ipairs(MOCK.calls) do
        if c == "Board:DamageSpace" then emulated = true end
    end
    check(emulated, "legacy ATTACK applied its own transit damage")
    eq(cmd("EXEC_MODE native"), "#7 OK EXEC_MODE native", "native again")

    for _, u in ipairs({jet, fly, swap}) do Board:RemovePawn(u) end
    MOCK.tiles[2][4].terrain = TERRAIN_ROAD
    MOCK.tiles[1][4].smoke = false
    MOCK.calls = {}
    SetPawn(nil)
    MOCK.busy, MOCK.busy_after_mutation = 0, 2
    os.time = real_time
end

---------------------------------------------------------------- final mission
-- Live freeze 2026-10-09: DEPLOY (a bare Pawn:SetSpace) put the Combat Mech
-- on a Mission_Final pylon tile, which the native deploy UI refuses (the
-- tile is BlockSpawn'd). After CONFIRM, Mission_Final:NextTurn (enemy turn
-- 0) drops a building on it and BoardSpace::DamageSpace loops forever
-- (`while IsPawnSpace(true) do Kill() end`, a dead mech stays a corpse).
-- The mock emulates that loop and records MOCK.native_hang.

local function game_file(rel)
    if not GAME_DIR then return nil end
    for _, base in ipairs({GAME_DIR .. "/scripts/", GAME_DIR .. "/"}) do
        local text = read_file(base .. rel)
        if text then return text, base .. rel end
    end
    return nil
end
local function run_game_file(rel)
    local text, path = game_file(rel)
    assert(text, "game script not found: " .. rel)
    assert(loadstring(text, "@" .. path))()
end
-- One top-level function of a game script (its `end` is at column 0).
local function game_function(rel, name)
    local text = assert(game_file(rel), "game script not found: " .. rel)
    local src = string.match(text, "\nfunction " .. string.gsub(name, "([%.%:])", "%%%1") .. "%(.-\nend")
    assert(src, name .. " not found in " .. rel)
    return src
end

local REAL = GAME_DIR ~= nil
HARNESS_REAL_SCRIPTS_RAN = false
if REAL then
    -- Helpers from global.lua, then the game's environment and mission
    -- classes (they replace the mock Environment / Env_Attack).
    for _, name in ipairs({"random_removal", "random_element", "copy_table"}) do
        assert(loadstring(game_function("global.lua", name)))()
    end
    Mission_Infinite = Mission_Infinite or Mission:new{InfiniteSpawn = true}
    Mission_Auto = Mission_Auto or Mission:new{}
    run_game_file("environments.lua")
    run_game_file("missions/final/env_volcano.lua")
    run_game_file("missions/final/mission_final.lua")
    run_game_file("missions/sand/mission_lightning.lua")
    -- The game's Mission hooks (missions.lua), then a bridge reload that
    -- wraps them: the reload path keeps _ITB_BRIDGE_ORIGINALS.
    local real = {}
    for _, name in ipairs({"ApplyEnvironmentEffect", "BaseNextTurn", "PlanEnvironment", "IsEnvironmentEffect"}) do
        local holder = {}
        local fn = assert(loadstring(game_function("missions/missions.lua", "Mission:" .. name)))
        setfenv(fn, setmetatable({Mission = holder}, {__index = _G}))
        fn()
        real[name] = assert(holder[name], name)
    end
    _ITB_BRIDGE_ORIGINALS.ApplyEnvironmentEffect = real.ApplyEnvironmentEffect
    _ITB_BRIDGE_ORIGINALS.BaseNextTurn = real.BaseNextTurn
    _ITB_BRIDGE_ORIGINALS.PlanEnvironment = real.PlanEnvironment
    Mission.IsEnvironmentEffect = real.IsEnvironmentEffect
    chunk()
    X = _ITB_BRIDGE_EXT
    eq(#MOCK.mismatches, 0, "strict-call mismatches after the bridge reload")
    check(Mission.ApplyEnvironmentEffect ~= real.ApplyEnvironmentEffect, "reload wraps the real ApplyEnvironmentEffect")
end

local PYLONS = {{1, 3}, {1, 4}, {2, 6}, {3, 3}, {3, 6}, {5, 1}, {6, 4}}  -- the live save's
local DEPLOY_ZONE = {{1, 2}, {1, 3}, {1, 4}, {2, 2}, {2, 3}, {2, 4}, {2, 5}, {3, 2}, {3, 3}, {3, 4}}
local function is_pylon(x, y)
    for _, p in ipairs(PYLONS) do
        if p[1] == x and p[2] == y then return true end
    end
    return false
end
local function reset_board()
    for x = 0, 7 do
        for y = 0, 7 do
            local t = MOCK.tiles[x][y]
            t.terrain, t.hp, t.max, t.fire, t.lava, t.acid, t.item, t.dangerous = 0, 0, 0, false, false, false, "", false
            t.smoke, t.frozen, t.shield, t.targeted, t.env, t.custom, t.populated = false, false, false, false, false, "", false
        end
    end
    MOCK.effects, MOCK.native_hang, MOCK.spawns, MOCK.busy = {}, nil, {}, 0
end
local fm = {}
local function reset_mechs()
    MOCK.pawns = {}
    for i = 0, 2 do
        fm[i] = MOCK.add_pawn{id = i, type = "PunchMech", x = -1, y = -1, team = 1, mech = true, shots = 1}
    end
end
local function final_save(turn)
    local pts, types = {}, {}
    for _, p in ipairs(PYLONS) do
        pts[#pts + 1] = "Point(" .. p[1] .. "," .. p[2] .. "), "
        types[#types + 1] = "2, "
    end
    return 'GameData = {["network"] = 6, ["networkMax"] = 7, ["difficulty"] = 0, ["seed"] = 7, }\n'
        .. 'RegionData = {\n["final_region"] = {["mission"] = "Mission0", ["player"] = {["iCurrentTurn"] = '
        .. turn .. ', ["iTeamTurn"] = 1, ["map_data"] = {\n'
        .. '["blocked_points"] = {' .. table.concat(pts) .. '},\n'
        .. '["blocked_type"] = {' .. table.concat(types) .. '},\n'
        .. '}, }, }, \n["iBattleRegion"] = 20, }\n'
end

reset_board()
reset_mechs()
MOCK.zones = {deployment = DEPLOY_ZONE, pylons = PYLONS}
write_file(save_dir .. "/saveData.lua", final_save(0))
local FinalClass = REAL and Mission_Final or Mission:new{}
local MF = FinalClass:new{ID = "Mission_Final"}
MF.LiveEnvironment = REAL and Env_Volcano:new{} or Env_Null:new{}
if REAL then MF.LiveEnvironment:Start() end
GAME.Missions = {[0] = MF}
_ITB_CURRENT_MISSION = MF
CUR_M = MF
MOCK.turn, MOCK.team = 0, 1

-- Reserved tiles and the exported zones.
do
    local reserved = X.reserved_tiles()
    for _, p in ipairs(PYLONS) do
        eq(reserved[p[1] .. "," .. p[2]], "pylon", "pylon " .. p[1] .. "," .. p[2] .. " reserved")
    end
    MOCK.zones.pylons = nil
    reserved = X.reserved_tiles()
    eq(reserved["3,3"], "spawn_block", "save blocked_points reserve 3,3 without the zone")
    MOCK.zones.pylons = PYLONS
    X._dump_state()
    st = load_state()
    eq(st.deploying, true, "final mission deploying")
    eq(#st.deployment_zone, 7, "deployment_zone: 10 zone tiles minus 3 pylons")
    eq(#st.drop_zone, 7, "drop_zone: 10 zone tiles minus 3 pylons")
    for _, p in ipairs(st.deployment_zone) do
        check(not is_pylon(p[1], p[2]), "deployment_zone excludes pylon " .. p[1] .. "," .. p[2])
    end
    for _, p in ipairs(st.drop_zone) do
        check(not is_pylon(p[1], p[2]), "drop_zone excludes pylon " .. p[1] .. "," .. p[2])
    end
end

-- Before the fix: the old DEPLOY was a bare SetSpace, and the opening
-- enemy phase then hangs in the native building drop.
if REAL then
    MOCK.data(fm[0]).x, MOCK.data(fm[0]).y = 3, 3
    MOCK.team = 6
    _ITB_BRIDGE_ORIGINALS.BaseNextTurn(MF)  -- unwrapped, as without the guard
    MOCK.resolve_effects()
    check(MOCK.native_hang ~= nil, "repro: a mech on a pylon tile hangs the pylon drop")
    if MOCK.native_hang then
        eq(MOCK.native_hang.x * 10 + MOCK.native_hang.y, 33, "repro: the hang is on 3,3")
        eq(MOCK.native_hang.pawn, 0, "repro: the mech under the pylon")
    end
    reset_board()
    reset_mechs()
    MOCK.team = 1
end

-- After the fix: DEPLOY refuses what the deploy UI refuses.
do
    local ack = run_command("DEPLOY 0 3 3")
    check(ack and string.find(ack, "ERROR: DEPLOY refused: 3,3 reserved tile (pylon)", 1, true) ~= nil,
          "DEPLOY onto a pylon refused: " .. tostring(ack))
    eq(MOCK.data(fm[0]).x, -1, "refused DEPLOY leaves the mech")
    ack = run_command("DEPLOY 1 1 3")
    check(ack and string.find(ack, "refused", 1, true) ~= nil, "DEPLOY 1 3 (pylon) refused")
    ack = run_command("DEPLOY 0 7 7")
    check(ack and string.find(ack, "not in the drop zone", 1, true) ~= nil, "DEPLOY outside the zone refused: " .. tostring(ack))
    eq(run_command("DEPLOY 0 3 4"), "#7 OK DEPLOY 0 at 3,4", "DEPLOY 0 3 4")
    ack = run_command("DEPLOY 1 3 4")
    check(ack and string.find(ack, "occupied", 1, true) ~= nil, "DEPLOY onto a mech refused")
    eq(run_command("DEPLOY 1 2 4"), "#7 OK DEPLOY 1 at 2,4", "DEPLOY 1 2 4")
    eq(run_command("DEPLOY 2 3 2"), "#7 OK DEPLOY 2 at 3,2", "DEPLOY 2 3 2")
    eq(run_command("DEPLOY 2 3 2"), "#7 OK DEPLOY 2 at 3,2", "re-DEPLOY on its own tile")
    st = load_state()
    eq(#st.drop_zone, 7, "drop zone keeps the deployed mechs' tiles")
end

-- Last line of defence: a mech that still stands on a pylon when the
-- enemy turn 0 begins is moved off before Mission_Final:NextTurn.
do
    MOCK.data(fm[2]).x, MOCK.data(fm[2]).y = 3, 3
    MOCK.team = 6
    Mission.BaseNextTurn(MF)
    local d2 = MOCK.data(fm[2])
    check(not is_pylon(d2.x, d2.y), "pylon guard moved the mech off 3,3")
    local in_zone = false
    for _, p in ipairs(DEPLOY_ZONE) do
        if p[1] == d2.x and p[2] == d2.y then in_zone = true end
    end
    check(in_zone, "pylon guard moved it to a drop-zone tile")
    check(string.find(read_file(DIR .. "/itb_bridge.log") or "", "PYLON GUARD: moved pawn 2", 1, true) ~= nil,
          "pylon guard logged")
    if REAL then
        MOCK.resolve_effects()
        eq(MOCK.native_hang, nil, "no native hang after the guard")
        for _, p in ipairs(PYLONS) do
            eq(MOCK.tiles[p[1]][p[2]].terrain, TERRAIN_BUILDING, "pylon landed on " .. p[1] .. "," .. p[2])
        end
        for i = 0, 2 do
            check(MOCK.data(fm[i]).hp > 0, "mech " .. i .. " alive after the pylon drop")
        end
    end
end

-- The volcano enemy phase, driven like BoardPlayer (stage 7 spec 1.3-1.8):
-- state 1: IsEnvironmentEffect once, then ApplyEnvironmentEffect on each
-- idle board while it returns true; state 2: BaseNextTurn, then
-- PlanEnvironment polled until false. Every wrapped return is checked
-- against the Env_Attack contract (return self:IsEffect()).
local function poll_plan(mission, label)
    local results = {}
    for _ = 1, 50 do
        local r = Mission.PlanEnvironment(mission)
        results[#results + 1] = r
        if not r then break end
    end
    local n = #results
    eq(results[n], false, label .. ": PlanEnvironment ends with false")
    check(n < 50, label .. ": PlanEnvironment poll ends")
    local env = mission.LiveEnvironment
    eq(#env.Locations, n - 1, label .. ": one location per true")
    eq(#env.Planned, n - 1, label .. ": all planned locations placed")
    return n - 1
end
local function apply_env(mission, label, expect_steps)
    local env = mission.LiveEnvironment
    local flag = Mission.IsEnvironmentEffect(mission)
    eq(flag, expect_steps > 0, label .. ": IsEnvironmentEffect")
    local steps = 0
    local function env_log()
        local l = _ITB_BRIDGE_PHASE_LOG
        if l and rawequal(l.mission, mission) then return l.env end
        return {}
    end
    local log_before = #env_log()
    while flag do
        MOCK.resolve_effects()
        local r = X.pack(Mission.ApplyEnvironmentEffect(mission))
        steps = steps + 1
        eq(r.n, 1, label .. ": one return value")
        eq(r[1], #env.Locations ~= 0, label .. ": step " .. steps .. " returns IsEffect()")
        flag = r[1]
        if steps > 50 then break end
    end
    MOCK.resolve_effects()
    eq(steps, expect_steps, label .. ": one step per location")
    local log = env_log()
    eq(#log - log_before, steps, label .. ": env steps logged")
    if #log > 0 then eq(log[#log].returned, false, label .. ": last logged step returned false") end
end
if REAL then
    -- Opening phase (enemy turn 0) continues: PlanEnvironment after the
    -- pylons (volcano phase 1, lava).
    local planned = poll_plan(MF, "volcano turn 0")
    check(planned > 0, "volcano plans at turn 0")
    for turn = 1, 3 do
        MOCK.turn, MOCK.team = turn, 1
        Mission.BaseNextTurn(MF)
        MOCK.team = 6
        apply_env(MF, "volcano turn " .. turn .. " mode " .. tostring(MF.LiveEnvironment.Mode), planned)
        Mission.BaseNextTurn(MF)
        MOCK.resolve_effects()
        planned = poll_plan(MF, "volcano plan turn " .. turn)
    end
    eq(MOCK.native_hang, nil, "no native hang over three volcano rounds")

    -- Several return values pass through the wrap untouched.
    local env = MF.LiveEnvironment
    env.ApplyEffect = function() return true, "extra", nil end
    local r = X.pack(Mission.ApplyEnvironmentEffect(MF))
    eq(r.n, 3, "all return values passed through")
    eq(r[2], "extra", "second return value")
    env.ApplyEffect = nil

    -- Lightning: an unordered Env_Attack (random strike order).
    local ML = Mission_Lightning:new{ID = "Mission_Lightning"}
    ML.LiveEnvironment = Env_Lightning:new{}
    ML.LiveEnvironment:Start()
    _ITB_CURRENT_MISSION = ML
    MOCK.turn, MOCK.team = 1, 6
    local n = poll_plan(ML, "lightning plan")
    check(n >= 3, "lightning plans several strikes")
    apply_env(ML, "lightning", n)
    _ITB_CURRENT_MISSION = MF
    HARNESS_REAL_SCRIPTS_RAN = true
else
    print("bridge harness: no game dir, final-mission / env script checks skipped")
end
MOCK.turn, MOCK.team = 1, 1

-- Save-file cache: no re-read or re-parse while the files are unchanged.
do
    X._dump_state()
    local reads, parses = _SAVE_READ_COUNT, _SAVE_PARSE_COUNT
    for _ = 1, 5 do X._dump_state() end
    run_command("DEBUG_STATUS")
    eq(_SAVE_READ_COUNT, reads, "unchanged save: no full re-read")
    eq(_SAVE_PARSE_COUNT, parses, "unchanged save: no re-parse")
    write_file(save_dir .. "/saveData.lua", final_save(1) .. "-- changed\n")
    X._dump_state()
    eq(_SAVE_READ_COUNT, reads + 1, "changed save: one re-read")
    eq(_SAVE_PARSE_COUNT, parses + 1, "changed save: one re-parse")
end

-- Live grid loss since the player's turn start (the save grid is stale
-- inside a turn; Game:GetPower is not safe to call).
do
    MOCK.turn, MOCK.team = 1, 1
    local t1, t2 = MOCK.tiles[6][1], MOCK.tiles[7][1]
    t1.terrain, t1.hp, t1.max, t1.populated = TERRAIN_BUILDING, 2, 2, true
    t2.terrain, t2.hp, t2.max, t2.populated = TERRAIN_BUILDING, 1, 1, true
    Mission.BaseNextTurn(MF)  -- player turn start: buildings recorded
    X._dump_state()
    st = load_state()
    eq(st.grid_lost_this_turn, 0, "no grid lost at turn start")
    t1.hp = 1
    t2.terrain, t2.hp = TERRAIN_RUBBLE, 0
    X._dump_state()
    st = load_state()
    eq(st.grid_lost_this_turn, 2, "grid lost: 1 damaged + 1 destroyed")
    eq(#st.grid_lost_tiles, 2, "grid lost tiles")
    eq(st.grid_power_estimate, st.grid_power - 2, "grid estimate in the player phase")
    MOCK.team = 6
    X._dump_state()
    st = load_state()
    eq(st.grid_lost_this_turn, 2, "grid lost still counted in the enemy phase")
    eq(st.grid_power_estimate, nil, "no estimate outside the player phase")
    MOCK.team = 1
    t1.terrain, t1.hp, t1.max, t1.populated = 0, 0, 0, false
    t2.terrain = 0
end

-- Safety valve.
do
    local TM = Mission:new{ID = "Mission_Test"}
    TM.LiveEnvironment = setmetatable({}, {__index = {ApplyEffect = function() return true, 7 end}})
    local saved = {log_env_step = X.log_env_step, on_base_next_turn = X.on_base_next_turn}

    -- An error in the bridge's own work changes nothing for the game.
    X.log_env_step = function() error("boom") end
    local r = X.pack(Mission.ApplyEnvironmentEffect(TM))
    eq(r[1], true, "error in hook work: return passed through")
    eq(r[2], 7, "error in hook work: second return passed through")
    check(not X.disabled, "an error does not disable the extension")

    -- Re-entrancy: hook work that re-enters the hook runs once.
    MOCK.hook_calls = {}
    X.on_base_next_turn = function(m) Mission.BaseNextTurn(m) end
    Mission.BaseNextTurn(TM)
    local next_turns = 0
    for _, h in ipairs(MOCK.hook_calls) do
        if h == "NextTurn" then next_turns = next_turns + 1 end
    end
    eq(next_turns, 2, "re-entrant hook work does not recurse")
    X.on_base_next_turn = saved.on_base_next_turn

    -- An endless loop in the bridge's work is cut off and the extension
    -- switched off; the game's call still returns its own values.
    X.log_env_step = function() while true do CLOCK = CLOCK + 0.001 end end
    r = X.pack(Mission.ApplyEnvironmentEffect(TM))
    eq(r[1], true, "endless hook work: return passed through")
    eq(r[2], 7, "endless hook work: second return passed through")
    check(X.disabled == true, "endless hook work disables the extension")
    check(string.find(tostring(X.disabled_reason), "log_env_step", 1, true) ~= nil, "disable reason names the hook")
    check(string.find(read_file(DIR .. "/itb_bridge.log") or "", "BRIDGE EXT DISABLED", 1, true) ~= nil, "disable logged")
    check(debug.gethook() == nil, "count hook removed")
    X.log_env_step = saved.log_env_step

    -- Disabled: the dump keeps the old fields and the deploy guard.
    _ITB_CURRENT_MISSION = MF
    MOCK.turn = 0
    X._dump_state()
    st = load_state()
    eq(#st.tiles, 64, "disabled: old fields still dumped")
    check(st.bridge_ext_disabled ~= nil, "disabled: state says so")
    eq(st.mission_state, nil, "disabled: extension exports off")
    for _, p in ipairs(st.deployment_zone or {}) do
        check(not is_pylon(p[1], p[2]), "disabled: deployment_zone still excludes pylons")
    end
    local ack = run_command("DEPLOY 0 3 3")
    check(ack and string.find(ack, "refused", 1, true) ~= nil, "disabled: DEPLOY guard still on")
    MOCK.turn = 1

    -- Slow but finite work: three slow calls switch it off.
    X.disabled, X.disabled_reason, X._strikes = nil, nil, 0
    X.log_env_step = function() CLOCK = CLOCK + 0.3 end
    for i = 1, 3 do
        check(not X.disabled, "slow call " .. i .. " before the limit")
        Mission.ApplyEnvironmentEffect(TM)
    end
    check(X.disabled == true, "three slow calls disable the extension")
    X.log_env_step = saved.log_env_step
    X.disabled, X.disabled_reason, X._strikes = nil, nil, 0
end

eq(#MOCK.mismatches, 0, "strict-call mismatches overall")

if #failures > 0 then
    error(#failures .. " of " .. checks .. " bridge checks failed:\n  " .. table.concat(failures, "\n  "), 0)
end
print("bridge harness: " .. checks .. " checks passed")
return checks
