-- ===========================================================================
-- Achaea HUD adapter
-- ---------------------------------------------------------------------------
-- Reads the Lua state your existing third-party systems already maintain
-- (Legacy, AK/affstrack, the limb tracker, GMCP) and hands it to the fork's
-- native HUD panel via setHudData(). It is READ-ONLY with respect to the game:
-- it never calls send(), expandAlias() or any command, and it registers no
-- timer that acts. It also never modifies Legacy, AK or Ltracker state.
--
-- When Legacy updates and moves a table, only THIS file changes.
--
-- INSTALL (once):
--   1. In Mudlet: Scripts (the gear/scroll icon) -> Add Item.
--   2. Name it "Achaea HUD adapter". Leave the "Registered Event Handlers"
--      box empty (this script registers its own handlers in code).
--   3. Paste this whole file into the big code box. Click the green Save/checkmark.
--   4. It starts feeding the HUD immediately and on every Mudlet restart.
--
-- Requires a Mudlet build with the HUD panel compiled in. On stock Mudlet it
-- prints one line and does nothing else.
-- ===========================================================================

AchaeaHUD = AchaeaHUD or {}

AchaeaHUD.version = "achaea-hud-adapter 1.6"

-- Vitals fire on every prompt; this is the same floor the MCP forwarder uses.
AchaeaHUD.throttle = 0.25

-- Which sections this adapter feeds. A section that is never sent is collapsed
-- by the panel and takes no space, which is how the room moved out to the Geyser
-- "People Here" panel without leaving a gap behind. Setting one to false here is
-- not the same as clearing it: clearing says "this is gone", omitting says "I do
-- not report this at all".
AchaeaHUD.sections = AchaeaHUD.sections or {
  vitals      = true,
  limbs       = true,
  afflictions = true,
  defences    = true,
  target      = true,
  room        = false, -- shown by the People Here panel instead
}

-- How often an UNCHANGED section is re-sent purely to refresh its timestamp.
-- The panel ages each section independently, and a fact that has not changed is
-- still true: without this, defences you have had up for ten minutes render as
-- ten minutes stale.
AchaeaHUD.refresh = 2.0

-- Avoid stacking duplicate handlers if you re-save the script. This runs BEFORE
-- the setHudData check so that re-saving under a stock Mudlet also cleans up
-- handlers left behind by a previous HUD-enabled session.
-- (No tempTimer is used anywhere in this file, so there is no timer id to kill.)
if AchaeaHUD.handlers then
  for _, id in ipairs(AchaeaHUD.handlers) do killAnonymousEventHandler(id) end
end
AchaeaHUD.handlers = {}

if setHudData == nil then
  cecho("\n<yellow>[Achaea HUD]<reset> setHudData() is not available - this Mudlet was built without the HUD panel. Adapter idle.\n")
  return
end

-- State survives a re-save so the panel does not flash "everything is new".
AchaeaHUD._since   = AchaeaHUD._since   or {} -- first-seen epochs, per section
AchaeaHUD._sig     = AchaeaHUD._sig     or {} -- last-sent signature, per section
AchaeaHUD._sent    = AchaeaHUD._sent    or {} -- last-sent epoch, per section
AchaeaHUD._players = AchaeaHUD._players or {} -- room occupants we believe present
-- These three are guarded for the same reason. _roomId in particular: resetting it
-- makes the next Room.Info look like a room change, which clears the occupant list
-- even though the player never moved.
AchaeaHUD._lastVitals = AchaeaHUD._lastVitals or 0
AchaeaHUD._roomDirty  = true -- a re-save should always repush the room once
AchaeaHUD._roomId     = AchaeaHUD._roomId

local LIMBS = { "head", "torso", "left arm", "right arm", "left leg", "right leg" }

-- ---------------------------------------------------------------------------
-- helpers
-- ---------------------------------------------------------------------------

-- Every table below belongs to third-party code and may be absent or reshaped.
local function dig(root, ...)
  local node = root
  for _, key in ipairs({ ... }) do
    if type(node) ~= "table" then return nil end
    node = node[key]
  end
  return node
end

local function titled(name)
  local ok, t = pcall(function() return name:title() end)
  if ok and type(t) == "string" then return t end
  return name
end

