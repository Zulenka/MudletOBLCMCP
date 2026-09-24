-- ===========================================================================
-- Achaea "People Here" panel
-- ---------------------------------------------------------------------------
-- A Geyser panel listing who and what is in the room: players (enemies flagged
-- and coloured by class) and denizens. It takes over the slot the Items panel
-- occupied, and lets the native HUD drop its own Room section, which was making
-- the HUD tall enough to need scrolling.
--
-- A list is the one thing Geyser is genuinely good at, so it belongs here rather
-- than in the painted HUD panel.
--
-- READ-ONLY with respect to the game: no send(), no timers that act, and it
-- never writes to Legacy's tables or its saved state.
--
-- INSTALL (once):
--   1. Mudlet: Scripts -> Add Item.
--   2. Name it "Achaea People Here". Leave Registered Event Handlers empty.
--   3. Paste this whole file in. Save.
--
--   AchaeaPeople.show() / .hide() / .update()
-- ===========================================================================

AchaeaPeople = AchaeaPeople or {}
AchaeaPeople.version = "achaea-people-here 1.0"

-- ---------------------------------------------------------------------------
-- Geometry
-- ---------------------------------------------------------------------------
-- Defaults to the slot Items used, widened to match Map and PlayerInfo above it
-- and stopping short of Chatbox below. Adjustable.Container remembers whatever
-- the user drags it to, so these are first-run values only.
AchaeaPeople.geometry = { x = 1108, y = 410, width = 484, height = 215 }

-- Colours: enemies must be findable at a glance, denizens must not compete with
-- players for attention.
AchaeaPeople.colours = {
  enemy    = "red",
  player   = "cyan",
  denizen  = "ansi_yellow",
  room     = "white",
  exits    = "ansi_cyan",
  heading  = "grey",
}

-- ---------------------------------------------------------------------------
-- Reading the game
-- ---------------------------------------------------------------------------
local function dig(root, ...)
  local node = root
  for _, key in ipairs({...}) do
    if type(node) ~= "table" then return nil end
    node = node[key]
  end
  return node
end

-- Legacy.CT.Enemies is keyed by title-cased name; the value is a class string
-- that is often "" when the class is not yet known.
local function enemyClass(name)
  local enemies = dig(Legacy, "CT", "Enemies")
  if type(enemies) ~= "table" then return false, nil end
  local key = name
  if enemies[key] == nil and type(name) == "string" then
    key = name:sub(1, 1):upper() .. name:sub(2):lower()
  end
  local value = enemies[key]
  if value == nil then return false, nil end
  return true, (type(value) == "string" and value ~= "") and value or nil
end

