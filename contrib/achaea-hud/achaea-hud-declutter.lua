-- ===========================================================================
-- Achaea HUD declutter
-- ---------------------------------------------------------------------------
-- Hides Geyser panels whose information the native HUD panel already shows, plus
-- a couple of empty orphan containers, to give the game text back its screen.
--
-- It HIDES at runtime. It does not delete anything, does not edit any package,
-- and above all does not write to Legacy/Legacy.lua - that file is Legacy's
-- serialized state and holds the curing priorities another session is actively
-- tuning. Everything here is reversible with AchaeaDeclutter.restore().
--
-- INSTALL (once):
--   1. Mudlet: Scripts -> Add Item.
--   2. Name it "Achaea HUD declutter". Leave Registered Event Handlers empty.
--   3. Paste this whole file in. Save.
--
--   AchaeaDeclutter.restore()  -- put everything back
--   AchaeaDeclutter.apply()    -- hide again
--   AchaeaDeclutter.report()   -- what is hidden, and what space it freed
-- ===========================================================================

AchaeaDeclutter = AchaeaDeclutter or {}
AchaeaDeclutter.version = "achaea-hud-declutter 1.2"

-- ---------------------------------------------------------------------------
-- What to hide, and why
-- ---------------------------------------------------------------------------
-- Only panels the HUD genuinely replaces, plus empty containers. Deliberately
-- NOT listed: Items, Chatbox, PlayerInfo, Map and poopDeck - nothing in the HUD
-- covers those.
--
-- SelfLimbCounter WAS excluded, because the HUD's figure showed the target's
-- limbs and this one shows your own - a different fact. The HUD gained its own
-- "Your limbs" section in adapter 1.4, so it is now a real duplicate.
AchaeaDeclutter.targets = {
  -- Duplicated by the HUD's Vitals section.
  { name = "HPContainer",        why = "HUD Vitals shows HP" },
  { name = "MPContainer",        why = "HUD Vitals shows MP" },
  -- Duplicated by the HUD's own-afflictions section.
  { name = "Aff Tracking",       why = "HUD Afflictions" },
  -- Duplicated by the HUD's Room section, which lists players and denizens.
  { name = "WhoHere",            why = "HUD Room > Players" },
  -- Empty containers: chrome with no content in it.
  { name = "left_container_top", why = "empty container" },
  { name = "anon_window_0",      why = "unnamed empty VBox" },
  -- Duplicated by the HUD's "Your limbs" section (adapter 1.4 and later).
  { name = "SelfLimbCounter.window", why = "HUD Your limbs", stopUpdater = "SelfLimbCounter" },
}

-- ---------------------------------------------------------------------------
-- Panels that re-show themselves
-- ---------------------------------------------------------------------------
-- SelfLimbCounter redraws on a 0.2s timer and calls show() each time, so hiding
-- it alone does not stick - it reappears within a fraction of a second, and with
-- the left border reclaimed it lands on top of the game text. Stopping that timer
-- is the only thing that makes the hide hold.
--
-- This reaches into another package's state, which is why it is spelled out here
-- rather than buried: restore() puts the panel back and calls its init() to bring
-- the redraw loop back with it. If that ever fails, reloading the limb package or
-- restarting Mudlet restores it cleanly.
local function stopUpdater(name)
  local pkg = _G[name]
  if type(pkg) ~= "table" or pkg._updater == nil then return end
  pcall(function() killTimer(pkg._updater) end)
  pkg._updater = nil
end

local function startUpdater(name)
  local pkg = _G[name]
  if type(pkg) ~= "table" then return end
  if type(pkg.init) == "function" then pcall(pkg.init) end
end

-- ---------------------------------------------------------------------------
-- Helpers
-- ---------------------------------------------------------------------------
-- Geyser.windowList is the only registry that sees every container regardless of
-- which package made it, so names are resolved through it rather than through a
-- package's own table.
local function find(name)
  return Geyser.windowList and Geyser.windowList[name] or nil
end

local function area(w)
  local ok, width, height = pcall(function() return w:get_width(), w:get_height() end)
  if not ok or type(width) ~= "number" or type(height) ~= "number" then return 0 end
  return math.floor(width) * math.floor(height)
end