-- Legacy.CT.Enemies[Name] present => enemy. The value is a class string, and is
-- often "" before the class has been observed - that is unknown, not a class.
local function enemyClass(name)
  local enemies = dig(Legacy, "CT", "Enemies")
  if type(enemies) ~= "table" then return false, nil end
  local cls = enemies[titled(name)]
  if cls == nil then return false, nil end
  if type(cls) == "string" and cls ~= "" then return true, cls end
  return true, nil
end

local function push(payload)
  payload.adapter = AchaeaHUD.version
  payload.generated = getEpoch()
  setHudData(payload)
end

-- ---------------------------------------------------------------------------
-- sections
-- ---------------------------------------------------------------------------

-- gmcp.Char.Vitals: Achaea sends every number as a STRING, and bal/eq as "1"/"0".
-- Verified against a captured payload: hp maxhp mp maxmp ep maxep wp maxwp bal eq.
local function buildVitals()
  local v = dig(gmcp, "Char", "Vitals")
  if type(v) ~= "table" then return nil end
  return {
    hp = tonumber(v.hp), maxhp = tonumber(v.maxhp),
    mp = tonumber(v.mp), maxmp = tonumber(v.maxmp),
    ep = tonumber(v.ep), maxep = tonumber(v.maxep),
    wp = tonumber(v.wp), maxwp = tonumber(v.maxwp),
    balance     = v.bal == "1",
    equilibrium = v.eq == "1",
    -- When the game last told us, NOT when we last read the cached table. gmcp
    -- keeps the previous payload forever, so stamping this "now" on a later
    -- read would report vitals as fresh after the game stopped sending them.
    updated = AchaeaHUD._vitalsAt or getEpoch(),
  }
end

