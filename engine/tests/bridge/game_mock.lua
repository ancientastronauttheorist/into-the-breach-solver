-- A strict stand-in for the game's Lua API, enough to load and run the
-- bridge (src/bridge/modloader.lua) outside the game.
--
-- Native objects are userdata (newproxy) like luabind instances. Bound
-- methods follow luabind's rules: one of the listed overloads must match the
-- call exactly (argument count and Lua types, no number<->bool coercion),
-- otherwise the call raises "No matching overload". Every such failure is
-- also recorded in MOCK.mismatches, so a call the bridge wraps in pcall
-- still fails the harness. Methods the game does not bind (e.g.
-- Pawn:GetMaxHealth) index to nil, as on a luabind instance. Signatures are
-- copied from the binding table of build 21601364 (stage 6 notes).
--
-- The state is deliberately simple: tiles, an ordered pawn list, a spawn
-- queue. Mutators change it the way the bridge's scenario code expects
-- (terrain, statuses, add/remove/move pawns); no game rules run.

MOCK = {
    mismatches = {},  -- strict-call failures
    fired = {},       -- Pawn:FireWeapon calls
    calls = {},       -- "Class:method" in call order (mutators only)
    busy = 0,         -- Board:IsBusy() returns true while > 0 (counts down)
    busy_after_mutation = 2,
    fire_busy = nil,  -- if set: a mech's Pawn:FireWeapon keeps the board busy this long (effects)
    pawn_busy_after_fire = nil,  -- if set: the firing mech's Pawn:IsBusy() stays true this many polls
    turn = 1,
    team = 1,
    pawn_ids = 100,
}

---------------------------------------------------------------- objects
local DATA = setmetatable({}, {__mode = "k"})  -- proxy -> fields
local KIND = setmetatable({}, {__mode = "k"})  -- proxy -> class name

local function new_class(name, index_fn, extra)
    local proto = newproxy(true)
    local mt = getmetatable(proto)
    mt.__index = function(u, k) return index_fn(u, k) end
    mt.__tostring = function() error("luabind: no __tostring for " .. name, 2) end
    mt.__eq = function() error("No such operator defined", 2) end
    for k, v in pairs(extra or {}) do mt[k] = v end
    return function(fields)
        local u = newproxy(proto)
        DATA[u] = fields
        KIND[u] = name
        return u
    end, mt
end

local function kind(v)
    if type(v) == "userdata" then return KIND[v] end
    return nil
end