-- Bottom border
-- ---------------------------------------------------------------------------
-- The HP/MP bars lived in the bottom border strip. With them hidden that strip
-- is dead space and the console can have it back. Only the bottom is touched:
-- the right border is genuinely occupied by Chatbox, Items, Map and PlayerInfo,
-- and the left by SelfLimbCounter.
--
-- Legacy manages borders too, so if it resets this on a window resize, just
-- re-run AchaeaDeclutter.apply().
AchaeaDeclutter.bottomBorder = 8

-- The left strip held only SelfLimbCounter and two empty containers, all now
-- hidden. poopDeck is a userwindow rather than a border panel, so it floats over
-- this area instead of reserving it - if it is in the way, hide it separately.
AchaeaDeclutter.leftBorder = 8

-- ---------------------------------------------------------------------------
-- ---------------------------------------------------------------------------
-- Apply / restore
-- ---------------------------------------------------------------------------
-- Records what was visible BEFORE hiding, so restore() only re-shows things this
-- script actually hid and never un-hides something the user had hidden already.
function AchaeaDeclutter.apply()
  AchaeaDeclutter.hidden = AchaeaDeclutter.hidden or {}
  local freed, count = 0, 0
  for _, entry in ipairs(AchaeaDeclutter.targets) do
    local w = find(entry.name)
    if w and not w.hidden then
      freed = freed + area(w)
      count = count + 1
      AchaeaDeclutter.hidden[entry.name] = true
      if entry.stopUpdater then stopUpdater(entry.stopUpdater) end
      pcall(function() w:hide() end)
    end
  end
  -- Remember the original border once, so restore() puts back exactly what was there.
  if AchaeaDeclutter.priorLeftBorder == nil then
    AchaeaDeclutter.priorLeftBorder = getBorderLeft()
  end
  if AchaeaDeclutter.priorBottomBorder == nil then
    AchaeaDeclutter.priorBottomBorder = getBorderBottom()
  end
  setBorderBottom(AchaeaDeclutter.bottomBorder)
  setBorderLeft(AchaeaDeclutter.leftBorder)

  AchaeaDeclutter.freed = freed
  cecho(string.format("\n<green>[declutter]<reset> hid %d panel(s), freeing ~%d px%s.\n",
    count, freed, "\194\178"))
  return count, freed
end

function AchaeaDeclutter.restore()
  local count = 0
  for name in pairs(AchaeaDeclutter.hidden or {}) do
    local w = find(name)
    if w then
      count = count + 1
      pcall(function() w:show() end)
      for _, entry in ipairs(AchaeaDeclutter.targets) do
        if entry.name == name and entry.stopUpdater then startUpdater(entry.stopUpdater) end
      end
    end
  end
  AchaeaDeclutter.hidden = {}
  if AchaeaDeclutter.priorBottomBorder then
    setBorderBottom(AchaeaDeclutter.priorBottomBorder)
  end
  if AchaeaDeclutter.priorLeftBorder then
    setBorderLeft(AchaeaDeclutter.priorLeftBorder)
  end
  cecho(string.format("\n<green>[declutter]<reset> restored %d panel(s).\n", count))
  return count
end

function AchaeaDeclutter.report()
  cecho("\n<cyan>[declutter]<reset> " .. AchaeaDeclutter.version .. "\n")
  for _, entry in ipairs(AchaeaDeclutter.targets) do
    local w = find(entry.name)
    local state
    if not w then state = "absent"
    elseif w.hidden then state = "hidden"
    else state = "VISIBLE" end
    cecho(string.format("  %-22s %-8s  %s\n", entry.name, state, entry.why))
  end
end

-- ---------------------------------------------------------------------------
-- Holding the borders
-- ---------------------------------------------------------------------------
-- Legacy manages borders too and resets them after its own load and on every
-- window resize, which would otherwise undo the reclaim. Re-assert on the same
-- event rather than polling for it.
if AchaeaDeclutter.handlers then
  for _, id in ipairs(AchaeaDeclutter.handlers) do killAnonymousEventHandler(id) end
end
AchaeaDeclutter.handlers = {}
AchaeaDeclutter.handlers[1] = registerAnonymousEventHandler("sysWindowResizeEvent", function()
  if AchaeaDeclutter.hidden and next(AchaeaDeclutter.hidden) then
    setBorderLeft(AchaeaDeclutter.leftBorder)
    setBorderBottom(AchaeaDeclutter.bottomBorder)
  end
end)

AchaeaDeclutter.apply()