-- Players come from GMCP. Denizens are not in GMCP at all - Legacy keeps them in
-- Legacy.Room.mobs, keyed by the game's object id, so they are read with pairs()
-- and sorted by name to keep the panel order stable.
local function gather()
  local players, denizens = {}, {}

  local list = dig(gmcp, "Room", "Players")
  if type(list) == "table" then
    for _, entry in ipairs(list) do
      local name = type(entry) == "table" and entry.name or entry
      if type(name) == "string" and name ~= "" then
        local isEnemy, class = enemyClass(name)
        players[#players + 1] = { name = name, enemy = isEnemy, class = class }
      end
    end
  end
  table.sort(players, function(a, b) return a.name < b.name end)

  local mobs = dig(Legacy, "Room", "mobs")
  if type(mobs) == "table" then
    for _, mname in pairs(mobs) do
      if type(mname) == "string" and mname ~= "" then denizens[#denizens + 1] = mname end
    end
  end
  table.sort(denizens)

  return players, denizens
end

-- ---------------------------------------------------------------------------
-- The panel
-- ---------------------------------------------------------------------------
-- Re-saving the script must not stack a second container on top of the first, so
-- an existing one is reused rather than recreated.
local function build()
  if AchaeaPeople.container then return end

  local g = AchaeaPeople.geometry
  local made = false
  if Adjustable and Adjustable.Container then
    local ok, container = pcall(function()
      return Adjustable.Container:new({
        name = "PeopleHere",
        x = g.x, y = g.y, width = g.width, height = g.height,
        titleText = "People Here",
        autoSave = true, autoLoad = true,
      })
    end)
    if ok and container then AchaeaPeople.container = container; made = true end
  end
  if not made then
    -- Adjustable is a package, not core Mudlet, so fall back to a plain container.
    AchaeaPeople.container = Geyser.Container:new({
      name = "PeopleHerePlain",
      x = g.x, y = g.y, width = g.width, height = g.height,
    })
  end

  AchaeaPeople.console = Geyser.MiniConsole:new({
    name = "PeopleHereConsole",
    x = 2, y = 2, width = "100%", height = "100%",
    color = "black",
    fontSize = 9,
    wrapAt = 80,
    scrollBar = false,
  }, AchaeaPeople.container)
  AchaeaPeople.console:setColor(12, 12, 12)
end

-- ---------------------------------------------------------------------------
-- Rendering
-- ---------------------------------------------------------------------------
function AchaeaPeople.update()
  if not AchaeaPeople.console then return end
  local c = AchaeaPeople.colours
  local players, denizens = gather()
  local info = dig(gmcp, "Room", "Info") or {}

  AchaeaPeople.console:clear()

  -- Room line: name, then area only when the game actually gives one.
  local name = type(info.name) == "string" and info.name or "?"
  cecho(AchaeaPeople.console.name, string.format("<%s>%s\n", c.room, name))
  if type(info.area) == "string" and info.area ~= "" then
    cecho(AchaeaPeople.console.name, string.format("<%s>%s\n", c.heading, info.area))
  end

  if type(info.exits) == "table" then
    local dirs = {}
    for dir in pairs(info.exits) do dirs[#dirs + 1] = dir end
    table.sort(dirs)
    if #dirs > 0 then
      cecho(AchaeaPeople.console.name,
        string.format("<%s>exits: <%s>%s\n", c.heading, c.exits, table.concat(dirs, " ")))
    end
  end

  cecho(AchaeaPeople.console.name, string.format("\n<%s>Players (%d)\n", c.heading, #players))
  if #players == 0 then
    cecho(AchaeaPeople.console.name, string.format("<%s>  none\n", c.heading))
  end
  for _, p in ipairs(players) do
    local colour = p.enemy and c.enemy or c.player
    local suffix = ""
    if p.enemy then suffix = p.class and (" [" .. p.class .. "]") or " [enemy]" end
    cecho(AchaeaPeople.console.name, string.format("<%s>  %s%s\n", colour, p.name, suffix))
  end

  cecho(AchaeaPeople.console.name, string.format("\n<%s>Denizens (%d)\n", c.heading, #denizens))
  if #denizens == 0 then
    cecho(AchaeaPeople.console.name, string.format("<%s>  none\n", c.heading))
  end
  for _, d in ipairs(denizens) do
    cecho(AchaeaPeople.console.name, string.format("<%s>  %s\n", c.denizen, d))
  end
end

function AchaeaPeople.show() if AchaeaPeople.container then AchaeaPeople.container:show() end end
function AchaeaPeople.hide() if AchaeaPeople.container then AchaeaPeople.container:hide() end end

-- ---------------------------------------------------------------------------
-- Handlers
-- ---------------------------------------------------------------------------
-- Killed before registering, because Mudlet re-runs a Script on every save and
-- unguarded registrations stack silently.
if AchaeaPeople.handlers then
  for _, id in ipairs(AchaeaPeople.handlers) do killAnonymousEventHandler(id) end
end
AchaeaPeople.handlers = {}

local function on(event)
  AchaeaPeople.handlers[#AchaeaPeople.handlers + 1] =
    registerAnonymousEventHandler(event, function()
      -- Third-party tables are unreliable; nothing may escape into the user's
      -- combat system.
      pcall(AchaeaPeople.update)
    end)
end

for _, ev in ipairs({
  "gmcp.Room.Info", "gmcp.Room.Players",
  "gmcp.Room.AddPlayer", "gmcp.Room.RemovePlayer",
}) do
  on(ev)
end

-- ---------------------------------------------------------------------------
-- Start
-- ---------------------------------------------------------------------------
build()

-- The Items panel occupied this slot and nothing else shows it, so it goes.
local items = Geyser.windowList and Geyser.windowList["Items"]
if items and not items.hidden then pcall(function() items:hide() end) end

pcall(AchaeaPeople.update)
cecho("\n<green>[people here]<reset> panel active (" .. AchaeaPeople.version .. ").\n")