local function describe(args, n)
    local parts = {}
    for i = 1, n do
        local v = args[i]
        parts[#parts + 1] = kind(v) or type(v)
    end
    return table.concat(parts, ", ")
end

local function matches(v, t)
    if t == "int" or t == "float" then return type(v) == "number" end
    if t == "bool" then return type(v) == "boolean" end
    if t == "string" then return type(v) == "string" end
    if t == "Pawn" then return v == nil or kind(v) == "Pawn" end
    return kind(v) == t
end

-- A bound method: `overloads` is a list of argument-type lists.
local function bind(class, name, overloads, impl, mutator)
    return function(self, ...)
        local n = select("#", ...)
        local args = {...}
        if kind(self) ~= class then
            local msg = class .. ":" .. name .. " called without a " .. class .. " self"
            MOCK.mismatches[#MOCK.mismatches + 1] = msg
            error(msg, 2)
        end
        for _, sig in ipairs(overloads) do
            if #sig == n then
                local ok = true
                for i, t in ipairs(sig) do
                    if not matches(args[i], t) then ok = false break end
                end
                if ok then
                    if mutator then
                        MOCK.calls[#MOCK.calls + 1] = class .. ":" .. name
                        -- A change never cuts a running animation short.
                        MOCK.busy = math.max(MOCK.busy, MOCK.busy_after_mutation)
                    end
                    return impl(self, ...)
                end
            end
        end
        local msg = "No matching overload found, candidates: " .. class .. ":" .. name
            .. "(" .. describe(args, n) .. ")"
        MOCK.mismatches[#MOCK.mismatches + 1] = msg
        error(msg, 2)
    end
end

---------------------------------------------------------------- Point
local point_methods = {}
local new_point, PointMT
new_point, PointMT = new_class("Point", function(u, k)
    local d = DATA[u]
    if k == "x" then return d.x end
    if k == "y" then return d.y end
    return point_methods[k]
end, {
    __add = function(a, b) return new_point({x = a.x + b.x, y = a.y + b.y}) end,
    __sub = function(a, b) return new_point({x = a.x - b.x, y = a.y - b.y}) end,
    __eq = function(a, b) return a.x == b.x and a.y == b.y end,
    __newindex = function(u, k, v)
        if k ~= "x" and k ~= "y" then error("luabind: no member " .. tostring(k), 2) end
        DATA[u][k] = v
    end,
})

function Point(...)
    local n = select("#", ...)
    local x, y = ...
    if n == 0 then return new_point({x = 0, y = 0}) end
    if n == 2 and type(x) == "number" and type(y) == "number" then
        return new_point({x = x, y = y})
    end
    local msg = "No matching overload found, candidates: Point(" .. describe({...}, n) .. ")"
    MOCK.mismatches[#MOCK.mismatches + 1] = msg
    error(msg, 2)
end

point_methods.GetString = function(self) return "Point(" .. self.x .. "," .. self.y .. ")" end

local function P(x, y) return Point(x, y) end
local function valid(p) return p.x >= 0 and p.x < 8 and p.y >= 0 and p.y < 8 end

---------------------------------------------------------------- lists
local function list_class(name)
    local methods = {}
    local make = new_class(name, function(u, k) return methods[k] end)
    methods.size = bind(name, "size", {{}}, function(self) return #DATA[self].items end)
    methods.index = bind(name, "index", {{"int"}}, function(self, i) return DATA[self].items[i] end)
    local item = name == "PointList" and "Point" or "int"
    methods.push_back = bind(name, "push_back", {{item}}, function(self, v)
        local items = DATA[self].items
        items[#items + 1] = v
    end)
    return function(items) return make({items = items}) end
end
local new_point_list = list_class("PointList")
local new_int_list = list_class("IntList")

-- PointList(): an empty list (weapon scripts build target areas with it).
function PointList() return new_point_list({}) end

function extract_table(list)
    local out = {}
    for i = 1, list:size() do out[#out + 1] = list:index(i) end
    return out
end

---------------------------------------------------------------- SpaceDamage
local SD_FIELDS = {
    loc = true, iDamage = true, iPush = true, iShield = true, iInjure = true, iCrack = true,
    iFire = true, bKO_Effect = true, iFrozen = true, iSmoke = true, sSound = true, sPawn = true,
    iPawnTeam = true, bHide = true, bHideIcon = true, bHidePath = true, iAcid = true,
    fDelay = true, iTerrain = true, bEvacuate = true, sImageMark = true, sScript = true,
    bSimpleMark = true, sItem = true, sAnimation = true,
}
local new_sd = new_class("SpaceDamage", function(u, k)
    if not SD_FIELDS[k] then return nil end
    return DATA[u][k]
end, {
    __newindex = function(u, k, v)
        if not SD_FIELDS[k] then error("luabind: no writable member " .. tostring(k), 2) end
        DATA[u][k] = v
    end,
})

function SpaceDamage(...)
    local n = select("#", ...)
    local a, b, c = ...
    local f = {loc = P(-1, -1), iDamage = 0, iPush = 4, iFire = 0, iAcid = 0, iSmoke = 0,
               iFrozen = 0, iShield = 0, iInjure = 0, iCrack = 0, iTerrain = -1}
    if n == 0 then return new_sd(f) end
    if n == 1 and type(a) == "number" then f.iDamage = a; return new_sd(f) end
    if n == 1 and kind(a) == "Point" then f.loc = a; return new_sd(f) end
    if n == 2 and kind(a) == "Point" and type(b) == "number" then
        f.loc = a; f.iDamage = b; return new_sd(f)
    end
    if n == 3 and kind(a) == "Point" and type(b) == "number" and type(c) == "number" then
        f.loc = a; f.iDamage = b; f.iPush = c; return new_sd(f)
    end
    local msg = "No matching overload found, candidates: SpaceDamage(" .. describe({...}, n) .. ")"
    MOCK.mismatches[#MOCK.mismatches + 1] = msg
    error(msg, 2)
end

---------------------------------------------------------------- constants
TEAM_PLAYER, TEAM_NONE, TEAM_ANY, TEAM_MECH, TEAM_ENEMY, TEAM_BOTS = 1, 2, 2, 4, 6, 8
TEAM_ENEMY_MAJOR = -1
TERRAIN_ROAD, TERRAIN_BUILDING, TERRAIN_RUBBLE, TERRAIN_WATER, TERRAIN_MOUNTAIN = 0, 1, 2, 3, 4
TERRAIN_ICE, TERRAIN_FOREST, TERRAIN_SAND, TERRAIN_HOLE = 5, 6, 7, 9
TERRAIN_FIRE, TERRAIN_ACID, TERRAIN_LAVA = 11, 12, 14
EFFECT_NONE, EFFECT_CREATE, EFFECT_REMOVE = 0, 1, 2
DIR_UP, DIR_RIGHT, DIR_DOWN, DIR_LEFT, DIR_NONE, DIR_FLIP = 0, 1, 2, 3, 4, 5
DIR_START, DIR_END = 0, 3
DIR_VECTORS = {[0] = P(0, -1), [1] = P(1, 0), [2] = P(0, 1), [3] = P(-1, 0)}
BLOCKED_NONE, BLOCKED_TEMP, BLOCKED_PERM = 0, 1, 2
PATH_GROUND, PATH_FLYER, PATH_MASSIVE, PATH_PROJECTILE = 0, 1, 2, 3
DAMAGE_DEATH, ENV_EFFECT, RAIN_NORMAL, EFFECT_DEADLY = 1000, -10, 0, true

---------------------------------------------------------------- board state
local TILES = {}
for x = 0, 7 do
    TILES[x] = {}
    for y = 0, 7 do
        TILES[x][y] = {terrain = 0, hp = 0, max = 0, populated = false, fire = false,
                       smoke = false, acid = false, cracked = false, frozen = false,
                       shield = false, item = "", custom = "", lava = false,
                       targeted = false, env = false, dangerous = false}
    end
end
MOCK.tiles = TILES
MOCK.pawns = {}       -- board-list order
MOCK.spawns = {}      -- {type=, x=, y=, id=}
MOCK.zones = {}       -- name -> {{x,y},...}

local function tile(p) return TILES[p.x][p.y] end

local pawn_methods = {}
local new_pawn = new_class("Pawn", function(u, k) return pawn_methods[k] end)

function MOCK.add_pawn(fields)
    local def = _G[fields.type] or {}
    local f = {
        id = fields.id or MOCK.next_id(), type = fields.type, x = fields.x or -1, y = fields.y or -1,
        hp = fields.hp or def.Health or 1, max = fields.max or def.Health or 1,
        team = fields.team or def.DefaultTeam or 6, mech = fields.mech or false,
        active = fields.active ~= false, move = fields.move or def.MoveSpeed or 3,
        fire = fields.fire or false, acid = fields.acid or false, shield = fields.shield or false,
        frozen = fields.frozen or false, boosted = false, infected = false, powered = true,
        neutral = false, selected = false, shots = fields.shots, queued = nil,
        selected_weapon = fields.selected_weapon or 0,
    }
    local u = new_pawn(f)
    MOCK.pawns[#MOCK.pawns + 1] = u
    return u
end

function MOCK.next_id()
    MOCK.pawn_ids = MOCK.pawn_ids + 1
    return MOCK.pawn_ids
end

local function pawn_at(p)
    for _, u in ipairs(MOCK.pawns) do
        local d = DATA[u]
        if d.x == p.x and d.y == p.y and (d.hp > 0 or d.mech) then return u end
    end
    return nil
end
MOCK.pawn_at = pawn_at

local function find_pawn(id)
    for _, u in ipairs(MOCK.pawns) do
        if DATA[u].id == id then return u end
    end
    return nil
end
MOCK.find_pawn = find_pawn
MOCK.data = function(u) return DATA[u] end

local function remove_pawn(u)
    for i, v in ipairs(MOCK.pawns) do
        if rawequal(v, u) then table.remove(MOCK.pawns, i) return end
    end
end

---------------------------------------------------------------- Pawn methods
local function getter(name, field)
    pawn_methods[name] = bind("Pawn", name, {{}}, function(self) return DATA[self][field] end)
end
getter("GetHealth", "hp")
getter("GetTeam", "team")
getter("GetType", "type")
getter("GetId", "id")
getter("IsMech", "mech")
getter("IsFire", "fire")
getter("IsAcid", "acid")
getter("IsShield", "shield")
getter("IsFrozen", "frozen")
getter("IsBoosted", "boosted")
getter("IsInfected", "infected")
getter("IsPowered", "powered")
getter("IsSelected", "selected")
getter("GetMoveSpeed", "move")
getter("GetBaseMove", "move")
getter("GetSelectedWeapon", "selected_weapon")
local function static_flag(name, lua_field)
    pawn_methods[name] = bind("Pawn", name, {{}}, function(self)
        local def = _G[DATA[self].type] or {}
        return def[lua_field] == true
    end)
end
static_flag("IsFlying", "Flying")
static_flag("IsJumper", "Jumper")
static_flag("IsBurrower", "Burrows")
static_flag("IsCorpse", "Corpse")
pawn_methods.IsGuarding = bind("Pawn", "IsGuarding", {{}}, function(self)
    local def = _G[DATA[self].type] or {}
    return def.Pushable == false
end)
pawn_methods.IsRanged = bind("Pawn", "IsRanged", {{}}, function(self)
    local def = _G[DATA[self].type] or {}
    return def.Ranged == 1
end)
pawn_methods.IsGrappled = bind("Pawn", "IsGrappled", {{}}, function() return false end)
pawn_methods.IsDead = bind("Pawn", "IsDead", {{}}, function(self) return DATA[self].hp < 1 end)
pawn_methods.IsActive = bind("Pawn", "IsActive", {{}}, function(self)
    local d = DATA[self]
    return d.active and d.hp > 0 and d.powered
end)
pawn_methods.GetSpace = bind("Pawn", "GetSpace", {{}}, function(self)
    local d = DATA[self]
    return P(d.x, d.y)
end)
pawn_methods.GetShotsRemaining = bind("Pawn", "GetShotsRemaining", {{}}, function(self)
    return DATA[self].shots or 1
end)
pawn_methods.IsAbility = bind("Pawn", "IsAbility", {{"string"}}, function() return false end)
-- Pawn::IsBusy: walking, leaping, being pushed. `busy = true` stays busy;
-- `busy_polls = n` is busy for the next n polls.
pawn_methods.IsBusy = bind("Pawn", "IsBusy", {{}}, function(self)
    local d = DATA[self]
    if d.busy then return true end
    if (d.busy_polls or 0) > 0 then
        d.busy_polls = d.busy_polls - 1
        return true
    end
    return false
end)
-- Pawn::GetPathingProfile without the team bits: flyer 1, massive 2, ground 0.
pawn_methods.GetPathProf = bind("Pawn", "GetPathProf", {{}}, function(self)
    local def = _G[DATA[self].type] or {}
    if def.Flying then return PATH_FLYER end
    if def.Massive then return PATH_MASSIVE end
    return PATH_GROUND
end)

local function setter(name, field, t)
    pawn_methods[name] = bind("Pawn", name, {{t}}, function(self, v) DATA[self][field] = v end, true)
end
setter("SetAcid", "acid", "bool")
setter("SetShield", "shield", "bool")
setter("SetFrozen", "frozen", "bool")
setter("SetBoosted", "boosted", "bool")
setter("SetInfected", "infected", "bool")
setter("SetPowered", "powered", "bool")
setter("SetActive", "active", "bool")
setter("SetNeutral", "neutral", "bool")
setter("SetMoveSpeed", "move", "int")
setter("SetMutation", "mutation", "int")
setter("SetTeam", "team", "int")
pawn_methods.SetHealth = bind("Pawn", "SetHealth", {{"int"}}, function(self, v)
    local d = DATA[self]
    d.hp = math.min(v, d.max)
end, true)
pawn_methods.SetSpace = bind("Pawn", "SetSpace", {{"Point"}}, function(self, p)
    local d = DATA[self]
    d.x, d.y = p.x, p.y
    if tile(p).fire then d.fire = true end
end, true)
pawn_methods.Move = bind("Pawn", "Move", {{"Point"}}, function(self, p)
    local d = DATA[self]
    d.x, d.y = p.x, p.y
    return true
end, true)
pawn_methods.ClearQueued = bind("Pawn", "ClearQueued", {{}}, function(self)
    DATA[self].queued = nil
end, true)
-- The native rules the bridge relies on (SkillManager::FireWeapon): frozen
-- -> 0; a weapon whose Lua table has GetTargetArea fires only at a tile of
-- that area, computed with the global `Pawn` as the game does -> else 0; a
-- TwoClick weapon takes a first click (2) then fires on the second (1);
-- slot 50 is the repair skill. A weapon table with MockLeap moves the
-- shooter to the target (stand-in for AddLeap). `selected` records the
-- global Pawn's id at the call.
local function weapon_name(d, slot)
    local def = _G[d.type] or {}
    local list = def.SkillList or {}
    if slot <= #list then return list[slot] end
    return (d.added or {})[slot - #list]
end
pawn_methods.FireWeapon = bind("Pawn", "FireWeapon", {{"Point", "int"}}, function(self, p, slot)
    local d = DATA[self]
    local sel = (kind(Pawn) == "Pawn") and DATA[Pawn].id or nil
    local rec = {id = d.id, slot = slot, x = p.x, y = p.y, selected = sel}
    MOCK.fired[#MOCK.fired + 1] = rec
    if d.frozen and slot ~= 50 then rec.ret = 0 return 0 end
    if slot == 0 then
        if not d.active then rec.ret = 0 return 0 end
        d.x, d.y = p.x, p.y
        d.undo = true
        if tile(p).fire then d.fire = true end
        if MOCK.pawn_busy_after_fire then d.busy_polls = MOCK.pawn_busy_after_fire end
        rec.ret = 1
        return 1
    end
    if d.team == TEAM_ENEMY then
        d.queued = {x = p.x, y = p.y, slot = slot}
        tile(p).targeted = true
        d.selected_weapon = slot
        return 1
    end
    if slot ~= 50 then
        local skill = _G[weapon_name(d, slot) or ""]
        if type(skill) == "table" and type(skill.GetTargetArea) == "function" and not d.first_click then
            local area = skill:GetTargetArea(P(d.x, d.y))
            local inside = false
            for i = 1, area:size() do
                local q = area:index(i)
                if q.x == p.x and q.y == p.y then inside = true end
            end
            if not inside then rec.ret = 0 return 0 end
        end
        if type(skill) == "table" and skill.TwoClick and not d.first_click then
            d.first_click = {x = p.x, y = p.y, slot = slot}
            rec.ret = 2
            return 2
        end
        d.first_click = nil
        if type(skill) == "table" and skill.MockLeap then d.x, d.y = p.x, p.y end
    end
    d.active = false
    if MOCK.fire_busy then MOCK.busy = MOCK.fire_busy end
    if MOCK.pawn_busy_after_fire then d.busy_polls = MOCK.pawn_busy_after_fire end
    rec.ret = 1
    return 1
end, true)
pawn_methods.AddWeapon = bind("Pawn", "AddWeapon", {{"string"}}, function(self, w)
    local d = DATA[self]
    d.added = d.added or {}
    d.added[#d.added + 1] = w
end, true)
pawn_methods.IsUndoPossible = bind("Pawn", "IsUndoPossible", {{}}, function(self)
    return DATA[self].undo == true
end)
pawn_methods.Kill = bind("Pawn", "Kill", {{"bool"}}, function(self) DATA[self].hp = 0 end, true)

---------------------------------------------------------------- Board
local board_methods = {}
local new_board = new_class("Board", function(u, k) return board_methods[k] end)
Board = new_board({})

local function tile_getter(name, fn)
    board_methods[name] = bind("Board", name, {{"Point"}}, function(self, p)
        if not valid(p) then return fn(nil, p) end
        return fn(tile(p), p)
    end)
end
tile_getter("GetTerrain", function(t) return t and t.terrain or 0 end)
tile_getter("IsFire", function(t) return t ~= nil and t.fire end)
tile_getter("IsSmoke", function(t) return t ~= nil and t.smoke end)
tile_getter("IsAcid", function(t) return t ~= nil and t.acid end)
tile_getter("IsCracked", function(t) return t ~= nil and t.cracked end)
tile_getter("IsFrozen", function(t) return t ~= nil and t.frozen end)
tile_getter("GetHealth", function(t) return t and t.hp or 0 end)
tile_getter("IsPod", function(t) return t ~= nil and t.pod == true end)
tile_getter("GetItem", function(t) return t and t.item or "" end)
tile_getter("IsItem", function(t) return t ~= nil and t.item ~= "" end)
tile_getter("IsDangerousItem", function(t) return t ~= nil and t.item ~= "" end)
tile_getter("GetCustomTile", function(t) return t and t.custom or "" end)
tile_getter("IsDangerous", function(t) return t ~= nil and t.dangerous end)
tile_getter("IsTargeted", function(t) return t ~= nil and t.targeted end)
tile_getter("IsEnvironmentDanger", function(t) return t ~= nil and t.env end)
tile_getter("IsUniqueBuilding", function(t) return t ~= nil and t.unique == true end)
tile_getter("IsPowered", function(t)
    return t ~= nil and t.terrain == TERRAIN_BUILDING and t.populated
end)
tile_getter("IsSpawning", function(t, p)
    for _, s in ipairs(MOCK.spawns) do
        if s.x == p.x and s.y == p.y then return true end
    end
    return false
end)
board_methods.IsTerrain = bind("Board", "IsTerrain", {{"Point", "int"}}, function(self, p, t)
    local tl = tile(p)
    if t == TERRAIN_LAVA then return tl.terrain == TERRAIN_WATER and tl.lava end
    return tl.terrain == t
end)
board_methods.IsBlocked = bind("Board", "IsBlocked", {{"Point", "int"}}, function(self, p, prof)
    if not valid(p) then return true end
    local t = tile(p).terrain
    -- Flyers may end on water and chasms (Board::IsBlocked, stage 5 3.4).
    if prof == PATH_FLYER and (t == TERRAIN_WATER or t == TERRAIN_HOLE) then
        return pawn_at(p) ~= nil
    end
    if t == TERRAIN_BUILDING or t == TERRAIN_MOUNTAIN or t == TERRAIN_WATER or t == TERRAIN_HOLE then
        return true
    end
    return pawn_at(p) ~= nil
end)
board_methods.IsPawnSpace = bind("Board", "IsPawnSpace", {{"Point"}, {"Point", "bool"}}, function(self, p)
    return valid(p) and pawn_at(p) ~= nil
end)
board_methods.IsBusy = bind("Board", "IsBusy", {{}}, function()
    if MOCK.busy > 0 then
        MOCK.busy = MOCK.busy - 1
        return true
    end
    return false
end)
board_methods.GetBusyState = bind("Board", "GetBusyState", {{}}, function()
    return MOCK.busy > 0 and 6 or 0
end)
board_methods.GetPawn = bind("Board", "GetPawn", {{"Point"}, {"Point", "bool"}, {"int"}}, function(self, a)
    if type(a) == "number" then return find_pawn(a) end
    if not valid(a) then return nil end
    return pawn_at(a)
end)
board_methods.GetPawns = bind("Board", "GetPawns", {{"int"}}, function(self, team)
    local ids = {}
    for _, u in ipairs(MOCK.pawns) do
        local d = DATA[u]
        local team_ok = team == TEAM_ANY or (team == TEAM_ENEMY and d.team >= 6)
            or (team == TEAM_MECH and (d.mech or d.team == 4)) or d.team == team
        if team_ok and (d.hp > 0 or d.mech) then ids[#ids + 1] = d.id end
    end
    return new_int_list(ids)
end)
board_methods.GetZone = bind("Board", "GetZone", {{"string"}}, function(self, name)
    local pts = {}
    for _, p in ipairs(MOCK.zones[name] or {}) do pts[#pts + 1] = P(p[1], p[2]) end
    return new_point_list(pts)
end)

-- Mutators.
board_methods.SetTerrain = bind("Board", "SetTerrain", {{"Point", "int"}}, function(self, p, t)
    local tl = tile(p)
    if t == TERRAIN_FIRE then tl.fire = true return end
    if t == TERRAIN_ACID then tl.acid = true return end
    if t == TERRAIN_LAVA then tl.lava = true; tl.terrain = TERRAIN_WATER; tl.fire = false return end
    tl.terrain = t
    if t == TERRAIN_MOUNTAIN or t == TERRAIN_ICE then tl.hp, tl.max = 2, 2 end
    if t == TERRAIN_WATER or t == TERRAIN_HOLE then tl.fire = false; tl.cracked = false end
end, true)
board_methods.SetLava = bind("Board", "SetLava", {{"Point", "bool"}}, function(self, p, b)
    local tl = tile(p)
    tl.lava = b
    if b then tl.terrain = TERRAIN_WATER; tl.fire = false end
end, true)
board_methods.SetHealth = bind("Board", "SetHealth", {{"Point", "int", "int"}}, function(self, p, cur, max)
    local tl = tile(p)
    tl.hp, tl.max = cur, max
end, true)
board_methods.SetPopulated = bind("Board", "SetPopulated", {{"bool", "Point"}}, function(self, b, p)
    tile(p).populated = b
end, true)
local function tile_setter(name, field)
    board_methods[name] = bind("Board", name, {{"Point", "bool"}}, function(self, p, b)
        tile(p)[field] = b
    end, true)
end
tile_setter("SetCracked", "cracked")
tile_setter("SetAcid", "acid")
tile_setter("SetFrozen", "frozen")
board_methods.SetSmoke = bind("Board", "SetSmoke", {{"Point", "bool", "bool"}}, function(self, p, b)
    tile(p).smoke = b
end, true)
board_methods.AddShield = bind("Board", "AddShield", {{"Point"}}, function(self, p)
    tile(p).shield = true
end, true)
board_methods.RemoveShield = bind("Board", "RemoveShield", {{"Point"}}, function(self, p)
    tile(p).shield = false
end, true)
board_methods.SetItem = bind("Board", "SetItem", {{"Point", "string"}}, function(self, p, s)
    tile(p).item = s
end, true)
board_methods.SetCustomTile = bind("Board", "SetCustomTile", {{"Point", "string"}}, function(self, p, s)
    tile(p).custom = s
end, true)
board_methods.DamageSpace = bind("Board", "DamageSpace", {{"SpaceDamage"}, {"Point", "int"}}, function(self, sd)
    if kind(sd) ~= "SpaceDamage" then return end
    local p = sd.loc
    if not valid(p) then return end
    local u = pawn_at(p)
    if u and sd.iInjure == EFFECT_CREATE then DATA[u].injured = true end
    if sd.iFire == EFFECT_CREATE then
        tile(p).fire = true
        if u then DATA[u].fire = true end
    elseif sd.iFire == EFFECT_REMOVE then
        tile(p).fire = false
        if u then DATA[u].fire = false end
    end
    if u and sd.iDamage > 0 then DATA[u].hp = DATA[u].hp - sd.iDamage end
end, true)
board_methods.ClearSpace = bind("Board", "ClearSpace", {{"Point"}}, function(self, p)
    local u = pawn_at(p)
    while u do
        remove_pawn(u)
        u = pawn_at(p)
    end
    TILES[p.x][p.y] = {terrain = 0, hp = 0, max = 0, populated = false, fire = false, smoke = false,
                       acid = false, cracked = false, frozen = false, shield = false, item = "",
                       custom = "", lava = false, targeted = false, env = false, dangerous = false}
    for i, s in ipairs(MOCK.spawns) do
        if s.x == p.x and s.y == p.y then table.remove(MOCK.spawns, i) break end
    end
end, true)
board_methods.RemovePawn = bind("Board", "RemovePawn", {{"Point"}, {"Pawn"}}, function(self, a)
    if kind(a) == "Point" then
        local u = pawn_at(a)
        while u do
            remove_pawn(u)
            u = pawn_at(a)
        end
    elseif a ~= nil then
        remove_pawn(a)
    end
end, true)
board_methods.AddPawn = bind("Board", "AddPawn",
    {{"Pawn", "Point"}, {"Pawn"}, {"Pawn", "string"}, {"string"}, {"string", "Point"}, {"string", "string"}},
    function(self, a, b)
        if kind(a) ~= "Pawn" or kind(b) ~= "Point" then error("mock: only AddPawn(Pawn*, Point)") end
        local d = DATA[a]
        d.x, d.y = b.x, b.y
        d.active = true
        MOCK.pawns[#MOCK.pawns + 1] = a
        return b
    end, true)
board_methods.SpawnPawn = bind("Board", "SpawnPawn",
    {{"Pawn", "Point"}, {"Pawn", "string"}, {"Pawn"}, {"string", "Point"}, {"string", "string"}, {"string"}},
    function(self, a, b)
        if type(a) ~= "string" or kind(b) ~= "Point" then error("mock: only SpawnPawn(string, Point)") end
        local id = MOCK.next_id()
        MOCK.spawns[#MOCK.spawns + 1] = {type = a, x = b.x, y = b.y, id = id}
        return id
    end, true)

board_methods.IsValid = bind("Board", "IsValid", {{"Point"}, {"int", "int"}}, function(self, a, b)
    if type(a) == "number" then return valid({x = a, y = b}) end
    return valid(a)
end)
board_methods.IsBuilding = bind("Board", "IsBuilding", {{"Point"}, {"int", "int"}}, function(self, a, b)
    local p = type(a) == "number" and {x = a, y = b} or a
    return valid(p) and tile(p).terrain == TERRAIN_BUILDING
end)
board_methods.IsDamaged = bind("Board", "IsDamaged", {{"Point"}}, function(self, p)
    return valid(p) and tile(p).hp < tile(p).max
end)
board_methods.MarkSpaceImage = bind("Board", "MarkSpaceImage", {{"Point", "string", "GL_Color"}}, function() end)
board_methods.MarkSpaceDesc = bind("Board", "MarkSpaceDesc", {{"Point", "string"}, {"Point", "string", "bool"}}, function() end)
-- The block-spawn map (Board+0x7480): written by BlockSpawn, read only by
-- native code (Board::IsAvailable); Lua cannot see it.
MOCK.spawn_blocks = {}
board_methods.BlockSpawn = bind("Board", "BlockSpawn", {{"Point", "int"}}, function(self, p, n)
    MOCK.spawn_blocks[p.x .. "," .. p.y] = n
end, true)

-- GL_Color (display only).
local new_color = new_class("GL_Color", function() return nil end)
function GL_Color(...)
    local n = select("#", ...)
    if n == 3 or n == 4 then return new_color({...}) end
    local msg = "No matching overload found, candidates: GL_Color(" .. describe({...}, n) .. ")"
    MOCK.mismatches[#MOCK.mismatches + 1] = msg
    error(msg, 2)
end

-- random_int: deterministic LCG, [0, n).
MOCK.rng = 12345
function random_int(a, b)
    if type(a) ~= "number" or (b ~= nil and type(b) ~= "number") then
        local msg = "No matching overload found, candidates: random_int"
        MOCK.mismatches[#MOCK.mismatches + 1] = msg
        error(msg, 2)
    end
    MOCK.rng = (MOCK.rng * 1103515245 + 12345) % 2147483648
    local lo, hi = 0, a
    if b ~= nil then lo, hi = a, b end
    if hi - lo <= 0 then return lo end
    return lo + math.floor(MOCK.rng / 65536) % (hi - lo)
end

---------------------------------------------------------------- SkillEffect
-- Records its entries; Board:AddEffect queues it; MOCK.resolve_effects()
-- applies the queue (the native effect stack) when the harness lets the
-- board go idle. SpaceDamage arguments are copied (C++ takes them by value).
local SE_FIELDS = {piOrigin = true, iOwner = true, impact_sound = true}
local se_methods = {}
local new_se = new_class("SkillEffect", function(u, k)
    if SE_FIELDS[k] then return DATA[u][k] end
    return se_methods[k]
end, {
    __newindex = function(u, k, v)
        if not SE_FIELDS[k] then error("luabind: no writable member " .. tostring(k), 2) end
        DATA[u][k] = v
    end,
})
function SkillEffect(...)
    if select("#", ...) ~= 0 then error("No matching overload found, candidates: SkillEffect()", 2) end
    return new_se({entries = {}, iOwner = 0})
end
local function copy_sd(sd)
    local f = {}
    for k, v in pairs(DATA[sd]) do f[k] = v end
    f.loc = P(sd.loc.x, sd.loc.y)
    return f
end
local function se_add(name, sigs, kind_name, sd_index)
    se_methods[name] = bind("SkillEffect", name, sigs, function(self, ...)
        local args = {...}
        local e = {kind = kind_name}
        if sd_index then e.sd = copy_sd(args[sd_index]) end
        local d = DATA[self].entries
        d[#d + 1] = e
    end)
end
se_add("AddDelay", {{"float"}}, "delay")
se_add("AddSound", {{"string"}}, "sound")
se_add("AddScript", {{"string"}}, "script")
se_add("AddVoice", {{"string", "int"}}, "voice")
se_add("AddDamage", {{"SpaceDamage"}}, "damage", 1)
se_add("AddDropper", {{"SpaceDamage", "string"}}, "dropper", 1)
se_methods.AddArtillery = bind("SkillEffect", "AddArtillery",
    {{"SpaceDamage", "string"}, {"SpaceDamage", "string", "float"}, {"Point", "SpaceDamage", "string", "float"}},
    function(self, a, b)
        local sd = kind(a) == "SpaceDamage" and a or b
        local d = DATA[self].entries
        d[#d + 1] = {kind = "artillery", sd = copy_sd(sd)}
    end)

MOCK.effects = {}
board_methods.AddEffect = bind("Board", "AddEffect", {{"SkillEffect"}, {"SpaceDamage"}}, function(self, e)
    if kind(e) == "SpaceDamage" then
        MOCK.effects[#MOCK.effects + 1] = {{kind = "damage", sd = copy_sd(e)}}
    else
        MOCK.effects[#MOCK.effects + 1] = DATA[e].entries
    end
end, true)

-- Is the space "occupied" for BoardSpace::IsPawnSpace(true) (BoardSpace.c
-- 2078-2105): a living pawn, or a corpse (mech, Corpse pawn) dead or alive.
local function pawn_space_true(p)
    for _, u in ipairs(MOCK.pawns) do
        local d = DATA[u]
        if d.x == p.x and d.y == p.y then
            local def = _G[d.type] or {}
            if d.hp > 0 or d.mech or def.Corpse == true then return u end
        end
    end
    return nil
end

-- BoardSpace::DamageSpace for one SpaceDamage, as far as the tests need:
-- terrain (lava / building), damage, fire. A building lands as the native
-- code does it (BoardSpace.c 19105-19117): `while IsPawnSpace(true) do
-- Kill(first pawn) end; AddBuilding()`. A dead mech stays as a corpse, so
-- the native loop never ends; here it stops after MOCK.native_loop_cap
-- turns and records MOCK.native_hang.
MOCK.native_loop_cap = 10000
local function apply_sd(sd)
    local p = sd.loc
    if not valid(p) then return end
    local tl = tile(p)
    if sd.iTerrain == TERRAIN_BUILDING then
        local n = 0
        local u = pawn_space_true(p)
        while u do
            DATA[u].hp = 0  -- Pawn::Kill; the pawn stays in the space
            n = n + 1
            if n >= MOCK.native_loop_cap then
                MOCK.native_hang = {x = p.x, y = p.y, pawn = DATA[u].id, iterations = n}
                return
            end
            u = pawn_space_true(p)
        end
        tl.terrain, tl.hp, tl.max = TERRAIN_BUILDING, 1, 1
        return
    end
    if sd.iTerrain == TERRAIN_LAVA then
        tl.terrain, tl.lava, tl.fire = TERRAIN_WATER, true, false
    elseif sd.iTerrain ~= nil and sd.iTerrain >= 0 then
        tl.terrain = sd.iTerrain
    end
    local u = pawn_at(p)
    if u and (sd.iDamage or 0) > 0 then DATA[u].hp = math.max(0, DATA[u].hp - sd.iDamage) end
    if sd.iFire == EFFECT_CREATE then
        tl.fire = true
        if u then DATA[u].fire = true end
    end
end
MOCK.apply_sd = apply_sd

function MOCK.resolve_effects()
    local n = 0
    while #MOCK.effects > 0 do
        local entries = table.remove(MOCK.effects, 1)
        for _, e in ipairs(entries) do
            if e.sd then apply_sd(e.sd) end
            if MOCK.native_hang then return n end
        end
        n = n + 1
    end
    MOCK.busy = 0
    return n
end

---------------------------------------------------------------- Game, factory
local game_methods = {}
local new_game = new_class("GameMap", function(u, k) return game_methods[k] end)
Game = new_game({})
game_methods.GetTurnCount = bind("GameMap", "GetTurnCount", {{}}, function() return MOCK.turn end)
game_methods.GetTeamTurn = bind("GameMap", "GetTeamTurn", {{}}, function() return MOCK.team end)
game_methods.GetSector = bind("GameMap", "GetSector", {{}}, function() return 2 end)
game_methods.TriggerSound = bind("GameMap", "TriggerSound", {{"string"}}, function() end)

local factory_methods = {}
local new_factory = new_class("PawnFactory", function(u, k) return factory_methods[k] end)
PAWN_FACTORY = new_factory({})
factory_methods.CreatePawn = bind("PawnFactory", "CreatePawn", {{"string"}, {"string", "int"}},
    function(self, ptype, team)
        local def = _G[ptype] or {}
        local u = new_pawn({
            id = MOCK.next_id(), type = ptype, x = -1, y = -1, hp = def.Health or 1,
            max = def.Health or 1, team = team or def.DefaultTeam or TEAM_NONE, mech = false,
            active = true, move = def.MoveSpeed or 0, fire = false, acid = false, shield = false,
            frozen = false, boosted = false, infected = false, powered = true, neutral = false,
            selected = false, selected_weapon = 0,
        })
        return u
    end)

-- global.lua: the native selection sets the `Pawn` global through this.
function SetPawn(pawn) Pawn = pawn end

function GetDifficulty() return 1 end
-- events.lua / game.lua (the bridge wraps both to see the Grid Defense base).
function CreateEffect(data) return {effect = data} end
function startNewGame() MOCK_NEW_GAMES = (MOCK_NEW_GAMES or 0) + 1 end
function IsRelease() return true end
function IsNewEnemies() return true end
function IsPassiveSkill() return false end
function ConsolePrint() end

---------------------------------------------------------------- classes
function CreateClass(newclass)
    newclass.new = function(self, o)
        o = o or {}
        setmetatable(o, self)
        self.__index = self
        return o
    end
end

MOCK.hook_calls = {}
local function note_hook(name) MOCK.hook_calls[#MOCK.hook_calls + 1] = name end

Mission = {ID = "", TurnLimit = 4, BonusObjs = {}, PowerStart = 0, BlockedSpawns = 0,
           KilledVek = 0, Environment = "Env_Null"}
CreateClass(Mission)
function Mission:BaseUpdate() note_hook("BaseUpdate") end
function Mission:NextTurn() note_hook("NextTurn") end
function Mission:BaseStart() note_hook("BaseStart") end
function Mission:MissionEnd() note_hook("MissionEnd") end
function Mission:BaseDeployment() note_hook("BaseDeployment") end
function Mission:BaseNextTurn()
    note_hook("BaseNextTurn")
    self:NextTurn()
end
function Mission:ApplyEnvironmentEffect()
    note_hook("ApplyEnvironmentEffect")
    return self.LiveEnvironment:ApplyEffect()
end
function Mission:PlanEnvironment()
    note_hook("PlanEnvironment")
    return "plan-result"
end

Environment = {Name = ""}
CreateClass(Environment)
function Environment:ApplyEffect() return false end
Env_Null = Environment:new{}
Env_Attack = Environment:new{Locations = nil, Planned = nil, CurrentAttack = nil, Ordered = false}
-- Deterministic stand-in for random_removal: the last location.
function Env_Attack:ApplyEffect()
    self.CurrentAttack = table.remove(self.Locations)
    return #self.Locations ~= 0
end
Env_Lightning = Env_Attack:new{}
Mission_Lightning = Mission:new{Environment = "Env_Lightning"}

return MOCK