-- OWN afflictions and defences are facts from the game, so no confidence field.
-- Legacy stores them as plain boolean maps with no timestamps, so we keep our
-- own first-seen table and drop names that disappear.
local function buildFactMap(slot, map)
  if type(map) ~= "table" then return nil, nil end
  local now = getEpoch()
  local names = {}
  for name, on in pairs(map) do
    if on and type(name) == "string" then names[#names + 1] = name end
  end
  table.sort(names)
  local seen, fresh = AchaeaHUD._since[slot] or {}, {}
  local out = { updated = now }
  for _, name in ipairs(names) do
    local since = seen[name] or now
    fresh[name] = since
    out[#out + 1] = { name = name, since = since }
  end
  AchaeaHUD._since[slot] = fresh
  return out, table.concat(names, ",")
end

-- Reads one person's limb counters out of lb. The table is keyed by character
-- name, so this serves both the player and the target; only the key differs.
-- lb, lb[name] and .hits can each be nil, and percent-to-break can exceed 100.
local function buildLimbs(name)
  local hits = dig(lb, name, "hits")
  if type(hits) ~= "table" then return nil, nil end
  local limbs, sig = {}, {}
  for _, part in ipairs(LIMBS) do
    local dmg = tonumber(hits[part])
    if dmg then
      limbs[#limbs + 1] = { name = part, damage = dmg, broken = dmg >= 100 }
      sig[#sig + 1] = part .. "=" .. dmg
    end
  end
  if #limbs == 0 then return nil, nil end
  limbs.updated = getEpoch()
  return limbs, table.concat(sig, ",")
end

-- Our own limb damage, keyed by our own name rather than the target's.
local function buildOwnLimbs()
  local me = dig(gmcp, "Char", "Status", "name") or dig(gmcp, "Char", "Name", "name")
  if type(me) ~= "string" or me == "" then return nil, nil end
  return buildLimbs(me)
end

-- TARGET afflictions are INFERENCES from the user's own attacks, so every entry
-- carries a confidence. affstrack scores are NOT bounded at 100 in this profile
-- (aflame gets compared against 300 and 400), so clamp into the 0-100 contract.
local function buildTarget()
  local name = target
  if type(name) ~= "string" or name == "" then return nil, nil end

  local now = getEpoch()
  local t = { updated = now, name = name }
  local sig = { name }

  local _, cls = enemyClass(name)
  if cls then
    t.class = cls
    sig[#sig + 1] = cls
  end

  local hpperc = dig(gmcp, "IRE", "Target", "Info", "hpperc")
  if type(hpperc) == "string" then
    local h = tonumber((hpperc:gsub("%%", "")))
    -- Achaea parks hpperc at "-1" when there is no live target.
    if h and h >= 0 then
      t.health = h
      sig[#sig + 1] = "hp" .. h
    end
  end

  local score = dig(affstrack, "score")
  if type(score) == "table" then
    local names = {}
    for aff, val in pairs(score) do
      if type(aff) == "string" and type(val) == "number" and val > 0 then
        names[#names + 1] = aff
      end
    end
    table.sort(names)
    local seen, fresh = AchaeaHUD._since.targetAffs or {}, {}
    local affs = {}
    for _, aff in ipairs(names) do
      local conf = score[aff]
      if conf > 100 then conf = 100 end
      local since = seen[aff] or now
      fresh[aff] = since
      affs[#affs + 1] = { name = aff, confidence = conf, since = since }
      sig[#sig + 1] = aff .. "=" .. conf
    end
    AchaeaHUD._since.targetAffs = fresh
    t.afflictions = affs
  end

  local limbs, limbSig = buildLimbs(name)
  if limbs then
    t.limbs = limbs
    sig[#sig + 1] = limbSig
  end

  return t, table.concat(sig, ";")
end

-- Occupants: gmcp.Room.Players carries players only. Denizens are not in it and
-- this profile has no reliable denizen list, so we send the players and omit
-- denizens rather than guessing at them.
local function buildRoom()
  local info = dig(gmcp, "Room", "Info")
  if type(info) ~= "table" then return nil, nil end

  local r = { updated = getEpoch() }
  local sig = {}
  if type(info.name) == "string" then r.name = info.name; sig[#sig + 1] = info.name end
  if type(info.area) == "string" then r.area = info.area; sig[#sig + 1] = info.area end

  if type(info.exits) == "table" then
    local exits = {}
    for dir in pairs(info.exits) do
      if type(dir) == "string" then exits[#exits + 1] = dir end
    end
    table.sort(exits)
    r.exits = exits
    sig[#sig + 1] = table.concat(exits, ",")
  end

  local occupants = {}
  for _, pname in ipairs(AchaeaHUD._players) do
    local entry = { name = pname, player = true }
    local isEnemy, cls = enemyClass(pname)
    if isEnemy then
      entry.enemy = true
      if cls then entry.class = cls end
    end
    occupants[#occupants + 1] = entry
    sig[#sig + 1] = pname .. "/" .. tostring(cls)
  end
  -- Denizens. Legacy keeps them in Legacy.Room.mobs, keyed by the game's object
  -- id, which is why they are read with pairs() and then sorted by name: the
  -- table has no array part, so the key order is arbitrary and would otherwise
  -- churn the signature on every read and force a needless repaint.
  local mobs = dig(Legacy, "Room", "mobs")
  if type(mobs) == "table" then
    local names = {}
    for _, mname in pairs(mobs) do
      if type(mname) == "string" and mname ~= "" then names[#names + 1] = mname end
    end
    table.sort(names)
    for _, mname in ipairs(names) do
      occupants[#occupants + 1] = { name = mname, player = false }
      sig[#sig + 1] = "m:" .. mname
    end
  end

  r.occupants = occupants

  return r, table.concat(sig, ";")
end

-- ---------------------------------------------------------------------------
-- occupant bookkeeping
-- ---------------------------------------------------------------------------

local function playersReset()
  local list = {}
  local players = dig(gmcp, "Room", "Players")
  if type(players) == "table" then
    for _, p in ipairs(players) do
      if type(p) == "table" and type(p.name) == "string" then
        list[#list + 1] = p.name
      end
    end
  end
  AchaeaHUD._players = list
end

local function playerAdd(name)
  if type(name) ~= "string" or name == "" then return end
  for _, existing in ipairs(AchaeaHUD._players) do
    if existing == name then return end
  end
  AchaeaHUD._players[#AchaeaHUD._players + 1] = name
end

-- Room.AddPlayer arrives as a table ({name=, fullname=}), but Room.RemovePlayer
-- arrives as a bare name string. Accept either shape rather than assuming one.
local function occupantName(value)
  if type(value) == "string" then return value end
  if type(value) == "table" and type(value.name) == "string" then return value.name end
  return nil
end

local function playerRemove(name)
  if name == nil then return end
  for i, existing in ipairs(AchaeaHUD._players) do
    if existing == name then
      table.remove(AchaeaHUD._players, i)
      return
    end
  end
end

-- ---------------------------------------------------------------------------
-- send paths
-- ---------------------------------------------------------------------------

-- Only sections that actually changed go into the payload, so a vitals tick
-- usually sends `vitals` alone. A section that has gone away is sent as `false`
-- so the panel blanks instead of showing stale data.
-- Returns the section to send, or nil to leave it out of the payload.
-- Sends when the contents actually changed, and also when the section's
-- timestamp is due for a refresh, so still-true data does not decay into
-- "stale". A section that has GONE (value nil) is cleared once and not re-sent.
local function changed(slot, value, sig)
  local now = getEpoch()
  local due = (now - (AchaeaHUD._sent[slot] or 0)) >= AchaeaHUD.refresh
  if sig == AchaeaHUD._sig[slot] and not (due and value ~= nil) then return nil end
  AchaeaHUD._sig[slot] = sig
  AchaeaHUD._sent[slot] = now
  if value == nil then return false end
  return value
end

function AchaeaHUD.flushRoom()
  if not AchaeaHUD.sections.room then return end
  local room, sig = buildRoom()
  local section = changed("room", room, sig)
  if section ~= nil then push{ room = section } end
  AchaeaHUD._roomDirty = false
end

-- A section switched off mid-session is still Present in the panel from earlier
-- payloads, and "absent from this payload" means KEEP. So a disabled section has
-- to be cleared once, explicitly, or it sits there frozen for ever.
local SECTION_KEYS = {
  vitals = "vitals", limbs = "limbs", afflictions = "afflictions",
  defences = "defences", target = "target", room = "room",
}

local function clearDisabledSections()
  AchaeaHUD._cleared = AchaeaHUD._cleared or {}
  local payload, dirty = {}, false
  for switch, key in pairs(SECTION_KEYS) do
    if not AchaeaHUD.sections[switch] and not AchaeaHUD._cleared[switch] then
      payload[key] = false
      AchaeaHUD._cleared[switch] = true
      AchaeaHUD._sig[switch] = nil
      dirty = true
    elseif AchaeaHUD.sections[switch] then
      -- Re-enabled: allow a future disable to clear it again.
      AchaeaHUD._cleared[switch] = nil
    end
  end
  if dirty then push(payload) end
end

function AchaeaHUD.tick()
  clearDisabledSections()

  local payload, dirty = {}, false

  local vitals = AchaeaHUD.sections.vitals and buildVitals() or nil
  if vitals then
    payload.vitals = vitals
    dirty = true
  end

  local affs, affSig
  if AchaeaHUD.sections.afflictions then
    affs, affSig = buildFactMap("affs", dig(Legacy, "Curing", "Affs"))
  end
  local section = changed("affs", affs, affSig)
  if AchaeaHUD.sections.afflictions and section ~= nil then
    payload.afflictions = section
    dirty = true
  end

  -- .current is the live defence set; Legacy.Curing.Defs itself also holds
  -- .all, .temp and saved sets, which are NOT what is up right now.
  local defs, defSig
  if AchaeaHUD.sections.defences then
    defs, defSig = buildFactMap("defs", dig(Legacy, "Curing", "Defs", "current"))
  end
  section = changed("defs", defs, defSig)
  if AchaeaHUD.sections.defences and section ~= nil then
    payload.defences = section
    dirty = true
  end

  local ownLimbs, ownLimbSig
  if AchaeaHUD.sections.limbs then ownLimbs, ownLimbSig = buildOwnLimbs() end
  section = changed("ownlimbs", ownLimbs, ownLimbSig)
  if AchaeaHUD.sections.limbs and section ~= nil then
    payload.limbs = section
    dirty = true
  end

  local tgt, tgtSig
  if AchaeaHUD.sections.target then tgt, tgtSig = buildTarget() end
  local tgtName = tgt and tgt.name or nil
  if tgtName ~= AchaeaHUD._lastTarget then
    -- New target: the previous target's inference ages are meaningless.
    AchaeaHUD._since.targetAffs = nil
    AchaeaHUD._lastTarget = tgtName
    tgt, tgtSig = buildTarget()
  end
  section = changed("target", tgt, tgtSig)
  if AchaeaHUD.sections.target and section ~= nil then
    payload.target = section
    dirty = true
  end

  if dirty then push(payload) end
  -- flushRoom() is self-gating via changed(): it sends when the room actually
  -- changed, or when its timestamp is due, and otherwise does nothing. A room
  -- you have not left should not decay into "stale".
  AchaeaHUD.flushRoom()
end

-- ---------------------------------------------------------------------------
-- handlers
-- ---------------------------------------------------------------------------

local function on(event, fn)
  AchaeaHUD.handlers[#AchaeaHUD.handlers + 1] =
    registerAnonymousEventHandler(event, function(...)
      -- Third-party tables are unreliable, and a nil-index here would throw
      -- inside the user's combat system, so nothing is allowed to escape.
      local ok, err = pcall(fn, ...)
      if not ok then AchaeaHUD.lastError = err end
    end)
end

on("gmcp.Char.Vitals", function()
  local now = getEpoch()
  -- Recorded before the throttle: this is when the data arrived, and that stays
  -- true whether or not this particular event turns into a repaint.
  AchaeaHUD._vitalsAt = now
  if now - AchaeaHUD._lastVitals < AchaeaHUD.throttle then return end
  AchaeaHUD._lastVitals = now
  AchaeaHUD.tick()
end)

-- The same GMCP messages Legacy and AK key off. Legacy updates its own tables
-- from these, so ticking here keeps the panel in step with Legacy rather than
-- with a clock - no polling timer anywhere in this file. Ordering between our
-- handler and Legacy's is not guaranteed, but the next prompt re-reads
-- everything, so at worst a section is one event behind for a fraction of a
-- second.
for _, ev in ipairs({
  "gmcp.Char.Afflictions.Add", "gmcp.Char.Afflictions.Remove", "gmcp.Char.Afflictions.List",
  "gmcp.Char.Defences.Add",    "gmcp.Char.Defences.Remove",    "gmcp.Char.Defences.List",
  "gmcp.IRE.Target.Info",      "gmcp.Char.Status",
}) do
  on(ev, function() AchaeaHUD.tick() end)
end

-- A room change is structural, so it goes out immediately. Room.Players follows
-- the move, so drop the old occupants rather than showing them in the new room.
on("gmcp.Room.Info", function()
  local info = dig(gmcp, "Room", "Info")
  local id = tostring(dig(info, "num")) .. "|" .. tostring(dig(info, "name"))
  if id ~= AchaeaHUD._roomId then
    AchaeaHUD._roomId = id
    AchaeaHUD._players = {}
  end
  AchaeaHUD.flushRoom()
end)

-- Occupant churn is folded into the throttled tick, so a busy room cannot make
-- the panel repaint faster than four times a second.
on("gmcp.Room.Players", function()
  playersReset()
  AchaeaHUD._roomDirty = true
end)

on("gmcp.Room.AddPlayer", function()
  playerAdd(occupantName(dig(gmcp, "Room", "AddPlayer")))
  AchaeaHUD._roomDirty = true
end)

on("gmcp.Room.RemovePlayer", function()
  playerRemove(occupantName(dig(gmcp, "Room", "RemovePlayer")))
  AchaeaHUD._roomDirty = true
end)

-- ---------------------------------------------------------------------------
-- Optional: clicking a HUD element. DISABLED BY DEFAULT.
-- ---------------------------------------------------------------------------
-- The panel raises "hudElementActivated" with (kind, id) when - and ONLY when -
-- the user physically clicks a HUD element. Uncommenting this turns a limb click
-- into a command, which is no different from the aliases and keybinds already in
-- this profile.
--
-- It must stay click-driven. Never wire this to a tempTimer or to a game event:
-- Iron Realms permits assistance to an attended player and prohibits unattended
-- automation, and "it sent because it noticed something" is the wrong side of
-- that line. For the same reason none of the adapter's own handlers above may
-- ever be given a send() - they fire on the game, not on you.
--
-- on("hudElementActivated", function(_, kind, id)
--   if kind == "limb" and type(target) == "string" and target ~= "" then
--     send("raze " .. target .. " " .. id, false)
--   end
-- end)

pcall(AchaeaHUD.tick)
cecho("\n<green>[Achaea HUD]<reset> adapter active (" .. AchaeaHUD.version .. ").\n")
