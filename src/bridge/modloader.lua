--------------------------------------------------------------------
-- ITB Bot Bridge — Production
-- Runs inside Into the Breach via modloader.lua injection.
-- Communicates with external Python bot via file-based IPC.
--
-- macOS default: /tmp/itb_*
-- Windows default: Documents/My Games/Into The Breach/itb_bridge/itb_*
--------------------------------------------------------------------

local function normalize_path(path)
    return (path:gsub("\\", "/"))
end

local function is_windows()
    return package.config:sub(1, 1) == "\\"
end

local function default_save_root()
    local override = os.getenv("ITB_SAVE_DIR")
    if override and override ~= "" then return normalize_path(override) end
    if is_windows() then
        local user = os.getenv("USERPROFILE") or os.getenv("HOME") or "."
        return normalize_path(user) .. "/Documents/My Games/Into The Breach"
    end
    local home = os.getenv("HOME") or "."
    return normalize_path(home) .. "/Library/Application Support/IntoTheBreach"
end

local function default_bridge_dir()
    local override = os.getenv("ITB_BRIDGE_DIR")
    if override and override ~= "" then return normalize_path(override) end
    if is_windows() then
        return default_save_root() .. "/itb_bridge"
    end
    return "/tmp"
end

local BRIDGE_DIR = default_bridge_dir()
local SAVE_ROOT = default_save_root()

local STATE_FILE = BRIDGE_DIR .. "/itb_state.json"
local STATE_TMP  = BRIDGE_DIR .. "/itb_state.json.tmp"
local CMD_FILE   = BRIDGE_DIR .. "/itb_cmd.txt"
local ACK_FILE   = BRIDGE_DIR .. "/itb_ack.txt"
local ACK_TMP    = BRIDGE_DIR .. "/itb_ack.tmp"
local LOG_FILE   = BRIDGE_DIR .. "/itb_bridge.log"
local HEARTBEAT_FILE = BRIDGE_DIR .. "/itb_bridge_heartbeat"

if is_windows() then
    os.execute('mkdir "' .. BRIDGE_DIR .. '" >NUL 2>NUL')
else
    os.execute('mkdir -p "' .. BRIDGE_DIR .. '"')
end

local TERRAIN_NAMES = {}

local function add_terrain_name(global_name, fallback_id, name)
    local id = _G[global_name]
    if type(id) ~= "number" then id = fallback_id end
    if type(id) == "number" then TERRAIN_NAMES[id] = name end
end

add_terrain_name("TERRAIN_ROAD", 0, "ground")
add_terrain_name("TERRAIN_BUILDING", 1, "building")
add_terrain_name("TERRAIN_RUBBLE", 2, "rubble")
add_terrain_name("TERRAIN_WATER", 3, "water")
add_terrain_name("TERRAIN_MOUNTAIN", 4, "mountain")
add_terrain_name("TERRAIN_ICE", 5, "ice")
add_terrain_name("TERRAIN_FOREST", 6, "forest")
add_terrain_name("TERRAIN_SAND", 7, "sand")
add_terrain_name("TERRAIN_HOLE", 9, "chasm")
add_terrain_name("TERRAIN_ACID", 10, "acid")
add_terrain_name("TERRAIN_LAVA", nil, "lava")

local _poll_interval = 0.2  -- seconds between command polls
local _last_poll = 0

--------------------------------------------------------------------
-- Minimal JSON encoder (no external deps)
--------------------------------------------------------------------
local function json_encode(val)
    if val == nil then return "null" end
    local t = type(val)
    if t == "boolean" then return val and "true" or "false" end
    if t == "number" then return tostring(val) end
    if t == "string" then
        return '"' .. val:gsub('\\','\\\\'):gsub('"','\\"'):gsub('\n','\\n') .. '"'
    end
    if t == "table" then
        -- Check if array (sequential integer keys starting at 1)
        if #val > 0 or next(val) == nil then
            local parts = {}
            for i, v in ipairs(val) do
                parts[i] = json_encode(v)
            end
            return "[" .. table.concat(parts, ",") .. "]"
        else
            local parts = {}
            for k, v in pairs(val) do
                parts[#parts+1] = json_encode(tostring(k)) .. ":" .. json_encode(v)
            end
            return "{" .. table.concat(parts, ",") .. "}"
        end
    end
    return '"<' .. t .. '>"'
end

--------------------------------------------------------------------
-- File helpers
--------------------------------------------------------------------
local function write_atomic(path, tmp_path, content)
    local f = io.open(tmp_path, "w")
    if f then
        f:write(content)
        f:close()
        local ok, _err = os.rename(tmp_path, path)
        if not ok and is_windows() then
            os.remove(path)
            os.rename(tmp_path, path)
        end
    end
end

local function log_bridge(msg)
    local f = io.open(LOG_FILE, "a")
    if f then
        f:write(os.date() .. " | " .. msg .. "\n")
        f:close()
    end
end

-- Read all save-file-derived data in a single I/O pass:
-- grid power, queued shots, and conveyor belts.
-- Reads saveData.lua (preferred) or undoSave.lua (fallback).
--------------------------------------------------------------------
local function strip_upgrade_suffix_lua(weapon_id)
    if type(weapon_id) ~= "string" then return "", "" end
    for _, suffix in ipairs({"_AB", "_A", "_B"}) do
        local n = string.len(suffix)
        if string.sub(weapon_id, -n) == suffix then
            return string.sub(weapon_id, 1, string.len(weapon_id) - n), suffix
        end
    end
    return weapon_id, ""
end

local function save_mod_group_fully_powered(block, key)
    local blob = block:match('%["' .. key .. '"%]%s*=%s*{(.-)}')
    if not blob then return false end
    local saw_value = false
    for value in blob:gmatch("%-?%d+") do
        saw_value = true
        if (tonumber(value) or 0) <= 0 then
            return false
        end
    end
    return saw_value
end

local function overlay_current_weapon_from_pawn_mods(result, uid, slot, base_weapon, mod1_key, mod2_key, block)
    if type(uid) ~= "number" or uid < 0 or uid > 2 then return end
    if type(base_weapon) ~= "string" or base_weapon == "" then return end

    local powered_a = save_mod_group_fully_powered(block, mod1_key)
    local powered_b = save_mod_group_fully_powered(block, mod2_key)
    local candidates = {}
    if powered_a and powered_b then candidates[#candidates + 1] = base_weapon .. "_AB" end
    if powered_a then candidates[#candidates + 1] = base_weapon .. "_A" end
    if powered_b then candidates[#candidates + 1] = base_weapon .. "_B" end

    local idx = uid * 2 + slot + 1
    local current = result.current_weapons[idx] or ""
    local current_base = strip_upgrade_suffix_lua(current)
    for _, upgraded in ipairs(candidates) do
        if _G[upgraded] ~= nil then
            local expected_base = strip_upgrade_suffix_lua(upgraded)
            if current == "" or current == expected_base or current == upgraded or current_base == expected_base then
                result.current_weapons[idx] = upgraded
                return
            end
        end
    end
end

local function _empty_save_result()
    return {
        network = nil,
        networkMax = nil,
        difficulty = nil,     -- GameData.difficulty (0=Easy, 1=Normal, 2=Hard, 3=Unfair)
        queued_shots = {},
        queued_origins = {},  -- [pawn_id] = {x, y} from piOrigin (attack queue source)
        queued_targets = {},  -- [pawn_id] = {x, y} from piTarget (leap/melee landing tile)
        queued_skills = {},   -- [pawn_id] = iQueuedSkill (>=0 when an attack is actually queued)
        conveyor_belts = {},
        pilots = {},  -- [pawn_id] = {id=..., level=..., skill1=..., skill2=...}
        pawn_max_health = {},  -- [pawn_id] = max_health
        infected = {},  -- [pawn_id] = bInfected (Vek Mites objective state)
        master_seed = nil,    -- GameData.seed — run-lifetime master RNG seed
        mission_seeds = {},   -- [region_key] = aiSeed — per-mission per-turn PRNG snapshot
        current_weapons = {}, -- GameData.current.weapons, 1-indexed loadout slots
        pawn_offsets = {},    -- [pawn_id] = raw save offset (diagnostic only)
    }
end

-- Save-file cache. The game rewrites saveData.lua / undoSave.lua whole, at
-- turn boundaries only, but the bridge used to read and pattern-parse them
-- on every state dump and command. Lua 5.1 has no stat(), so a file's
-- identity is its size plus its first and last 256 bytes (cheap: one open,
-- two short reads); the full read and the parse happen only when that key
-- changes, or when the cached copy is older than SAVE_CACHE_MAX_AGE wall
-- seconds (a safety net for a same-size rewrite).
local SAVE_CACHE_MAX_AGE = 20
local _save_file_cache = {}   -- path -> {key=, content=, at=}

local function save_file_key(f)
    local size = f:seek("end")
    if type(size) ~= "number" then return nil end
    f:seek("set", 0)
    local head = f:read(256) or ""
    local tail = ""
    if size > 256 then
        f:seek("set", size - 256)
        tail = f:read(256) or ""
    end
    return tostring(size) .. "|" .. head .. "|" .. tail
end

-- Returns the file's content (nil if it does not exist) and its cache key.
local function read_save_file_cached(path)
    local f = io.open(path, "r")
    if not f then
        _save_file_cache[path] = nil
        return nil, nil
    end
    local ok, key = pcall(save_file_key, f)
    if not ok then key = nil end
    local now = os.time()
    local c = _save_file_cache[path]
    if key ~= nil and c ~= nil and c.key == key and now - c.at < SAVE_CACHE_MAX_AGE then
        f:close()
        return c.content, path .. "|" .. c.key .. "|" .. c.at
    end
    f:seek("set", 0)
    local content = f:read("*a")
    f:close()
    if type(content) ~= "string" then return nil, nil end
    key = key or ("len" .. string.len(content))
    _save_file_cache[path] = {key = key, content = content, at = now}
    _SAVE_READ_COUNT = (_SAVE_READ_COUNT or 0) + 1  -- full reads (harness)
    return content, path .. "|" .. key .. "|" .. now
end

local _save_parse_cache = {key = nil, result = nil}

local function _read_save_data_uncached(content)
    local result = _empty_save_result()

    -- Grid power (in first line of file, very cheap pattern match)
    local net = content:match('%["network"%]%s*=%s*(%d+)')
    if net then result.network = tonumber(net) end
    local netMax = content:match('%["networkMax"%]%s*=%s*(%d+)')
    if netMax then result.networkMax = tonumber(netMax) end

    -- In-game difficulty: 0=Easy, 1=Normal, 2=Hard, 3=Unfair. Authoritative
    -- live value (the Python session.difficulty drifts after Timeline Lost
    -- continuations). Allow negative just in case the game ever stores it
    -- as -1 for "uninitialized".
    local diff = content:match('%["difficulty"%]%s*=%s*(%-?%d+)')
    if diff then result.difficulty = tonumber(diff) end

    -- RNG seeds — for grid-defense resist prediction probe.
    -- `seed` is the run-lifetime master seed (appears once, top-level GameData).
    -- `aiSeed` is per-mission and advances each turn — it's the PRNG state
    -- snapshot the game uses for AI / resist rolls starting from the next
    -- enemy phase. Recording it per turn lets us replay forward locally and
    -- fish which telegraphed attacks the game has pre-rolled as resists.
    local ms = content:match('%["seed"%]%s*=%s*(%-?%d+)')
    if ms then result.master_seed = tonumber(ms) end
    local weapons_blob = content:match('%["weapons"%]%s*=%s*{(.-)}')
    if weapons_blob then
        for w in weapons_blob:gmatch('"([^"]*)"') do
            result.current_weapons[#result.current_weapons + 1] = w
        end
    end
    for region_key, region_block in content:gmatch('%["(region%d+)"%]%s*=%s*(%b{})') do
        local ais = region_block:match('%["aiSeed"%]%s*=%s*(%-?%d+)')
        if ais then
            local sMission = region_block:match('%["sMission"%]%s*=%s*"([^"]+)"')
            local iTurn = region_block:match('%["iCurrentTurn"%]%s*=%s*(%-?%d+)')
            local iState = region_block:match('%["iState"%]%s*=%s*(%-?%d+)')
            result.mission_seeds[region_key] = {
                ai_seed = tonumber(ais),
                mission = sMission,
                turn = tonumber(iTurn),
                state = tonumber(iState),
            }
        end
    end

    -- Queued shots + pilot data: per-pawn, in a single pass.
    -- Save has blocks like `["pawn3"] = { ["id"] = 3, ["piQueuedShot"] =
    -- Point(5,0), ["pilot"] = { ["id"] = "Pilot_Original", ["level"] = 2,
    -- ["skill1"] = 0, ["skill2"] = 2, ... }, ... }`.
    for block in content:gmatch('%["pawn%d+"%]%s*=%s*(%b{})') do
        local pid = block:match('%["id"%]%s*=%s*(%d+)')
        if pid then
            local pid_n = tonumber(pid)
            if pid_n and pid_n >= 0 and pid_n <= 2 then
                local primary = block:match('%["primary"%]%s*=%s*"([^"]+)"')
                if primary then
                    overlay_current_weapon_from_pawn_mods(
                        result, pid_n, 0, primary,
                        "primary_mod1", "primary_mod2", block)
                end
                local secondary = block:match('%["secondary"%]%s*=%s*"([^"]+)"')
                if secondary then
                    overlay_current_weapon_from_pawn_mods(
                        result, pid_n, 1, secondary,
                        "secondary_mod1", "secondary_mod2", block)
                end
            end
            local off = block:match('%["offset"%]%s*=%s*(%d+)')
            if off then
                result.pawn_offsets[pid_n] = tonumber(off)
            end
            -- Queued shot (projectile/laser/artillery end-tile)
            local qs = block:match('%["piQueuedShot"%]%s*=%s*Point%s*%(([^%)]+)%)')
            if qs then
                local qsx, qsy = qs:match('(%-?%d+)%s*,%s*(%-?%d+)')
                if qsx and qsy then
                    result.queued_shots[pid_n] = {x = tonumber(qsx), y = tonumber(qsy)}
                end
            end
            -- Origin of the queued attack. piQueuedShot is stored relative
            -- to this point; if a Vek is pushed mid-turn, the live target
            -- shifts by current_position + (piQueuedShot - piOrigin).
            local qo = block:match('%["piOrigin"%]%s*=%s*Point%s*%(([^%)]+)%)')
            if qo then
                local qox, qoy = qo:match('(%-?%d+)%s*,%s*(%-?%d+)')
                if qox and qoy then
                    result.queued_origins[pid_n] = {x = tonumber(qox), y = tonumber(qoy)}
                end
            end
            -- piTarget (leap landing tile, melee target, move-style queued attacks).
            -- Populated for Leapers and other Jumper pawns when piQueuedShot is
            -- (-1,-1). Also populated for non-queued pawns (stale last-target),
            -- so the consumer must gate on iQueuedSkill >= 0.
            local pt = block:match('%["piTarget"%]%s*=%s*Point%s*%(([^%)]+)%)')
            if pt then
                local ptx, pty = pt:match('(%-?%d+)%s*,%s*(%-?%d+)')
                if ptx and pty then
                    result.queued_targets[pid_n] = {x = tonumber(ptx), y = tonumber(pty)}
                end
            end
            -- iQueuedSkill: -1 when no skill is queued, >=0 when queued.
            local qsk = block:match('%["iQueuedSkill"%]%s*=%s*(%-?%d+)')
            if qsk then
                result.queued_skills[pid_n] = tonumber(qsk)
            end
            local mh = block:match('%["max_health"%]%s*=%s*(%d+)')
            if mh then
                result.pawn_max_health[pid_n] = tonumber(mh)
            end
            local infected = block:match('%["bInfected"%]%s*=%s*(true|false)')
            if infected then
                result.infected[pid_n] = infected == "true"
            end
            -- Pilot: nested table inside the pawn block
            local pilot_block = block:match('%["pilot"%]%s*=%s*(%b{})')
            if pilot_block then
                local pilot_id = pilot_block:match('%["id"%]%s*=%s*"([^"]+)"')
                if pilot_id then
                    local pd = {id = pilot_id}
                    local lvl = pilot_block:match('%["level"%]%s*=%s*(%-?%d+)')
                    if lvl then pd.level = tonumber(lvl) end
                    local s1 = pilot_block:match('%["skill1"%]%s*=%s*(%-?%d+)')
                    if s1 then pd.skill1 = tonumber(s1) end
                    local s2 = pilot_block:match('%["skill2"%]%s*=%s*(%-?%d+)')
                    if s2 then pd.skill2 = tonumber(s2) end
                    result.pilots[pid_n] = pd
                end
            end
        end
    end

    -- Conveyor belts: direction from custom tile sprites. Keep loc/custom on
    -- the same serialized tile row; a cross-entry pattern can pair a plain
    -- building loc with the next conveyor custom and create phantom belts.
    for line in content:gmatch("[^\r\n]+") do
        local loc_x, loc_y = line:match('%["loc"%]%s*=%s*Point%(%s*(%d+)%s*,%s*(%d+)%s*%)')
        local dir = line:match('%["custom"%]%s*=%s*"conveyor(%d+)%.png"')
        if loc_x and loc_y and dir then
            local key = loc_x .. "," .. loc_y
            result.conveyor_belts[key] = tonumber(dir)
        end
    end

    -- The raw text, for the bridge extension's active-region parse.
    result.raw_content = content
    return result
end

-- Reads saveData.lua (preferred) or undoSave.lua (fallback), parsed once
-- per file version. Callers share the returned table: read-only.
local function _read_save_data()
    local base = SAVE_ROOT .. "/profile_Alpha/"
    local content, key = read_save_file_cached(base .. "saveData.lua")
    if not content then
        content, key = read_save_file_cached(base .. "undoSave.lua")
    end
    if not content then return _empty_save_result() end
    if _save_parse_cache.key == key and _save_parse_cache.result then
        return _save_parse_cache.result
    end
    local result = _read_save_data_uncached(content)
    _save_parse_cache = {key = key, result = result}
    _SAVE_PARSE_COUNT = (_SAVE_PARSE_COUNT or 0) + 1  -- full parses (harness)
    return result
end

local function get_pawn_max_health(pawn, uid, save_data)
    local pawn_def = _G[pawn:GetType()]
    local base = (pawn_def and pawn_def.Health) or pawn:GetHealth()
    local max_hp = base

    local fn = pawn.GetMaxHealth
    if type(fn) == "function" then
        local ok, mh = pcall(function() return fn(pawn) end)
        if ok and type(mh) == "number" and mh > 0 then
            max_hp = math.max(max_hp, mh)
        end
    end

    local saved = save_data and save_data.pawn_max_health and save_data.pawn_max_health[uid]
    if type(saved) == "number" and saved > 0 then
        max_hp = math.max(max_hp, saved)
    end

    local bonus = 0
    local pilot = save_data and save_data.pilots and save_data.pilots[uid]
    if pilot then
        local level = pilot.level or 0
        -- Pilot perk ID 0 is a real active skill (+2 HP) once the slot is
        -- unlocked, so gate by level instead of treating zero as empty.
        local active_skills = {}
        if level >= 1 then active_skills[#active_skills + 1] = pilot.skill1 end
        if level >= 2 then active_skills[#active_skills + 1] = pilot.skill2 end
        for _, skill in ipairs(active_skills) do
            if skill == 0 or skill == 8 then
                bonus = bonus + 2
            end
        end
    end
    if bonus > 0 and max_hp < base + bonus then
        return max_hp + bonus
    end
    return max_hp
end

local function normalize_queued_target(raw, origin, current_x, current_y)
    if not raw then return nil, false end
    if not origin then return {raw.x, raw.y}, false end
    local nx = current_x + (raw.x - origin.x)
    local ny = current_y + (raw.y - origin.y)
    if nx < 0 or nx > 7 or ny < 0 or ny > 7 then
        return nil, true
    end
    return {nx, ny}, true
end

--------------------------------------------------------------------
-- Deployment zone capture
--------------------------------------------------------------------
-- Read the live deploy zone from Board:GetZone("deployment") and filter
-- to tiles that are CURRENTLY valid for placement:
--   (a) no pawn already on the tile (Coal Plant, just-deployed mech, etc.)
--   (b) terrain is deployable (excludes building/water/mountain/lava/chasm)
-- Without this filter the bridge reports tiles that aren't yellow on screen
-- and clicks silently fail. Returns a list of {x, y} pairs (possibly empty).
local function capture_deploy_zone()
    if not (Board and Board.GetZone) then return {} end
    local ok, ptList = pcall(function() return Board:GetZone("deployment") end)
    if not ok or not ptList or not ptList.size then return {} end
    local n = ptList:size()
    if n == 0 then return {} end
    local zone = {}
    for i = 1, n do
        local p = ptList:index(i)
        local pawn_ok, pawn = pcall(function() return Board:GetPawn(p) end)
        local terr_ok, terrain = pcall(function() return Board:GetTerrain(p) end)
        local has_pawn = pawn_ok and pawn ~= nil
        -- Deployable: 0=ground, 2=rubble, 6=forest, 7=sand, 8=ice
        local terrain_ok = terr_ok and terrain ~= nil and (
            terrain == 0 or terrain == 2 or terrain == 6
            or terrain == 7 or terrain == 8
        )
        if not has_pawn and terrain_ok then
            zone[#zone + 1] = {p.x, p.y}
        end
    end
    -- (c) the tiles the native deploy UI refuses (pylons, spawn blocks,
    -- items, pods, danger, ...): see ITBX.deploy_tile_ok. A mech placed on
    -- a Mission_Final pylon tile hangs the game when the pylon lands.
    local ext = rawget(_G, "_ITB_BRIDGE_EXT")
    if ext and ext.filter_deploy_tiles then
        local ok_f, filtered = pcall(ext.filter_deploy_tiles, zone)
        if ok_f and type(filtered) == "table" then zone = filtered end
    end
    return zone
end

--------------------------------------------------------------------
-- State serializer: Board → JSON
--------------------------------------------------------------------
-- Vanilla Into the Breach does not expose a reliable Board:IsShield(Point)
-- query for terrain. memedit adds that method, but an unextended game raises
-- when it is called. RegionData contains the mission's static map baseline;
-- native damage does not clear consumed shields there (it can even retain a
-- shield after the tile becomes rubble). Export it only as a turn-1 seed and
-- never claim it is live. Python refreshes later turns from saveData at the
-- all-actors-active boundary and carries a verified replay-backed ledger.
local function get_runtime_region_tile_shields()
    local region_data = _G.RegionData
    if type(region_data) ~= "table" then return nil end

    local battle_region = region_data.iBattleRegion
    if type(battle_region) ~= "number" then return nil end

    local region
    if battle_region == 20 then
        region = region_data.final_region
    else
        region = region_data["region" .. tostring(battle_region)]
    end
    if type(region) ~= "table"
            or type(region.player) ~= "table"
            or type(region.player.map_data) ~= "table"
            or type(region.player.map_data.map) ~= "table" then
        return nil
    end

    local shields = {}
    for _, entry in pairs(region.player.map_data.map) do
        if type(entry) == "table" and entry.loc ~= nil then
            local ok_xy, x, y = pcall(function()
                return entry.loc.x, entry.loc.y
            end)
            if ok_xy and type(x) == "number" and type(y) == "number" then
                shields[x .. "," .. y] = entry.shield == true
            end
        end
    end
    return shields
end

local function get_live_tile_shield(pt, runtime_region_shields)
    -- memedit's Board:IsShield is the strongest source when installed: both
    -- true and false are authoritative and must beat all fallbacks.
    local ok, shield = pcall(function() return Board:IsShield(pt) end)
    if ok and type(shield) == "boolean" then return shield end

    -- RegionData is a static positive baseline supplied only on turn 1 by the
    -- caller. It is intentionally absent on later turns.
    if runtime_region_shields ~= nil then
        shield = runtime_region_shields[pt.x .. "," .. pt.y]
        if shield ~= nil then return shield end
    end

    -- Compatibility with any external extension that supplies this spelling.
    ok, shield = pcall(function() return Board:IsShielded(pt) end)
    if ok and type(shield) == "boolean" then return shield end
    return false
end

local function point_list_contains(pt_list, point)
    if pt_list == nil or point == nil then return false end
    local ok_size, size = pcall(function() return pt_list:size() end)
    if not ok_size or type(size) ~= "number" then return false end
    for i = 1, size do
        local ok_point, candidate = pcall(function() return pt_list:index(i) end)
        if ok_point and candidate ~= nil
            and candidate.x == point.x and candidate.y == point.y then
            return true
        end
    end
    return false
end

-- Env_Tides source metadata that Board:IsEnvironmentDanger cannot express.
-- Mission_Terratide inherits the same Index and reverses its row mapping.
-- Keep this helper pure so the mission-scoped scalar is independently
-- testable without loading the game or installing this bridge.
local function mission_tides_index(mission_id, live_environment)
    if mission_id ~= "Mission_Tides"
            and mission_id ~= "Mission_Terratide" then
        return nil
    end
    if type(live_environment) ~= "table"
            or type(live_environment.Index) ~= "number" then
        return nil
    end

    local index = live_environment.Index
    if index ~= index or index == math.huge or index == -math.huge
            or index < 1 or index > 8 or index ~= math.floor(index) then
        return nil
    end
    return index
end

-- Planned distinguishes a pending Env_Tides/Terratide wave from the brief
-- post-ApplyEffect state where Index persists but MarkBoard is inactive.
local function mission_tides_planned(mission_id, live_environment)
    if mission_id ~= "Mission_Tides"
            and mission_id ~= "Mission_Terratide" then
        return nil
    end
    if type(live_environment) ~= "table"
            or type(live_environment.Planned) ~= "boolean" then
        return nil
    end
    return live_environment.Planned
end

-- Mission_Final's Env_Volcano consumes native RNG while Plan builds its
-- ordered Locations list. Export that already-selected list plus the compact
-- phase/mode state instead of guessing random_removal in the solver.
local function mission_final_volcano_points(points, maximum)
    if type(points) ~= "table" or #points > maximum then return nil end
    local result = {}
    local seen = {}
    for i = 1, #points do
        local point = points[i]
        local x = point and point.x
        local y = point and point.y
        if type(x) ~= "number" or x ~= math.floor(x) or x < 0 or x > 7
                or type(y) ~= "number" or y ~= math.floor(y) or y < 0 or y > 7 then
            return nil
        end
        local key = x .. "," .. y
        if seen[key] then return nil end
        seen[key] = true
        result[#result + 1] = {x, y}
    end
    return result
end

local function mission_final_volcano(mission_id, live_environment)
    if mission_id ~= "Mission_Final" then return nil end
    local incomplete = {
        complete = false,
        mode = 0,
        phase = 0,
        lava_start = {},
        locations = {},
        planned = {},
    }
    if type(live_environment) ~= "table" then return incomplete end

    local mode = live_environment.Mode
    local phase = live_environment.Phase
    if type(mode) ~= "number" or mode ~= math.floor(mode)
            or (mode ~= 1 and mode ~= 2)
            or type(phase) ~= "number" or phase ~= math.floor(phase)
            or phase < 0 or phase > 4 then
        return incomplete
    end
    if (phase == 0 and mode ~= 1)
            or ((phase == 1 or phase == 3) and mode ~= 2)
            or ((phase == 2 or phase == 4) and mode ~= 1) then
        return incomplete
    end

    local lava_start = mission_final_volcano_points(
        live_environment.LavaStart,
        2
    )
    local locations = mission_final_volcano_points(
        live_environment.Locations,
        4
    )
    local planned = mission_final_volcano_points(
        live_environment.Planned,
        4
    )
    if lava_start == nil or locations == nil or planned == nil
            or #locations ~= #planned
            or (phase > 0 and #locations == 0) then
        return incomplete
    end
    for i = 1, #locations do
        if locations[i][1] ~= planned[i][1]
                or locations[i][2] ~= planned[i][2] then
            return incomplete
        end
    end

    local start_seen = {}
    for _, point in ipairs(lava_start) do
        if not ((point[1] == 2 and point[2] == 1)
                or (point[1] == 1 and point[2] == 2)) then
            return incomplete
        end
        start_seen[point[1] .. "," .. point[2]] = true
    end
    local expected_start_count = phase == 0 and 2
        or ((phase == 1 or phase == 2) and 1 or 0)
    if #lava_start ~= expected_start_count then return incomplete end

    if mode == 2 then
        local first = locations[1]
        if first == nil or not ((first[1] == 2 and first[2] == 1)
                or (first[1] == 1 and first[2] == 2)) then
            return incomplete
        end
        if phase == 1 and start_seen[first[1] .. "," .. first[2]] then
            return incomplete
        end
        for i = 2, #locations do
            local dx = locations[i][1] - locations[i - 1][1]
            local dy = locations[i][2] - locations[i - 1][2]
            if not ((dx == 1 and dy == 0) or (dx == 0 and dy == 1)) then
                return incomplete
            end
        end
    elseif phase > 0 then
        local last_quarter = -1
        for _, point in ipairs(locations) do
            local x, y = point[1], point[2]
            if x < 1 or x > 6 or y < 1 or y > 6
                    or (x == 1 and y == 1) then
                return incomplete
            end
            local quarter = (x >= 4 and 2 or 0) + (y >= 4 and 1 or 0)
            if quarter <= last_quarter then return incomplete end
            last_quarter = quarter
        end
    end

    return {
        complete = true,
        mode = mode,
        phase = phase,
        lava_start = lava_start,
        locations = locations,
        planned = planned,
    }
end

-- Mission_Final_Cave's Env_Final also consumes native RNG before player
-- control. Preserve the already-selected ordered list, including the Instant
-- phase contract, and never try to rebuild the future selector in Rust.
local function mission_final_cave_points(points, maximum)
    return mission_final_volcano_points(points, maximum)
end

local function mission_final_cave(mission_id, live_environment)
    if mission_id ~= "Mission_Final_Cave" then return nil end
    local incomplete = {
        complete = false,
        mode = 0,
        phase = 0,
        ordered = false,
        instant = false,
        water_target = false,
        lava_path = {},
        locations = {},
        planned = {},
    }
    if type(live_environment) ~= "table" then return incomplete end

    local mode = live_environment.Mode
    local phase = live_environment.Phase
    if type(mode) ~= "number" or mode ~= math.floor(mode)
            or (mode ~= 1 and mode ~= 2)
            or type(phase) ~= "number" or phase ~= math.floor(phase)
            or phase < 1 or phase > 4 then
        return incomplete
    end
    local expected_mode = (phase == 1 or phase == 3) and 1 or 2
    local expected_instant = phase == 3 or phase == 4
    local expected_water_target = expected_mode == 1
    if mode ~= expected_mode
            or live_environment.Ordered ~= true
            or live_environment.Instant ~= expected_instant
            or live_environment.WaterTarget ~= expected_water_target then
        return incomplete
    end

    local lava_path = mission_final_cave_points(live_environment.LavaPath, 64)
    local locations = mission_final_cave_points(live_environment.Locations, 64)
    local planned = mission_final_cave_points(live_environment.Planned, 64)
    if lava_path == nil or #lava_path == 0
            or locations == nil or #locations == 0
            or planned == nil or #locations ~= #planned then
        return incomplete
    end
    for i = 1, #locations do
        if locations[i][1] ~= planned[i][1]
                or locations[i][2] ~= planned[i][2] then
            return incomplete
        end
    end

    if phase == 1 then
        if #locations < 3 or #locations > 4 then return incomplete end
        local last_quarter = -1
        for _, point in ipairs(locations) do
            local x, y = point[1], point[2]
            if x < 1 or x > 6 or y < 1 or y > 6 then return incomplete end
            local quarter = (x >= 4 and 2 or 0) + (y >= 4 and 1 or 0)
            if quarter <= last_quarter then return incomplete end
            last_quarter = quarter
        end
    elseif phase == 2 then
        if #locations < 1 or #locations > 3 then return incomplete end
    elseif phase == 3 then
        if #locations > 6 then return incomplete end
        local center = locations[1]
        if center[1] < 2 or center[1] > 5
                or center[2] < 2 or center[2] > 5 then
            return incomplete
        end
        for i = 2, #locations do
            local point = locations[i]
            if math.max(math.abs(point[1] - center[1]),
                    math.abs(point[2] - center[2])) ~= 1 then
                return incomplete
            end
        end
    else
        local gaps = 0
        for i = 2, #locations do
            local distance = math.abs(locations[i][1] - locations[i - 1][1])
                + math.abs(locations[i][2] - locations[i - 1][2])
            if distance == 2 then
                gaps = gaps + 1
            elseif distance ~= 1 then
                return incomplete
            end
        end
        if gaps > 1 then return incomplete end
    end

    return {
        complete = true,
        mode = mode,
        phase = phase,
        ordered = true,
        instant = expected_instant,
        water_target = expected_water_target,
        lava_path = lava_path,
        locations = locations,
        planned = planned,
    }
end

-- Mission_Terraform completes only when no point in Board:GetZone("grass")
-- retains the exact custom grass sprite. Save map_data also contains
-- decorative ground_grass.png markers outside that objective zone, so export
-- the live, zone-filtered remainder rather than making Python guess from the
-- static map. nil means unavailable/malformed; an empty table is an
-- authoritative completed objective.
local function mission_terraform_grass_tiles(mission_id, board)
    if mission_id ~= "Mission_Terraform" then return nil end

    local ok_zone, zone = pcall(function() return board:GetZone("grass") end)
    if not ok_zone or zone == nil then return nil end

    local ok_size, size = pcall(function() return zone:size() end)
    if not ok_size or type(size) ~= "number"
            or size ~= math.floor(size) or size < 0 or size > 64 then
        return nil
    end

    local result = {}
    local seen = {}
    for i = 1, size do
        local ok_point, point = pcall(function() return zone:index(i) end)
        if not ok_point or point == nil then return nil end
        local ok_xy, x, y = pcall(function() return point.x, point.y end)
        if not ok_xy or type(x) ~= "number" or type(y) ~= "number"
                or x ~= math.floor(x) or y ~= math.floor(y)
                or x < 0 or x > 7 or y < 0 or y > 7 then
            return nil
        end

        local ok_custom, custom = pcall(function()
            return board:GetCustomTile(point)
        end)
        if not ok_custom then return nil end
        if custom == "ground_grass.png" then
            local key = x .. "," .. y
            if not seen[key] then
                seen[key] = true
                result[#result + 1] = {x, y}
            end
        end
    end
    table.sort(result, function(a, b)
        return a[1] < b[1] or (a[1] == b[1] and a[2] < b[2])
    end)
    return result
end

-- Exact identity for Mission_Hacking's stored Cannon Bot and facility. Return
-- the pair together or nothing: partial/malformed identity must never make the
-- simulator guess from another Snowtank1 of the same type.
local function mission_hacking_ids(mission_id, mission)
    if mission_id ~= "Mission_Hacking" or type(mission) ~= "table" then
        return nil, nil
    end
    local bot_id = mission.BotID
    local hack_id = mission.HackID
    local function valid_pawn_id(value)
        return type(value) == "number"
            and value == value
            and value ~= math.huge
            and value ~= -math.huge
            and value >= 0
            and value <= 65535
            and value == math.floor(value)
    end
    if not valid_pawn_id(bot_id) or not valid_pawn_id(hack_id)
            or bot_id == hack_id then
        return nil, nil
    end
    return bot_id, hack_id
end

-- Mission_Piston creates up to four neutral Trash Compactors whose exact pawn
-- type fixes the tile they push. Export the entire live action set atomically
-- in state.units / native Board pawn-vector order. Native build 13725832 plans
-- and dispatches neutral Pistons from that same vector; sorting by UID would
-- destroy their exact interleave with queued Vek. complete=true with an empty
-- list is distinct from an older/malformed bridge that could not inspect the
-- mission.
local function mission_pistons(mission_id, units)
    if mission_id ~= "Mission_Piston" or type(units) ~= "table" then
        return nil
    end
    local offsets = {
        Pawn_Piston_U = {0, -1},
        Pawn_Piston_R = {1, 0},
        Pawn_Piston_D = {0, 1},
        Pawn_Piston_L = {-1, 0},
    }
    local actions = {}
    local seen = {}
    local piston_count = 0
    for _, unit in ipairs(units) do
        local offset = type(unit) == "table" and offsets[unit.type] or nil
        if offset ~= nil and not unit.is_extra_tile then
            piston_count = piston_count + 1
            local uid = unit.uid
            local x = unit.x
            local y = unit.y
            local hp = unit.hp
            local valid = type(uid) == "number"
                and uid == math.floor(uid) and uid >= 0 and uid <= 65535
                and not seen[uid]
                and type(x) == "number" and x == math.floor(x) and x >= 0 and x < 8
                and type(y) == "number" and y == math.floor(y) and y >= 0 and y < 8
                and type(hp) == "number" and hp == math.floor(hp)
                and unit.team == 2
            if not valid or piston_count > 4 then
                return { complete = false, actions = {} }
            end
            seen[uid] = true
            if hp > 0 then
                local front_x = x + offset[1]
                local front_y = y + offset[2]
                if front_x < 0 or front_x >= 8 or front_y < 0 or front_y >= 8 then
                    return { complete = false, actions = {} }
                end
                actions[#actions + 1] = {
                    uid = uid,
                    front = {front_x, front_y},
                }
            end
        end
    end
    return { complete = true, actions = actions }
end

-- CreateTutorial constructs Mission_Tutorial without assigning the ID field
-- used by ordinary CreateMission.  Preserve every explicit mission ID and
-- synthesize only this source-defined tutorial identity so safety gates do not
-- mistake its scripted native lifecycle for an ordinary combat mission.
local function mission_bridge_id(mission)
    if mission == nil then return nil end
    local mission_id = mission.ID
    if (mission_id == nil or mission_id == "")
            and mission.Name == "Tutorial" then
        return "Mission_Tutorial"
    end
    return mission_id
end

--------------------------------------------------------------------
-- Bridge extension (live validation, 2026-10): engine-facing exports,
-- scenario tooling and enemy-phase ledgers.
--
-- Everything in ITBX is additive. Each export runs under pcall and reports
-- a failure in state.bridge_errors instead of breaking the dump, and none of
-- it writes into Lua mission or environment instances (the game saves those).
-- Only Lua API calls listed in the game's binding table are used; calls that
-- may not exist in a build are probed with pcall. The table is also reachable
-- as the global _ITB_BRIDGE_EXT so the offline harness
-- (engine/tests/bridge/bridge_harness.lua) can test the pure helpers.
--------------------------------------------------------------------
local ITBX = {
    -- 2: native-only player actions (EXEC_MODE), idle waits on the actor.
    VERSION = 2,
    -- "native" (default): every player action is the game's own
    -- Pawn:FireWeapon. "legacy": the old bot's emulations (EXEC_MODE).
    exec_mode = "native",
    DEBUG_FLAG_FILE = BRIDGE_DIR .. "/itb_bridge_debug",
    PRE_SPAWN_FILE = BRIDGE_DIR .. "/itb_state_enemy_prespawn.json",
    POST_SPAWN_FILE = BRIDGE_DIR .. "/itb_state_enemy_postspawn.json",
    SNAPSHOT_PREFIX = BRIDGE_DIR .. "/itb_snapshot_",
    PHASE_LOG_FILE = BRIDGE_DIR .. "/itb_phase_log.json",
    PHASE_LOG_MAX = 4000,
    ENV_LOG_MAX = 64,
    DUMP_NODE_BUDGET = 3000,
    DUMP_MAX_DEPTH = 4,
    -- Board:GetZone names read by shipped combat hooks (stage 7 spec 4.3).
    ZONES = {
        "dam", "satellite", "pylons", "falling", "mountain", "deployment",
        "enemy", "hornets", "grass", "terraformer", "filler", "disposal",
        "lasers", "pistons", "flooding",
    },
    JSON_NULL = setmetatable({}, {__tostring = function() return "null" end}),
}
_ITB_BRIDGE_EXT = ITBX

-- The debug flag file enables the scenario command and the per-frame phase
-- log. Checked at most once per wall-clock second.
function ITBX.debug_enabled()
    local now = os.time()
    if ITBX._debug_at == now and ITBX._debug_on ~= nil then
        return ITBX._debug_on
    end
    local f = io.open(ITBX.DEBUG_FLAG_FILE, "r")
    local on = f ~= nil
    if f then f:close() end
    ITBX._debug_on = on
    ITBX._debug_at = now
    return on
end

local function itbx_turn_team()
    local turn, team = nil, nil
    pcall(function() turn = Game:GetTurnCount() end)
    pcall(function() team = Game:GetTeamTurn() end)
    return turn, team
end

---------------------------------------------------------------- settled state
-- Board:GetBusyState(): 0 when idle; otherwise a projectile is in flight
-- (8), a tile animates (2), a pawn animates (1: walk, push, leap, fall), a
-- death effect is pending (9), effects are stacked (6), status ticks are
-- pending (7) or spawning runs (3-5). Board:IsBusy() is "state ~= 0". A
-- pushed pawn changes tile only when its 0.4 s push finishes, so a dump
-- taken while busy can show pawns on their pre-push tiles (live session
-- 2026-10-09: Ranged_Ignite's side pushes read before they landed).
ITBX.cmd_waiting = false      -- a command coroutine is waiting for the board
ITBX.redump_when_idle = false -- the last dump was not settled: dump again when idle
ITBX.dump_seq = 0

function ITBX.busy_state()
    local ok, s = pcall(function() return Board:GetBusyState() end)
    if ok and type(s) == "number" then return s end
    local ok_b, b = pcall(function() return Board:IsBusy() end)
    if ok_b and type(b) == "boolean" then return b and 1 or 0 end
    return nil
end

-- board_busy / busy_state: the board at dump time. command_waiting: a bridge
-- command is still waiting for its effects. stable: neither, so positions
-- and HP are final for now. dump_seq: increases with every dump.
function ITBX.mark_settled(state)
    local s = ITBX.busy_state()
    ITBX.dump_seq = ITBX.dump_seq + 1
    state.dump_seq = ITBX.dump_seq
    state.busy_state = s
    state.board_busy = s ~= nil and s ~= 0
    state.command_waiting = ITBX.cmd_waiting == true
    state.stable = not state.board_busy and not state.command_waiting
    ITBX.redump_when_idle = not state.stable
end

-- Called every frame: once the board is idle again after an unsettled
-- dump, dump again so readers do not wait for the periodic dump.
function ITBX.settled_redump(dump)
    if not ITBX.redump_when_idle or ITBX.cmd_waiting then return false end
    if ITBX.busy_state() ~= 0 then return false end
    ITBX.redump_when_idle = false
    pcall(dump)
    return true
end

---------------------------------------------------------------- errors
function ITBX.begin_dump()
    ITBX.errs = {}
    ITBX.err_seen = {}
end

function ITBX.str(s)
    s = tostring(s)
    if string.len(s) > 400 then s = string.sub(s, 1, 400) .. "..." end
    return (string.gsub(s, "%c", " "))
end

-- Records one failure per `where` per dump (a failing per-tile probe would
-- otherwise repeat 64 times).
function ITBX.note_error(where, err)
    ITBX.errs = ITBX.errs or {}
    ITBX.err_seen = ITBX.err_seen or {}
    if ITBX.err_seen[where] then return end
    ITBX.err_seen[where] = true
    if #ITBX.errs < 64 then
        ITBX.errs[#ITBX.errs + 1] = {where = where, error = ITBX.str(err)}
    end
end

---------------------------------------------------------------- safety valve
-- Every piece of extension work (dump exports, hook bookkeeping, the
-- per-frame log) runs through ITBX.guard. The outermost guarded call gets a
-- CPU budget: a Lua count hook (when the game's Lua has the debug library
-- and no hook of its own is set) aborts the call once GUARD.hard seconds
-- are spent, and a call that ran over GUARD.hard, or GUARD.soft_max calls
-- over GUARD.soft, switch the extension off for the rest of the session
-- (ITBX.disabled; logged once, exported as state.bridge_ext_disabled).
-- Disabled, every guarded call is a no-op and the old bridge carries on,
-- so an extension bug can cost one slow frame but never hang the game.
ITBX.GUARD = {hard = 1.0, soft = 0.25, soft_max = 3, count = 1000}
ITBX.ABORT = "ITBX: time budget exceeded"
ITBX._depth = 0
ITBX._strikes = 0

local function itbx_pack(...)
    return {n = select("#", ...), ...}
end
ITBX.pack = itbx_pack

function ITBX.disable(where, why)
    if ITBX.disabled then return end
    ITBX.disabled = true
    ITBX.disabled_reason = tostring(where) .. ": " .. tostring(why)
    pcall(log_bridge, "BRIDGE EXT DISABLED (" .. ITBX.disabled_reason
        .. "); old bridge fields continue")
end

-- pcall(fn, ...) with the budget above. Returns pcall's results.
function ITBX.guard(where, fn, ...)
    if ITBX.disabled then return false, "bridge extension disabled" end
    if ITBX._depth > 0 then
        -- Nested: the outermost call holds the budget. Re-raise an abort so
        -- a pcall inside the extension cannot swallow it.
        local r = itbx_pack(pcall(fn, ...))
        if ITBX._aborting then error(ITBX.ABORT, 0) end
        return unpack(r, 1, r.n)
    end
    ITBX._depth = 1
    ITBX._aborting = false
    local start = os.clock()
    local deadline = start + ITBX.GUARD.hard
    local dbg = rawget(_G, "debug")
    local hooked = false
    if type(dbg) == "table" and dbg.sethook and dbg.gethook and dbg.gethook() == nil then
        hooked = pcall(dbg.sethook, function()
            if ITBX._aborting or os.clock() > deadline then
                ITBX._aborting = true
                error(ITBX.ABORT, 0)
            end
        end, "", ITBX.GUARD.count)
    end
    local r = itbx_pack(pcall(fn, ...))
    if hooked then pcall(dbg.sethook) end
    ITBX._depth = 0
    local elapsed = os.clock() - start
    local aborted = ITBX._aborting
    ITBX._aborting = false
    if aborted or elapsed > ITBX.GUARD.hard then
        ITBX.disable(where, string.format("ran %.2fs CPU (budget %.2fs)%s", elapsed,
            ITBX.GUARD.hard, aborted and ", aborted" or ""))
        if aborted then return false, ITBX.ABORT end
    elseif elapsed > ITBX.GUARD.soft then
        ITBX._strikes = ITBX._strikes + 1
        pcall(log_bridge, string.format("BRIDGE EXT SLOW: %s took %.2fs CPU (%d/%d)",
            tostring(where), elapsed, ITBX._strikes, ITBX.GUARD.soft_max))
        if ITBX._strikes >= ITBX.GUARD.soft_max then
            ITBX.disable(where, ITBX._strikes .. " calls over " .. ITBX.GUARD.soft .. "s")
        end
    end
    return unpack(r, 1, r.n)
end

-- Guarded call that records the failure; returns fn's first result or nil.
function ITBX.try(where, fn, ...)
    local ok, res = ITBX.guard(where, fn, ...)
    if not ok then
        if not ITBX.disabled then ITBX.note_error(where, res) end
        return nil
    end
    return res
end

-- Bookkeeping run from a game hook: guarded, never re-entered (a dump
-- inside a hook can call back into game Lua), failures dropped.
function ITBX.hook_work(where, fn, ...)
    if ITBX.disabled or ITBX._in_hook then return end
    ITBX._in_hook = true
    pcall(ITBX.guard, where, fn, ...)
    ITBX._in_hook = false
end

---------------------------------------------------------------- values
function ITBX.num(v)
    if v ~= v or v == math.huge or v == -math.huge then return tostring(v) end
    return v
end

-- A Point (userdata, or a table with numeric x/y) -> x, y; else nil.
function ITBX.point_xy(v)
    local tv = type(v)
    if tv ~= "userdata" and tv ~= "table" then return nil end
    local ok, x, y = pcall(function() return v.x, v.y end)
    if ok and type(x) == "number" and type(y) == "number" then return x, y end
    return nil
end

-- PointList / IntList userdata -> plain array ({x=,y=} or numbers); nil if
-- `v` is not a list.
function ITBX.list_of_ud(v)
    if type(v) ~= "userdata" then return nil end
    local ok_n, n = pcall(function() return v:size() end)
    if not ok_n or type(n) ~= "number" or n < 0 or n > 4096 then return nil end
    local out = {}
    for i = 1, n do
        local ok_e, e = pcall(function() return v:index(i) end)
        if not ok_e then return nil end
        local x, y = ITBX.point_xy(e)
        if x then
            out[#out + 1] = {x = x, y = y}
        elseif type(e) == "number" then
            out[#out + 1] = ITBX.num(e)
        else
            out[#out + 1] = "<ud>"
        end
    end
    return out
end

-- Points of a PointList / Lua array of Points as [[x, y], ...].
function ITBX.point_pairs(v)
    local out = {}
    if type(v) == "userdata" then
        local list = ITBX.list_of_ud(v) or {}
        for _, p in ipairs(list) do
            if type(p) == "table" then out[#out + 1] = {p.x, p.y} end
        end
    elseif type(v) == "table" then
        for _, p in ipairs(v) do
            local x, y = ITBX.point_xy(p)
            if x then out[#out + 1] = {x, y} end
        end
    end
    return out
end

local function itbx_key_string(k)
    local tk = type(k)
    if tk == "string" then return k end
    if tk == "number" then return tostring(ITBX.num(k)) end
    if tk == "boolean" then return tostring(k) end
    local x, y = ITBX.point_xy(k)
    if x then return "Point(" .. x .. "," .. y .. ")" end
    return "<" .. tk .. ">"
end

-- Plain-data copy of a Lua value for the JSON export (stage 7 spec 4.2):
-- instance fields only (raw pairs, no metatable lookups), functions
-- skipped, Points -> {x=,y=}, PointList/IntList -> arrays, other userdata
-- -> "<ud>" (counted), nested tables up to ctx.max_depth, at most
-- ctx.budget table/userdata nodes. Arrays are tables whose keys are exactly
-- 1..n; anything else becomes an object with string keys.
function ITBX.dump_value(v, depth, ctx, exclude)
    local tv = type(v)
    if tv == "nil" then return nil end
    if tv == "boolean" then return v end
    if tv == "number" then return ITBX.num(v) end
    if tv == "string" then return ITBX.str(v) end
    if tv ~= "table" and tv ~= "userdata" then return nil end
    if rawequal(v, ITBX.JSON_NULL) then return nil end
    ctx.nodes = ctx.nodes + 1
    if ctx.nodes > ctx.budget then
        ctx.truncated = true
        return "<budget>"
    end
    if tv == "userdata" then
        local x, y = ITBX.point_xy(v)
        if x then return {x = ITBX.num(x), y = ITBX.num(y)} end
        local list = ITBX.list_of_ud(v)
        if list then return list end
        ctx.userdata = ctx.userdata + 1
        return "<ud>"
    end
    if ctx.seen[v] then return "<cycle>" end
    if depth >= ctx.max_depth then
        ctx.truncated = true
        return "<depth>"
    end
    ctx.seen[v] = true
    local count = 0
    for _ in pairs(v) do count = count + 1 end
    local is_array = count > 0
    for i = 1, count do
        if rawget(v, i) == nil then
            is_array = false
            break
        end
    end
    local out = {}
    if is_array then
        for i = 1, count do
            local e = ITBX.dump_value(rawget(v, i), depth + 1, ctx)
            if e == nil then e = "<fn>" end
            out[i] = e
        end
    else
        for k, val in pairs(v) do
            local key = itbx_key_string(k)
            if not (exclude and exclude[key]) and type(val) ~= "function" then
                local e = ITBX.dump_value(val, depth + 1, ctx)
                if e ~= nil then out[key] = e end
            end
        end
    end
    ctx.seen[v] = nil
    return out
end

function ITBX.new_dump_ctx()
    return {
        nodes = 0, budget = ITBX.DUMP_NODE_BUDGET, max_depth = ITBX.DUMP_MAX_DEPTH,
        seen = {}, userdata = 0, truncated = false,
    }
end

---------------------------------------------------------------- JSON in
-- Minimal JSON decoder for the SCENARIO payload. null decodes to
-- ITBX.JSON_NULL (kept in arrays, dropped from objects).
function ITBX.json_decode(s)
    if type(s) ~= "string" then error("json: not a string", 0) end
    local pos = 1
    local len = string.len(s)
    local function fail(msg)
        error("json: " .. msg .. " at byte " .. pos, 0)
    end
    local function skip_ws()
        local p = string.find(s, "[^ \t\r\n]", pos)
        pos = p or (len + 1)
    end
    local escapes = {
        ['"'] = '"', ["\\"] = "\\", ["/"] = "/",
        b = "\b", f = "\f", n = "\n", r = "\r", t = "\t",
    }
    local function parse_string()
        pos = pos + 1  -- opening quote
        local buf = {}
        while true do
            local p = string.find(s, '["\\]', pos)
            if not p then fail("unterminated string") end
            buf[#buf + 1] = string.sub(s, pos, p - 1)
            local c = string.sub(s, p, p)
            if c == '"' then
                pos = p + 1
                break
            end
            local e = string.sub(s, p + 1, p + 1)
            if escapes[e] then
                buf[#buf + 1] = escapes[e]
                pos = p + 2
            elseif e == "u" then
                local code = tonumber(string.sub(s, p + 2, p + 5), 16)
                if not code then fail("bad \\u escape") end
                buf[#buf + 1] = code < 128 and string.char(code) or "?"
                pos = p + 6
            else
                fail("bad escape")
            end
        end
        return table.concat(buf)
    end
    local parse_value
    local function parse_array()
        pos = pos + 1
        local out = {}
        skip_ws()
        if string.sub(s, pos, pos) == "]" then
            pos = pos + 1
            return out
        end
        while true do
            out[#out + 1] = parse_value()
            skip_ws()
            local c = string.sub(s, pos, pos)
            pos = pos + 1
            if c == "]" then return out end
            if c ~= "," then fail("expected , or ]") end
        end
    end
    local function parse_object()
        pos = pos + 1
        local out = {}
        skip_ws()
        if string.sub(s, pos, pos) == "}" then
            pos = pos + 1
            return out
        end
        while true do
            skip_ws()
            if string.sub(s, pos, pos) ~= '"' then fail("expected key") end
            local key = parse_string()
            skip_ws()
            if string.sub(s, pos, pos) ~= ":" then fail("expected :") end
            pos = pos + 1
            local v = parse_value()
            if not rawequal(v, ITBX.JSON_NULL) then out[key] = v end
            skip_ws()
            local c = string.sub(s, pos, pos)
            pos = pos + 1
            if c == "}" then return out end
            if c ~= "," then fail("expected , or }") end
        end
    end
    parse_value = function()
        skip_ws()
        local c = string.sub(s, pos, pos)
        if c == "{" then return parse_object() end
        if c == "[" then return parse_array() end
        if c == '"' then return parse_string() end
        if string.sub(s, pos, pos + 3) == "true" then pos = pos + 4; return true end
        if string.sub(s, pos, pos + 4) == "false" then pos = pos + 5; return false end
        if string.sub(s, pos, pos + 3) == "null" then pos = pos + 4; return ITBX.JSON_NULL end
        local numtext = string.match(s, "^-?%d+%.?%d*[eE]?[-+]?%d*", pos)
        if numtext and numtext ~= "" and numtext ~= "-" then
            local n = tonumber(numtext)
            if n == nil then fail("bad number") end
            pos = pos + string.len(numtext)
            return n
        end
        fail("unexpected character '" .. c .. "'")
    end
    local v = parse_value()
    skip_ws()
    if pos <= len then fail("trailing data") end
    return v
end

---------------------------------------------------------------- classes
-- Reverse lookup table -> global name for mission and environment classes.
function ITBX.class_names(rebuild)
    if ITBX._class_names and not rebuild then return ITBX._class_names end
    local names = {}
    for k, v in pairs(_G) do
        if type(k) == "string" and type(v) == "table"
                and (string.find(k, "^Env_") or string.find(k, "^Mission")
                     or k == "Environment") then
            if names[v] == nil or string.len(k) < string.len(names[v]) then
                names[v] = k
            end
        end
    end
    ITBX._class_names = names
    return names
end

-- Class names from the instance's metatable up (CreateClass sets an
-- instance's metatable to its class, and a class's to its parent).
function ITBX.class_chain(obj)
    local chain = {}
    if type(obj) ~= "table" then return chain end
    local names = ITBX.class_names()
    local first = getmetatable(obj)
    if type(first) == "table" and names[first] == nil then
        names = ITBX.class_names(true)
    end
    local mt = first
    local guard = 0
    while type(mt) == "table" and guard < 16 do
        chain[#chain + 1] = names[mt] or "?"
        mt = getmetatable(mt)
        guard = guard + 1
    end
    return chain
end

---------------------------------------------------------------- mission
function ITBX.mission_key(mission)
    local game = rawget(_G, "GAME")
    if type(game) ~= "table" or type(game.Missions) ~= "table" then return nil end
    for k, m in pairs(game.Missions) do
        if rawequal(m, mission) then return k end
    end
    return nil
end

-- Stage 7 spec 4.1 / 4.2: identity plus raw instance dumps of the mission
-- and its LiveEnvironment.
function ITBX.mission_state(mission)
    local ms = {}
    local key = ITBX.mission_key(mission)
    if key ~= nil then
        ms.key = key
        ms.native_key = "Mission" .. tostring(key)
    end
    if type(mission.ID) == "string" then ms.id = mission.ID end
    ms.turn = ITBX.try("mission_state.turn", function() return Game:GetTurnCount() end)
    ms.team_turn = ITBX.try("mission_state.team_turn", function() return Game:GetTeamTurn() end)
    ms.sector = ITBX.try("mission_state.sector", function() return Game:GetSector() end)
    ms.difficulty = ITBX.try("mission_state.difficulty", function() return GetDifficulty() end)
    ms.new_enemies = ITBX.try("mission_state.new_enemies", function() return IsNewEnemies() end)
    if type(mission.TurnLimit) == "number" then ms.turn_limit = mission.TurnLimit end
    if type(mission.Environment) == "string" then ms.environment = mission.Environment end
    ms.class_chain = ITBX.class_chain(mission)
    local ctx = ITBX.new_dump_ctx()
    ms.instance = ITBX.dump_value(mission, 0, ctx, {LiveEnvironment = true})
    local le = rawget(mission, "LiveEnvironment")
    ms.env_is_instance = le ~= nil
    if le == nil then le = mission.LiveEnvironment end
    if type(le) == "table" then
        ms.env_class_chain = ITBX.class_chain(le)
        if rawget(le, "ApplyEffect") ~= nil then ms.env_instance_overrides_apply = true end
        ms.env_instance = ITBX.dump_value(le, 0, ctx)
    end
    ms.dump_truncated = ctx.truncated
    ms.dump_userdata = ctx.userdata
    return ms
end

---------------------------------------------------------------- save file
local function itbx_int_list(blob)
    local out = {}
    if not blob then return out end
    for n in string.gmatch(blob, "%-?%d+") do out[#out + 1] = tonumber(n) end
    return out
end

local function itbx_save_point(block, key)
    local x, y = string.match(block,
        '%["' .. key .. '"%]%s*=%s*Point%s*%(%s*(%-?%d+)%s*,%s*(%-?%d+)%s*%)')
    if x then return {tonumber(x), tonumber(y)} end
    return nil
end

-- The active battle region of the save (iBattleRegion): its mission, turn,
-- spawn queue in queue order (Board::SaveScript `spawns` / `spawn_ids` /
-- `spawn_points`), spawn blocks, and per-pawn weapons, pilots, mutation and
-- queued shots. The save is written at turn boundaries only.
function ITBX.parse_save(content)
    local out = {pawns = {}}
    if type(content) ~= "string" then return out end
    local br = string.match(content, '%["iBattleRegion"%]%s*=%s*(%-?%d+)')
    if not br then return out end
    out.battle_region = tonumber(br)
    local key = out.battle_region == 20 and "final_region" or ("region" .. br)
    local block = string.match(content, '%["' .. key .. '"%]%s*=%s*(%b{})')
    if not block then return out end
    out.region_key = key
    out.mission = string.match(block, '%["mission"%]%s*=%s*"([^"]*)"')
    out.turn = tonumber(string.match(block, '%["iCurrentTurn"%]%s*=%s*(%-?%d+)') or "")
    out.team_turn = tonumber(string.match(block, '%["iTeamTurn"%]%s*=%s*(%-?%d+)') or "")
    local types_blob = string.match(block, '%["spawns"%]%s*=%s*(%b{})')
    local points_blob = string.match(block, '%["spawn_points"%]%s*=%s*(%b{})')
    if types_blob and points_blob then
        local types, pts = {}, {}
        for t in string.gmatch(types_blob, '"([^"]*)"') do types[#types + 1] = t end
        for x, y in string.gmatch(points_blob, "Point%s*%(%s*(%-?%d+)%s*,%s*(%-?%d+)%s*%)") do
            pts[#pts + 1] = {tonumber(x), tonumber(y)}
        end
        local ids = itbx_int_list(string.match(block, '%["spawn_ids"%]%s*=%s*(%b{})'))
        out.spawns = {}
        for i = 1, math.max(#types, #pts) do
            out.spawns[i] = {
                type = types[i] or "",
                uid = ids[i] or -1,
                x = pts[i] and pts[i][1] or -1,
                y = pts[i] and pts[i][2] or -1,
            }
        end
    end
    local bp = string.match(block, '%["blocked_points"%]%s*=%s*(%b{})')
    if bp then
        local bt = itbx_int_list(string.match(block, '%["blocked_type"%]%s*=%s*(%b{})'))
        out.spawn_blocks = {}
        local i = 0
        for x, y in string.gmatch(bp, "Point%s*%(%s*(%-?%d+)%s*,%s*(%-?%d+)%s*%)") do
            i = i + 1
            out.spawn_blocks[i] = {tonumber(x), tonumber(y), bt[i] or -1}
        end
    end
    for pb in string.gmatch(block, '%["pawn%d+"%]%s*=%s*(%b{})') do
        local id = tonumber(string.match(pb, '%["id"%]%s*=%s*(%-?%d+)') or "")
        if id then
            local rec = {id = id}
            rec.type = string.match(pb, '%["type"%]%s*=%s*"([^"]*)"')
            rec.mutation = tonumber(string.match(pb, '%["iMutation"%]%s*=%s*(%-?%d+)') or "")
            rec.queued_skill = tonumber(string.match(pb, '%["iQueuedSkill"%]%s*=%s*(%-?%d+)') or "")
            rec.queued_shot = itbx_save_point(pb, "piQueuedShot")
            rec.queued_origin = itbx_save_point(pb, "piOrigin")
            rec.queued_target = itbx_save_point(pb, "piTarget")
            rec.weapons = {}
            for slot_index, slot in ipairs({"primary", "secondary"}) do
                local base = string.match(pb, '%["' .. slot .. '"%]%s*=%s*"([^"]*)"')
                if base and base ~= "" then
                    rec.weapons[#rec.weapons + 1] = {
                        slot = slot_index - 1,
                        base = base,
                        power = itbx_int_list(string.match(pb, '%["' .. slot .. '_power"%]%s*=%s*(%b{})')),
                        mod1 = itbx_int_list(string.match(pb, '%["' .. slot .. '_mod1"%]%s*=%s*(%b{})')),
                        mod2 = itbx_int_list(string.match(pb, '%["' .. slot .. '_mod2"%]%s*=%s*(%b{})')),
                        uses = tonumber(string.match(pb, '%["' .. slot .. '_uses"%]%s*=%s*(%-?%d+)') or ""),
                        damaged = string.match(pb, '%["' .. slot .. '_damaged"%]%s*=%s*(%a+)') == "true",
                    }
                end
            end
            local pilot = string.match(pb, '%["pilot"%]%s*=%s*(%b{})')
            if pilot then
                rec.pilot = {
                    id = string.match(pilot, '%["id"%]%s*=%s*"([^"]*)"'),
                    level = tonumber(string.match(pilot, '%["level"%]%s*=%s*(%-?%d+)') or ""),
                    xp = tonumber(string.match(pilot, '%["exp"%]%s*=%s*(%-?%d+)') or ""),
                    skill1 = tonumber(string.match(pilot, '%["skill1"%]%s*=%s*(%-?%d+)') or ""),
                    skill2 = tonumber(string.match(pilot, '%["skill2"%]%s*=%s*(%-?%d+)') or ""),
                }
            end
            out.pawns[id] = rec
        end
    end
    return out
end

-- parse_save once per file version (Lua strings are interned, so the
-- equality test is a pointer compare). One cache slot per file name.
ITBX._parse_cache = {}
function ITBX.parse_save_cached(name, content)
    local c = ITBX._parse_cache[name]
    if c and c.content == content then return c.result end
    local result = ITBX.parse_save(content)
    ITBX._parse_cache[name] = {content = content, result = result}
    return result
end

---------------------------------------------------------------- weapons
local function itbx_all_positive(list)
    if type(list) ~= "table" or #list == 0 then return false end
    for _, v in ipairs(list) do
        if (tonumber(v) or 0) <= 0 then return false end
    end
    return true
end

-- The weapon table actually fired: base + _A / _B / _AB when every power
-- point of that upgrade is filled (the same rule as the old save overlay).
function ITBX.exact_weapon_id(w)
    local base = w.base
    local a = itbx_all_positive(w.mod1)
    local b = itbx_all_positive(w.mod2)
    local candidates = {}
    if a and b then candidates[#candidates + 1] = base .. "_AB" end
    if a then candidates[#candidates + 1] = base .. "_A" end
    if b then candidates[#candidates + 1] = base .. "_B" end
    for _, c in ipairs(candidates) do
        if rawget(_G, c) ~= nil then return c end
    end
    return base
end

-- Per-slot weapon facts. `uses` is the live count of a limited weapon
-- derived from Pawn:GetShotsRemaining() (sum over non-Move skills: uses for
-- limited ones, +1 for each unlimited one); `uses_saved` is the save's
-- value at the last turn boundary. Returns the slots and the saved total
-- of limited uses.
function ITBX.weapon_slots(save_weapons, shots_remaining, unlimited_known)
    local slots = {}
    local limited_total_saved = 0
    local unlimited_all, unlimited_active = 0, 0
    local limited_slots = {}
    for _, w in ipairs(save_weapons or {}) do
        local id = ITBX.exact_weapon_id(w)
        local def = rawget(_G, id) or rawget(_G, w.base)
        local limited = 0
        local passive = false
        local power_cost = 0
        if type(def) == "table" then
            limited = tonumber(def.Limited) or 0
            passive = type(def.Passive) == "string" and def.Passive ~= ""
            power_cost = tonumber(def.PowerCost) or 0
        end
        -- Reactor cores in the weapon's own power slots (save *_power).
        local cores = 0
        for _, v in ipairs(w.power or {}) do
            if (tonumber(v) or 0) > 0 then cores = cores + 1 end
        end
        local s = {
            slot = w.slot, base = w.base, id = id, power = w.power,
            mod1 = w.mod1, mod2 = w.mod2, limited = limited, passive = passive,
            uses_saved = w.uses, damaged = w.damaged,
            power_cost = power_cost, powered = cores >= power_cost,
        }
        if limited > 0 then
            limited_slots[#limited_slots + 1] = s
            limited_total_saved = limited_total_saved + (w.uses or limited)
            s.uses = w.uses
        else
            unlimited_all = unlimited_all + 1
            if not passive then unlimited_active = unlimited_active + 1 end
        end
        slots[#slots + 1] = s
    end
    if type(shots_remaining) == "number" and #limited_slots > 0 then
        -- How many unlimited skills GetShotsRemaining counts: calibrated
        -- (see ITBX.unit_ext) when known; otherwise passive weapons may or
        -- may not be skills, so keep the readings that land in
        -- [0, saved uses] and give up if they disagree.
        local candidates = {}
        if type(unlimited_known) == "number" then
            candidates[1] = {"calibrated", shots_remaining - unlimited_known}
        else
            for _, basis in ipairs({{"all", unlimited_all}, {"non_passive", unlimited_active}}) do
                local live = shots_remaining - basis[2]
                if live >= 0 and live <= limited_total_saved then
                    candidates[#candidates + 1] = {basis[1], live}
                end
            end
        end
        if #candidates >= 1 and (#candidates == 1 or candidates[1][2] == candidates[2][2]) then
            local live = math.max(0, candidates[1][2])
            if #limited_slots == 1 then
                limited_slots[1].uses = live
                limited_slots[1].uses_basis = candidates[1][1]
            elseif live == limited_total_saved then
                for _, s in ipairs(limited_slots) do s.uses_basis = candidates[1][1] end
            else
                for _, s in ipairs(limited_slots) do s.uses_ambiguous = true end
            end
        else
            for _, s in ipairs(limited_slots) do s.uses_ambiguous = true end
        end
    end
    return slots, limited_total_saved
end

-- Per-mission uid -> number of unlimited skills GetShotsRemaining counts.
function ITBX.uses_calibration()
    local c = _ITB_BRIDGE_USES_CALIB
    if c == nil or not rawequal(c.mission, _ITB_CURRENT_MISSION) then
        c = {mission = _ITB_CURRENT_MISSION, by_uid = {}}
        _ITB_BRIDGE_USES_CALIB = c
    end
    return c.by_uid
end

---------------------------------------------------------------- units
-- Static Lua traits of a pawn type (read through the class chain, as the
-- native LuaData getters do).
function ITBX.traits(def)
    if type(def) ~= "table" then return nil end
    local function b(v) return v == true end
    local t = {
        minor = b(def.Minor), explodes = b(def.Explodes),
        ignore_smoke = b(def.IgnoreSmoke), ignore_fire = b(def.IgnoreFire),
        burns = b(def.Burns), corpse = b(def.Corpse), armor = b(def.Armor),
        massive = b(def.Massive), flying = b(def.Flying), jumper = b(def.Jumper),
        teleporter = b(def.Teleporter), burrows = b(def.Burrows),
        neutral = b(def.Neutral), pushable = def.Pushable ~= false,
        ignore_flip = b(def.IgnoreFlip), non_grid = b(def.NonGrid),
        spawn_limit = def.SpawnLimit ~= false,
    }
    if type(def.Leader) == "number" then t.leader = def.Leader end
    if type(def.DefaultFaction) == "number" then t.faction = def.DefaultFaction end
    if type(def.Health) == "number" then t.health = def.Health end
    if type(def.MoveSpeed) == "number" then t.move_speed = def.MoveSpeed end
    return t
end

-- Adds the extension fields to each main unit entry; runs before the old
-- attack_order pass so scenario-queued shots are part of it.
function ITBX.after_units(state, save_data)
    local save = ITBX.best_save(save_data and save_data.raw_content, state.units)
    ITBX._save = save
    if save.file then state.save_source = save.file end
    local scenario = ITBX.active_scenario()
    for _, u in ipairs(state.units or {}) do
        if not u.is_extra_tile then
            ITBX.try("unit_ext", ITBX.unit_ext, u, save, scenario)
            ITBX.try("unit_moved", ITBX.unit_moved, u)
        end
    end
end

-- saveData.lua and undoSave.lua can hold different moments (and an old
-- run's mission under the same key): keep the active region whose pawns
-- match the live board best, then the later turn; undoSave on a tie.
function ITBX.best_save(save_data_content, units)
    local live = {}
    for _, u in ipairs(units or {}) do live[u.uid] = u.type end
    local key = ITBX.mission_key(_ITB_CURRENT_MISSION)
    local best, best_rank = nil, nil
    local candidates = {{"saveData.lua", save_data_content}}
    local undo = read_save_file_cached(SAVE_ROOT .. "/profile_Alpha/undoSave.lua")
    if undo then candidates[2] = {"undoSave.lua", undo} end
    for i, c in ipairs(candidates) do
        local s = ITBX.parse_save_cached(c[1], c[2])
        s.file = c[1]
        local mission_ok = key == nil or s.mission == nil or s.mission == ("Mission" .. tostring(key))
        local matches = 0
        for id, rec in pairs(s.pawns) do
            if live[id] ~= nil and live[id] == rec.type then matches = matches + 1 end
        end
        local rank = {mission_ok and 1 or 0, matches, s.turn or -1, i}
        local better = best_rank == nil
        if not better then
            for k = 1, 4 do
                if rank[k] ~= best_rank[k] then
                    better = rank[k] > best_rank[k]
                    break
                end
            end
        end
        if better then best, best_rank = s, rank end
    end
    return best or {pawns = {}}
end

-- Player-team positions when the player's turn began (Mission:BaseNextTurn
-- with TEAM_PLAYER), for the `moved` flag.
function ITBX.record_turn_start(mission)
    local turn = itbx_turn_team()
    local pos = {}
    for _, id in ipairs(extract_table(Board:GetPawns(TEAM_PLAYER))) do
        local p = Board:GetPawn(id)
        if p then
            local sp = p:GetSpace()
            pos[id] = {sp.x, sp.y}
        end
    end
    _ITB_BRIDGE_TURN_START = {mission = mission, turn = turn, pos = pos,
                              buildings = ITBX.grid_buildings()}
end

-- Populated (grid) buildings: "x,y" -> HP. For grid_lost_this_turn.
function ITBX.grid_buildings()
    local out = {}
    local building = _G.TERRAIN_BUILDING or 1
    for x = 0, 7 do
        for y = 0, 7 do
            local pt = Point(x, y)
            if Board:GetTerrain(pt) == building and Board:IsPowered(pt) then
                out[x .. "," .. y] = Board:GetHealth(pt)
            end
        end
    end
    return out
end

-- Live grid loss. state.grid_power comes from the save, written at turn
-- boundaries only, and Game:GetPower() is not safe to call (commit
-- f73b05a9: crashed the game's Lua). So: the HP the populated buildings
-- of the player's turn start have lost since (a destroyed building counts
-- its whole HP), which is the grid lost unless a building was repaired
-- in between. Exported only when that snapshot is for this mission and
-- turn (it is taken in Mission:BaseNextTurn with TEAM_PLAYER).
function ITBX.grid_live(state, mission)
    local ts = _ITB_BRIDGE_TURN_START
    if ts == nil or ts.buildings == nil or not rawequal(ts.mission, mission) then return end
    local turn, team = itbx_turn_team()
    if ts.turn ~= turn then return end
    local building = _G.TERRAIN_BUILDING or 1
    local lost = 0
    local tiles = {}
    for key, hp0 in pairs(ts.buildings) do
        local x, y = string.match(key, "^(%d+),(%d+)$")
        local pt = Point(tonumber(x), tonumber(y))
        local hp = 0
        if Board:GetTerrain(pt) == building then hp = Board:GetHealth(pt) end
        if type(hp0) == "number" and type(hp) == "number" and hp < hp0 then
            lost = lost + (hp0 - hp)
            tiles[#tiles + 1] = {tonumber(x), tonumber(y), hp0, hp}
        end
    end
    state.grid_lost_this_turn = lost
    state.grid_lost_tiles = tiles
    state.grid_turn_start_turn = ts.turn
    -- An estimate only while the save grid is this player turn's start
    -- value: player phase, and the save's region is at this turn.
    local save = ITBX._save
    if type(state.grid_power) == "number" and team == TEAM_PLAYER
            and save ~= nil and save.turn == turn then
        state.grid_power_estimate = math.max(0, state.grid_power - lost)
    end
end

-- `moved`: the unit left its turn-start tile this turn, or can still undo
-- a move (Pawn:IsUndoPossible). `moved_source` says what was known.
function ITBX.unit_moved(u)
    if u.team ~= 1 then return end
    local p = Board:GetPawn(u.uid)
    if p == nil then return end
    local moved = false
    local source = "undo"
    local ok_u, undo = pcall(function() return p:IsUndoPossible() end)
    if ok_u and undo == true then moved = true end
    local ts = _ITB_BRIDGE_TURN_START
    local turn, team = itbx_turn_team()
    if ts and rawequal(ts.mission, _ITB_CURRENT_MISSION) and ts.turn == turn then
        source = "turn_start"
        local start = ts.pos[u.uid]
        if start and (start[1] ~= u.x or start[2] ~= u.y) then moved = true end
    end
    if team ~= TEAM_PLAYER then moved = false end
    u.moved = moved
    u.moved_source = source
end

function ITBX.unit_ext(u, save, scenario)
    local def = rawget(_G, u.type) or _G[u.type]
    u.traits = ITBX.traits(def)
    if type(def) == "table" and type(def.ExtraSpaces) == "table" and #def.ExtraSpaces > 0 then
        u.extra_spaces = ITBX.point_pairs(def.ExtraSpaces)
    end
    local p = Board:GetPawn(u.uid)
    local shots = nil
    if p then
        local ok_s, s = pcall(function() return p:GetShotsRemaining() end)
        if ok_s and type(s) == "number" then
            shots = s
            u.shots_remaining = s
        end
    end
    local rec = save and save.pawns and save.pawns[u.uid]
    if rec and (rec.type == nil or rec.type == u.type) then
        if rec.mutation then u.mutation = rec.mutation end
        if rec.pilot and rec.pilot.id then u.pilot = rec.pilot end
        if #rec.weapons > 0 then
            -- Calibrate GetShotsRemaining: while the pawn has not acted in
            -- the turn the save was written, its live uses equal the saved
            -- ones, so the rest of the count is its unlimited skills.
            local calib = ITBX.uses_calibration()
            local slots, saved_total = ITBX.weapon_slots(rec.weapons, shots, calib[u.uid])
            local turn, team = itbx_turn_team()
            if shots and u.active == true and save.turn == turn and team == TEAM_PLAYER then
                calib[u.uid] = shots - saved_total
                slots = ITBX.weapon_slots(rec.weapons, shots, calib[u.uid])
            end
            u.weapon_slots = slots
            u.weapons_exact = {}
            for _, s in ipairs(u.weapon_slots) do
                u.weapons_exact[#u.weapons_exact + 1] = s.id
            end
        end
        -- Queued shots of non-enemy units (trains, satellite rockets, bots
        -- on the player team): the old fields cover team 6 only.
        if u.team ~= 6 and rec.queued_skill and rec.queued_skill >= 0 then
            local target = rec.queued_shot
            if not target or target[1] < 0 then target = rec.queued_target end
            u.queued_any = {
                skill = rec.queued_skill,
                target = target,
                origin = rec.queued_origin,
                source = "save",
            }
        end
    end
    -- Scenario-queued attacks replace the (stale) save for this turn.
    if scenario and scenario.queued then
        local q = scenario.queued[u.uid]
        if q == false then
            u.has_queued_attack = false
            u.queued_target = nil
            u.queued_target_raw = nil
            u.queued_any = nil
            u.queued_source = "scenario"
        elseif type(q) == "table" then
            if u.team == 6 then
                u.has_queued_attack = true
                u.queued_target = {q.target[1], q.target[2]}
                u.queued_origin = {q.origin[1], q.origin[2]}
                u.queued_target_raw = nil
                u.queued_target_normalized = nil
            else
                u.queued_any = {skill = q.slot, target = q.target, origin = q.origin, source = "scenario"}
            end
            u.queued_source = "scenario"
        end
    end
end

---------------------------------------------------------------- tiles
function ITBX.tiles_ext(state)
    local ice_id = _G.TERRAIN_ICE or 5
    local building_id = _G.TERRAIN_BUILDING or 1
    for _, tile in ipairs(state.tiles or {}) do
        local pt = Point(tile.x, tile.y)
        local ok_c, custom = pcall(function() return Board:GetCustomTile(pt) end)
        if ok_c then
            if type(custom) == "string" and custom ~= "" then tile.custom_tile = custom end
            -- The live belt sprite (Env_Belt:MarkBoard) replaces the save
            -- scan's conveyor, which reads every region's map in the save:
            -- phantom belts and wrong directions on islands with another
            -- belt map (live m34, 2026-10-11).
            local belt = type(custom) == "string" and custom:match("^conveyor(%d)%.png$")
            tile.conveyor = belt and tonumber(belt) or nil
        else
            ITBX.note_error("tile.GetCustomTile", custom)
        end
        local ok_d, dangerous = pcall(function() return Board:IsDangerous(pt) end)
        if ok_d then
            if dangerous == true then tile.dangerous = true end
        else
            ITBX.note_error("tile.IsDangerous", dangerous)
        end
        if tile.item then
            local ok_i, armed = pcall(function() return Board:IsDangerousItem(pt) end)
            if ok_i then tile.item_armed = armed == true
            else ITBX.note_error("tile.IsDangerousItem", armed) end
        end
        if tile.terrain_id == ice_id then
            local ok_h, hp = pcall(function() return Board:GetHealth(pt) end)
            if ok_h and type(hp) == "number" then tile.ice_hp = hp
            else ITBX.note_error("tile.ice_hp", hp) end
        elseif tile.terrain_id == building_id then
            local ok_p, powered = pcall(function() return Board:IsPowered(pt) end)
            if ok_p and type(powered) == "boolean" then tile.populated = powered
            else ITBX.note_error("tile.IsPowered", powered) end
            local ok_u, unique = pcall(function() return Board:IsUniqueBuilding(pt) end)
            if ok_u and unique == true then tile.unique_building_live = true end
        end
    end
end

function ITBX.zones()
    local out = {}
    for _, z in ipairs(ITBX.ZONES) do
        local ok, list = pcall(function() return Board:GetZone(z) end)
        if ok and list ~= nil then
            local pts = ITBX.point_pairs(list)
            if #pts > 0 then out[z] = pts end
        elseif not ok then
            ITBX.note_error("zones." .. z, list)
        end
    end
    return out
end

-- Deployment. Board::GetDropZone (Board.c 14585-14830; used by the deploy
-- UI, BoardPlayer::TouchDeploy / ComputeDeployment) keeps the "deployment"
-- zone tiles that pass Board::IsAvailable(p, 1, 2) or hold a mech; with 2
-- or fewer left it uses the default columns x = 1..3 (rows 1..6), then
-- whole columns in the order 1,2,3,4,0,5,6,7 until more than 3.
-- IsAvailable (Board.c 14837-15127) is not bound to Lua; it rejects:
--   item, pod; a block-spawn mark (Board+0x7480, BLOCKED_TEMP or _PERM);
--   Board::IsDangerous; IsBlocked(p, PATH_MASSIVE) (building, mountain,
--   chasm, any pawn; water allowed); acid; spikes (not readable); fire;
--   chasm; a queued Vek emerge point.
-- The block-spawn map has no Lua getter either. ITBX.reserved_tiles reads
-- it from the save's blocked_points (the last saved copy, for this mission)
-- plus the Mission_Final "pylons" zone, which StartMission blocks
-- (BLOCKED_PERM) and Mission_Final:NextTurn drops buildings on at turn 0.
-- A mech on such a tile hangs the game for good: BoardSpace::DamageSpace
-- (BoardSpace.c 19105-19117) loops `while IsPawnSpace(true) do Kill()`
-- before AddBuilding, and a dead mech stays in the space as a corpse.

local function itbx_key(x, y) return x .. "," .. y end

-- "x,y" -> reason for every tile a mech must not be deployed on.
function ITBX.reserved_tiles()
    local out = {}
    local ok, list = pcall(function() return Board:GetZone("pylons") end)
    if ok and list ~= nil then
        for _, p in ipairs(ITBX.point_pairs(list)) do out[itbx_key(p[1], p[2])] = "pylon" end
    end
    local ok_s, save = pcall(function()
        return ITBX.parse_save_cached("saveData.lua", _read_save_data().raw_content)
    end)
    if ok_s and type(save) == "table" and save.spawn_blocks then
        local key = ITBX.mission_key(_ITB_CURRENT_MISSION)
        if key == nil or save.mission == nil or save.mission == ("Mission" .. tostring(key)) then
            for _, b in ipairs(save.spawn_blocks) do
                if b[3] == 1 or b[3] == 2 then
                    local k = itbx_key(b[1], b[2])
                    if out[k] == nil then out[k] = "spawn_block" end
                end
            end
        end
    end
    return out
end

-- Board::IsAvailable(p, 1, 2) as far as Lua can see it. `reserved` is
-- ITBX.reserved_tiles() (passed in to compute it once per zone). A tile
-- held by a mech fails here (blocked) but GetDropZone keeps it.
function ITBX.deploy_tile_ok(x, y, reserved)
    if type(x) ~= "number" or type(y) ~= "number" or x < 0 or x > 7 or y < 0 or y > 7 then
        return false, "off the board"
    end
    reserved = reserved or ITBX.reserved_tiles()
    local why = reserved[itbx_key(x, y)]
    if why then return false, why end
    local pt = Point(x, y)
    if Board:IsItem(pt) then return false, "item" end
    if Board:IsPod(pt) then return false, "pod" end
    if Board:IsDangerous(pt) then return false, "dangerous" end
    if Board:IsSpawning(pt) then return false, "spawn point" end
    if Board:GetTerrain(pt) == (_G.TERRAIN_HOLE or 9) then return false, "chasm" end
    if Board:IsAcid(pt) then return false, "acid" end
    if Board:IsFire(pt) then return false, "fire" end
    if Board:IsBlocked(pt, _G.PATH_MASSIVE or 2) then return false, "blocked" end
    return true
end

function ITBX.drop_zone()
    local reserved = ITBX.reserved_tiles()
    local function usable(x, y)
        local pt = Point(x, y)
        if Board:IsPawnSpace(pt) then
            local p = Board:GetPawn(pt)
            if p ~= nil and p:IsMech() then
                -- GetDropZone keeps a mech's tile; never a reserved one.
                return reserved[itbx_key(x, y)] == nil
            end
        end
        return (ITBX.deploy_tile_ok(x, y, reserved))
    end
    local out, seen = {}, {}
    local function add(x, y)
        local k = x .. "," .. y
        if not seen[k] then
            seen[k] = true
            out[#out + 1] = {x, y}
        end
    end
    local zone = ITBX.point_pairs(Board:GetZone("deployment"))
    local source = "zone"
    for _, p in ipairs(zone) do
        if usable(p[1], p[2]) then add(p[1], p[2]) end
    end
    if #zone == 0 then
        source = "default"
        for x = 1, 3 do
            for y = 1, 6 do
                if usable(x, y) then add(x, y) end
            end
        end
    end
    if #out <= 2 then
        source = source .. "+columns"
        for _, x in ipairs({1, 2, 3, 4, 0, 5, 6, 7}) do
            for y = 0, 7 do
                if usable(x, y) then add(x, y) end
            end
            if #out > 3 then break end
        end
    end
    return out, source
end

-- The old deployment_zone capture: drop the tiles the deploy UI refuses.
-- Runs even with the extension disabled (it is the hang guard), unguarded
-- but under pcall by the caller.
function ITBX.filter_deploy_tiles(tiles)
    local reserved = ITBX.reserved_tiles()
    local out = {}
    for _, p in ipairs(tiles) do
        local ok_t, ok = pcall(ITBX.deploy_tile_ok, p[1], p[2], reserved)
        if ok_t and ok then out[#out + 1] = p end
    end
    return out
end

-- DEPLOY uid x y precondition: the tile is in the (approximated) drop zone
-- or is the mech's own tile, and never a reserved one. Returns ok, reason.
function ITBX.deploy_check(pawn, x, y)
    local reserved = {}
    local ok_r, r = pcall(ITBX.reserved_tiles)
    if ok_r and type(r) == "table" then reserved = r end
    if reserved[itbx_key(x, y)] then
        return false, "reserved tile (" .. reserved[itbx_key(x, y)]
            .. "): the native deploy UI refuses it"
    end
    if x < 0 or x > 7 or y < 0 or y > 7 then return false, "off the board" end
    local sp = pawn:GetSpace()
    if sp.x == x and sp.y == y then return true end
    local other = Board:GetPawn(Point(x, y))
    if other ~= nil then return false, "occupied by pawn " .. tostring(other:GetId()) end
    local ok_z, zone = pcall(ITBX.drop_zone)
    if not ok_z then
        -- No zone (API failure): fall back to the per-tile test.
        local ok_t, ok, why = pcall(ITBX.deploy_tile_ok, x, y, reserved)
        if ok_t and not ok then return false, why end
        return true
    end
    for _, p in ipairs(zone) do
        if p[1] == x and p[2] == y then return true end
    end
    local _, why = ITBX.deploy_tile_ok(x, y, reserved)
    local list = {}
    for _, p in ipairs(zone) do list[#list + 1] = p[1] .. "," .. p[2] end
    return false, "not in the drop zone (" .. tostring(why or "outside the deployment zone")
        .. "); drop zone: " .. table.concat(list, " ")
end

-- Last line of defence, from the BaseNextTurn wrap before the enemy turn 0
-- (when Mission_Final:NextTurn drops its pylons): a player pawn that left
-- a corpse standing on a "pylons" tile would hang the game (see above), so
-- move it to a free drop-zone tile first. Returns the moves made.
function ITBX.evacuate_pylon_tiles()
    local turn, team = itbx_turn_team()
    if turn ~= 0 or team ~= TEAM_ENEMY then return {} end
    local pylons = ITBX.point_pairs(Board:GetZone("pylons"))
    if #pylons == 0 then return {} end
    local moves = {}
    for _, pp in ipairs(pylons) do
        local p = Board:GetPawn(Point(pp[1], pp[2]))
        if p ~= nil and p:GetTeam() == TEAM_PLAYER then
            local dest = nil
            local ok_z, zone = pcall(ITBX.drop_zone)
            for _, z in ipairs(ok_z and zone or {}) do
                if not Board:IsPawnSpace(Point(z[1], z[2])) then dest = z break end
            end
            if dest == nil then
                local reserved = ITBX.reserved_tiles()
                for x = 0, 7 do
                    for y = 0, 7 do
                        if dest == nil and ITBX.deploy_tile_ok(x, y, reserved) then dest = {x, y} end
                    end
                end
            end
            local id = p:GetId()
            if dest then
                p:SetSpace(Point(dest[1], dest[2]))
                moves[#moves + 1] = {uid = id, from = {pp[1], pp[2]}, to = dest}
                pcall(log_bridge, "PYLON GUARD: moved pawn " .. id .. " off pylon tile "
                    .. pp[1] .. "," .. pp[2] .. " to " .. dest[1] .. "," .. dest[2])
            else
                pcall(log_bridge, "PYLON GUARD: pawn " .. id .. " is on pylon tile "
                    .. pp[1] .. "," .. pp[2] .. " and no free tile was found")
            end
        end
    end
    return moves
end

---------------------------------------------------------------- spawns
-- Queued spawns in queue order. Source: the scenario ledger when a
-- scenario edited this turn's queue, else the save (turn-boundary data).
function ITBX.spawn_queue(state, mission, save, scenario)
    local queue, source = nil, nil
    if scenario and scenario.spawn_queue then
        queue, source = scenario.spawn_queue, "scenario"
    elseif save and save.spawns then
        local key = ITBX.mission_key(mission)
        if key ~= nil and save.mission ~= nil and save.mission ~= ("Mission" .. tostring(key)) then
            state.spawn_queue_mismatch = "save region " .. tostring(save.mission)
                .. " is not Mission" .. tostring(key)
            return
        end
        queue, source = save.spawns, "save"
    end
    if not queue then return end
    state.spawn_queue = queue
    state.spawn_queue_source = source
    if save and save.turn then state.spawn_queue_save_turn = save.turn end
    -- Do the queue points equal the live markers (as multisets)?
    local markers = {}
    for _, p in ipairs(state.spawning_tiles or {}) do
        local k = p[1] .. "," .. p[2]
        markers[k] = (markers[k] or 0) + 1
    end
    local match = true
    for _, s in ipairs(queue) do
        local k = tostring(s.x) .. "," .. tostring(s.y)
        if (markers[k] or 0) <= 0 then
            match = false
        else
            markers[k] = markers[k] - 1
        end
    end
    for _, n in pairs(markers) do
        if n ~= 0 then match = false end
    end
    state.spawn_queue_matches_markers = match
end

---------------------------------------------------------------- ledgers
-- Per-mission logs: env steps (always) and, with the debug flag, a
-- per-frame record of HP/position/selection/busy changes during play.
function ITBX.log_for(mission)
    local log = _ITB_BRIDGE_PHASE_LOG
    if log == nil or not rawequal(log.mission, mission) then
        log = {mission = mission, entries = {}, env = {}, track = {}, seq = 0,
               frame = 0, initialized = false}
        _ITB_BRIDGE_PHASE_LOG = log
    end
    return log
end

function ITBX.append(log, list, max, entry)
    log.seq = log.seq + 1
    entry.seq = log.seq
    entry.frame = log.frame
    entry.clock = os.clock()
    list[#list + 1] = entry
    if #list > max then
        local keep = {}
        for i = #list - math.floor(max / 2) + 1, #list do keep[#keep + 1] = list[i] end
        for i = #list, 1, -1 do list[i] = nil end
        for i, e in ipairs(keep) do list[i] = e end
    end
end

-- Mission:ApplyEnvironmentEffect wrap: one entry per environment step
-- (stage 7 spec 4.4: the realised strike order, e.g. Lightning).
function ITBX.log_env_step(mission, returned)
    local log = ITBX.log_for(mission)
    local le = mission.LiveEnvironment
    local turn, team = itbx_turn_team()
    local ctx = ITBX.new_dump_ctx()
    local entry = {kind = "env_step", turn = turn, team = team, returned = returned == true}
    if type(le) == "table" then
        entry.current_attack = ITBX.dump_value(rawget(le, "CurrentAttack"), 0, ctx)
        entry.remaining = ITBX.dump_value(rawget(le, "Locations"), 0, ctx)
        if type(le.Mode) == "number" then entry.mode = le.Mode end
        if type(le.Phase) == "number" then entry.phase = le.Phase end
        if type(le.Index) == "number" then entry.index = le.Index end
    end
    ITBX.append(log, log.env, ITBX.ENV_LOG_MAX, entry)
    if ITBX.debug_enabled() then
        local copy = {}
        for k, v in pairs(entry) do copy[k] = v end
        ITBX.append(log, log.entries, ITBX.PHASE_LOG_MAX, copy)
    end
end

function ITBX.poll_frame(mission)
    local log = ITBX.log_for(mission)
    log.frame = log.frame + 1
    local turn, team = itbx_turn_team()
    local ok_b, busy = pcall(function() return Board:GetBusyState() end)
    if ok_b and busy ~= log.busy then
        if log.initialized then
            ITBX.append(log, log.entries, ITBX.PHASE_LOG_MAX,
                {kind = "busy", turn = turn, team = team, from = log.busy, to = busy})
        end
        log.busy = busy
    end
    local ids = extract_table(Board:GetPawns(TEAM_ANY))
    local seen = {}
    for _, id in ipairs(ids) do
        local p = Board:GetPawn(id)
        if p then
            seen[id] = true
            local hp = p:GetHealth()
            local sp = p:GetSpace()
            local sel = false
            local ok_s, s = pcall(function() return p:IsSelected() end)
            if ok_s then sel = s == true end
            local prev = log.track[id]
            if prev == nil then
                if log.initialized then
                    ITBX.append(log, log.entries, ITBX.PHASE_LOG_MAX,
                        {kind = "appeared", turn = turn, team = team, uid = id,
                         type = p:GetType(), x = sp.x, y = sp.y, hp = hp})
                end
                log.track[id] = {hp = hp, x = sp.x, y = sp.y, sel = sel}
            else
                if prev.hp ~= hp then
                    ITBX.append(log, log.entries, ITBX.PHASE_LOG_MAX,
                        {kind = "hp", turn = turn, team = team, uid = id,
                         from = prev.hp, to = hp, x = sp.x, y = sp.y})
                    prev.hp = hp
                end
                if prev.x ~= sp.x or prev.y ~= sp.y then
                    ITBX.append(log, log.entries, ITBX.PHASE_LOG_MAX,
                        {kind = "pos", turn = turn, team = team, uid = id,
                         from = {prev.x, prev.y}, to = {sp.x, sp.y}})
                    prev.x, prev.y = sp.x, sp.y
                end
                if sel and not prev.sel then
                    ITBX.append(log, log.entries, ITBX.PHASE_LOG_MAX,
                        {kind = "selected", turn = turn, team = team, uid = id,
                         type = p:GetType(), pawn_team = p:GetTeam()})
                end
                prev.sel = sel
            end
        end
    end
    for id, _ in pairs(log.track) do
        if not seen[id] then
            ITBX.append(log, log.entries, ITBX.PHASE_LOG_MAX,
                {kind = "gone", turn = turn, team = team, uid = id})
            log.track[id] = nil
        end
    end
    -- Terrain and structure HP (building damage shows when grid is lost).
    log.tiles = log.tiles or {}
    for x = 0, 7 do
        for y = 0, 7 do
            local pt = Point(x, y)
            local terrain = Board:GetTerrain(pt)
            local hp = Board:GetHealth(pt)
            local key = x * 8 + y
            local prev = log.tiles[key]
            if prev and (prev[1] ~= terrain or prev[2] ~= hp) and log.initialized then
                ITBX.append(log, log.entries, ITBX.PHASE_LOG_MAX,
                    {kind = "tile", turn = turn, team = team, x = x, y = y,
                     from = {prev[1], prev[2]}, to = {terrain, hp}})
            end
            log.tiles[key] = {terrain, hp}
        end
    end
    log.initialized = true
end

function ITBX.mark(mission, kind, extra)
    local log = ITBX.log_for(mission)
    local turn, team = itbx_turn_team()
    local entry = {kind = kind, turn = turn, team = team}
    for k, v in pairs(extra or {}) do entry[k] = v end
    ITBX.append(log, log.entries, ITBX.PHASE_LOG_MAX, entry)
end

---------------------------------------------------------------- scenario
-- The ledger a SCENARIO command leaves for the rest of its turn: the
-- attacks it queued (the save would still show the old ones) and the spawn
-- queue it built.
function ITBX.active_scenario()
    local st = _ITB_BRIDGE_SCENARIO
    if st == nil or not rawequal(st.mission, _ITB_CURRENT_MISSION) then return nil end
    local turn, team = itbx_turn_team()
    if turn ~= st.turn or team ~= TEAM_PLAYER then return nil end
    return st
end

---------------------------------------------------------------- finish
-- Everything that needs the finished unit list; runs just before the
-- state is written.
function ITBX.finish(state, mission)
    local scenario = ITBX.active_scenario()
    local debug = ITBX.debug_enabled()
    state.bridge_ext_version = ITBX.VERSION
    state.bridge_debug = debug
    state.exec_mode = ITBX.exec_mode
    if mission ~= nil then
        state.mission_state = ITBX.try("mission_state", ITBX.mission_state, mission)
        -- The LiveEnvironment's class (Env_Null for missions without one),
        -- beside the old env_type heuristic.
        if state.mission_state and state.mission_state.env_class_chain then
            state.env_class = state.mission_state.env_class_chain[1]
        end
        ITBX.try("tiles_ext", ITBX.tiles_ext, state)
        ITBX.try("drop_zone", function()
            local zone, source = ITBX.drop_zone()
            state.drop_zone = zone
            state.drop_zone_source = source
            local turn = itbx_turn_team()
            if turn == 0 then
                state.deploying = true
                -- The old field, when the old capture found nothing: the
                -- free tiles of the drop zone.
                if state.deployment_zone == nil or #state.deployment_zone == 0 then
                    local free = {}
                    for _, p in ipairs(zone) do
                        if not Board:IsPawnSpace(Point(p[1], p[2])) then free[#free + 1] = p end
                    end
                    if #free > 0 then state.deployment_zone = free end
                end
            end
        end)
        state.zones = ITBX.try("zones", ITBX.zones)
        ITBX.try("spawn_queue", ITBX.spawn_queue, state, mission, ITBX._save, scenario)
        if ITBX._save and ITBX._save.spawn_blocks then
            state.spawn_blocks = ITBX._save.spawn_blocks
        end
        ITBX.try("objectives_ext", ITBX.objectives_ext, state, mission)
        ITBX.try("grid_live", ITBX.grid_live, state, mission)
        local log = _ITB_BRIDGE_PHASE_LOG
        if log and rawequal(log.mission, mission) then
            state.env_strike_log = log.env
            if debug then
                state.phase_log = {frame = log.frame, entries = log.entries}
            end
        end
        if scenario then
            state.scenario = {
                name = scenario.name, turn = scenario.turn,
                errors = scenario.errors, created = scenario.created,
            }
        end
    end
    -- Every unit with a queued shot, any team, in pawn-list order (the
    -- native shooter order, stage 7 spec 1.6; trains and rockets come after
    -- every Vek because AddPawn groups them last).
    local order = {}
    for _, u in ipairs(state.units or {}) do
        if not u.is_extra_tile then
            local queued = (u.team == 6 and u.has_queued_attack)
                or (u.queued_any ~= nil) or u.queued_launch == true
            if queued then order[#order + 1] = u.uid end
        end
    end
    state.attack_order_all = order
    state.bridge_errors = ITBX.errs or {}
end

function ITBX.objectives_ext(state, mission)
    if type(mission.PowerStart) == "number" then
        state.mission_power_start = mission.PowerStart
    end
    if type(mission.BonusObjs) == "table" then
        local ids = {}
        local block = false
        for _, b in ipairs(mission.BonusObjs) do
            if type(b) == "number" then
                ids[#ids + 1] = b
                if b == 5 then block = true end
            end
        end
        if state.bonus_objective_ids == nil then state.bonus_objective_ids = ids end
        -- Mission.BlockedSpawns only counts while BONUS_BLOCK is active
        -- (Mission:BaseUpdate).
        if block and type(mission.BlockedSpawns) == "number" then
            state.mission_blocked_spawns = mission.BlockedSpawns
        end
    end
    if mission.ID == "Mission_Missiles" then
        state.mission_missiles = {
            shots_used = mission.ShotsUsed,
            disposal_id = mission.DisposalId,
        }
    end
end

-- out_path / out_tmp: optional destination (default STATE_FILE); the
-- debug captures and SNAPSHOT write the same payload elsewhere.
local function dump_state(out_path, out_tmp)
    if not Board then return end

    local state = {}
    ITBX.begin_dump()

    local mission_id = mission_bridge_id(_ITB_CURRENT_MISSION)

    local terraform_grass_lookup = {}
    local terraform_grass_tiles = mission_terraform_grass_tiles(
        mission_id,
        Board
    )
    if terraform_grass_tiles ~= nil then
        state.terraform_grass_live = true
        state.terraform_grass_tiles = terraform_grass_tiles
        for _, grass in ipairs(terraform_grass_tiles) do
            terraform_grass_lookup[grass[1] .. "," .. grass[2]] = true
        end
    end

    -- Phase detection. Game:GetTeamTurn() can keep returning the last combat
    -- team after MissionEnd, so require the active-mission cache too.
    local in_active_mission = (_ITB_CURRENT_MISSION ~= nil)
    state.in_active_mission = in_active_mission
    local team_turn = Game and Game:GetTeamTurn() or 0
    if not in_active_mission then
        state.phase = "unknown"
    elseif team_turn == 1 then
        state.phase = "combat_player"
    elseif team_turn == 6 then
        state.phase = "combat_enemy"
    else
        state.phase = "unknown"
    end

    state.turn = Game and Game:GetTurnCount() or 0
    state.total_turns = 5  -- Default; overridden from save file below if available

    -- Read all save-file-derived data in one I/O pass (grid power, queued shots, conveyors)
    local save_data = _read_save_data()

    -- Only extension APIs are live. RegionData is a turn-1 static baseline;
    -- the Python reader must replace it from the boundary save/ledger.
    local runtime_region_tile_shields = get_runtime_region_tile_shields()
    local ok_shield_api, shield_probe = pcall(function()
        return Board:IsShield(Point(0, 0))
    end)
    local ok_shielded_api, shielded_probe = pcall(function()
        return Board:IsShielded(Point(0, 0))
    end)
    local has_shield_api = ok_shield_api and type(shield_probe) == "boolean"
    local has_shielded_api = ok_shielded_api and type(shielded_probe) == "boolean"
    local use_runtime_region_baseline = state.turn <= 1
        and runtime_region_tile_shields ~= nil
    local runtime_region_shields_for_export = nil
    if use_runtime_region_baseline then
        runtime_region_shields_for_export = runtime_region_tile_shields
    end
    state.tile_shields_live = has_shield_api or has_shielded_api
    state.tile_shields_static_baseline = use_runtime_region_baseline
    if has_shield_api then
        state.tile_shield_source = "board_api"
    elseif has_shielded_api then
        state.tile_shield_source = "board_is_shielded"
    elseif use_runtime_region_baseline then
        state.tile_shield_source = "runtime_region_turn1_baseline"
    end

    -- Grid power: prefer save file value (authoritative, updated at turn boundaries).
    -- Falls back to GameData globals which may be stale at run transitions.
    -- Game:GetPower() crashes the Lua runtime so we can't use it.
    state.grid_power = save_data.network or (GameData and GameData.network) or 0
    state.grid_power_max = save_data.networkMax or (GameData and GameData.networkMax) or 7
    state.timestamp = os.time()

    -- In-game difficulty (0=Easy, 1=Normal, 2=Hard, 3=Unfair). Mirrors the
    -- save-file source-of-truth so Python can cross-check session metadata
    -- without parsing Lua. See cmd_auto_turn difficulty cross-check.
    state.difficulty = save_data.difficulty
        or (GameData and GameData.difficulty) or 0

    -- RNG seeds for grid-defense resist prediction probe. master_seed is the
    -- run-lifetime constant; mission_seeds is a {region_key -> aiSeed} map
    -- that updates each turn. Python side decides which region is "active".
    if save_data.master_seed ~= nil then
        state.master_seed = save_data.master_seed
    end
    if next(save_data.mission_seeds) ~= nil then
        state.mission_seeds = save_data.mission_seeds
    end

    -- Conveyor belts from consolidated save read
    local conveyor_belts = save_data.conveyor_belts

    -- Objective building lookup:
    --   * Single-objective missions set self.AssetLoc (Coal Plant / Power
    --     Generator / Emergency Batteries). AssetId names the asset.
    --   * Mission_Critical and its subclasses (Solar / Wind / Power) set
    --     self.Criticals = {Point, Point} — two Solar Farms / Wind Farms /
    --     Power Plants. FlavorBase names the asset ("Mission_Solar" etc.).
    -- Both populate the same `objective_keys` map; the solver scores each
    -- tagged tile independently via building_objective_bonus.
    local objective_keys = {}
    if _ITB_CURRENT_MISSION then
        -- Single AssetLoc path
        local ok_loc, loc = pcall(function() return _ITB_CURRENT_MISSION.AssetLoc end)
        local ok_id, aid = pcall(function() return _ITB_CURRENT_MISSION.AssetId end)
        if ok_loc and loc and type(loc) == "userdata" then
            local ok_xy, ox, oy = pcall(function() return loc.x, loc.y end)
            if ok_xy and ox and oy then
                objective_keys[ox .. "," .. oy] = (ok_id and aid) or true
            end
        end
        -- Mission_Critical Criticals path (2 buildings)
        local ok_c, criticals = pcall(function() return _ITB_CURRENT_MISSION.Criticals end)
        local ok_fb, flavor = pcall(function() return _ITB_CURRENT_MISSION.FlavorBase end)
        if ok_c and type(criticals) == "table" then
            for _, cpt in ipairs(criticals) do
                if type(cpt) == "userdata" then
                    local ok_xy, cx, cy = pcall(function() return cpt.x, cpt.y end)
                    if ok_xy and cx and cy then
                        objective_keys[cx .. "," .. cy] = (ok_fb and flavor) or true
                    end
                end
            end
        end
    end

    -- Tiles (all 64)
    state.tiles = {}
    for y = 0, 7 do
        for x = 0, 7 do
            local pt = Point(x, y)
            local terrain_id = Board:GetTerrain(pt)
            local tile = {
                x = x, y = y,
                terrain = TERRAIN_NAMES[terrain_id] or "ground",
                terrain_id = terrain_id,
            }
            -- TERRAIN_LAVA may not have existed yet when the static numeric
            -- name table was initialized. Probe it at dump time so final-
            -- island Lava cannot be serialized as ordinary Water.
            local ok_lava, is_lava = pcall(function()
                return Board:IsTerrain(pt, TERRAIN_LAVA)
            end)
            if ok_lava and is_lava then
                tile.lava = true
                tile.terrain = "lava"
            end
            if terraform_grass_lookup[x .. "," .. y] then
                tile.grass = true
            end

            -- Status effects
            local ok_f, fire = pcall(function() return Board:IsFire(pt) end)
            if ok_f and fire then tile.fire = true end
            local ok_s, smoke = pcall(function() return Board:IsSmoke(pt) end)
            if ok_s and smoke then tile.smoke = true end
            local ok_a, acid = pcall(function() return Board:IsAcid(pt) end)
            if ok_a and acid then tile.acid = true end
            if get_live_tile_shield(pt, runtime_region_shields_for_export) then
                tile.shield = true
            end
            local ok_fr, frozen = pcall(function() return Board:IsFrozen(pt) end)
            if ok_fr and frozen then tile.frozen = true end
            local ok_cr, cracked = pcall(function() return Board:IsCracked(pt) end)
            if ok_cr and cracked then tile.cracked = true end

            -- Conveyor belt direction (from save file)
            local belt_dir = conveyor_belts[x .. "," .. y]
            if belt_dir then tile.conveyor = belt_dir end

            -- Building data
            if terrain_id == (_G.TERRAIN_BUILDING or 1) then
                local ok_h, hp = pcall(function() return Board:GetHealth(pt) end)
                if ok_h then tile.building_hp = hp end
                -- Objective building (Coal Plant / Power Generator /
                -- Batteries via AssetLoc, or Solar Farms / Wind Farms /
                -- Power Plants via Mission_Critical.Criticals).
                local obj_tag = objective_keys[x .. "," .. y]
                if obj_tag then
                    tile.unique_building = true
                    if type(obj_tag) == "string" then
                        tile.objective_name = obj_tag
                    end
                end
            -- Mountain data (2 = full, 1 = damaged, 0 = rubble)
            elseif terrain_id == (_G.TERRAIN_MOUNTAIN or 4) then
                local ok_h, hp = pcall(function() return Board:GetHealth(pt) end)
                if ok_h then tile.building_hp = hp else tile.building_hp = 2 end
                tile.population = 1
            end

            -- Pod
            local ok_p, pod = pcall(function() return Board:IsPod(pt) end)
            if ok_p and pod then tile.pod = true end

            -- Tile items (freeze mines, old earth mines, etc.)
            local ok_i, item = pcall(function() return Board:GetItem(pt) end)
            if ok_i and item and item ~= "" then
                tile.item = item
                if item == "Freeze_Mine" or item == "Freeze_Mine_Vek" then
                    tile.freeze_mine = true
                elseif item == "Item_Mine" then
                    tile.old_earth_mine = true
                elseif item == "Item_Repair_Mine" then
                    tile.repair_platform = true
                end
            end

            state.tiles[#state.tiles + 1] = tile
        end
    end

    -- Queued shots from consolidated save read
    local queued_shots = save_data.queued_shots

    -- Units (all teams)
    state.units = {}
    local all_ids = extract_table(Board:GetPawns(TEAM_ANY))
    for _, pid in ipairs(all_ids) do
        local ok, p = pcall(function() return Board:GetPawn(pid) end)
        if ok and p then
            local sp = p:GetSpace()
            if sp.x >= 0 then  -- skip off-board pawns
                local ptype = p:GetType()
                local pawn_def = _G[ptype]

                -- max_hp: prefer pawn's live GetMaxHealth() over pawn_def.Health
                -- because pilots can buff mech HP (e.g. +2 from a passive) and
                -- the live value reflects that. Fall back to def base if API
                -- unavailable. Previous code reported base HP, which was
                -- strictly less than current HP for pilot-boosted mechs.
                local live_max_hp = nil
                local ok_mh, mh = pcall(function() return p:GetMaxHealth() end)
                if ok_mh and type(mh) == "number" and mh > 0 then
                    live_max_hp = mh
                end
                -- Pawn:GetMaxHealth is not bound in the shipped game, so
                -- without this the dump said the type's Health: no Networked
                -- Armor, pilot or core bonus. The engine then saw a Regen
                -- pilot's mech as full and missed its +1 at turn start
                -- (2026-10-09: IgniteMech 3/3 in the dump, 4 max in the save).
                -- The save's max_health (turn-boundary value) fixes it.
                if live_max_hp == nil then
                    local ok_sv, sv = pcall(get_pawn_max_health, p, pid, save_data)
                    if ok_sv and type(sv) == "number" and sv > 0 then
                        live_max_hp = sv
                    end
                end
                local base_move = pawn_def and pawn_def.MoveSpeed or p:GetMoveSpeed()
                local ok_bm, live_base_move = pcall(function() return p:GetBaseMove() end)
                if ok_bm and type(live_base_move) == "number" then
                    base_move = live_base_move
                end
                -- Native mode-1 path occupancy counts a dead pawn only while
                -- Pawn:IsCorpse() is true. Export both the live predicate and
                -- source/static Corpse property so projected deaths retain
                -- the right blocker identity without installing any hook.
                local current_corpse = false
                local ok_co, is_corpse = pcall(function() return p:IsCorpse() end)
                if ok_co and is_corpse == true then
                    current_corpse = true
                end
                local corpse_on_death =
                    pawn_def and pawn_def.Corpse == true or false
                local unit = {
                    uid = pid,
                    type = ptype,
                    x = sp.x, y = sp.y,
                    hp = p:GetHealth(),
                    max_hp = live_max_hp or (pawn_def and pawn_def.Health) or p:GetHealth(),
                    team = p:GetTeam(),
                    mech = p:IsMech(),
                    active = p:IsActive(),
                    move = p:GetMoveSpeed(),
                    base_move = base_move,
                    minor = pawn_def and pawn_def.Minor or false,
                    corpse = current_corpse,
                    corpse_on_death = corpse_on_death,
                    void_shock_immune = pawn_def and pawn_def.VoidShockImmune or false,
                }

                -- Export the live Lua-backed ranged flag. Static PawnStats
                -- remains a fallback for older bridge payloads, but must not
                -- overwrite this value when a runtime definition has been
                -- mutated.
                do
                    local ok_ra, ranged = pcall(function()
                        return p:IsRanged()
                    end)
                    if ok_ra and type(ranged) == "boolean" then
                        unit.ranged = ranged and 1 or 0
                    end
                end

                -- Pilot info (mechs only). Save-file-derived is the most
                -- reliable source; Lua-API probes are a fallback. Save
                -- structure is `pawnN.pilot.{id,level,skill1,skill2}` per
                -- entry, keyed by pawn id (matches `pid` here).
                if p:IsMech() then
                    local pilot_id = nil
                    local pilot_level = nil
                    local pilot_skills = {}
                    local save_pilot = save_data.pilots[pid]
                    if save_pilot then
                        pilot_id = save_pilot.id
                        pilot_level = save_pilot.level
                        if save_pilot.skill1 and save_pilot.skill1 ~= 0 then
                            pilot_skills[#pilot_skills + 1] = "skill1=" .. save_pilot.skill1
                        end
                        if save_pilot.skill2 and save_pilot.skill2 ~= 0 then
                            pilot_skills[#pilot_skills + 1] = "skill2=" .. save_pilot.skill2
                        end
                    end
                    -- Lua API probe fallback (if save had no match)
                    if not pilot_id then
                        for _, mname in ipairs({"GetPilotId", "GetPilot"}) do
                            local ok_pm, pv = pcall(function() return p[mname](p) end)
                            if ok_pm and pv then
                                if type(pv) == "string" and pv ~= "" then
                                    pilot_id = pv; break
                                elseif type(pv) == "table" and pv.id then
                                    pilot_id = pv.id
                                    if pv.level then pilot_level = pv.level end
                                    break
                                end
                            end
                        end
                    end
                    if pilot_id then unit.pilot_id = pilot_id end
                    if pilot_level then unit.pilot_level = pilot_level end
                    if #pilot_skills > 0 then unit.pilot_skills = pilot_skills end
                end

                -- Massive trait (walks in water, immune to drowning)
                -- Read from pawn_def since there's no direct IsMassive() API
                if pawn_def and pawn_def.Massive then
                    unit.massive = true
                end

                -- Stable / guarding units cannot be moved by push/teleport
                -- effects even when their static pawn type is normally
                -- pushable. Bridge this as live pushability because bosses and
                -- mission units can gain the status dynamically.
                local pushable = nil
                if pawn_def and pawn_def.Pushable == false then
                    pushable = false
                end
                local ok_g, guarding = pcall(function() return p:IsGuarding() end)
                if ok_g then
                    unit.guarding = guarding
                    unit.stable = guarding
                    if guarding then
                        pushable = false
                    elseif pushable == nil and pawn_def and pawn_def.Pushable ~= nil then
                        pushable = pawn_def.Pushable ~= false
                    end
                end
                if pushable ~= nil then
                    unit.pushable = pushable
                end
                local ok_pw, powered = pcall(function() return p:IsPowered() end)
                if ok_pw and type(powered) == "boolean" then unit.powered = powered end
                local ok_bu, burrower = pcall(function() return p:IsBurrower() end)
                if ok_bu and type(burrower) == "boolean" then unit.burrower = burrower end
                local ok_ju, jumper = pcall(function() return p:IsJumper() end)
                if ok_ju and type(jumper) == "boolean" then unit.jumper = jumper end

                -- Status effects
                local ok_f, fly = pcall(function() return p:IsFlying() end)
                if ok_f then unit.flying = fly end
                local ok_s, sh = pcall(function() return p:IsShield() end)
                if ok_s then unit.shield = sh end
                local ok_a, ac = pcall(function() return p:IsAcid() end)
                if ok_a then unit.acid = ac end
                local ok_fi, fi = pcall(function() return p:IsFire() end)
                if ok_fi then unit.fire = fi end
                local ok_fr, fr = pcall(function() return p:IsFrozen() end)
                if ok_fr then unit.frozen = fr end
                local infected = nil
                for _, mname in ipairs({"IsInfected", "IsInfested", "IsMiteInfected"}) do
                    local ok_m, v = pcall(function() return p[mname](p) end)
                    if ok_m and type(v) == "boolean" then
                        infected = v
                        break
                    end
                end
                if infected == nil and save_data.infected[pid] ~= nil then
                    infected = save_data.infected[pid]
                end
                if infected ~= nil then unit.infected = infected end
                local ok_bo, boosted = pcall(function() return p:IsBoosted() end)
                if ok_bo and boosted then unit.boosted = true end
                -- Web/grapple detection: try multiple API method names.
                -- IsGrappled() alone misses Spider-egg webs on mechs; probe
                -- alternatives so either the Scorpion-grapple or the Spider-
                -- egg web lands in unit.web.
                local web = false
                local web_probes = {}
                for _, mname in ipairs({
                    "IsGrappled", "IsWebbed", "IsWeb", "IsPinned",
                    "IsHeld", "IsHold",
                }) do
                    local ok_m, v = pcall(function() return p[mname](p) end)
                    if ok_m then
                        web_probes[mname] = v
                        if v == true then web = true end
                    end
                end
                unit.web = web
                if type(web_probes.IsGrappled) == "boolean" then
                    unit.grappled = web_probes.IsGrappled
                end
                unit.web_probes = web_probes  -- diagnostic; remove when verified
                -- Webber identification: try API methods first, fall back later (post-loop)
                if web then
                    for _, mname in ipairs({"GetGrappler", "GetGrappledBy", "GetGrapplerPawn", "GetPinnedBy"}) do
                        local ok_m, src = pcall(function() return p[mname](p) end)
                        if ok_m and src then
                            local ok_id, sid = pcall(function() return src:GetId() end)
                            if ok_id and sid then unit.web_source_uid = sid; break end
                        end
                    end
                end
                local ok_ar, ar = pcall(function() return p:IsArmor() end)
                if ok_ar and ar then unit.armor = true end

                -- Weapons from type definition. Python applies the narrow
                -- modeled save overlay before solving and action execution;
                -- exporting every purchased passive here could bypass the
                -- solver's known-type/research gate.
                unit.weapons = {}
                if pawn_def and pawn_def.SkillList then
                    for _, wname in ipairs(pawn_def.SkillList) do
                        unit.weapons[#unit.weapons + 1] = wname
                    end
                end

                -- Enemy attack data
                if p:GetTeam() == TEAM_ENEMY then
                    local qskill = save_data.queued_skills[pid]
                    local ok_sw, sw = pcall(function() return p:GetSelectedWeapon() end)
                    if qskill ~= nil then
                        unit.has_queued_attack = qskill >= 0
                        if ok_sw and sw and sw > 0 and not unit.has_queued_attack then
                            log_bridge(string.format(
                                "selected_weapon ignored for non-attacking %s/%d: GetSelectedWeapon=%s iQueuedSkill=%s",
                                ptype or "?", pid, tostring(sw), tostring(qskill)))
                        end
                    elseif ok_sw and sw and sw > 0 then
                        unit.has_queued_attack = true
                    end

                    -- Per-enemy target: piQueuedShot first (projectile/laser/
                    -- artillery attacks), then piTarget (leap/melee landing
                    -- tile — used by Jumper pawns like Leaper1/Leaper2 where
                    -- piQueuedShot is (-1,-1)), then Lua API probes. We must
                    -- gate the piTarget read on iQueuedSkill >= 0 because the
                    -- save stores piTarget as a stale last-target even on
                    -- pawns that have no queued skill this turn.
                    local qorigin = save_data.queued_origins[pid]
                    if qorigin then
                        unit.queued_origin = {qorigin.x, qorigin.y}
                    end
                    local qs = save_data.queued_shots[pid]
                    if qs and qs.x >= 0 and qs.y >= 0 then
                        unit.queued_target_raw = {qs.x, qs.y}
                        local normalized, did_normalize =
                            normalize_queued_target(qs, qorigin, unit.x, unit.y)
                        unit.queued_target = normalized
                        if did_normalize then
                            unit.queued_target_normalized = true
                        end
                        -- Save-file piQueuedShot can remain stale after live
                        -- attack retarget effects such as DIR_FLIP. Prefer
                        -- the C++ pawn's current queued shot when it returns a
                        -- valid board tile; it already reflects the current
                        -- position/target and must not be normalized again by
                        -- the Python reader.
                        -- NOTE (verified live 2026-10-10): Pawn has no
                        -- GetQueuedShot method ("attempt to call method
                        -- 'GetQueuedShot' (a nil value)"), so this pcall
                        -- always fails and the override never applies. No
                        -- Lua API exposes a pawn's live queued shot: a shot
                        -- cleared (smoke, freeze, water) or flipped during the
                        -- turn stays stale here. scripts/live_play.py takes
                        -- the engine's tracked queued shots instead.
                        if unit.has_queued_attack then
                            local ok_gqs, gqs = pcall(function() return p:GetQueuedShot() end)
                            if ok_gqs and gqs and (type(gqs) == "userdata" or type(gqs) == "table") then
                                local gx, gy = gqs.x, gqs.y
                                if type(gx) == "number" and type(gy) == "number"
                                        and gx >= 0 and gy >= 0
                                        and gx <= 7 and gy <= 7
                                        and (not unit.queued_target
                                             or unit.queued_target[1] ~= gx
                                             or unit.queued_target[2] ~= gy) then
                                    log_bridge(string.format(
                                        "queued_target live override for %s/%d: save=(%d,%d) normalized=%s GetQueuedShot=(%d,%d)",
                                        ptype or "?", pid, qs.x, qs.y,
                                        unit.queued_target and string.format("(%d,%d)", unit.queued_target[1], unit.queued_target[2]) or "nil",
                                        gx, gy))
                                    unit.queued_target = {gx, gy}
                                    unit.queued_target_normalized = true
                                end
                            end
                        end
                    elseif unit.has_queued_attack then
                        local resolved_via = nil
                        -- (1) Save-file piTarget (works for Leapers, Scorpions,
                        --     any melee/jumper pawn with AddQueuedMelee).
                        local qt = save_data.queued_targets[pid]
                        if qt and qskill and qskill >= 0
                                and qt.x >= 0 and qt.y >= 0
                                and qt.x <= 7 and qt.y <= 7 then
                            unit.queued_target = {qt.x, qt.y}
                            resolved_via = "save_piTarget"
                        end
                        -- (2) Live Lua API: GetQueuedShot() — works for
                        --     HornetBoss and similar shots that don't land
                        --     in piQueuedShot. Try even if (1) succeeded so
                        --     we can log a mismatch for calibration.
                        --     NOTE: Pawn has no GetQueuedShot (verified live
                        --     2026-10-10); this always logs pcall_err.
                        local ok_gqs, gqs = pcall(function() return p:GetQueuedShot() end)
                        local gqs_desc = "nil"
                        if ok_gqs and gqs and (type(gqs) == "userdata" or type(gqs) == "table") then
                            local gx, gy = gqs.x, gqs.y
                            if type(gx) == "number" and type(gy) == "number" then
                                gqs_desc = string.format("(%d,%d)", gx, gy)
                                if not unit.queued_target
                                        and gx >= 0 and gy >= 0
                                        and gx <= 7 and gy <= 7 then
                                    unit.queued_target = {gx, gy}
                                    resolved_via = "GetQueuedShot"
                                end
                            else
                                gqs_desc = "non_numeric"
                            end
                        elseif not ok_gqs then
                            gqs_desc = "pcall_err"
                        end
                        -- (3) Additional Lua API probes as last resort —
                        --     these may or may not exist on the C++ Pawn
                        --     binding; pcall swallows missing-method errors.
                        --     Logged so the next run tells us which (if any)
                        --     succeeded for stubborn pawn types.
                        if not unit.queued_target then
                            for _, mname in ipairs({
                                "GetQueuedTarget", "GetTarget",
                                "GetQueuedMove", "GetQueuedLocation",
                            }) do
                                local ok_m, v = pcall(function() return p[mname](p) end)
                                if ok_m and v and (type(v) == "userdata" or type(v) == "table") then
                                    local vx, vy = v.x, v.y
                                    if type(vx) == "number" and type(vy) == "number"
                                            and vx >= 0 and vy >= 0
                                            and vx <= 7 and vy <= 7 then
                                        unit.queued_target = {vx, vy}
                                        resolved_via = mname
                                        break
                                    end
                                end
                            end
                        end
                        log_bridge(string.format(
                            "queued_target fallback for %s/%d: via=%s piTarget=%s iQueuedSkill=%s GetQueuedShot=%s result=%s",
                            ptype or "?", pid,
                            resolved_via or "none",
                            qt and string.format("(%d,%d)", qt.x, qt.y) or "nil",
                            tostring(qskill),
                            gqs_desc,
                            unit.queued_target and string.format("(%d,%d)", unit.queued_target[1], unit.queued_target[2]) or "UNRESOLVED"))
                    end

                    -- Weapon properties from game globals
                    local weapon_name = unit.weapons[1]
                    if weapon_name then
                        local wdef = _G[weapon_name]
                        if wdef then
                            unit.weapon_damage = wdef.Damage or 0
                            unit.weapon_target_behind = wdef.TargetBehind or false
                            unit.weapon_push = wdef.Push or 0
                        end
                    end
                end

                state.units[#state.units + 1] = unit

                -- Multi-tile pawns (Dam_Pawn ExtraSpaces): emit a separate
                -- unit entry per extra tile. Downstream solver mirrors HP
                -- across all entries with matching uid at damage time.
                if pawn_def and pawn_def.ExtraSpaces then
                    for _, offset in ipairs(pawn_def.ExtraSpaces) do
                        local ex = sp.x + offset.x
                        local ey = sp.y + offset.y
                        if ex >= 0 and ex < 8 and ey >= 0 and ey < 8 then
                            local extra = {}
                            for k, v in pairs(unit) do extra[k] = v end
                            extra.x = ex
                            extra.y = ey
                            extra.is_extra_tile = true
                            extra.weapons = {}  -- don't double-emit attacks
                            state.units[#state.units + 1] = extra
                        end
                    end
                end
            end
        end
    end

    -- Bridge extension: per-unit traits, weapons, pilots, non-enemy queued
    -- shots and scenario-queued attacks (before attack_order reads them).
    ITBX.try("after_units", ITBX.after_units, state, save_data)

    -- Attack order: enemies with queued attacks in live unit-list order.
    -- Do not sort by UID; Mission_Factory captures showed Pinnacle bots can
    -- resolve Snowlaser before a lower-UID Burnbug kills it.
    state.attack_order = {}
    for _, u in ipairs(state.units) do
        if u.team == 6 and u.has_queued_attack then
            state.attack_order[#state.attack_order + 1] = u.uid
        end
    end

    -- Webber fallback: for any webbed unit without a known web_source_uid
    -- (Lua API didn't expose it), pick the closest alive enemy whose primary
    -- weapon has Web=true. ITB rule: web breaks when webber is pushed or killed,
    -- so the solver needs to know which enemy unwebs the unit. If no webber is
    -- found (all dead), clear the stale web flag entirely.
    local WEB_WEAPONS = {ScorpionAtk1=true, ScorpionAtk2=true, ScorpionAtkB=true,
                         LeaperAtk1=true, LeaperAtk2=true, MosquitoAtkB=true}
    for _, u in ipairs(state.units) do
        if u.web and not u.web_source_uid then
            local best_uid, best_dist = nil, 999
            for _, e in ipairs(state.units) do
                if e.team == 6 and e.hp > 0 and e.weapons and e.weapons[1]
                        and WEB_WEAPONS[e.weapons[1]] then
                    local d = math.abs(e.x - u.x) + math.abs(e.y - u.y)
                    if d < best_dist then best_uid, best_dist = e.uid, d end
                end
            end
            if best_uid then
                u.web_source_uid = best_uid
            elseif u.grappled ~= true then
                -- No webber alive and the game does not report the pawn
                -- grappled: stale web. Clear it and restore base move. (Spider
                -- eggs web without a webbing weapon: Pawn:IsGrappled wins.)
                u.web = false
                u.move = u.base_move
            end
        end
    end

    -- Targeted tiles (enemy attack indicators)
    state.targeted_tiles = {}
    for y = 0, 7 do
        for x = 0, 7 do
            if Board:IsTargeted(Point(x, y)) then
                state.targeted_tiles[#state.targeted_tiles + 1] = {x, y}
            end
        end
    end

    -- Spawning tiles
    state.spawning_tiles = {}
    for y = 0, 7 do
        for x = 0, 7 do
            if Board:IsSpawning(Point(x, y)) then
                state.spawning_tiles[#state.spawning_tiles + 1] = {x, y}
            end
        end
    end

    -- Environment danger (v1 + v2). v1 = flat list of [x,y] tiles.
    -- v2 = list of [x, y, damage, kill_int, flying_immune] where:
    --   kill_int=1      → Deadly Threat (instant-kill, bypasses shield/
    --                     frozen/armor/ACID per ITB spec)
    --   flying_immune=1 → terrain-conversion lethal (Tidal Wave, Cataclysm,
    --                     Seismic). Effectively-flying units survive
    --                     because water/chasm rules let them hover.
    --                     Air Strike / Lightning / Final Cave falling rocks
    --                     emit flying_immune=0 — those hit flyers too.
    --                     Satellite launch exhaust emits flying_immune=1:
    --                     ground units die, flyers survive.
    -- The 5th field landed at SIMULATOR_VERSION 19 (2026-04-25) closing the
    -- "Hornet on Tidal tile" silent kill desync. Older bridges emit only 4
    -- fields; the Rust deserializer falls back to env_type when the 5th is
    -- missing.
    --
    -- environment_freeze (sim v25): list of [x,y] for Ice Storm tiles (vanilla
    -- Env_SnowStorm, Acid=false). Applies Frozen=true to units at start of
    -- enemy turn — non-lethal status, separate channel from env_danger so the
    -- evaluator scores "lose a turn" rather than "die". NanoStorm
    -- (Env_NanoStorm = Env_SnowStorm:new{Acid=true}) routes into env_danger
    -- with kill=0, damage=1 (the existing non-lethal path handles 1 damage).
    state.environment_danger = {}
    state.environment_danger_v2 = {}
    state.environment_freeze = {}

    pcall(function()
        local mission = _ITB_CURRENT_MISSION
        if not mission or not mission.LiveEnvironment then return end
        local volcano = mission_final_volcano(
            mission_id,
            mission.LiveEnvironment
        )
        if volcano ~= nil then
            state.mission_final_volcano = volcano
        end
    end)

    pcall(function()
        local mission = _ITB_CURRENT_MISSION
        if not mission or not mission.LiveEnvironment then return end
        local final_cave = mission_final_cave(
            mission_id,
            mission.LiveEnvironment
        )
        if final_cave ~= nil then
            state.mission_final_cave = final_cave
        end
    end)

    -- Default all env_danger tiles to lethal (kill=1). Most hazards ARE
    -- lethal to ground units: Air Strike, Lightning, Cataclysm→chasm,
    -- Seismic→chasm, Tidal Waves→water. Non-lethal hazards (Wind Storm,
    -- Sandstorm, NanoStorm) detected via class match / field signatures
    -- and get kill=0. Vanilla Ice Storm bypasses env_danger entirely and
    -- routes through env_freeze instead.
    local env_damage = 1
    local env_kill_default = true
    -- Default flying_immune is false. Set true for terrain-conversion
    -- env types when env_type detection lands on tidal/cataclysm/seismic.
    local env_flying_immune_default = false
    -- When the env class is Env_SnowStorm with Acid=false, route IsEnvironmentDanger
    -- tiles into environment_freeze instead of environment_danger. NanoStorm (Acid=true)
    -- uses env_danger with non-lethal damage. Set during class-metatable detection below.
    local route_to_freeze = false

    -- Class-metatable detection FIRES BEFORE field signatures: Env_SnowStorm
    -- shares the `Locations` field with Lightning/Air Strike/Seismic, so the
    -- old field-first heuristic flagged Ice Storm as kill=1 lethal. Walk the
    -- metatable chain so subclasses (Env_NanoStorm extends Env_SnowStorm)
    -- match too. Field signatures stay as a fallback for envs we don't
    -- explicitly recognize.
    local env_type = "unknown"
    pcall(function()
        local mission = _ITB_CURRENT_MISSION
        if not mission or not mission.LiveEnvironment then return end
        local le = mission.LiveEnvironment
        local mission_id = mission.ID or ""

        -- Mission_Final_Cave uses Env_Final, whose marked tiles are falling
        -- rock / tentacle death effects (SpaceDamage(..., DAMAGE_DEATH)),
        -- not ordinary chasm conversion. Prospero/flying mechs die here.
        if mission_id == "Mission_Final_Cave" then
            env_type = "final_cave"
            env_kill_default = true
            env_flying_immune_default = false
            return
        end

        -- Surface Volcanic Hive alternates source-defined ordered Rocks and
        -- Lava modes. Lava carries zero direct damage on the wire because its
        -- lethality comes from the permanent TERRAIN_LAVA conversion; Rust
        -- consumes the ordered mission_final_volcano payload for exact unit
        -- and terrain semantics.
        if mission_id == "Mission_Final" then
            env_type = "volcano"
            env_flying_immune_default = false
            local volcano = state.mission_final_volcano
            if volcano and volcano.complete and volcano.mode == 2 then
                env_damage = 0
                env_kill_default = false
            else
                env_damage = 1
                env_kill_default = true
            end
            return
        end

        -- Mission_Terratide subclasses Env_Tides but replaces the lethal
        -- water wave with a smoke wave (NewTerrain=TERRAIN_SAND).  Its live
        -- environment still exposes `Index`, so without this authoritative
        -- mission override it falls through as lethal tidal/cataclysm danger.
        -- Keep the warned row on the v2 channel with kill=0; the Rust bridge
        -- decoder routes this mission's row to pending smoke rather than the
        -- generic non-lethal 1-damage path.
        if mission_id == "Mission_Terratide" then
            env_type = "sandstorm"
            env_kill_default = false
            env_flying_immune_default = false
            return
        end

        -- Walk metatable chain. For each link, check membership in our
        -- known-env table. Stops at first match. `_G` lookup is safe — class
        -- globals are always set before any LiveEnvironment instance exists.
        local mt = getmetatable(le)
        while mt do
            local cls_table = mt.__index or mt
            -- Env_SnowStorm: vanilla Ice Storm (Acid=false, freeze) OR Env_NanoStorm
            -- which inherits from it (Acid=true, 1 acid damage). Distinguish by the
            -- live instance's Acid flag — covers both directly-instantiated SnowStorms
            -- and the Nano subclass without a separate metatable check.
            if _G["Env_SnowStorm"] and cls_table == _G["Env_SnowStorm"] then
                if le.Acid then
                    -- NanoStorm: 1 damage + ACID, non-lethal, no freeze.
                    -- ACID application itself is a separate gap — bridge
                    -- doesn't carry per-tile-acid yet — but the 1-damage
                    -- non-lethal path is correct for now.
                    env_type = "nanostorm"
                    env_kill_default = false
                else
                    -- Vanilla Ice Storm: 0 damage, Frozen=true.
                    env_type = "snow"
                    env_kill_default = false
                    route_to_freeze = true
                end
                return
            end
            if _G["Env_Sandstorm"] and cls_table == _G["Env_Sandstorm"] then
                env_type = "sandstorm"
                env_kill_default = false
                return
            end
            mt = getmetatable(cls_table)
        end

        -- Mission IDs are authoritative when class/field signatures collide.
        -- Archive Airstrike exposes StartEffect like Seismic/Cataclysm on some
        -- bridge builds, but bombs still kill flying units. Terrain-conversion
        -- missions are the ones where flyers hover over the new water/chasm.
        if mission_id == "Mission_Airstrike"
                or mission_id == "Mission_Lightning"
                or mission_id == "Mission_LightningStorm" then
            env_type = "lightning_or_airstrike"
            env_kill_default = true
            env_flying_immune_default = false
            return
        elseif mission_id == "Mission_Tides" then
            env_type = "tidal"
            env_kill_default = true
            env_flying_immune_default = true
            return
        elseif mission_id == "Mission_Cataclysm"
                or mission_id == "Mission_Crack" then
            env_type = "cataclysm_or_seismic"
            env_kill_default = true
            env_flying_immune_default = true
            return
        end

        -- Field-signature fallback for envs without an explicit class match
        -- (mods, edge-case classes). Order tightened: WindDir/Row/Index/StartEffect
        -- are unique enough; Locations is checked LAST since SnowStorm shares it.
        if le.WindDir ~= nil then
            env_type = "wind"
            env_kill_default = false
        elseif le.Row ~= nil then
            env_type = "sandstorm"
            env_kill_default = false
        elseif le.Indices ~= nil then
            -- No known vanilla env uses bare Indices — kept for mod compat.
            env_type = "snow"
            env_kill_default = false
        elseif le.Index ~= nil then
            env_type = "tidal_or_cataclysm"
            env_flying_immune_default = true
        elseif le.StartEffect ~= nil then
            env_type = "cataclysm_or_seismic"
            env_flying_immune_default = true
        elseif le.Locations ~= nil then
            -- After the Env_SnowStorm metatable check above, Locations now
            -- means Lightning / Air Strike / Seismic.
            env_type = "lightning_or_airstrike"
            env_flying_immune_default = false
        else
            local fields = {}
            pcall(function()
                for k, _ in pairs(le) do fields[#fields+1] = tostring(k) end
            end)
            log_bridge("[env] WARNING: unknown env_type. Fields: " .. table.concat(fields, ", "))
        end
    end)
    state.env_type = env_type

    -- The visible warning mask can be empty when every column is hidden by a
    -- building shadow or already has the target terrain. Export the live
    -- Index so Rust can still advance Env_Tides::Plan exactly. For Tides, Rust
    -- derives the full-row permanent spawn-block boundary from the inventoried
    -- source; Terratide does not execute that water-only BlockSpawn branch.
    -- No native blocked-cell getter has been identified for this build.
    pcall(function()
        local mission = _ITB_CURRENT_MISSION
        if not mission or not mission.LiveEnvironment then return end
        local index = mission_tides_index(
            mission.ID or "",
            mission.LiveEnvironment
        )
        if index ~= nil then
            state.environment_tides_index = index
        end
        local planned = mission_tides_planned(
            mission.ID or "",
            mission.LiveEnvironment
        )
        if planned ~= nil then
            state.environment_tides_planned = planned
        end
    end)

    if env_type == "wind" then
        pcall(function()
            local mission = _ITB_CURRENT_MISSION
            if mission and mission.LiveEnvironment
                    and mission.LiveEnvironment.WindDir ~= nil then
                state.environment_wind_dir = mission.LiveEnvironment.WindDir
            end
        end)
    end

    -- Helper: add a danger tile to both v1 and v2 fields. The optional
    -- `flying_immune_override` controls the 5th field.
    local function add_danger(x, y, kill_override, flying_immune_override)
        state.environment_danger[#state.environment_danger + 1] = {x, y}
        local k = env_kill_default
        if kill_override ~= nil then
            k = kill_override
        end
        local fi = env_flying_immune_default
        if flying_immune_override ~= nil then
            fi = flying_immune_override
        end
        -- flying_immune is meaningless on non-lethal tiles (1 dmg already
        -- skips flying via the bump path); zero it out to keep the wire
        -- representation tidy.
        if not k then fi = false end
        state.environment_danger_v2[#state.environment_danger_v2 + 1] =
            {x, y, env_damage, k and 1 or 0, fi and 1 or 0}
    end

    for y = 0, 7 do
        for x = 0, 7 do
            local ok, danger = pcall(function() return Board:IsEnvironmentDanger(Point(x, y)) end)
            if ok and danger then
                if route_to_freeze then
                    -- Vanilla Ice Storm: tiles freeze at start of enemy turn.
                    -- Non-lethal status effect; bypasses env_danger entirely.
                    state.environment_freeze[#state.environment_freeze + 1] = {x, y}
                else
                    add_danger(x, y)
                end
            end
        end
    end

    -- Exact final-island payloads are more authoritative than a separately
    -- sampled Board:IsEnvironmentDanger scan, especially for Env_Final's
    -- Instant phases. Rebuild both warning channels from the same atomic list.
    local exact_final = state.mission_final_volcano
    if not (exact_final and exact_final.complete) then
        exact_final = state.mission_final_cave
    end
    if exact_final and exact_final.complete then
        state.environment_danger = {}
        state.environment_danger_v2 = {}
        for _, point in ipairs(exact_final.locations) do
            add_danger(point[1], point[2], env_kill_default, false)
        end
    end

    -- Satellite rocket deadly threat: 4 adjacent tiles kill grounded units on
    -- launch. Board:IsEnvironmentDanger() does NOT detect these, so we add
    -- them manually.
    -- Only flag tiles on the turn the rocket is queued to fire (GetSelectedWeapon > 0).
    -- Satellite rockets are always lethal regardless of mission environment,
    -- but live launch exhaust spares flying pawns.
    for _, u in ipairs(state.units) do
        if u.type and string.find(u.type, "Satellite") then
            local ok, p = pcall(function() return Board:GetPawn(u.uid) end)
            if ok and p then
                local ok_sw, sw = pcall(function() return p:GetSelectedWeapon() end)
                local queued = ok_sw and sw and sw > 0
                if queued then
                    u.queued_launch = true
                    local dirs = {{-1,0},{1,0},{0,-1},{0,1}}
                    for _, d in ipairs(dirs) do
                        local nx, ny = u.x + d[1], u.y + d[2]
                        if nx >= 0 and nx <= 7 and ny >= 0 and ny <= 7 then
                            add_danger(nx, ny, true, true)
                        end
                    end
                end
            end
        end
    end

    -- Deployment zone: prefer a live Board:GetZone("deployment") read on every
    -- dump so the zone is correct even when BaseDeployment hasn't fired yet
    -- (e.g. between missions) or has been cleared by MissionEnd. Falls back to
    -- the cached BaseDeployment capture if the live read returns nothing.
    pcall(function()
        local zone = capture_deploy_zone()
        if zone and #zone > 0 then
            state.deployment_zone = zone
            _ITB_DEPLOY_ZONE = zone  -- refresh cache for consistency
        end
    end)
    if not state.deployment_zone and _ITB_DEPLOY_ZONE and #_ITB_DEPLOY_ZONE > 0 then
        state.deployment_zone = _ITB_DEPLOY_ZONE
    end

    -- Mission metadata for hazard classification
    pcall(function()
        local mission = _ITB_CURRENT_MISSION
        if mission then
            state.mission_id = mission_id
            -- Mission_Hacking converts one specific stored Cannon Bot after
            -- one specific stored facility dies. Export both live pawn IDs so
            -- the simulator never guesses from type alone when other
            -- Snowtank1 enemies are present. Missing/invalid IDs are omitted;
            -- old bridge payloads therefore fail closed in Rust.
            local bot_id, hack_id = mission_hacking_ids(mission.ID, mission)
            if bot_id ~= nil then
                state.mission_hacking_bot_id = bot_id
                state.mission_hacking_hack_id = hack_id
            end
            local pistons = mission_pistons(mission.ID, state.units)
            if pistons ~= nil then
                state.mission_pistons = pistons
            end
        end
    end)

    -- Teleporter pads: populated by the Mission_Teleporter:StartMission
    -- wrap below. Each entry = {x1, y1, x2, y2}. Empty list / absent on
    -- non-teleporter missions; stale pairs must not leak into other
    -- missions. The earlier Board.AddTeleport global
    -- override crashed mac OS at file-load with "no static 'AddTeleport'
    -- in class 'Board'" (commit 456ba49 → rolled back in 63e0e18); the
    -- current scope-rebinds AddTeleport only inside StartMission and pcalls
    -- everything so a future API change can't take down mission load.
    if state.mission_id == "Mission_Teleporter"
       and _ITB_TELEPORT_PAIRS and #_ITB_TELEPORT_PAIRS > 0 then
        state.teleporter_pairs = {}
        for _, pair in ipairs(_ITB_TELEPORT_PAIRS) do
            state.teleporter_pairs[#state.teleporter_pairs + 1] = pair
        end
    end

    -- Bonus-objective progress for enemy kill-count objectives.
    -- BONUS_KILL_FIVE is "kill at least N"; BONUS_PACIFIST is
    -- "kill N or fewer". Emit separate fields because the former rewards
    -- extra kills while the latter fails immediately when exceeded. Per
    -- scripts/missions/missions.lua:
    --   BONUS_KILL_FIVE = 6 in the enum
    --   BONUS_PACIFIST = 9 in the enum
    --   mission.BonusObjs is the chosen bonus list (random from BonusPool)
    --   mission.KilledVek is cumulative generic kill-count progress
    --   mission:GetKillBonus() is difficulty-scaled (5 easy / 7 normal/hard)
    --   mission:GetPacifistCount() is difficulty-scaled (4 easy / 5 normal/hard / 6 unfair)
    -- Mission_AcidTank is a built-in Detritus objective rather than a
    -- BONUS_KILL_FIVE entry: kill 4 acid-inflicted Vek. It tracks mission.AcidKills,
    -- so expose that through the same solver fields.
    pcall(function()
        local mission = _ITB_CURRENT_MISSION
        local is_acid_tank = mission and mission.ID == "Mission_AcidTank"
        if is_acid_tank then
            state.mission_kill_target = 4
        end
        if mission and mission.BonusObjs then
            local has_kill_five = false
            local has_pacifist = false
            for _, obj in ipairs(mission.BonusObjs) do
                if obj == 6 then has_kill_five = true end
                if obj == 9 then has_pacifist = true end
            end
            if has_kill_five and mission.GetKillBonus then
                local ok, target = pcall(function() return mission:GetKillBonus() end)
                if ok and type(target) == "number" then
                    state.mission_kill_target = target
                end
            end
            if has_pacifist and mission.GetPacifistCount then
                local ok, limit = pcall(function() return mission:GetPacifistCount() end)
                if ok and type(limit) == "number" then
                    state.mission_kill_limit = limit
                end
            end
        end
        if is_acid_tank and mission.AcidKills ~= nil then
            state.mission_kills_done = mission.AcidKills
        elseif mission and mission.KilledVek ~= nil then
            state.mission_kills_done = mission.KilledVek
        end
    end)

    -- Mission_Repair objective progress ("Use 3 Repair Platforms"). The
    -- game increments RepairPickups from EVENT_REPAIR_PICKUP; any unit that
    -- triggers Item_Repair_Mine counts. Expose both target and cumulative
    -- progress so the solver can value using the remaining platforms.
    pcall(function()
        local mission = _ITB_CURRENT_MISSION
        if mission and mission.ID == "Mission_Repair" then
            state.repair_platform_target = 3
            if mission.RepairPickups ~= nil then
                state.repair_platforms_used = mission.RepairPickups
            end
        end
    end)

    -- Mission_Force objective progress ("Destroy 2 mountains"). The game
    -- increments mission.Mountains from EVENT_MOUNTAIN_DESTROYED and gates
    -- mission end on mission.MountainsGoal.
    pcall(function()
        local mission = _ITB_CURRENT_MISSION
        if mission and mission.ID == "Mission_Force" then
            if mission.MountainsGoal ~= nil then
                state.mission_mountain_target = mission.MountainsGoal
            else
                state.mission_mountain_target = 2
            end
            if mission.Mountains ~= nil then
                state.mission_mountains_destroyed = mission.Mountains
            end
        end
    end)

    -- TODO(sim_v21): emit `state.bonus_objective_unit_types` — the list of
    -- pawn-type strings the active mission's BonusObjs flag as "do not
    -- kill X" (e.g. BONUS_PROTECT_VOLATILE → {"GlowingScorpion"}). The
    -- Rust side already reads `JsonInput::bonus_objective_unit_types`
    -- and gates `volatile_enemy_killed` on it; while this Lua hook is
    -- unimplemented, Python falls back to `data/mission_bonus_objectives.json`
    -- keyed by mission_id (see src/solver/mission_bonus_objectives.py).
    -- Implementing this in Lua requires inspecting mission.BonusObjs for
    -- the protect-X enum values + walking the mission's Pawn list to map
    -- enum→type-name; safe to do but needs in-game testing to validate
    -- the enum values, hence deferred. The Python fallback covers all
    -- catalogued protect-X missions today.

    -- Victory signal: when mission:IsFinalTurn() is true, no more Vek will
    -- emerge after this turn's enemy phase. Solver treats this as the final
    -- turn (future_factor = 0). Also expose mission.TurnLimit as authoritative
    -- total_turns — this matches "Hold out for N turns" better than the
    -- hardcoded 5 when the mission actually runs for a different length.
    -- API reference: scripts/missions/missions.lua
    --   Mission:IsFinalTurn() → Game:GetTurnCount() == self.TurnLimit - 1
    --   Mission:GetSpawnCount() returns 0 on final turn (no reinforcements)
    pcall(function()
        local mission = _ITB_CURRENT_MISSION
        if mission then
            if mission.TurnLimit ~= nil then
                state.total_turns = mission.TurnLimit
            end
            if mission.IsFinalTurn and mission:IsFinalTurn() then
                state.remaining_spawns = 0
            else
                -- Not final: set a positive sentinel so future_factor uses
                -- the normal turn-based decay instead of collapsing to 0.
                state.remaining_spawns = 1
            end
        end
    end)

    -- Island map: per-region mission preview for the squad-aware mission
    -- picker. The currently-selected island's mission slate lives at the
    -- global `GAME.Missions` (see scripts/islands.lua createIncidents:319 —
    -- "GAME.Missions = incidents"). Each entry is a Mission object with:
    --   .ID         -- "Mission_Train", "Mission_Volatile", etc.
    --   .BonusObjs  -- list of int enums (1-9). See missions.lua:32-40 —
    --                 BONUS_ASSET=1 BONUS_KILL=2 BONUS_GRID=3 BONUS_MECHS=4
    --                 BONUS_BLOCK=5 BONUS_KILL_FIVE=6 BONUS_DEBRIS=7
    --                 BONUS_SELFDAMAGE=8 BONUS_PACIFIST=9
    --   .Environment -- "Env_Lava", "Env_TidalWaves", "Env_Conveyor",
    --                   "Env_Null", etc.
    --   .DiffMod    -- DIFF_MOD_EASY=-1, DIFF_MOD_NONE=0, DIFF_MOD_HARD=1
    --   .AssetId    -- e.g. "Mission_Mech_Boss" — only set for some missions
    -- GAME.Island holds the 1-based corp slot for the current island (set
    -- in createIncidents:191). Defensive: GAME and GAME.Missions may be
    -- nil during boot or non-island screens; pcall guards every read.
    --
    -- Emit only when we are NOT in an active mission (combat/deployment) —
    -- in combat, _ITB_CURRENT_MISSION is set and the slate is irrelevant.
    -- Outside combat the player is on the corp island map, between-mission
    -- transition, or shop; the bridge phase will read "unknown" and the
    -- picker can score the available missions.
    state.island_map = nil
    state.island_map_debug = nil
    -- Unconditional reachability probe: if state.island_map_probe shows up
    -- in the JSON but state.island_map does not, we know the pcall block is
    -- the failure point (not the surrounding scope or write_atomic).
    state.island_map_probe = "scope_alive"
    -- Resolve GAME via _G first, fall back to bare global. Both should work
    -- given Lua's scoping rules, but we record which path succeeded so a
    -- failed lookup can be told apart from a missing-Missions case.
    local _game_ref = rawget(_G, "GAME")
    if _game_ref == nil then
        _game_ref = GAME  -- bare-global fallback (ITB's own scope convention)
    end
    state.island_map_game_seen = (_game_ref ~= nil) and type(_game_ref) or "nil"
    local ok_island_map, err_island_map = pcall(function()
        if _ITB_CURRENT_MISSION ~= nil then
            state.island_map_debug = "skipped: in active mission"
            return  -- in active mission; slate is not the right answer
        end
        if not _game_ref or type(_game_ref) ~= "table" then
            state.island_map_debug = "GAME is " .. tostring(_game_ref)
            return
        end
        local missions = _game_ref.Missions
        if type(missions) ~= "table" then
            state.island_map_debug = "GAME.Missions is " .. type(missions) ..
                " (value=" .. tostring(missions) .. ")"
            return
        end
        local out = {}
        -- GAME.Missions is 1-indexed for regular missions; key 0 is the
        -- boss/final mission when present. Walk both 0 and 1..N.
        local indices = {}
        local n_keys = 0
        for k, _ in pairs(missions) do
            n_keys = n_keys + 1
            if type(k) == "number" then
                indices[#indices + 1] = k
            end
        end
        table.sort(indices)
        for _, k in ipairs(indices) do
            local m = missions[k]
            if type(m) == "table" then
                local entry = {
                    region_id = k,
                    mission_id = m.ID or "",
                }
                local bonus_ids = {}
                if type(m.BonusObjs) == "table" then
                    for _, b in ipairs(m.BonusObjs) do
                        if type(b) == "number" then
                            bonus_ids[#bonus_ids + 1] = b
                        end
                    end
                end
                entry.bonus_objective_ids = bonus_ids
                if type(m.Environment) == "string" and m.Environment ~= "" then
                    entry.environment = m.Environment
                else
                    entry.environment = nil
                end
                if type(m.DiffMod) == "number" then
                    entry.diff_mod = m.DiffMod
                end
                if type(m.AssetId) == "string" and m.AssetId ~= "" then
                    entry.asset_id = m.AssetId
                end
                if type(m.BossMission) == "boolean" then
                    entry.boss = m.BossMission
                end
                out[#out + 1] = entry
            end
        end
        state.island_map = out
        state.island_map_debug = "ok: " .. tostring(#out) .. " entries from " ..
            tostring(n_keys) .. " keys"
        if type(_game_ref.Island) == "number" then
            state.island_index = _game_ref.Island
        end
    end)
    if not ok_island_map then
        state.island_map_debug = "pcall error: " .. tostring(err_island_map)
    end

    -- Bridge extension: mission instance, zones, spawn queue, ledgers.
    ITBX.try("finish", ITBX.finish, state, _ITB_CURRENT_MISSION)
    state.bridge_errors = ITBX.errs or {}
    if ITBX.disabled then state.bridge_ext_disabled = ITBX.disabled_reason end
    -- Not an extension export: kept even with the extension switched off.
    pcall(ITBX.mark_settled, state)

    write_atomic(out_path or STATE_FILE, out_tmp or STATE_TMP, json_encode(state))
end
ITBX._dump_state = dump_state  -- for the offline harness

--------------------------------------------------------------------
-- Bridge configuration
--------------------------------------------------------------------
local _bridge_speed = "fast"  -- "fast" or "visual"

--------------------------------------------------------------------
-- Animation/effect handling
--------------------------------------------------------------------
-- Commands run inside a coroutine created by poll_commands() so that
-- wait_for_board_coro / wait_until_coro can yield control back to the
-- engine while the effect queue drains. The OLD wait_for_board() was a
-- tight os.clock() spin inside the same Lua thread as Mission:BaseUpdate
-- — the engine could never advance the animation queue while Lua was
-- spinning, so Board:IsBusy() stayed true until the 15 s timeout fired.
-- Yielding lets BaseUpdate return, the engine advance, and the next
-- BaseUpdate tick resume the coroutine with a fresh Board state.

local _running_coroutine = nil

local function bridge_fast_mode()
    return _bridge_speed == "fast"
end

-- NOTE: os.time() (wall clock, second precision) rather than os.clock()
-- (process CPU time). When the coroutine yields back to the engine, CPU
-- time barely advances relative to wall time, so an os.clock()-based
-- deadline stretches out to ~3x its nominal wall-clock length. Python's
-- wait_for_ack uses wall clock, so the two must agree.
local function wait_until_coro(predicate, max_wait)
    max_wait = max_wait or 15
    local start = os.time()
    while os.time() - start < max_wait do
        local ok, ready = pcall(predicate)
        if not ok or ready then
            ITBX.cmd_waiting = false
            return true
        end
        -- Dumps taken while we yield (periodic ones) say command_waiting.
        ITBX.cmd_waiting = true
        coroutine.yield()
    end
    ITBX.cmd_waiting = false
    log_bridge("WARN: wait_until_coro timed out after " .. max_wait .. "s (wall)")
    return false
end

-- After a SetSpace move (fast mode) nothing animates: a short wait is enough.
local function wait_for_board_coro(max_wait)
    if bridge_fast_mode() then
        max_wait = math.min(max_wait or 15, 2)
    end
    return wait_until_coro(function()
        return not Board:IsBusy()
    end, max_wait)
end

-- After a weapon or a repair, in every speed mode: the state dumped right
-- after the ack is what callers read, so wait until the effects have fully
-- resolved. The 2 s fast-mode cap (os.time ticks, so 1-2 s) cut off
-- Ranged_Ignite's side pushes (artillery flight, then 0.4 s pushes) in the
-- 2026-10-09 live session: the dump showed the target tile burning but the
-- pushed Vek on their old tiles. ITBX.settle_coro (below) does this for
-- every action, on the board and the acting pawn, and acks an error when
-- the budget runs out.

local function move_pawn_for_bridge(pawn, point)
    if bridge_fast_mode() then
        local ok, err = pcall(function() pawn:SetSpace(point) end)
        if ok then return true, "SetSpace" end
        return false, err
    end
    local ok, err = pcall(function() pawn:Move(point) end)
    if ok then return true, "Move" end
    return false, err
end

--------------------------------------------------------------------
-- Weapon skill execution
--------------------------------------------------------------------
-- Previous versions of this helper called
--   Board:AddEffect(skill:GetSkillEffect(source, target))
-- which fires the SkillEffect outside any pawn ownership context and
-- leaves the engine's effect queue in a permanently-busy state
-- (Board:IsBusy() stays true forever). Vanilla ITB — including the
-- game's own trailer script — exclusively uses `pawn:FireWeapon(target,
-- slot)` to invoke a weapon: that C-side method handles ownership,
-- animation scheduling and queue drain the way the engine expects.
-- Slot is 1-indexed into the pawn type's SkillList; see the weapon
-- extraction loop in dump_state() where SkillList is read in the same
-- order.
-- find_weapon_slot: name-based lookup kept for backward compat / diagnostics
local function find_weapon_slot(pawn, weapon_id)
    local ptype = pawn:GetType()
    local pawn_def = _G[ptype]
    if not (pawn_def and pawn_def.SkillList) then return nil end
    for i, wname in ipairs(pawn_def.SkillList) do
        if wname == weapon_id then return i end
    end
    return nil
end

local function effective_weapon_from_save(save_data, uid, weapon_slot, fallback)
    if not save_data then return fallback, "static" end
    local weapons = save_data.current_weapons or {}
    local idx = nil
    -- GameData.current.weapons stores two loadout slots per player mech, keyed
    -- by the stable player mech ids 0,1,2. Do not use pawn["offset"] here:
    -- live combat saves can stamp every squad mech with the same value (for
    -- example 5), which is a board/undo offset rather than a loadout offset.
    if type(uid) == "number" and uid >= 0 and uid <= 2 then
        idx = uid * 2 + weapon_slot + 1
    end
    local wname = idx and weapons[idx] or nil
    if type(wname) == "string" and wname ~= "" then
        if _G[wname] ~= nil then
            return wname, "save"
        end
        log_bridge("WARN: save weapon " .. wname ..
                   " for uid=" .. tostring(uid) ..
                   " slot=" .. tostring(weapon_slot) ..
                   " has no Lua skill; falling back to " .. tostring(fallback))
    end
    return fallback, "static"
end

local function tile_damage_snapshot(pt)
    local snap = {}
    local ok_t, terrain_id = pcall(function() return Board:GetTerrain(pt) end)
    if ok_t then snap.terrain_id = terrain_id end
    local ok_h, hp = pcall(function() return Board:GetHealth(pt) end)
    if ok_h then snap.health = hp end
    local ok_cr, cracked = pcall(function() return Board:IsCracked(pt) end)
    if ok_cr then snap.cracked = cracked and true or false end
    return snap
end

local function tile_damage_changed(before, pt)
    if before == nil then return false end
    local ok_t, terrain_id = pcall(function() return Board:GetTerrain(pt) end)
    if ok_t and before.terrain_id ~= nil and terrain_id ~= before.terrain_id then
        return true
    end
    local ok_h, hp = pcall(function() return Board:GetHealth(pt) end)
    if ok_h and before.health ~= nil and hp ~= before.health then
        return true
    end
    local ok_cr, cracked = pcall(function() return Board:IsCracked(pt) end)
    if ok_cr and before.cracked ~= nil and (cracked and true or false) ~= before.cracked then
        return true
    end
    return false
end

-- The bridge used to replace GetTargetArea of the Aerial Bombs (Brute_Jetmech,
-- Brute_Bombrun, Support_Smoke) and Leap_Attack (Prime_Leap,
-- Support_Boosters, Prime_SpikeLeap) families with nil-safe copies
-- (6e7db627, 7c43c2fd): the game's versions read the global `Pawn`, which
-- the bridge never set, so after a bridge FireWeapon it could be nil or a
-- dead Vek and the UI refresh raised in Brute_Jetmech::GetTargetArea. The
-- copies still read the same stale `Pawn` whenever it was set. The bridge
-- now selects the acting pawn the way a click does (ITBX.select_pawn) and
-- leaves the game's scripts alone. A Lua state patched by an older bridge
-- keeps the copies until the game restarts.
if _ITB_BRIDGE_SAFE_JET_TARGET_AREA or _ITB_BRIDGE_SAFE_LEAP_TARGET_AREA then
    log_bridge("WARN: an older bridge patched GetTargetArea in this Lua state; "
               .. "restart the game to run the original weapon scripts")
end

--------------------------------------------------------------------
-- Legacy execution: EXEC_MODE legacy only (the old Python bot's opt-in;
-- never live_play). From here to execute_weapon_by_slot the bridge plays
-- weapons itself instead of the game: Prime_TC_Punt and most two-click
-- weapons through Board:AddEffect(GetFinalEffect) or hand-built
-- SpaceDamage with Boost mirrored by hand, the Seismic DIR_FLIP fallback,
-- the Aerial Bombs TRANSIT damage/smoke emulation (the game's own
-- GetSkillEffect already does that through FireWeapon: double damage /
-- smoke), and a temporary SkillList swap. REPAIR's legacy path edits HP
-- and statuses directly.
--------------------------------------------------------------------
local function execute_prime_tc_punt(pawn, wname, tx, ty)
    local skill = _G[wname]
    if not skill then
        return false, "Prime_TC_Punt skill missing: " .. tostring(wname)
    end

    local source = pawn:GetSpace()
    local dx = tx - source.x
    local dy = ty - source.y
    local dist = math.abs(dx) + math.abs(dy)
    if dist < 2 or (dx ~= 0 and dy ~= 0) then
        return false, "invalid Prime_TC_Punt landing " .. tx .. "," .. ty ..
               " from " .. source.x .. "," .. source.y
    end

    local sx = dx == 0 and 0 or (dx / math.abs(dx))
    local sy = dy == 0 and 0 or (dy / math.abs(dy))
    local first = Point(source.x + sx, source.y + sy)
    local landing = Point(tx, ty)

    if not Board:IsValid(landing) then
        return false, "Prime_TC_Punt landing off-board " .. tx .. "," .. ty
    end
    if Board:IsBlocked(landing, PATH_FLYER) then
        return false, "Prime_TC_Punt landing blocked " .. tx .. "," .. ty
    end

    local target = Board:GetPawn(first)
    if not target then
        return false, "Prime_TC_Punt first click has no pawn at " ..
               first.x .. "," .. first.y
    end
    local guarding = false
    local ok_guard, guard_val = pcall(function() return target:IsGuarding() end)
    if ok_guard and guard_val then guarding = true end
    if guarding then
        return false, "Prime_TC_Punt target is guarding at " ..
               first.x .. "," .. first.y
    end

    local uid = nil
    local ok_uid, uid_val = pcall(function() return pawn:GetId() end)
    if ok_uid then uid = uid_val end
    local save_data = _read_save_data()
    local save_pilot = uid and save_data.pilots[uid] or nil
    local boosted = false
    local ok_bo, bo = pcall(function() return pawn:IsBoosted() end)
    if ok_bo and bo then boosted = true end
    local target_was_enemy = false
    local ok_team, target_team = pcall(function() return target:GetTeam() end)
    if ok_team and target_team == (_G.TEAM_ENEMY or 6) then
        target_was_enemy = true
    end

    -- Board:AddEffect(GetFinalEffect(...)) bypasses the engine ability-use
    -- wrapper that normally applies and consumes Boost. Mirror that wrapper
    -- here so the bridge execution matches the solver's weapon semantics.
    local old_damage = nil
    if boosted and type(skill.Damage) == "number" and skill.Damage > 0 then
        old_damage = skill.Damage
        skill.Damage = old_damage + 1
    end
    local ok, err = pcall(function()
        Board:AddEffect(skill:GetFinalEffect(source, first, landing))
    end)
    if old_damage ~= nil then
        pcall(function() skill.Damage = old_damage end)
    end
    if not ok then
        return false, "Prime_TC_Punt GetFinalEffect failed: " .. tostring(err)
    end

    local desired_boosted = false
    if save_pilot and save_pilot.id == "Pilot_Arrogant" then
        local hp = pawn:GetHealth()
        local max_hp = get_pawn_max_health(pawn, uid, save_data)
        desired_boosted = hp >= max_hp
    elseif save_pilot and save_pilot.id == "Pilot_Chemical" and target_was_enemy then
        local dead = false
        local ok_dead, is_dead = pcall(function() return target:IsDead() end)
        if ok_dead and is_dead then dead = true end
        local ok_hp, hp = pcall(function() return target:GetHealth() end)
        if ok_hp and hp <= 0 then dead = true end
        desired_boosted = dead
    end
    if boosted or desired_boosted then
        for _, mname in ipairs({"SetBoosted", "SetBoost"}) do
            local ok_set, did_set = pcall(function()
                local fn = pawn[mname]
                if type(fn) == "function" then
                    fn(pawn, desired_boosted)
                    return true
                end
                return false
            end)
            if ok_set and did_set then break end
        end
    end
    log_bridge("FIRE: " .. wname .. " two_click " ..
               source.x .. "," .. source.y .. " -> " ..
               first.x .. "," .. first.y .. " -> " .. tx .. "," .. ty)
    return true, "GetFinalEffect(" .. wname .. ") first=" ..
           first.x .. "," .. first.y .. " landing=" .. tx .. "," .. ty
end

local function effective_weapon_name_by_slot(pawn, weapon_slot)
    local slot = weapon_slot + 1
    local ptype = pawn:GetType()
    local pawn_def = _G[ptype]
    if not (pawn_def and pawn_def.SkillList) then
        return nil, nil, nil, "weapon slot " .. weapon_slot ..
               " unavailable (pawn " .. ptype .. " has no SkillList)"
    end

    local uid = nil
    local ok_uid, uid_val = pcall(function() return pawn:GetId() end)
    if ok_uid then uid = uid_val end
    local base_wname = pawn_def.SkillList[slot]
    local save_data = _read_save_data()
    local wname, wsource =
        effective_weapon_from_save(save_data, uid, weapon_slot, base_wname)
    if wname == nil then
        return nil, nil, nil, "weapon slot " .. weapon_slot ..
               " out of range (pawn " .. ptype .. " has " ..
               tostring(#pawn_def.SkillList) ..
               " static skills and no save-backed weapon)"
    end
    return wname, base_wname, slot, nil, wsource, pawn_def, uid
end

local function pawn_is_guarding(pawn)
    local ok_guard, guard_val = pcall(function() return pawn:IsGuarding() end)
    return ok_guard and guard_val
end

local function pawn_is_boosted(pawn)
    local ok_bo, boosted = pcall(function() return pawn:IsBoosted() end)
    return ok_bo and boosted
end

local function set_pawn_boosted(pawn, desired)
    for _, mname in ipairs({"SetBoosted", "SetBoost"}) do
        local ok_set, did_set = pcall(function()
            local fn = pawn[mname]
            if type(fn) == "function" then
                fn(pawn, desired)
                return true
            end
            return false
        end)
        if ok_set and did_set then return true end
    end
    return false
end

local function pawn_is_enemy(pawn)
    if pawn == nil then return false end
    local ok_team, team = pcall(function() return pawn:GetTeam() end)
    return ok_team and team == (_G.TEAM_ENEMY or 6)
end

local function pawn_is_dead_or_zero(pawn)
    if pawn == nil then return false end
    local ok_dead, dead = pcall(function() return pawn:IsDead() end)
    if ok_dead and dead then return true end
    local ok_hp, hp = pcall(function() return pawn:GetHealth() end)
    return ok_hp and hp <= 0
end

local function get_projectile_end_safe(source, target)
    local ok, final = pcall(function()
        return GetProjectileEnd(source, target, PATH_PROJECTILE)
    end)
    if ok and final ~= nil then return final end
    return target
end

local function execute_ricochet_native(pawn, wname, skill, first, second)
    local source = pawn:GetSpace()
    local first_dir = GetDirection(first - source)
    local first_tar = get_projectile_end_safe(source, first)
    local second_dir = GetDirection(second - first)
    local second_tar = get_projectile_end_safe(first, second)
    local damage = tonumber(skill.Damage) or 1
    local boosted = pawn_is_boosted(pawn)
    if boosted and damage > 0 then
        damage = damage + 1
    end

    local uid = nil
    local ok_uid, uid_val = pcall(function() return pawn:GetId() end)
    if ok_uid then uid = uid_val end
    local save_data = _read_save_data()
    local save_pilot = uid and save_data.pilots[uid] or nil

    local targets = {
        {point = second_tar, dir = second_dir},
        {point = first_tar, dir = first_dir},
    }
    local killed_enemy = false
    for _, entry in ipairs(targets) do
        local pt = entry.point
        if Board:IsValid(pt) then
            local target_pawn = Board:GetPawn(pt)
            local target_was_enemy = pawn_is_enemy(target_pawn)
            local dmg = damage
            if not skill.AllyDamage and Board:IsPawnTeam(pt, TEAM_PLAYER) then
                dmg = DAMAGE_ZERO
            end
            local sd = SpaceDamage(pt, dmg, entry.dir)
            local ok_dmg, err_dmg = pcall(function() Board:DamageSpace(sd) end)
            if not ok_dmg then
                return false, "Ricochet DamageSpace failed at " ..
                       pt.x .. "," .. pt.y .. ": " .. tostring(err_dmg)
            end
            if target_was_enemy and pawn_is_dead_or_zero(target_pawn) then
                killed_enemy = true
            end
        end
    end

    if boosted or killed_enemy then
        local desired_boosted = false
        if save_pilot and save_pilot.id == "Pilot_Arrogant" then
            local hp = pawn:GetHealth()
            local max_hp = get_pawn_max_health(pawn, uid, save_data)
            desired_boosted = hp >= max_hp
        elseif save_pilot and save_pilot.id == "Pilot_Chemical" and killed_enemy then
            desired_boosted = true
        end
        set_pawn_boosted(pawn, desired_boosted)
    end

    log_bridge("FIRE: " .. wname .. " direct_ricochet " ..
               source.x .. "," .. source.y .. " -> " ..
               first.x .. "," .. first.y .. " -> " ..
               second.x .. "," .. second.y)
    return true, "DamageSpace(" .. wname .. ") first=" ..
           first_tar.x .. "," .. first_tar.y .. " second=" ..
           second_tar.x .. "," .. second_tar.y
end

local function execute_quick_fire_native(pawn, wname, skill, first, second)
    local source = pawn:GetSpace()
    local first_dir = GetDirection(first - source)
    local second_dir = GetDirection(second - source)
    local first_tar = get_projectile_end_safe(source, first)
    local second_tar = get_projectile_end_safe(source, second)
    local damage = tonumber(skill.Damage) or 1
    local boosted = pawn_is_boosted(pawn)
    if boosted and damage > 0 then
        damage = damage + 1
    end

    local uid = nil
    local ok_uid, uid_val = pcall(function() return pawn:GetId() end)
    if ok_uid then uid = uid_val end
    local save_data = _read_save_data()
    local save_pilot = uid and save_data.pilots[uid] or nil

    local killed_enemy = false
    local targets = {
        {point = first_tar, dir = first_dir},
        {point = second_tar, dir = second_dir},
    }
    for _, entry in ipairs(targets) do
        local pt = entry.point
        if Board:IsValid(pt) then
            local target_pawn = Board:GetPawn(pt)
            local target_was_enemy = pawn_is_enemy(target_pawn)
            local sd = SpaceDamage(pt, damage)
            if tonumber(skill.Push) == 1 then
                sd.iPush = entry.dir
            end
            local ok_dmg, err_dmg = pcall(function() Board:DamageSpace(sd) end)
            if not ok_dmg then
                return false, "Quick-Fire DamageSpace failed at " ..
                       pt.x .. "," .. pt.y .. ": " .. tostring(err_dmg)
            end
            if target_was_enemy and pawn_is_dead_or_zero(target_pawn) then
                killed_enemy = true
            end
        end
    end

    if boosted or killed_enemy then
        local desired_boosted = false
        if save_pilot and save_pilot.id == "Pilot_Arrogant" then
            local hp = pawn:GetHealth()
            local max_hp = get_pawn_max_health(pawn, uid, save_data)
            desired_boosted = hp >= max_hp
        elseif save_pilot and save_pilot.id == "Pilot_Chemical" and killed_enemy then
            desired_boosted = true
        end
        set_pawn_boosted(pawn, desired_boosted)
    end

    log_bridge("FIRE: " .. wname .. " direct_quick_fire " ..
               source.x .. "," .. source.y .. " -> " ..
               first.x .. "," .. first.y .. " -> " ..
               second.x .. "," .. second.y)
    return true, "DamageSpace(" .. wname .. ") first=" ..
           first_tar.x .. "," .. first_tar.y .. " second=" ..
           second_tar.x .. "," .. second_tar.y
end

local function execute_two_click_by_slot(pawn, weapon_slot, tx1, ty1, tx2, ty2)
    local wname, _base_wname, slot, err =
        effective_weapon_name_by_slot(pawn, weapon_slot)
    if err ~= nil then
        return false, err
    end
    local skill = _G[wname]
    if not skill then
        return false, "two-click skill missing: " .. tostring(wname)
    end

    local source = pawn:GetSpace()
    local first = Point(tx1, ty1)
    local second = Point(tx2, ty2)
    if not Board:IsValid(first) or not Board:IsValid(second) then
        return false, "two-click target off-board"
    end

    if string.find(wname, "^Brute_TC_DoubleShot") ~= nil then
        local function shot_dir(point)
            local dx = point.x - source.x
            local dy = point.y - source.y
            if dx == 0 and dy > 0 then return 0 end
            if dx > 0 and dy == 0 then return 1 end
            if dx == 0 and dy < 0 then return 2 end
            if dx < 0 and dy == 0 then return 3 end
            return nil
        end
        local first_dir = shot_dir(first)
        local second_dir = shot_dir(second)
        if first_dir == nil then
            return false, "Quick-Fire first target not cardinal " ..
                   first.x .. "," .. first.y .. " from " ..
                   source.x .. "," .. source.y
        end
        if second_dir == nil then
            return false, "Quick-Fire second target not cardinal " ..
                   second.x .. "," .. second.y .. " from " ..
                   source.x .. "," .. source.y
        end
        if first_dir == second_dir then
            return false, "Quick-Fire targets must be in different directions"
        end
        return execute_quick_fire_native(pawn, wname, skill, first, second)
    end

    if string.find(wname, "^Brute_TC_Ricochet") ~= nil then
        if first.x == source.x and first.y == source.y then
            return false, "Ricochet first target is source"
        end
        if first.x ~= source.x and first.y ~= source.y then
            return false, "Ricochet first target not cardinal " ..
                   first.x .. "," .. first.y .. " from " ..
                   source.x .. "," .. source.y
        end
        local first_effect = first
        local ok_translate, translated = pcall(function()
            if type(skill.TranslateFirstClick) == "function" then
                return skill:TranslateFirstClick(source, first)
            end
            return nil
        end)
        if ok_translate and translated ~= nil then
            first_effect = translated
        end
        if second.x ~= first_effect.x and second.y ~= first_effect.y then
            return false, "Ricochet second target not cardinal " ..
                   second.x .. "," .. second.y .. " from " ..
                   first_effect.x .. "," .. first_effect.y
        end
        return execute_ricochet_native(pawn, wname, skill, first, second)
    end

    if string.find(wname, "^Science_TC_Control") ~= nil then
        local target_pawn = Board:GetPawn(first)
        if not target_pawn then
            return false, "Control Shot first click has no pawn at " ..
                   first.x .. "," .. first.y
        end
        local ok_first, first_targets = pcall(function()
            return skill:GetTargetArea(source)
        end)
        if not ok_first then
            return false, "Control Shot GetTargetArea failed: " .. tostring(first_targets)
        end
        if not point_list_contains(first_targets, first) then
            return false, "Control Shot first click is not natively eligible at " ..
                   first.x .. "," .. first.y
        end
        local ok_second, second_targets = pcall(function()
            return skill:GetSecondTargetArea(source, first)
        end)
        if not ok_second then
            return false, "Control Shot GetSecondTargetArea failed: " .. tostring(second_targets)
        end
        if not point_list_contains(second_targets, second) then
            return false, "Control Shot second click is not natively reachable at " ..
                   second.x .. "," .. second.y
        end
        local ok, ctrl_err = pcall(function()
            Board:AddEffect(skill:GetFinalEffect(source, first, second))
        end)
        if not ok then
            return false, "Control Shot GetFinalEffect failed: " .. tostring(ctrl_err)
        end
        log_bridge("FIRE: " .. wname .. " control_shot slot=" .. slot .. " " ..
                   source.x .. "," .. source.y .. " -> " ..
                   first.x .. "," .. first.y .. " -> " ..
                   second.x .. "," .. second.y)
        return true, "GetFinalEffect(" .. wname .. ") first=" ..
               first.x .. "," .. first.y .. " second=" ..
               second.x .. "," .. second.y
    end

    if wname == "Ranged_DeployBomb_A" then
        local function deploy_dir(point)
            local dx = point.x - source.x
            local dy = point.y - source.y
            if dx == 0 and dy > 0 then return 0 end
            if dx > 0 and dy == 0 then return 1 end
            if dx == 0 and dy < 0 then return 2 end
            if dx < 0 and dy == 0 then return 3 end
            return nil
        end
        local function deploy_dist(point)
            return math.abs(point.x - source.x) + math.abs(point.y - source.y)
        end

        local first_dir = deploy_dir(first)
        local second_dir = deploy_dir(second)
        if first_dir == nil then
            return false, "2 Bombs first target not cardinal " ..
                   first.x .. "," .. first.y .. " from " ..
                   source.x .. "," .. source.y
        end
        if second_dir == nil then
            return false, "2 Bombs second target not cardinal " ..
                   second.x .. "," .. second.y .. " from " ..
                   source.x .. "," .. source.y
        end
        if deploy_dist(first) < 2 then
            return false, "2 Bombs first target below min range " ..
                   first.x .. "," .. first.y
        end
        if deploy_dist(second) < 2 then
            return false, "2 Bombs second target below min range " ..
                   second.x .. "," .. second.y
        end
        if first_dir == second_dir then
            return false, "2 Bombs targets must be in different directions"
        end
        if Board:IsBlocked(first, PATH_GROUND) then
            return false, "2 Bombs first target blocked " ..
                   first.x .. "," .. first.y
        end
        if Board:IsBlocked(second, PATH_GROUND) then
            return false, "2 Bombs second target blocked " ..
                   second.x .. "," .. second.y
        end

        local ok, bomb_err = pcall(function()
            Board:AddEffect(skill:GetFinalEffect(source, first, second))
        end)
        if not ok then
            return false, "2 Bombs GetFinalEffect failed: " .. tostring(bomb_err)
        end
        log_bridge("FIRE: " .. wname .. " two_bombs slot=" .. slot .. " " ..
                   source.x .. "," .. source.y .. " -> " ..
                   first.x .. "," .. first.y .. " -> " ..
                   second.x .. "," .. second.y)
        return true, "GetFinalEffect(" .. wname .. ") first=" ..
               first.x .. "," .. first.y .. " second=" ..
               second.x .. "," .. second.y
    end

    if string.find(wname, "^Science_TC_SwapOther") == nil then
        return false, "unsupported two-click weapon " .. tostring(wname)
    end
    local dist = math.abs(first.x - source.x) + math.abs(first.y - source.y)
    if dist ~= 1 then
        return false, "Force Swap first target not adjacent " ..
               first.x .. "," .. first.y .. " from " ..
               source.x .. "," .. source.y
    end
    local first_pawn = Board:GetPawn(first)
    local second_pawn = Board:GetPawn(second)
    if not first_pawn then
        return false, "Force Swap first click has no pawn at " ..
               first.x .. "," .. first.y
    end
    if not second_pawn then
        return false, "Force Swap second click has no pawn at " ..
               second.x .. "," .. second.y
    end
    if first.x == second.x and first.y == second.y then
        return false, "Force Swap targets must be different"
    end
    if pawn_is_guarding(first_pawn) or pawn_is_guarding(second_pawn) then
        return false, "Force Swap target is guarding/stable"
    end

    local ok, fx_err = pcall(function()
        Board:AddEffect(skill:GetFinalEffect(source, first, second))
    end)
    if not ok then
        return false, "Force Swap GetFinalEffect failed: " .. tostring(fx_err)
    end
    log_bridge("FIRE: " .. wname .. " two_click slot=" .. slot .. " " ..
               source.x .. "," .. source.y .. " -> " ..
               first.x .. "," .. first.y .. " -> " ..
               second.x .. "," .. second.y)
    return true, "GetFinalEffect(" .. wname .. ") first=" ..
           first.x .. "," .. first.y .. " second=" ..
           second.x .. "," .. second.y
end

-- execute_weapon_by_slot: fire weapon using a 0-based slot index from
-- the Python side (maps to 1-indexed Lua SkillList).
-- This avoids name-matching issues where the solver's weapon ID doesn't
-- match the pawn type's SkillList entry (e.g. purchased / upgraded weapons,
-- or names the Rust solver doesn't recognise → "Unknown").
local function execute_weapon_by_slot(pawn, weapon_slot, tx, ty)
    -- weapon_slot is 0-based from Python; Lua SkillList is 1-indexed
    local wname, base_wname, slot, err, wsource, pawn_def, uid =
        effective_weapon_name_by_slot(pawn, weapon_slot)
    if err ~= nil then
        return false, err
    end
    local restore_skill_list = false
    if wname ~= base_wname then
        restore_skill_list = true
        pawn_def.SkillList[slot] = wname
        log_bridge("EFFECTIVE_WEAPON: uid=" .. tostring(uid) ..
                   " slot=" .. slot .. " " .. tostring(base_wname) ..
                   " -> " .. tostring(wname) .. " source=" .. tostring(wsource))
    end
    local source = pawn:GetSpace()
    -- pawn:FireWeapon() applies Seismic Capacitor's damage through the file
    -- bridge, but some engine builds omit its DIR_FLIP retarget side effect.
    -- Snapshot a live queued enemy now so we can add only the missing flip
    -- after FireWeapon returns.  GetQueuedShot is the same C++ live probe used
    -- by state extraction to override save-stale piQueuedShot values.
    -- NOTE: Pawn has no GetQueuedShot (verified live 2026-10-10), so the
    -- probe always fails and this legacy-mode fallback never fires.
    local seismic_flip_before = nil
    if string.find(wname, "^Science_KO_Crack") ~= nil then
        local target = Board:GetPawn(Point(tx, ty))
        if target ~= nil and not target:IsDead() and target:GetTeam() == TEAM_ENEMY then
            local ok_id, target_id = pcall(function() return target:GetId() end)
            local ok_qs, queued = pcall(function() return target:GetQueuedShot() end)
            if ok_id and ok_qs and queued ~= nil
                    and type(queued.x) == "number" and type(queued.y) == "number"
                    and queued.x >= 0 and queued.y >= 0
                    and queued.x <= 7 and queued.y <= 7 then
                seismic_flip_before = {
                    id = target_id,
                    x = queued.x,
                    y = queued.y,
                }
            end
        end
    end
    if string.find(wname, "^Prime_TC_Punt") ~= nil then
        local ok_punt, method = execute_prime_tc_punt(pawn, wname, tx, ty)
        if restore_skill_list then
            pawn_def.SkillList[slot] = base_wname
        end
        if not ok_punt then
            log_bridge("WARN: Prime_TC_Punt failed for slot " .. slot ..
                       " (" .. tostring(wname) .. "): " .. tostring(method))
            return false, method
        end
        return true, method
    end

    local is_transit_leap =
        string.find(wname, "^Brute_Jetmech") ~= nil or
        string.find(wname, "^Brute_Bombrun") ~= nil
    local skill = is_transit_leap and _G[wname] or nil
    local transit_before = nil
    if skill and skill.Damage and skill.Damage > 0 then
        local dx = tx - source.x
        local dy = ty - source.y
        local dist = math.abs(dx) + math.abs(dy)
        if dist >= 2 and (dx == 0 or dy == 0) then
            local sx = dx == 0 and 0 or (dx / math.abs(dx))
            local sy = dy == 0 and 0 or (dy / math.abs(dy))
            transit_before = {sx = sx, sy = sy, dist = dist, tiles = {}}
            for k = 1, dist - 1 do
                local tp = Point(source.x + sx * k, source.y + sy * k)
                transit_before.tiles[k] = tile_damage_snapshot(tp)
            end
        end
    end
    local ok, fired_or_err = pcall(function()
        return pawn:FireWeapon(Point(tx, ty), slot)
    end)
    if restore_skill_list then
        pawn_def.SkillList[slot] = base_wname
    end
    if not ok then
        log_bridge("WARN: FireWeapon failed for slot " .. slot ..
                   " (" .. wname .. "): " .. tostring(fired_or_err))
        return false, "FireWeapon failed: " .. tostring(fired_or_err)
    end
    if fired_or_err == false or fired_or_err == 0 then
        log_bridge("WARN: FireWeapon returned " .. tostring(fired_or_err) .. " for slot " .. slot ..
                   " (" .. wname .. ")")
        return false, "FireWeapon returned " .. tostring(fired_or_err) .. " for slot " .. tostring(slot) ..
               " (" .. tostring(wname) .. ")"
    end
    log_bridge("FIRE: " .. wname .. " slot=" .. slot .. " " ..
               source.x .. "," .. source.y .. " -> " .. tx .. "," .. ty)

    if seismic_flip_before ~= nil then
        local target = Board:GetPawn(Point(tx, ty))
        local same_target = false
        if target ~= nil and not target:IsDead() and target:GetTeam() == TEAM_ENEMY then
            local ok_id, target_id = pcall(function() return target:GetId() end)
            same_target = ok_id and target_id == seismic_flip_before.id
        end
        if same_target then
            local ok_qs, queued = pcall(function() return target:GetQueuedShot() end)
            local unchanged = ok_qs and queued ~= nil
                and queued.x == seismic_flip_before.x
                and queued.y == seismic_flip_before.y
            if unchanged then
                local flip = SpaceDamage(Point(tx, ty), 0)
                flip.iPush = DIR_FLIP
                local ok_flip, flip_err = pcall(function()
                    Board:DamageSpace(flip)
                end)
                if ok_flip then
                    log_bridge(string.format(
                        "SEISMIC_FLIP_FALLBACK: uid=%s queued=(%d,%d)",
                        tostring(seismic_flip_before.id),
                        seismic_flip_before.x, seismic_flip_before.y))
                else
                    log_bridge("WARN: Seismic DIR_FLIP fallback failed for uid=" ..
                               tostring(seismic_flip_before.id) .. ": " ..
                               tostring(flip_err))
                end
            else
                log_bridge("SEISMIC_FLIP_ENGINE: uid=" ..
                           tostring(seismic_flip_before.id))
            end
        end
    end

    -- Transit-damage workaround for Brute_Jetmech (Aerial Bombs) and
    -- Brute_Bombrun (Bombing Run). The game's weapons_brute.lua
    -- Brute_Jetmech:GetSkillEffect (and Brute_Bombrun which inherits
    -- from it) loops k=1..Range-1 and calls Board:DamageSpace with
    -- damage + iSmoke on each transit tile. pawn:FireWeapon() dispatches
    -- the leap movement but does NOT execute that Lua script —
    -- 5/5 snapshots (grid_drop_20260421_211617_239_t02_a0,
    -- _20260421_215501_106_t02_a0, _20260423_131700_144_t01_a0,
    -- _20260424_144237_364_t01_a1, plus t03_a0 in the 211617 run) show
    -- transit tiles at predicted HP-1 vs actual HP unchanged, and zero
    -- smoke tiles in all five actual boards. Replicate the game's own
    -- GetSkillEffect loop here, pulling skill.Damage / skill.Smoke off
    -- the live Lua skill so weapon upgrades (Jetmech_A Damage=2,
    -- Bombrun_B Damage=3) flow through automatically.
    --
    -- NOT using skill:GetSkillEffect + Board:AddEffect: the comment at
    -- the top of this section records that path leaves Board:IsBusy()
    -- stuck true and broke the engine queue.
    if is_transit_leap then
        skill = skill or _G[wname]
        if skill and skill.Damage and skill.Damage > 0 then
            local dx = tx - source.x
            local dy = ty - source.y
            local dist = math.abs(dx) + math.abs(dy)
            -- Cardinal-only (leap enumerator already guarantees this,
            -- but guard defensively).
            if dist >= 2 and (dx == 0 or dy == 0) then
                local sx = dx == 0 and 0 or (dx / math.abs(dx))
                local sy = dy == 0 and 0 or (dy / math.abs(dy))
                local dmg_applied = 0
                local smoke_applied = 0
                local engine_tile_damage_seen = 0
                for k = 1, dist - 1 do
                    local nx = source.x + sx * k
                    local ny = source.y + sy * k
                    local tp = Point(nx, ny)
                    -- FireWeapon DOES apply transit damage to any unit
                    -- standing on the transit tile (observed 2026-04-24
                    -- turn 1 and turn 2: acid-statused Firefly1 on
                    -- transit died after 1 of our manual + 1 of the
                    -- engine's damage, instead of surviving at HP=1).
                    -- Some engine paths do apply transit terrain damage
                    -- during FireWeapon, while older captures showed no
                    -- terrain damage. Snapshot-before / compare-after keeps
                    -- the workaround adaptive: apply synthetic damage only
                    -- when FireWeapon left the tile's terrain state unchanged,
                    -- and always apply smoke/acid via SpaceDamage.
                    local occupant = Board:GetPawn(tp)
                    local has_live = occupant ~= nil and not occupant:IsDead()
                    local before = nil
                    if transit_before and transit_before.tiles then
                        before = transit_before.tiles[k]
                    end
                    local engine_changed_tile = tile_damage_changed(before, tp)
                    if engine_changed_tile then
                        engine_tile_damage_seen = engine_tile_damage_seen + 1
                    end
                    local dmg_val = (has_live or engine_changed_tile) and 0 or skill.Damage
                    local smoke_val = skill.Smoke or 0
                    local acid_val = skill.Acid or 0
                    local used_direct_status = false
                    local ok_d = false
                    local err_d = nil
                    if dmg_val == 0 and has_live and smoke_val > 0 and acid_val == 0 then
                        -- Use a real SpaceDamage payload even for occupied
                        -- transit tiles. Direct Board:SetSmoke paints the
                        -- tile but can leave already-queued Vek attacks live;
                        -- DamageSpace(iSmoke) follows the weapon/status path
                        -- that attack cancellation code observes.
                        local sd = SpaceDamage(tp, 0)
                        sd.iSmoke = smoke_val
                        ok_d, err_d = pcall(function()
                            Board:DamageSpace(sd)
                        end)
                        used_direct_status = ok_d
                    end
                    if not used_direct_status then
                        local sd = SpaceDamage(tp, dmg_val)
                        if smoke_val > 0 then
                            sd.iSmoke = smoke_val
                        end
                        if acid_val > 0 then
                            sd.iAcid = acid_val
                        end
                        ok_d, err_d = pcall(function()
                            Board:DamageSpace(sd)
                        end)
                    end
                    if ok_d then
                        if dmg_val > 0 then dmg_applied = dmg_applied + 1 end
                        if smoke_val > 0 then
                            smoke_applied = smoke_applied + 1
                        end
                    else
                        log_bridge("WARN: transit DamageSpace failed at (" ..
                                   nx .. "," .. ny .. ") for " .. wname ..
                                   ": " .. tostring(err_d))
                    end
                end
                log_bridge("TRANSIT: " .. wname ..
                           " dmg_applied=" .. dmg_applied ..
                           " smoke_applied=" .. smoke_applied ..
                           " engine_tile_damage_seen=" .. engine_tile_damage_seen ..
                           " damage=" .. skill.Damage ..
                           " smoke=" .. tostring(skill.Smoke or 0))
            end
        end
    end

    return true, "FireWeapon[" .. slot .. "](" .. wname .. ")"
end

--------------------------------------------------------------------
-- Native execution (EXEC_MODE native, the default; live_play uses it)
--------------------------------------------------------------------
-- Every player action is Pawn:FireWeapon(target, slot), the call a click
-- makes (SkillManager::FireWeapon @008512a0: ArmWeapon, SetTarget, which
-- runs the weapon's Lua GetTargetArea and GetSkillEffect, then FireInstant
-- applies the whole effect; Pawn::FireWeapon @0087a250 then does the
-- bookkeeping: moved, active, Boost, Shifty / Double_Shot / Post_Move).
-- Native slots: 0 Move, 1.. the weapons (the callers' 0-based slot + 1),
-- 50 the repair skill. It returns 1 fired, 2 first click of a two-click
-- weapon taken, 3 two-click with no valid second target, 0 nothing fired
-- (frozen, the target is not in the weapon's target area, not ready).
--
-- Selection: a click selects the pawn first (Pawn::SetSelected -> the Lua
-- SetPawn of global.lua), and weapon scripts read the global `Pawn`: the
-- Move skill (move speed, path profile), Brute_Jetmech / Leap_Attack
-- GetTargetArea (Pawn:GetPathProf()), Science_KO_Crack (Board:IsDeadly(_,
-- Pawn)). The bridge does not click, so without ITBX.select_pawn `Pawn` is
-- whatever was selected last, usually the last Vek the AI planned. Live
-- 2026-10-09, Mission_Barrels turn 4: Aerial Bombs (4,4) -> water (2,4)
-- with `Pawn` a ground Vek: IsBlocked(water, PATH_GROUND) dropped (2,4)
-- from the target area, FireWeapon returned 0, the Jet stayed, and the old
-- path (which only rejected `false`) acked OK after its TRANSIT emulation
-- smoked (3,4).
--
-- No emulation here: no synthetic damage, smoke, flips, HP or Boost edits.

function ITBX.select_pawn(pawn)
    if type(SetPawn) == "function" then
        SetPawn(pawn)
    else
        Pawn = pawn
    end
end

function ITBX.pawn_busy(pawn)
    if pawn == nil then return false end
    local ok, busy = pcall(function() return pawn:IsBusy() end)
    return ok and busy == true
end

-- What is busy, for error acks.
function ITBX.busy_text(pawn)
    local s = ITBX.busy_state()
    return "board busy_state=" .. tostring(s) .. ", pawn busy=" .. tostring(ITBX.pawn_busy(pawn))
end

-- Waits (yielding frames) until the board and `pawn` (optional) are idle on
-- two polls in a row. `settle_frames` frames are yielded first, so an effect
-- the action just queued has started. Returns true, or false on timeout.
function ITBX.wait_idle_coro(pawn, max_wait, settle_frames)
    for _ = 1, (settle_frames or 0) do
        ITBX.cmd_waiting = true
        coroutine.yield()
    end
    local idle_polls = 0
    return wait_until_coro(function()
        if Board:IsBusy() or ITBX.pawn_busy(pawn) then
            idle_polls = 0
            return false
        end
        idle_polls = idle_polls + 1
        return idle_polls >= 2
    end, max_wait or 15)
end

-- Before an action: the board and the actor must be idle (a short grace
-- for the tail of the previous action). Returns true, or false + reason.
function ITBX.require_idle_coro(pawn, grace)
    if ITBX.wait_idle_coro(pawn, grace or 3, 0) then return true end
    return false, "BUSY: " .. ITBX.busy_text(pawn) .. "; nothing sent to the game"
end

-- After an action: wait for its effects. Returns true, or false + reason.
function ITBX.settle_coro(pawn, what, max_wait)
    if ITBX.wait_idle_coro(pawn, max_wait or 15, 1) then return true end
    return false, "TIMEOUT: " .. what .. " was sent but the game is still busy after "
        .. tostring(max_wait or 15) .. "s (" .. ITBX.busy_text(pawn) .. ")"
end

local NATIVE_RET = {
    [0] = "0: nothing fired (frozen, target outside the weapon's target area, or not ready)",
    [2] = "2: first click taken, waiting for a second click",
    [3] = "3: two-click weapon with no valid second target",
}

-- Pawn:FireWeapon with the actor selected. Returns the int result, or nil
-- and the error.
function ITBX.native_fire(pawn, native_slot, pt)
    ITBX.select_pawn(pawn)
    local ok, ret = pcall(function() return pawn:FireWeapon(pt, native_slot) end)
    if not ok then return nil, ITBX.str(ret) end
    return ret
end

local function native_ret_text(ret)
    return NATIVE_RET[ret] or tostring(ret)
end

-- The weapon in 0-based slot `weapon_slot` (name for logs and the two-click
-- check only; FireWeapon fires the pawn's own native skill).
local function native_weapon(pawn, weapon_slot)
    local ok, wname = pcall(effective_weapon_name_by_slot, pawn, weapon_slot)
    if ok and type(wname) == "string" then return wname, rawget(_G, wname) or _G[wname] end
    return "slot" .. tostring(weapon_slot), nil
end

-- true when the game would take (source -> target) as the first click of a
-- two-click shot (Skill::NeedsSecondClick: TwoClick and not an exception).
local function needs_second_click(skill, source, target)
    if type(skill) ~= "table" or skill.TwoClick ~= true then return false end
    local ok, exc = pcall(function()
        if type(skill.IsTwoClickException) ~= "function" then return false end
        return skill:IsTwoClickException(source, target)
    end)
    return not (ok and exc == true)
end

-- MOVE: the Move skill only (no Pawn:Move / SetSpace fallback).
function ITBX.native_move_strict(pawn, pt)
    local ret, err = ITBX.native_fire(pawn, 0, pt)
    if ret == nil then return false, "FireWeapon[0] failed: " .. tostring(err) end
    if ret ~= 1 then
        return false, "FireWeapon[0] returned " .. native_ret_text(ret)
            .. " (the tile is not a legal move for this pawn now)"
    end
    return true, "FireWeapon[0]"
end

function ITBX.native_attack(pawn, weapon_slot, tx, ty)
    local slot = weapon_slot + 1
    local wname, skill = native_weapon(pawn, weapon_slot)
    local target = Point(tx, ty)
    if needs_second_click(skill, pawn:GetSpace(), target) then
        return false, wname .. " is a two-click weapon: use TWO_CLICK_ATTACK; nothing fired"
    end
    local ret, err = ITBX.native_fire(pawn, slot, target)
    if ret == nil then
        log_bridge("WARN: FireWeapon failed for slot " .. slot .. " (" .. wname .. "): " .. tostring(err))
        return false, "FireWeapon[" .. slot .. "](" .. wname .. ") failed: " .. tostring(err)
    end
    if ret ~= 1 then
        log_bridge("WARN: FireWeapon[" .. slot .. "](" .. wname .. ") returned " .. tostring(ret))
        return false, "FireWeapon[" .. slot .. "](" .. wname .. ") returned " .. native_ret_text(ret)
    end
    log_bridge("FIRE: " .. wname .. " native slot=" .. slot .. " -> " .. tx .. "," .. ty)
    return true, "FireWeapon[" .. slot .. "](" .. wname .. ")"
end

function ITBX.native_two_click(pawn, weapon_slot, tx1, ty1, tx2, ty2)
    local slot = weapon_slot + 1
    local wname, skill = native_weapon(pawn, weapon_slot)
    local first, second = Point(tx1, ty1), Point(tx2, ty2)
    if not needs_second_click(skill, pawn:GetSpace(), first) then
        return false, wname .. " takes this shot in one click: use ATTACK; nothing fired"
    end
    local ret, err = ITBX.native_fire(pawn, slot, first)
    if ret == nil then return false, "FireWeapon[" .. slot .. "](" .. wname .. ") first click failed: " .. tostring(err) end
    if ret ~= 2 then
        return false, "FireWeapon[" .. slot .. "](" .. wname .. ") first click returned " .. native_ret_text(ret)
            .. (ret == 1 and " (it fired)" or "")
    end
    -- Same slot: ArmWeapon keeps the armed weapon and its first click.
    ret, err = ITBX.native_fire(pawn, slot, second)
    if ret == nil then return false, "FireWeapon[" .. slot .. "](" .. wname .. ") second click failed: " .. tostring(err) end
    if ret ~= 1 then
        return false, "FireWeapon[" .. slot .. "](" .. wname .. ") second click returned " .. native_ret_text(ret)
            .. " (the first click stays armed in the game)"
    end
    log_bridge("FIRE: " .. wname .. " native two_click slot=" .. slot .. " -> "
        .. tx1 .. "," .. ty1 .. " -> " .. tx2 .. "," .. ty2)
    return true, "FireWeapon[" .. slot .. "](" .. wname .. ") x2"
end

-- REPAIR: the pawn's repair skill (native slot 50; pilot and passive
-- variants such as Mass_Repair are the game's).
function ITBX.native_repair(pawn)
    local ret, err = ITBX.native_fire(pawn, 50, pawn:GetSpace())
    if ret == nil then return false, "FireWeapon[50] failed: " .. tostring(err) end
    if ret ~= 1 then return false, "FireWeapon[50] (repair) returned " .. native_ret_text(ret) end
    return true, "FireWeapon[50]"
end

function ITBX.legacy_mode()
    return ITBX.exec_mode == "legacy"
end

--------------------------------------------------------------------
-- Command executor
--------------------------------------------------------------------
--------------------------------------------------------------------
-- Bridge extension: SCENARIO (debug only)
--------------------------------------------------------------------
-- Builds a test board during the player's turn with the game's own Lua
-- bindings; the state dump that follows is the engine's input. Spec (JSON,
-- bridge coordinates, x/y in 0..7):
--   name           label for logs and snapshot files
--   clear_pawns    "all" (default: every non-mech pawn), "enemies" (team 6
--                  only) or false
--   remove         [uid, ...] extra pawns to remove (mechs refused)
--   clear_spawns   true: drop every queued spawn (ClearSpace on its tile,
--                  which also resets that tile to ground)
--   clear_tiles    true: every tile not listed in `tiles` becomes plain
--                  ground (no fire, smoke, acid or crack; items stay)
--   tiles          [{x, y, terrain, hp, max_hp, populated, fire, smoke,
--                  acid, cracked, frozen, shield, item, custom}]
--                  terrain: ground|building|rubble|water|mountain|ice|
--                  forest|sand|chasm|lava or a terrain id
--   pawns          [{uid | type, team, x, y, hp, move, fire, acid, shield,
--                  frozen, boosted, infected, powered, neutral, active,
--                  mutation, queue: {x, y, slot}, clear_queue}]
--                  `uid` moves an existing pawn (mechs included); `type`
--                  creates one (PAWN_FACTORY:CreatePawn + Board:AddPawn) in
--                  list order, which is the order Vek attack in. `queue`
--                  fires weapon `slot` (default 1, the first weapon) at the
--                  tile with Pawn:FireWeapon, the call the AI makes: the
--                  attack is queued and its instant part, if any, applied.
--   spawns         [{type, x, y}] queued emerging Vek (Board:SpawnPawn)
-- Fire on a pawn is set with a 0-damage SpaceDamage (iFire); if its tile
-- must not burn, the pawn steps to a free tile while the tile fire is put
-- out, then steps back.

local function itbx_terrain_id(name)
    if type(name) == "number" then return name end
    local map = {
        ground = _G.TERRAIN_ROAD or 0, road = _G.TERRAIN_ROAD or 0,
        building = _G.TERRAIN_BUILDING or 1, rubble = _G.TERRAIN_RUBBLE or 2,
        water = _G.TERRAIN_WATER or 3, mountain = _G.TERRAIN_MOUNTAIN or 4,
        ice = _G.TERRAIN_ICE or 5, forest = _G.TERRAIN_FOREST or 6,
        sand = _G.TERRAIN_SAND or 7, chasm = _G.TERRAIN_HOLE or 9,
        hole = _G.TERRAIN_HOLE or 9,
    }
    return map[name]
end

local function itbx_space_fire(pt, create)
    local sd = SpaceDamage(pt, 0)
    sd.iFire = create and (_G.EFFECT_CREATE or 1) or (_G.EFFECT_REMOVE or 2)
    Board:DamageSpace(sd)
end

function ITBX.scenario_tile(t)
    local pt = Point(t.x, t.y)
    if t.terrain ~= nil then
        if t.terrain == "lava" then
            Board:SetLava(pt, true)
        else
            local id = itbx_terrain_id(t.terrain)
            if id == nil then error("unknown terrain " .. tostring(t.terrain)) end
            Board:SetTerrain(pt, id)
        end
    end
    if type(t.hp) == "number" then
        local max = t.max_hp
        if type(max) ~= "number" then
            local terr = Board:GetTerrain(pt)
            if terr == (_G.TERRAIN_MOUNTAIN or 4) or terr == (_G.TERRAIN_ICE or 5) then
                max = 2
            else
                max = math.max(t.hp, 1)
            end
        end
        Board:SetHealth(pt, t.hp, max)
    end
    if type(t.populated) == "boolean" then Board:SetPopulated(t.populated, pt) end
    if type(t.cracked) == "boolean" then Board:SetCracked(pt, t.cracked) end
    if type(t.acid) == "boolean" then Board:SetAcid(pt, t.acid) end
    if type(t.smoke) == "boolean" then Board:SetSmoke(pt, t.smoke, true) end
    if t.fire == true then
        Board:SetTerrain(pt, _G.TERRAIN_FIRE or 11)
    elseif t.fire == false and Board:IsFire(pt) then
        itbx_space_fire(pt, false)
    end
    if type(t.frozen) == "boolean" then Board:SetFrozen(pt, t.frozen) end
    if t.shield == true then
        Board:AddShield(pt)
    elseif t.shield == false then
        Board:RemoveShield(pt)
    end
    if type(t.item) == "string" then Board:SetItem(pt, t.item) end
    if type(t.custom) == "string" then Board:SetCustomTile(pt, t.custom) end
end

function ITBX.scenario_reset_tile(pt)
    Board:SetTerrain(pt, _G.TERRAIN_ROAD or 0)
    if Board:IsFire(pt) then itbx_space_fire(pt, false) end
    if Board:IsSmoke(pt) then Board:SetSmoke(pt, false, true) end
    if Board:IsAcid(pt) then Board:SetAcid(pt, false) end
    if Board:IsCracked(pt) then Board:SetCracked(pt, false) end
end

-- A free plain tile to park a pawn on for a moment.
local function itbx_staging_tile(avoid)
    for y = 0, 7 do
        for x = 0, 7 do
            local pt = Point(x, y)
            if not (avoid and avoid[x .. "," .. y])
                    and Board:GetTerrain(pt) == (_G.TERRAIN_ROAD or 0)
                    and not Board:IsPawnSpace(pt)
                    and not Board:IsFire(pt) and not Board:IsSpawning(pt)
                    and not Board:IsItem(pt) and not Board:IsPod(pt)
                    and not Board:IsAcid(pt) then
                return pt
            end
        end
    end
    return nil
end

-- Statuses other than fire (scenario_apply sets fire first: it needs
-- waits, and a coroutine cannot yield across pcall).
function ITBX.scenario_pawn_status(p, ps, report)
    local where = "pawn " .. tostring(ps.uid or ps.type)
    local function step(label, fn)
        local ok, err = pcall(fn)
        if not ok then
            report.errors[#report.errors + 1] = where .. " " .. label .. ": " .. ITBX.str(err)
        end
    end
    if type(ps.hp) == "number" then step("hp", function() p:SetHealth(ps.hp) end) end
    if type(ps.move) == "number" then step("move", function() p:SetMoveSpeed(ps.move) end) end
    if type(ps.acid) == "boolean" then step("acid", function() p:SetAcid(ps.acid) end) end
    if ps.injured == true then
        -- AE Injured: a 0-damage SpaceDamage with iInjure (as weapons set it).
        step("injured", function()
            local sd = SpaceDamage(p:GetSpace(), 0)
            sd.iInjure = _G.EFFECT_CREATE or 1
            Board:DamageSpace(sd)
        end)
    end
    if type(ps.boosted) == "boolean" then step("boosted", function() p:SetBoosted(ps.boosted) end) end
    if type(ps.infected) == "boolean" then step("infected", function() p:SetInfected(ps.infected) end) end
    if type(ps.neutral) == "boolean" then step("neutral", function() p:SetNeutral(ps.neutral) end) end
    if type(ps.mutation) == "number" then step("mutation", function() p:SetMutation(ps.mutation) end) end
    if type(ps.shield) == "boolean" then step("shield", function() p:SetShield(ps.shield) end) end
    if type(ps.frozen) == "boolean" then step("frozen", function() p:SetFrozen(ps.frozen) end) end
    if type(ps.powered) == "boolean" then step("powered", function() p:SetPowered(ps.powered) end) end
    if type(ps.active) == "boolean" then step("active", function() p:SetActive(ps.active) end) end
end

-- Runs inside the command coroutine and waits for an idle board between
-- stages, so it must be called directly (never under pcall). Every board
-- call sits in its own pcall'd step. Returns the report, or nil and an
-- error message when the preconditions fail.
function ITBX.scenario_apply(spec, wait_idle)
    if type(spec) ~= "table" then return nil, "scenario must be a JSON object" end
    local mission = _ITB_CURRENT_MISSION
    if not Board or mission == nil then return nil, "no active mission" end
    local turn, team = itbx_turn_team()
    if team ~= TEAM_PLAYER then
        return nil, "not the player turn (team " .. tostring(team) .. ")"
    end
    local function wait() wait_idle(5) end
    wait()

    local report = {name = spec.name or "scenario", errors = {}, created = {},
                    removed = {}, queued = {}}
    local function step(label, fn)
        local ok, err = pcall(fn)
        if not ok then
            report.errors[#report.errors + 1] = label .. ": " .. ITBX.str(err)
        end
        return ok
    end
    local st = {mission = mission, turn = turn, name = report.name, queued = {},
                errors = report.errors, created = report.created, spawn_queue = {}}
    step("read save", function()
        local save = ITBX.parse_save_cached("saveData.lua", _read_save_data().raw_content)
        local key = ITBX.mission_key(mission)
        if save.spawns and (key == nil or save.mission == ("Mission" .. tostring(key))) then
            for _, s in ipairs(save.spawns) do st.spawn_queue[#st.spawn_queue + 1] = s end
        end
    end)

    -- 1. Pawns out.
    local clear = spec.clear_pawns
    if clear == nil then clear = "all" end
    step("clear_pawns", function()
        if clear == false then return end
        local ids = extract_table(Board:GetPawns(TEAM_ANY))
        for _, id in ipairs(ids) do
            local p = Board:GetPawn(id)
            if p and not p:IsMech() then
                local remove = clear == "all" or clear == true
                    or (clear == "enemies" and p:GetTeam() == TEAM_ENEMY)
                if remove and step("remove " .. id, function() Board:RemovePawn(p) end) then
                    report.removed[#report.removed + 1] = id
                end
            end
        end
    end)
    for _, id in ipairs(spec.remove or {}) do
        step("remove " .. tostring(id), function()
            local p = Board:GetPawn(id)
            if p == nil then error("no such pawn") end
            if p:IsMech() then error("refusing to remove a mech") end
            Board:RemovePawn(p)
            report.removed[#report.removed + 1] = id
        end)
    end
    wait()

    -- 2. Spawns out.
    if spec.clear_spawns then
        local kept = {}
        step("clear_spawns", function()
            for y = 0, 7 do
                for x = 0, 7 do
                    local pt = Point(x, y)
                    if Board:IsSpawning(pt) then
                        if Board:IsPawnSpace(pt) then
                            report.errors[#report.errors + 1] =
                                "clear_spawns: (" .. x .. "," .. y .. ") is occupied, kept"
                            kept[x .. "," .. y] = true
                        else
                            step("clear_spawn " .. x .. "," .. y, function() Board:ClearSpace(pt) end)
                        end
                    end
                end
            end
        end)
        local queue = {}
        for _, s in ipairs(st.spawn_queue) do
            if kept[tostring(s.x) .. "," .. tostring(s.y)] then queue[#queue + 1] = s end
        end
        st.spawn_queue = queue
        wait()
    end

    -- 3. Tiles.
    local listed, tile_fire = {}, {}
    for _, t in ipairs(spec.tiles or {}) do
        if type(t.x) == "number" and type(t.y) == "number" then
            listed[t.x .. "," .. t.y] = true
            if t.fire == true then tile_fire[t.x .. "," .. t.y] = true end
        end
    end
    if spec.clear_tiles then
        for y = 0, 7 do
            for x = 0, 7 do
                if not listed[x .. "," .. y] then
                    step("reset_tile " .. x .. "," .. y, function()
                        ITBX.scenario_reset_tile(Point(x, y))
                    end)
                end
            end
        end
    else
        step("scan fire", function()
            for y = 0, 7 do
                for x = 0, 7 do
                    if not listed[x .. "," .. y] and Board:IsFire(Point(x, y)) then
                        tile_fire[x .. "," .. y] = true
                    end
                end
            end
        end)
    end
    for _, t in ipairs(spec.tiles or {}) do
        step("tile " .. tostring(t.x) .. "," .. tostring(t.y), function() ITBX.scenario_tile(t) end)
    end
    wait()

    -- 4. Pawns in (new pawns in list order = board-list order).
    local placed = {}
    for i, ps in ipairs(spec.pawns or {}) do
        step("pawn " .. i, function()
            local pt = nil
            if type(ps.x) == "number" and type(ps.y) == "number" then pt = Point(ps.x, ps.y) end
            local p = nil
            if type(ps.uid) == "number" or type(ps.ref) == "number" then
                if type(ps.ref) == "number" then
                    -- An entry earlier in this list (1-based).
                    p = placed[ps.ref]
                    if p == nil then error("ref " .. ps.ref .. " is not placed (yet)") end
                else
                    p = Board:GetPawn(ps.uid)
                    if p == nil then error("no pawn " .. ps.uid) end
                end
                local sp = p:GetSpace()
                if ps.readd then
                    -- Off the board and back: Board::AddPawn appends it to
                    -- its list group (the order re-added Vek attack in).
                    if p:IsMech() then error("refusing to re-add a mech") end
                    local dest = pt or Point(sp.x, sp.y)
                    Board:RemovePawn(p)
                    if Board:IsPawnSpace(dest) then error("tile occupied") end
                    Board:AddPawn(p, dest)
                elseif pt and (sp.x ~= ps.x or sp.y ~= ps.y) then
                    if Board:IsPawnSpace(pt) then error("tile occupied") end
                    p:SetSpace(pt)
                end
                if type(ps.uid) == "number" and not p:IsMech() and ps.queue == nil
                        and ps.clear_queue ~= false then
                    p:ClearQueued()
                    st.queued[ps.uid] = false
                end
            else
                if type(ps.type) ~= "string" then error("needs uid or type") end
                if pt == nil then error("new pawn needs x, y") end
                if Board:IsPawnSpace(pt) then error("tile occupied") end
                if type(ps.team) == "number" and ps.team ~= (_G.TEAM_NONE or 2) then
                    p = PAWN_FACTORY:CreatePawn(ps.type, ps.team)
                else
                    p = PAWN_FACTORY:CreatePawn(ps.type)
                end
                if p == nil then error("CreatePawn returned nil") end
                -- Extra weapons (SkillManager::AddWeapon: native slots after
                -- the type's own; Move is slot 0). New pawns only: a squad
                -- mech would keep the weapon for the rest of the run.
                for _, w in ipairs(ps.weapons_add or {}) do p:AddWeapon(w) end
                Board:AddPawn(p, pt)
                report.created[#report.created + 1] = {index = i, uid = p:GetId(), type = ps.type}
            end
            placed[i] = p
        end)
    end
    wait()

    -- 5. Fire on pawns, then every other status.
    local unburn = {}
    for i, ps in ipairs(spec.pawns or {}) do
        local p = placed[i]
        if p and ps.fire ~= nil then
            step("pawn " .. i .. " fire", function()
                local pos = p:GetSpace()
                itbx_space_fire(pos, ps.fire == true)
                if ps.fire == true and not tile_fire[pos.x .. "," .. pos.y] then
                    unburn[#unburn + 1] = {p = p, x = pos.x, y = pos.y, i = i}
                end
            end)
        end
    end
    wait()
    for _, u in ipairs(unburn) do
        local moved = step("pawn " .. u.i .. " step aside", function()
            local stage = itbx_staging_tile({[u.x .. "," .. u.y] = true})
            if stage == nil then error("no free tile to step aside to") end
            u.p:SetSpace(stage)
            itbx_space_fire(Point(u.x, u.y), false)
        end)
        wait()
        if moved then
            step("pawn " .. u.i .. " step back", function() u.p:SetSpace(Point(u.x, u.y)) end)
            wait()
        end
    end
    for i, ps in ipairs(spec.pawns or {}) do
        local p = placed[i]
        if p then ITBX.scenario_pawn_status(p, ps, report) end
    end
    wait()

    -- 6. Queued attacks.
    for i, ps in ipairs(spec.pawns or {}) do
        local p = placed[i]
        if p and type(ps.queue) == "table" then
            step("queue " .. i, function()
                local slot = ps.queue.slot or 1
                local origin = p:GetSpace()
                pcall(function() p:SetActive(true) end)
                local ret = p:FireWeapon(Point(ps.queue.x, ps.queue.y), slot)
                local id = p:GetId()
                report.queued[#report.queued + 1] = {uid = id, ret = ret}
                if ret == 0 or ret == false then error("FireWeapon returned " .. tostring(ret)) end
                st.queued[id] = {target = {ps.queue.x, ps.queue.y},
                                 origin = {origin.x, origin.y}, slot = slot}
            end)
            wait()
        elseif p and ps.clear_queue == true then
            step("clear_queue " .. i, function()
                p:ClearQueued()
                st.queued[p:GetId()] = false
            end)
        end
    end

    -- 7. Tiles changed under the pawns already standing there.
    for _, t in ipairs(spec.tiles_after or {}) do
        step("tile_after " .. tostring(t.x) .. "," .. tostring(t.y), function() ITBX.scenario_tile(t) end)
    end
    wait()

    -- 8. Spawns in (appended to the queue, as Board::QueuePawn does).
    for i, s in ipairs(spec.spawns or {}) do
        step("spawn " .. i, function()
            local id = Board:SpawnPawn(s.type, Point(s.x, s.y))
            st.spawn_queue[#st.spawn_queue + 1] = {type = s.type, uid = id or -1, x = s.x, y = s.y}
        end)
    end
    wait()

    _ITB_BRIDGE_SCENARIO = st
    pcall(ITBX.mark, mission, "scenario", {name = report.name, errors = #report.errors})
    return report
end

-- A player-style move: the Move skill (slot 0) through Pawn:FireWeapon,
-- else Pawn:Move (ManualMove), else Pawn:SetSpace. Returns the method and
-- a note on why earlier methods were skipped.
function ITBX.native_move(pawn, pt)
    local notes = {}
    local ok, ret = pcall(function() return pawn:FireWeapon(pt, 0) end)
    if ok and ret ~= 0 and ret ~= false and ret ~= nil then
        return "FireWeapon[0]", nil
    end
    notes[#notes + 1] = "FireWeapon[0]: " .. (ok and ("returned " .. tostring(ret)) or ITBX.str(ret))
    ok, ret = pcall(function() return pawn:Move(pt) end)
    if ok and ret ~= false then return "Move", table.concat(notes, "; ") end
    notes[#notes + 1] = "Move: " .. (ok and "returned false" or ITBX.str(ret))
    ok, ret = pcall(function() pawn:SetSpace(pt) end)
    if ok then return "SetSpace", table.concat(notes, "; ") end
    notes[#notes + 1] = "SetSpace: " .. ITBX.str(ret)
    return "none", table.concat(notes, "; ")
end

-- The SCENARIO payload: inline JSON, or "@path" to a JSON file.
function ITBX.scenario_payload(text)
    if type(text) ~= "string" or text == "" then return nil, "empty payload" end
    if string.sub(text, 1, 1) == "@" then
        local path = string.match(string.sub(text, 2), "^%s*(.-)%s*$")
        local f = io.open(path, "r")
        if not f then return nil, "cannot open " .. path end
        text = f:read("*a")
        f:close()
    end
    local ok, spec = pcall(ITBX.json_decode, text)
    if not ok then return nil, tostring(spec) end
    return spec
end

function ITBX.safe_label(label)
    label = tostring(label or "snapshot")
    label = string.gsub(label, "[^%w_%-]", "_")
    if label == "" then label = "snapshot" end
    return string.sub(label, 1, 64)
end

local _cmd_seq = nil

local function write_ack(msg)
    local ack = msg
    if _cmd_seq then
        ack = "#" .. _cmd_seq .. " " .. msg
    end
    write_atomic(ACK_FILE, ACK_TMP, ack)
end

local function ui_probe_value(label, fn)
    local ok, value = pcall(fn)
    local out = {label = label, ok = ok and true or false}
    if ok then
        local vt = type(value)
        out.type = vt
        if vt == "boolean" or vt == "number" or vt == "string" then
            out.value = value
        elseif value == nil then
            out.value = nil
        else
            out.value = tostring(value)
        end
    else
        out.error = tostring(value)
    end
    return out
end

local function ui_probe_has_callable(obj, name)
    if obj == nil then return false end
    local ok, value = pcall(function() return obj[name] end)
    return ok and type(value) == "function"
end

local function ui_probe_methods(label, obj, names)
    local out = {}
    for _, name in ipairs(names) do
        out[#out + 1] = ui_probe_value(label .. "." .. name, function()
            if obj == nil then error(label .. " unavailable") end
            local fn = obj[name]
            if type(fn) ~= "function" then error(name .. " not callable") end
            return fn(obj)
        end)
    end
    return out
end

local function ui_probe_globals()
    local out = {}
    local global_names = {
        "Game", "Board", "Mission", "GameData", "sdlext", "modApi",
        "UiRoot", "Ui", "UI", "PauseMenu", "Pause_Menu", "Menu",
        "Screen", "ScreenManager", "GetGame", "GetCurrentMission",
    }
    for _, name in ipairs(global_names) do
        out[#out + 1] = ui_probe_value("_G." .. name, function()
            return _G[name]
        end)
    end
    return out
end

local function ui_probe_menu_state()
    local probes = {
        bridge_speed = _bridge_speed,
        timestamp = os.time(),
        globals = ui_probe_globals(),
        values = {},
        callable = {},
    }

    local method_names = {
        "IsPaused", "IsPause", "IsPauseMenu", "IsMenuOpen", "IsGamePaused",
        "IsCombatPaused", "IsRunning", "IsBusy", "GetState", "GetCurrentState",
        "GetTeamTurn", "GetTurnCount",
    }
    local objects = {
        {"Game", Game},
        {"Board", Board},
        {"Mission", Mission},
    }
    if GetGame ~= nil then
        local ok_game, game_ref = pcall(function() return GetGame() end)
        probes.values[#probes.values + 1] = {
            label = "GetGame()",
            ok = ok_game and true or false,
            type = ok_game and type(game_ref) or nil,
            value = ok_game and tostring(game_ref) or nil,
            error = ok_game and nil or tostring(game_ref),
        }
        if ok_game then
            objects[#objects + 1] = {"GetGame()", game_ref}
        end
    end

    for _, pair in ipairs(objects) do
        local label = pair[1]
        local obj = pair[2]
        local callable = {}
        for _, name in ipairs(method_names) do
            if ui_probe_has_callable(obj, name) then
                callable[#callable + 1] = name
            end
        end
        probes.callable[label] = callable
        local method_values = ui_probe_methods(label, obj, method_names)
        for _, entry in ipairs(method_values) do
            probes.values[#probes.values + 1] = entry
        end
    end

    local globals_as_functions = {
        "IsPaused", "IsPauseMenu", "IsMenuOpen", "IsGamePaused",
        "GetCurrentScreen", "GetCurrentMenu", "GetUiState",
    }
    for _, name in ipairs(globals_as_functions) do
        probes.values[#probes.values + 1] = ui_probe_value(name .. "()", function()
            local fn = _G[name]
            if type(fn) ~= "function" then error(name .. " not callable") end
            return fn()
        end)
    end

    return probes
end

-- Apply the bridge's direct Repair mutation to one pawn. The command-context
-- Skill_Repair effect can ACK without changing live state, so Repair Field
-- must reuse this exact path for every TEAM_MECH pawn instead of delegating
-- the group effect back to the unreliable native call.
local function direct_repair_pawn(target, target_uid, heal, save_data, effect_remove)
    local target_pos = target:GetSpace()
    local hp = target:GetHealth()
    local max_hp = get_pawn_max_health(target, target_uid, save_data)
    local new_hp = math.min(hp, max_hp - heal) + heal

    local hp_set = false
    local ok_set_health, did_set_health = pcall(function()
        local fn = target.SetHealth
        if type(fn) == "function" then
            fn(target, new_hp)
            return true
        end
        return false
    end)
    hp_set = ok_set_health and did_set_health

    local sd = SpaceDamage(target_pos, hp_set and 0 or -heal)
    sd.iFire = effect_remove
    sd.iAcid = effect_remove
    sd.iFrozen = effect_remove
    sd.iInjure = effect_remove
    Board:DamageSpace(sd)
    pcall(function()
        local fn = target.SetInfected
        if type(fn) == "function" then
            fn(target, false)
        end
    end)

    return new_hp
end

local function execute_command(cmd_str)
    local parts = {}
    for word in cmd_str:gmatch("%S+") do
        parts[#parts + 1] = word
    end

    if #parts == 0 then
        write_ack("ERROR: empty command")
        return
    end

    -- Parse optional sequence ID prefix: #NNN
    _cmd_seq = nil
    if parts[1]:sub(1,1) == "#" then
        _cmd_seq = parts[1]:sub(2)
        table.remove(parts, 1)
        if #parts == 0 then
            write_ack("ERROR: empty command after sequence ID")
            return
        end
    end

    local cmd = parts[1]

    if cmd == "MOVE" then
        -- MOVE uid x y (does NOT deactivate — follow with ATTACK/REPAIR/SKIP)
        -- Native mode: the Move skill, like MOVE_NATIVE. Legacy mode:
        -- Pawn:SetSpace (fast) or Pawn:Move (visual).
        local uid = tonumber(parts[2])
        local x, y = tonumber(parts[3]), tonumber(parts[4])
        local pawn = uid and Board:GetPawn(uid)
        if not pawn or not x or not y then
            write_ack("ERROR: pawn " .. tostring(parts[2]) .. " not found or no x y")
            return
        end
        local ready, why = ITBX.require_idle_coro(pawn)
        if not ready then
            write_ack("ERROR: MOVE " .. why)
            return
        end
        local ok, err
        if ITBX.legacy_mode() then
            ok, err = move_pawn_for_bridge(pawn, Point(x, y))
        else
            ok, err = ITBX.native_move_strict(pawn, Point(x, y))
        end
        if not ok then
            write_ack("ERROR: Move failed: " .. tostring(err))
            return
        end
        local settled, terr = ITBX.settle_coro(pawn, "MOVE")
        if not settled then
            write_ack("ERROR: " .. terr)
            return
        end
        write_ack("OK MOVE " .. uid .. " to " .. x .. "," .. y .. " [" .. err .. "]")

    elseif cmd == "ATTACK" then
        -- ATTACK uid weapon_slot target_x target_y
        -- weapon_slot is 0-based index (0=primary, 1=secondary)
        local uid = tonumber(parts[2])
        local weapon_slot = tonumber(parts[3])
        local tx, ty = tonumber(parts[4]), tonumber(parts[5])
        local pawn = Board:GetPawn(uid)
        if not pawn then
            write_ack("ERROR: pawn " .. uid .. " not found")
            return
        end
        if weapon_slot == nil or tx == nil or ty == nil then
            write_ack("ERROR: invalid weapon slot '" .. tostring(parts[3]) .. "' or target")
            return
        end
        local ready, why = ITBX.require_idle_coro(pawn)
        if not ready then
            write_ack("ERROR: ATTACK " .. why)
            return
        end
        local ok, method
        if ITBX.legacy_mode() then
            ITBX.select_pawn(pawn)
            ok, method = execute_weapon_by_slot(pawn, weapon_slot, tx, ty)
        else
            ok, method = ITBX.native_attack(pawn, weapon_slot, tx, ty)
        end
        if not ok then
            write_ack("ERROR: " .. method)
            return
        end
        local settled, terr = ITBX.settle_coro(pawn, "ATTACK")
        if ITBX.legacy_mode() then pawn:SetActive(false) end
        if not settled then
            write_ack("ERROR: " .. terr)
            return
        end
        write_ack("OK ATTACK " .. uid .. " slot=" .. weapon_slot .. " at " ..
                  tx .. "," .. ty .. " [" .. method .. "]")

    elseif cmd == "TWO_CLICK_ATTACK" then
        -- TWO_CLICK_ATTACK uid weapon_slot target1_x target1_y target2_x target2_y
        -- weapon_slot is 0-based index (0=primary, 1=secondary)
        local uid = tonumber(parts[2])
        local weapon_slot = tonumber(parts[3])
        local tx1, ty1 = tonumber(parts[4]), tonumber(parts[5])
        local tx2, ty2 = tonumber(parts[6]), tonumber(parts[7])
        local pawn = Board:GetPawn(uid)
        if not pawn then
            write_ack("ERROR: pawn " .. uid .. " not found")
            return
        end
        if weapon_slot == nil then
            write_ack("ERROR: invalid weapon slot '" .. tostring(parts[3]) .. "'")
            return
        end
        if not (tx1 and ty1 and tx2 and ty2) then
            write_ack("ERROR: TWO_CLICK_ATTACK needs uid slot x1 y1 x2 y2")
            return
        end
        local ready, why = ITBX.require_idle_coro(pawn)
        if not ready then
            write_ack("ERROR: TWO_CLICK_ATTACK " .. why)
            return
        end
        local ok, method
        if ITBX.legacy_mode() then
            ITBX.select_pawn(pawn)
            ok, method = execute_two_click_by_slot(
                pawn, weapon_slot, tx1, ty1, tx2, ty2
            )
        else
            ok, method = ITBX.native_two_click(pawn, weapon_slot, tx1, ty1, tx2, ty2)
        end
        if not ok then
            write_ack("ERROR: " .. method)
            return
        end
        local settled, terr = ITBX.settle_coro(pawn, "TWO_CLICK_ATTACK")
        if ITBX.legacy_mode() then pawn:SetActive(false) end
        if not settled then
            write_ack("ERROR: " .. terr)
            return
        end
        write_ack("OK TWO_CLICK_ATTACK " .. uid .. " slot=" .. weapon_slot ..
                  " at " .. tx1 .. "," .. ty1 .. " and " ..
                  tx2 .. "," .. ty2 .. " [" .. method .. "]")

    elseif cmd == "MOVE_ATTACK" then
        -- MOVE_ATTACK uid mx my weapon_slot tx ty
        -- weapon_slot is 0-based index (0=primary, 1=secondary)
        local uid = tonumber(parts[2])
        local mx, my = tonumber(parts[3]), tonumber(parts[4])
        local weapon_slot = tonumber(parts[5])
        local tx, ty = tonumber(parts[6]), tonumber(parts[7])
        local pawn = Board:GetPawn(uid)
        if not pawn then
            write_ack("ERROR: pawn " .. uid .. " not found")
            return
        end
        if weapon_slot == nil then
            write_ack("ERROR: invalid weapon slot '" .. tostring(parts[5]) .. "'")
            return
        end
        local ready, why = ITBX.require_idle_coro(pawn)
        if not ready then
            write_ack("ERROR: MOVE_ATTACK " .. why)
            return
        end
        local legacy = ITBX.legacy_mode()
        local ok1, err1
        if legacy then
            ok1, err1 = move_pawn_for_bridge(pawn, Point(mx, my))
        else
            ok1, err1 = ITBX.native_move_strict(pawn, Point(mx, my))
        end
        if not ok1 then
            write_ack("ERROR: Move failed: " .. tostring(err1))
            return
        end
        local moved_ok, merr = ITBX.settle_coro(pawn, "MOVE_ATTACK move")
        if not moved_ok then
            write_ack("ERROR: " .. merr .. "; weapon not fired")
            return
        end
        local ok2, method
        if legacy then
            ITBX.select_pawn(pawn)
            ok2, method = execute_weapon_by_slot(pawn, weapon_slot, tx, ty)
        else
            ok2, method = ITBX.native_attack(pawn, weapon_slot, tx, ty)
        end
        if not ok2 then
            write_ack("ERROR: " .. method)
            return
        end
        local settled, terr = ITBX.settle_coro(pawn, "MOVE_ATTACK")
        if legacy then pawn:SetActive(false) end
        if not settled then
            write_ack("ERROR: " .. terr)
            return
        end
        write_ack("OK MOVE_ATTACK " .. uid .. " [" .. method .. "]")

    elseif cmd == "SKIP" then
        -- SKIP uid — mech takes no action this turn
        local uid = tonumber(parts[2])
        local pawn = Board:GetPawn(uid)
        if not pawn then
            write_ack("ERROR: pawn " .. uid .. " not found")
            return
        end
        pawn:SetActive(false)
        write_ack("OK SKIP " .. uid)

    elseif cmd == "REPAIR" then
        -- REPAIR uid — mech repairs at current position
        local uid = tonumber(parts[2])
        local pawn = Board:GetPawn(uid)
        if not pawn then
            write_ack("ERROR: pawn " .. uid .. " not found")
            return
        end
        local pos = pawn:GetSpace()
        local method = "unknown"
        local ready, why = ITBX.require_idle_coro(pawn)
        if not ready then
            write_ack("ERROR: REPAIR " .. why)
            return
        end
        local ok, err
        if not ITBX.legacy_mode() then
            -- Native: the pawn's repair skill through FireWeapon.
            ok, method = ITBX.native_repair(pawn)
            err = method
        else
        -- Legacy (EXEC_MODE legacy): HP and statuses edited directly.
        ok, err = pcall(function()
            local effect_remove = _G["EFFECT_REMOVE"] or 2
            local boosted = false
            local ok_bo, bo = pcall(function() return pawn:IsBoosted() end)
            if ok_bo and bo then boosted = true end
            local save_data = _read_save_data()
            local max_hp = get_pawn_max_health(pawn, uid, save_data)
            local heal = boosted and 2 or 1

            -- Board:AddEffect(Skill_Repair:GetSkillEffect(...)) can ACK while
            -- doing nothing from the bridge command context. Mutate HP/status
            -- directly so verify sees the live board update.
            local new_hp = direct_repair_pawn(
                pawn, uid, heal, save_data, effect_remove)

            local mass_repair = false
            local ok_mass, has_mass = pcall(function()
                local fn = _G["IsPassiveSkill"]
                return type(fn) == "function" and fn("Mass_Repair")
            end)
            mass_repair = ok_mass and has_mass and true or false
            if mass_repair then
                local mech_team = _G["TEAM_MECH"] or TEAM_PLAYER
                local mech_ids = extract_table(Board:GetPawns(mech_team))
                for _, mid in ipairs(mech_ids) do
                    if mid ~= uid then
                        local target = Board:GetPawn(mid)
                        if target then
                            direct_repair_pawn(
                                target, mid, heal, save_data, effect_remove)
                        end
                    end
                end
            end

            -- Direct SpaceDamage is not a normal ability use, so consume Boost
            -- manually. Kai's Arrogant Boost is state-based and returns if the
            -- repair leaves the mech at full HP.
            local save_pilot = save_data.pilots[uid]
            local is_kai = save_pilot and save_pilot.id == "Pilot_Arrogant"
            local desired_boosted = is_kai and new_hp >= max_hp
            if boosted or desired_boosted then
                for _, mname in ipairs({"SetBoosted", "SetBoost"}) do
                    local ok_set, did_set = pcall(function()
                        local fn = pawn[mname]
                        if type(fn) == "function" then
                            fn(pawn, desired_boosted)
                            return true
                        end
                        return false
                    end)
                    if ok_set and did_set then break end
                end
            end

            local is_repairman = save_pilot and save_pilot.id == "Pilot_Repairman"
            local ok_power, has_power = pcall(function()
                return pawn:IsAbility("Power_Repair")
            end)
            if ok_power and has_power then is_repairman = true end
            if is_repairman then
                for i = DIR_START, DIR_END do
                    local adj = pos + DIR_VECTORS[i]
                    Board:DamageSpace(SpaceDamage(adj, 0, i))
                end
            end
            method = boosted and "direct_repair_boosted" or "direct_repair"
            if mass_repair then method = method .. "_mass" end
        end)
        end
        if not ok then
            write_ack("ERROR: Repair failed: " .. tostring(err))
            return
        end
        local settled, terr = ITBX.settle_coro(pawn, "REPAIR")
        if ITBX.legacy_mode() then pawn:SetActive(false) end
        if not settled then
            write_ack("ERROR: " .. terr)
            return
        end
        write_ack("OK REPAIR " .. uid .. " [" .. method .. "]")

    elseif cmd == "DEPLOY" then
        -- DEPLOY uid x y — place mech at tile during deployment
        local uid = tonumber(parts[2])
        local x, y = tonumber(parts[3]), tonumber(parts[4])
        local pawn = Board:GetPawn(uid)
        if not pawn then
            write_ack("ERROR: pawn " .. uid .. " not found")
            return
        end
        if not x or not y then
            write_ack("ERROR: DEPLOY needs uid x y")
            return
        end
        -- Only tiles the native deploy UI accepts (Board::GetDropZone): a
        -- mech on a Mission_Final pylon tile hangs the game at turn 0.
        local ok_c, allowed, why = pcall(ITBX.deploy_check, pawn, x, y)
        if ok_c and not allowed then
            log_bridge("DEPLOY REFUSED: " .. uid .. " -> " .. x .. "," .. y .. ": " .. tostring(why))
            write_ack("ERROR: DEPLOY refused: " .. x .. "," .. y .. " " .. tostring(why))
            return
        elseif not ok_c then
            log_bridge("DEPLOY check failed (placing anyway): " .. tostring(allowed))
        end
        local ok, err = pcall(function() pawn:SetSpace(Point(x, y)) end)
        if not ok then
            write_ack("ERROR: Deploy failed: " .. tostring(err))
            return
        end
        write_ack("OK DEPLOY " .. uid .. " at " .. x .. "," .. y)

    elseif cmd == "END_TURN" then
        local method = "unknown"
        local ok, err = pcall(function()
            if Game and Game.EndTurn then
                Game:EndTurn()
                method = "EndTurn"
            elseif GetGame then
                local g = GetGame()
                if g and g.EndTurn then
                    g:EndTurn()
                    method = "GetGame"
                else
                    error("no EndTurn method available")
                end
            else
                error("no Game/GetGame available")
            end
        end)
        if not ok then
            -- Game:EndTurn() doesn't exist on this ITB build AND ITB-ModLoader
            -- is not installed, so there's no way to actually advance the turn
            -- from Lua alone — the engine only transitions on a real UI click
            -- on the End Turn button. We still SetActive all player pawns so
            -- the solver sees a consistent "no remaining actions" state, then
            -- hand back a NEEDS_MCP_CLICK sentinel so the Python side routes
            -- through plan_end_turn() and a computer_batch click dispatch.
            log_bridge("WARN: EndTurn() failed (" .. tostring(err) ..
                       "); SetActive only, caller must MCP-click End Turn")
            method = "SetActive"
            local ok2, err2 = pcall(function()
                local mech_ids = extract_table(Board:GetPawns(TEAM_PLAYER))
                for _, mid in ipairs(mech_ids) do
                    local m = Board:GetPawn(mid)
                    if m then m:SetActive(false) end
                end
            end)
            if not ok2 then
                write_ack("ERROR: END_TURN failed: " .. tostring(err2))
                log_bridge("END_TURN ERROR: " .. tostring(err2))
                return
            end
            write_ack("NEEDS_MCP_CLICK END_TURN method=SetActive")
            return
        end
        -- Game:EndTurn() branch (reserved for future ITB builds that expose
        -- the method). Wait for the full player→enemy→player cycle.
        local start_count = -1
        pcall(function() start_count = Game:GetTurnCount() end)
        wait_until_coro(function()
            if Board:IsBusy() then return false end
            local cur_count = -1
            pcall(function() cur_count = Game:GetTurnCount() end)
            return cur_count > start_count
        end, 60)
        local phase = "unknown"
        if Game then
            local ok_tt, tt = pcall(function() return Game:GetTeamTurn() end)
            if ok_tt then
                if tt == 1 then phase = "combat_player"
                elseif tt == 6 then phase = "combat_enemy"
                end
            end
        end
        write_ack("OK END_TURN phase=" .. phase .. " method=" .. method)

    elseif cmd == "EXEC_MODE" then
        -- EXEC_MODE [native|legacy]: how player actions run. native (the
        -- default, every load): Pawn:FireWeapon only. legacy: the old bot's
        -- emulations (transit damage, Seismic flip, direct repair, scripted
        -- two-click weapons, SetSpace moves); opt-in only.
        local mode = parts[2]
        if mode == nil then
            write_ack("OK EXEC_MODE " .. ITBX.exec_mode)
        elseif mode == "native" or mode == "legacy" then
            if mode ~= ITBX.exec_mode then log_bridge("EXEC_MODE " .. mode) end
            ITBX.exec_mode = mode
            write_ack("OK EXEC_MODE " .. mode)
        else
            write_ack("ERROR: invalid exec mode: " .. tostring(mode) .. " (use native or legacy)")
            return
        end

    elseif cmd == "SET_SPEED" then
        -- SET_SPEED fast|visual
        local mode = parts[2] or "fast"
        if mode == "fast" or mode == "visual" then
            _bridge_speed = mode
            write_ack("OK SET_SPEED " .. mode)
            return
        else
            write_ack("ERROR: invalid speed: " .. mode .. " (use fast or visual)")
            return
        end

    elseif cmd == "UI_PROBE" then
        -- Read-only probe for pause/menu/UI state candidates. Intended for
        -- before/after Esc comparisons; every candidate is protected by pcall.
        local ok, result = pcall(ui_probe_menu_state)
        if ok then
            write_ack("OK UI_PROBE " .. json_encode(result))
        else
            write_ack("ERROR: UI_PROBE failed: " .. tostring(result))
        end
        return

    elseif cmd == "LUA" then
        -- Raw Lua execution (for debugging)
        local lua_code = cmd_str:match("LUA%s+(.*)")
        if not lua_code or lua_code == "" then
            write_ack("ERROR: empty LUA command")
            return
        end
        local ok, result = pcall(loadstring(lua_code))
        write_ack(ok and ("OK LUA: " .. tostring(result))
                      or ("ERROR LUA: " .. tostring(result)))

    elseif cmd == "MOVE_NATIVE" then
        -- MOVE_NATIVE uid x y: move the way a player click does, the Move
        -- skill through Pawn:FireWeapon(target, 0) (walk, arrival effects,
        -- moved bookkeeping), with the pawn selected. An error ack if the
        -- game refuses the move. Legacy mode only: falls back to Pawn:Move,
        -- then SetSpace. The ack names the method used and comes once the
        -- board and the pawn are idle. Does not end the unit's turn.
        local uid = tonumber(parts[2])
        local x, y = tonumber(parts[3]), tonumber(parts[4])
        local pawn = uid and Board:GetPawn(uid)
        if not pawn or not x or not y then
            write_ack("ERROR: MOVE_NATIVE needs a pawn uid and x y")
            return
        end
        local ready, why = ITBX.require_idle_coro(pawn)
        if not ready then
            write_ack("ERROR: MOVE_NATIVE " .. why)
            return
        end
        local method, detail
        if ITBX.legacy_mode() then
            ITBX.select_pawn(pawn)
            method, detail = ITBX.native_move(pawn, Point(x, y))
        else
            local ok, m = ITBX.native_move_strict(pawn, Point(x, y))
            if not ok then
                write_ack("ERROR: MOVE_NATIVE " .. m)
                return
            end
            method = m
        end
        local settled, terr = ITBX.settle_coro(pawn, "MOVE_NATIVE", 20)
        if not settled then
            write_ack("ERROR: " .. terr)
            return
        end
        local sp = pawn:GetSpace()
        write_ack("OK MOVE_NATIVE " .. uid .. " to " .. x .. "," .. y .. " [" .. method .. "] at "
                  .. sp.x .. "," .. sp.y .. (detail and (" (" .. detail .. ")") or ""))

    elseif cmd == "FIRE" then
        -- FIRE uid native_slot x y: Pawn:FireWeapon(target, slot) with the
        -- game's own slot numbering (0 = Move, 1 = first weapon, weapons
        -- added by SCENARIO weapons_add after the type's own). Debug only.
        if not ITBX.debug_enabled() then
            write_ack("ERROR: FIRE disabled (create " .. ITBX.DEBUG_FLAG_FILE .. ")")
            return
        end
        local uid, slot = tonumber(parts[2]), tonumber(parts[3])
        local x, y = tonumber(parts[4]), tonumber(parts[5])
        local pawn = uid and Board:GetPawn(uid)
        if not pawn or not slot or not x or not y then
            write_ack("ERROR: FIRE needs uid slot x y")
            return
        end
        local ok, ret = pcall(function() return pawn:FireWeapon(Point(x, y), slot) end)
        if not ok then
            write_ack("ERROR: FIRE failed: " .. tostring(ret))
            return
        end
        wait_until_coro(function() return not Board:IsBusy() end, 20)
        write_ack("OK FIRE " .. uid .. " slot=" .. slot .. " at " .. x .. "," .. y .. " ret=" .. tostring(ret))

    elseif cmd == "SNAPSHOT" then
        -- SNAPSHOT [label]: dump the state now, and a copy to
        -- itb_snapshot_<label>.json that later dumps do not overwrite.
        local path = ITBX.SNAPSHOT_PREFIX .. ITBX.safe_label(parts[2]) .. ".json"
        local ok, err = pcall(dump_state, path, path .. ".tmp")
        if not ok then
            write_ack("ERROR: SNAPSHOT failed: " .. tostring(err))
            return
        end
        write_ack("OK SNAPSHOT " .. path)

    elseif cmd == "SCENARIO" then
        -- SCENARIO <json> | SCENARIO @<file>: build a test board (debug
        -- flag file required; see ITBX.scenario_apply for the format).
        if not ITBX.debug_enabled() then
            write_ack("ERROR: SCENARIO disabled (create " .. ITBX.DEBUG_FLAG_FILE .. ")")
            return
        end
        local spec, perr = ITBX.scenario_payload(string.match(cmd_str, "SCENARIO%s+(.*)$"))
        if not spec then
            write_ack("ERROR: SCENARIO payload: " .. tostring(perr))
            return
        end
        -- Not under pcall: scenario_apply waits (yields) between stages.
        local report, aerr = ITBX.scenario_apply(spec, wait_for_board_coro)
        if not report then
            write_ack("ERROR: SCENARIO " .. tostring(aerr))
            return
        end
        local path = ITBX.SNAPSHOT_PREFIX .. ITBX.safe_label("scenario_" .. tostring(report.name)) .. ".json"
        pcall(dump_state, path, path .. ".tmp")
        report.snapshot = path
        write_ack("OK SCENARIO " .. json_encode(report))

    elseif cmd == "SCENARIO_RESET" then
        -- Forget the scenario ledger (queued attacks, spawn queue).
        _ITB_BRIDGE_SCENARIO = nil
        write_ack("OK SCENARIO_RESET")

    elseif cmd == "PHASE_LOG" then
        -- PHASE_LOG [clear]: write the per-mission phase log to a file.
        if parts[2] == "clear" then
            _ITB_BRIDGE_PHASE_LOG = nil
            write_ack("OK PHASE_LOG cleared")
            return
        end
        local log = _ITB_BRIDGE_PHASE_LOG
        local payload = {
            debug = ITBX.debug_enabled(),
            frame = log and log.frame or 0,
            entries = log and log.entries or {},
            env = log and log.env or {},
        }
        write_atomic(ITBX.PHASE_LOG_FILE, ITBX.PHASE_LOG_FILE .. ".tmp", json_encode(payload))
        write_ack("OK PHASE_LOG " .. ITBX.PHASE_LOG_FILE .. " entries=" .. #payload.entries)
        return

    elseif cmd == "DEBUG_STATUS" then
        write_ack("OK DEBUG_STATUS " .. json_encode({
            debug = ITBX.debug_enabled(),
            flag_file = ITBX.DEBUG_FLAG_FILE,
            ext_version = ITBX.VERSION,
            scenario = _ITB_BRIDGE_SCENARIO and _ITB_BRIDGE_SCENARIO.name or nil,
        }))
        return

    else
        write_ack("ERROR: unknown command: " .. cmd)
    end

    -- Dump state after every command
    pcall(dump_state)
end

local function poll_commands()
    -- If a prior command's coroutine is still running (yielded on
    -- wait_for_board_coro), leave the cmd file alone until it completes.
    if _running_coroutine then return end

    local f = io.open(CMD_FILE, "r")
    if f then
        local cmd = f:read("*a")
        f:close()
        os.remove(CMD_FILE)
        if cmd and cmd:match("%S") then
            log_bridge("CMD: " .. cmd:gsub("\n", " "))
            local trimmed = cmd:match("^%s*(.-)%s*$")
            -- Wrap execute_command in a coroutine so wait_for_board_coro
            -- can yield control back to the engine between polls.
            _running_coroutine = coroutine.create(function()
                execute_command(trimmed)
            end)
            local ok, err = coroutine.resume(_running_coroutine)
            if not ok then
                log_bridge("CMD CORO ERROR: " .. tostring(err))
                write_atomic(ACK_FILE, ACK_TMP,
                             "ERROR: coroutine failed: " .. tostring(err))
                _running_coroutine = nil
            elseif coroutine.status(_running_coroutine) == "dead" then
                _running_coroutine = nil
            end
        end
    end
end

--------------------------------------------------------------------
-- Game hooks (with re-execution guard)
--------------------------------------------------------------------
-- Guard: store originals in a global so reloads don't compound hooks.
-- On first load, _ITB_BRIDGE_ORIGINALS is nil so we capture the real
-- game functions. On subsequent loads we reuse those same originals,
-- preventing the wrap-on-wrap stack that kills frame rate.
if not _ITB_BRIDGE_ORIGINALS then
    _ITB_BRIDGE_ORIGINALS = {
        BaseUpdate       = Mission.BaseUpdate,
        NextTurn         = Mission.NextTurn,
        BaseStart        = Mission.BaseStart,
        MissionEnd       = Mission.MissionEnd,
        BaseDeployment   = Mission.BaseDeployment,
        -- Mission_Teleporter is loaded earlier (scripts.lua line 65 vs
        -- modloader.lua at 160) but guard regardless — any plugin can
        -- redefine it before our hook installs.
        TeleporterStartMission = (Mission_Teleporter
            and Mission_Teleporter.StartMission) or nil,
    }
end

local _orig_BaseUpdate              = _ITB_BRIDGE_ORIGINALS.BaseUpdate
local _orig_NextTurn                = _ITB_BRIDGE_ORIGINALS.NextTurn
local _orig_BaseStart               = _ITB_BRIDGE_ORIGINALS.BaseStart
local _orig_MissionEnd              = _ITB_BRIDGE_ORIGINALS.MissionEnd
local _orig_BaseDeployment          = _ITB_BRIDGE_ORIGINALS.BaseDeployment
local _orig_TeleporterStartMission  = _ITB_BRIDGE_ORIGINALS.TeleporterStartMission

-- Teleporter pads for the CURRENT mission. Each entry = {x1, y1, x2, y2}.
-- Populated by the Mission_Teleporter:StartMission wrap further down: that
-- wrap scope-rebinds Board.AddTeleport in a pcall so the C++ class system
-- can reject the assignment (commit 456ba49 hit "no static 'AddTeleport'
-- in class 'Board'" with a permanent global rebind at file-load) without
-- taking down mission load. On rejection the list stays empty and the
-- solver falls back to pre-sim-v8 behavior for that mission.
_ITB_TELEPORT_PAIRS = _ITB_TELEPORT_PAIRS or {}

-- Cached deployment zone (captured in BaseDeployment, cleared on MissionEnd)
_ITB_DEPLOY_ZONE = _ITB_DEPLOY_ZONE or {}

-- Cached current mission reference. Populated via Mission:BaseStart,
-- BaseUpdate, NextTurn, and BaseDeployment hooks since the game does not
-- expose a top-level GetCurrentMission() global. Cleared in MissionEnd.
_ITB_CURRENT_MISSION = _ITB_CURRENT_MISSION or nil

-- State dump interval (separate from command poll)
local _state_dump_interval = 5  -- dump state every 5 seconds
local _last_state_dump = 0

local function clear_stale_teleporter_pairs_for(mission)
    if mission and mission.ID and mission.ID ~= "Mission_Teleporter"
       and _ITB_TELEPORT_PAIRS and #_ITB_TELEPORT_PAIRS > 0 then
        log_bridge("TELEPORT PAD: clearing stale pairs for "
            .. tostring(mission.ID))
        _ITB_TELEPORT_PAIRS = {}
    end
end

-- BaseUpdate: resume pending command coroutine, poll for new commands,
-- and periodically dump state. Coroutine resume happens FIRST so that
-- wait_for_board_coro yields get unblocked the moment the engine drains
-- its effect queue — without this, poll_commands could race a yielded
-- coroutine and clobber _running_coroutine.
Mission.BaseUpdate = function(self)
    _orig_BaseUpdate(self)
    -- Cache current mission (self is the active mission inside BaseUpdate)
    _ITB_CURRENT_MISSION = self
    -- No stale-pair clearing here: BaseUpdate also runs for other mission
    -- objects (live 2026-10-10: Mission_Civilians wiped the pairs a second
    -- after Mission_Teleporter captured them). BaseStart/BaseDeployment of
    -- the next mission clear them.
    -- Heartbeat: write mtime so Python can detect stuck/dead bridge
    pcall(function()
        local f = io.open(HEARTBEAT_FILE, "w")
        if f then f:write(tostring(os.clock())); f:close() end
    end)
    if _running_coroutine then
        local ok, err = coroutine.resume(_running_coroutine)
        if not ok then
            log_bridge("CORO RESUME ERROR: " .. tostring(err))
            write_atomic(ACK_FILE, ACK_TMP,
                         "ERROR: coroutine failed: " .. tostring(err))
            _running_coroutine = nil
        elseif coroutine.status(_running_coroutine) == "dead" then
            _running_coroutine = nil
        end
    end
    local now = os.clock()
    if now - _last_poll >= _poll_interval then
        _last_poll = now
        pcall(poll_commands)
    end
    -- If deploy zone is empty on turn 0, retry capture each update
    if #_ITB_DEPLOY_ZONE == 0 and Game and Game:GetTurnCount() == 0 then
        local zone = capture_deploy_zone()
        if #zone > 0 then
            _ITB_DEPLOY_ZONE = zone
            log_bridge("DEPLOY ZONE captured in BaseUpdate: " .. #zone .. " tiles")
        end
    end
    -- Periodically dump state so Python can detect the bridge
    if now - _last_state_dump >= _state_dump_interval then
        _last_state_dump = now
        pcall(dump_state)
    elseif not _running_coroutine then
        -- The last dump caught the board mid-animation: replace it as soon
        -- as the board is idle.
        pcall(ITBX.settled_redump, dump_state)
    end
    -- Bridge extension: per-frame phase log (debug flag only).
    if not ITBX.disabled and ITBX.debug_enabled() then
        ITBX.hook_work("poll_frame", ITBX.poll_frame, self)
    end
end

-- NextTurn: dump state on each turn change.
--
-- Legacy mode only (EXEC_MODE legacy): re-activate every player-team pawn
-- (2b9d3bb1, for the old bot's poller after its END_TURN SetActive(false)).
-- Natively, the player turn start (BoardPlayer state 0, after BaseNextTurn)
-- activates the player's non-neutral pawns itself; this ran before that and
-- also activated neutral ones, so it is off in native mode.
Mission.NextTurn = function(self)
    _orig_NextTurn(self)
    _ITB_CURRENT_MISSION = self
    pcall(function()
        if ITBX.legacy_mode() and Game and Game:GetTeamTurn() == TEAM_PLAYER then
            local mech_ids = extract_table(Board:GetPawns(TEAM_PLAYER))
            for _, mid in ipairs(mech_ids) do
                local m = Board:GetPawn(mid)
                if m and not m:IsDead() then m:SetActive(true) end
            end
        end
    end)
    pcall(dump_state)
    log_bridge("TURN " .. (Game and Game:GetTurnCount() or "?") .. " team=" .. (Game and Game:GetTeamTurn() or "?"))
end

-- BaseStart: dump state when mission starts (after deployment)
Mission.BaseStart = function(self)
    _orig_BaseStart(self)
    _ITB_CURRENT_MISSION = self
    clear_stale_teleporter_pairs_for(self)
    pcall(dump_state)
    log_bridge("MISSION START: " .. tostring(self.ID or self.Name or "unknown"))
end

-- BaseDeployment: capture deployment zone AFTER engine sets it up
Mission.BaseDeployment = function(self)
    _orig_BaseDeployment(self)
    _ITB_CURRENT_MISSION = self
    clear_stale_teleporter_pairs_for(self)
    -- Capture zone AFTER original runs (engine creates the zone in BaseDeployment)
    _ITB_DEPLOY_ZONE = capture_deploy_zone()
    if #_ITB_DEPLOY_ZONE > 0 then
        log_bridge("DEPLOY ZONE from Board:GetZone: " .. #_ITB_DEPLOY_ZONE .. " tiles")
    else
        log_bridge("DEPLOY ZONE: Board:GetZone returned 0 tiles")
    end
    -- Dump state so Python can see the deployment zone immediately
    pcall(dump_state)
end

-- MissionEnd: log mission completion, clear deployment zone + teleport pads
Mission.MissionEnd = function(self)
    log_bridge("MISSION END: " .. tostring(self.ID or self.Name or "unknown"))
    _ITB_DEPLOY_ZONE = {}
    _ITB_CURRENT_MISSION = nil
    _ITB_TELEPORT_PAIRS = {}
    _orig_MissionEnd(self)
    pcall(dump_state)
end

-- Mission_Teleporter:StartMission — capture pad pairs.
--
-- Why this wrap exists: Mission_Teleporter calls Board:AddTeleport(p1,p2)
-- twice during StartMission to register two pad pairs, and the C++ side
-- never re-exposes those pairs through a documented Lua getter. The Rust
-- sim's apply_teleport_on_land needs them to score post-move positions
-- correctly on Detritus disposal missions (commit 456ba49 added the sim
-- side; the bridge has been emitting an empty list since 63e0e18 rolled
-- back the global Board.AddTeleport hook that crashed macOS).
--
-- Why this is safer than the prior global hook:
--   * Wrap target is Mission_Teleporter, a pure-Lua subclass of
--     Mission_Auto. Method dispatch on Mission goes through plain Lua
--     metatables (the existing Mission.BaseStart wrap proves that works
--     on macOS); Board lives in the C++ class proxy that rejected the
--     earlier rawset.
--   * The Board.AddTeleport scope-rebind is wrapped in pcall. If the
--     proxy still refuses the assignment we just log + skip capture; the
--     original StartMission still runs, _ITB_TELEPORT_PAIRS stays empty,
--     and the simulator falls back to pre-v8 behavior — same outcome
--     we have today, no crash.
--   * No other mission ever touches Board.AddTeleport in our wrap, so
--     Vice Fist throw and Science_Swap (the other AddTeleport callers)
--     never see our shadow function.
--   * Every step is pcall-guarded. The worst case is that the next
--     teleporter mission solves on stale (empty) pad data; combat still
--     proceeds.
if Mission_Teleporter and _orig_TeleporterStartMission then
    Mission_Teleporter.StartMission = function(self)
        _ITB_TELEPORT_PAIRS = {}
        local original_AddTeleport = nil
        local rebound = false
        local capture_fn = function(board_self, p1, p2)
            -- Record the pair, then defer to the original engine method
            -- so the actual pad placement / animation still happens.
            local ok = pcall(function()
                if p1 and p2
                   and type(p1.x) == "number" and type(p1.y) == "number"
                   and type(p2.x) == "number" and type(p2.y) == "number" then
                    _ITB_TELEPORT_PAIRS[#_ITB_TELEPORT_PAIRS + 1] =
                        {p1.x, p1.y, p2.x, p2.y}
                    log_bridge(
                        "TELEPORT PAD pair captured: ("
                        .. p1.x .. "," .. p1.y .. ") <-> ("
                        .. p2.x .. "," .. p2.y .. ")")
                end
            end)
            if not ok then
                log_bridge("TELEPORT PAD capture: pair record failed (non-fatal)")
            end
            -- Always invoke the original — never swallow the pad placement.
            -- C++ binding signature is `void AddTeleport(Board&, Point, Point)`
            -- — exactly 3 args. Forwarding a 4th `delay` arg (even nil) tripped
            -- C++ overload resolution with "No matching overload found" and
            -- crashed ITB on the next mission load (observed on Detritus
            -- 2026-04-29: Mission_Disposal ended, next Mission_Teleporter
            -- mission's StartMission errored mid-AddTeleport and the game
            -- terminated). Dropping the trailing arg matches the engine
            -- signature exactly.
            return original_AddTeleport(board_self, p1, p2)
        end

        -- Try to install the capture. If the C++ proxy rejects the
        -- assignment (the failure mode that bit commit 456ba49) the
        -- pcall returns false and we proceed without recording — the
        -- original StartMission still runs through the unmodified
        -- engine method, mission load succeeds.
        local install_ok = pcall(function()
            original_AddTeleport = Board and Board.AddTeleport or nil
            if original_AddTeleport then
                Board.AddTeleport = capture_fn
                rebound = true
            end
        end)

        if not install_ok then
            log_bridge("TELEPORT PAD: Board.AddTeleport rebind rejected — "
                .. "running StartMission with empty pad list (sim falls back "
                .. "to pre-v8 behavior on this mission)")
            rebound = false
        end

        -- Run the original mission setup. This is the call that
        -- triggers Board:AddTeleport(start, finish) twice.
        local run_ok, run_err = pcall(function()
            _orig_TeleporterStartMission(self)
        end)

        -- ALWAYS restore, regardless of whether StartMission errored.
        -- Leaving our shadow function on Board would break Vice Fist /
        -- Science_Swap on subsequent turns.
        if rebound then
            pcall(function()
                Board.AddTeleport = original_AddTeleport
            end)
        end

        if not run_ok then
            log_bridge("TELEPORT PAD: original StartMission errored: "
                .. tostring(run_err))
            -- Re-raise so the engine sees the same error it would have
            -- without our wrap. Game-side error recovery owns this path.
            error(run_err)
        end

        log_bridge("TELEPORT PAD: StartMission complete, "
            .. #_ITB_TELEPORT_PAIRS .. " pair(s) captured")
    end
end

--------------------------------------------------------------------
-- Bridge extension hooks
--------------------------------------------------------------------
-- Mission:BaseNextTurn, ApplyEnvironmentEffect and PlanEnvironment are
-- defined once, on Mission (no shipped subclass overrides them), so these
-- class-level wraps see every mission without touching instances. Each
-- wrap calls the original exactly once, outside pcall, and returns its
-- result unchanged; the bridge's own work is pcall'd.
--   BaseNextTurn (team 6)    after the Vek attacks and environment, before
--                            the spawns: debug capture PRE_SPAWN_FILE
--   ApplyEnvironmentEffect   one env_strike_log entry per step
--   PlanEnvironment (1st, team 6)  after the spawns, before the AI moves
--                            (stage 7 spec 1.9 "this turn's result"):
--                            debug capture POST_SPAWN_FILE
_ITB_BRIDGE_ORIGINALS.BaseNextTurn =
    _ITB_BRIDGE_ORIGINALS.BaseNextTurn or Mission.BaseNextTurn
_ITB_BRIDGE_ORIGINALS.ApplyEnvironmentEffect =
    _ITB_BRIDGE_ORIGINALS.ApplyEnvironmentEffect or Mission.ApplyEnvironmentEffect
_ITB_BRIDGE_ORIGINALS.PlanEnvironment =
    _ITB_BRIDGE_ORIGINALS.PlanEnvironment or Mission.PlanEnvironment

local _orig_BaseNextTurn = _ITB_BRIDGE_ORIGINALS.BaseNextTurn
local _orig_ApplyEnvironmentEffect = _ITB_BRIDGE_ORIGINALS.ApplyEnvironmentEffect
local _orig_PlanEnvironment = _ITB_BRIDGE_ORIGINALS.PlanEnvironment

function ITBX.on_base_next_turn(mission, dump)
    local turn, team = itbx_turn_team()
    ITBX.mark(mission, "base_next_turn", {})
    if team == TEAM_PLAYER then
        ITBX.record_turn_start(mission)
    end
    if team == TEAM_ENEMY then
        ITBX.pending_post_spawn = {mission = mission, turn = turn}
        if ITBX.debug_enabled() then
            dump(ITBX.PRE_SPAWN_FILE, ITBX.PRE_SPAWN_FILE .. ".tmp")
        end
    end
end

function ITBX.on_plan_environment(mission, dump)
    local pending = ITBX.pending_post_spawn
    if pending == nil or not rawequal(pending.mission, mission) then return end
    local _, team = itbx_turn_team()
    if team ~= TEAM_ENEMY then return end
    ITBX.pending_post_spawn = nil
    ITBX.mark(mission, "plan_environment", {})
    if ITBX.debug_enabled() then
        dump(ITBX.POST_SPAWN_FILE, ITBX.POST_SPAWN_FILE .. ".tmp")
    end
end

-- The wraps forward every argument and every return value of the
-- original untouched (the native enemy-phase driver loops on
-- ApplyEnvironmentEffect / PlanEnvironment returning true), call it exactly
-- once and outside any pcall (its errors stay the game's), and do their own
-- work through ITBX.hook_work: guarded, budgeted, not re-entrant, a no-op
-- once the extension is disabled.
if _orig_BaseNextTurn then
    Mission.BaseNextTurn = function(self, ...)
        -- Hang guard first: it does not depend on the extension being on.
        pcall(ITBX.evacuate_pylon_tiles)
        ITBX.hook_work("on_base_next_turn", ITBX.on_base_next_turn, self, dump_state)
        return _orig_BaseNextTurn(self, ...)
    end
end

if _orig_ApplyEnvironmentEffect then
    Mission.ApplyEnvironmentEffect = function(self, ...)
        local r = itbx_pack(_orig_ApplyEnvironmentEffect(self, ...))
        ITBX.hook_work("log_env_step", ITBX.log_env_step, self, r[1])
        return unpack(r, 1, r.n)
    end
end

if _orig_PlanEnvironment then
    Mission.PlanEnvironment = function(self, ...)
        ITBX.hook_work("on_plan_environment", ITBX.on_plan_environment, self, dump_state)
        return _orig_PlanEnvironment(self, ...)
    end
end

--------------------------------------------------------------------
-- Startup
--------------------------------------------------------------------
-- Clean up stale files from previous session
pcall(function() os.remove(STATE_FILE) end)
pcall(function() os.remove(CMD_FILE) end)
pcall(function() os.remove(ACK_FILE) end)

local _reload_count = (_ITB_BRIDGE_LOAD_COUNT or 0) + 1
_ITB_BRIDGE_LOAD_COUNT = _reload_count

log_bridge("=== ITB Bot Bridge started (load #" .. _reload_count .. ") ===")
if ConsolePrint then
    ConsolePrint("ITB Bot Bridge loaded! IPC via " .. BRIDGE_DIR)
end
