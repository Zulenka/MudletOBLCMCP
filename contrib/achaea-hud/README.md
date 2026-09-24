# Achaea HUD — profile-side scripts

The Lua half of the HUD built behind `USE_ACHAEA_HUD`. These run **inside a Mudlet
profile**, not in the client: install each as a Script (Scripts → Add Item, leave the
Registered Event Handlers box empty, paste the file, save).

They live here because the C++ half is useless without them, and because keeping them
only in a profile directory has already cost real work — an earlier copy silently
reverted and took three weeks of edits with it, while the committed C++ was untouched.
Version control is the point.

## The files

| File | Script name | What it does |
|---|---|---|
| `achaea-hud-adapter.lua` | Achaea HUD adapter | Reads Legacy/AK/Ltracker and GMCP, calls `setHudData()` |
| `achaea-people-here.lua` | Achaea People Here | Geyser panel listing room players and denizens |
| `achaea-hud-declutter.lua` | Achaea HUD Declutter | Hides panels the HUD replaces, reclaims screen borders |

Each prints its version on load, and carries an INSTALL block at the top.

## Why the split

Achaea sends no enemy state over GMCP, so target afflictions and limb damage are
client-side inferences maintained by the player's own scripts. The C++ panel therefore
knows about *fields*, never about Legacy: the adapter reads the third-party tables and
hands over a plain payload, so a third-party layout change costs an adapter edit rather
than a rebuild. This mirrors how `TMCPServer` separates transport from game knowledge.

## Rules these scripts follow

- **Read-only with respect to the game.** No `send()`, no `expandAlias()`, and no timer
  that acts. Iron Realms permits assistance to an attended player and prohibits
  unattended automation; the HUD reports a click and Lua decides, so nothing can fire
  because it *noticed* something.
- **Idempotent across re-saves.** Mudlet re-runs a Script on every save, so unguarded
  `registerAnonymousEventHandler` calls stack silently. Every handler id is kept and
  killed before re-registering.
- **They never write to Legacy.** In particular not to `Legacy/Legacy.lua`, which is
  Legacy's serialized state and holds the curing priorities tuned elsewhere.

## One intrusion, deliberately visible

`achaea-hud-declutter.lua` stops `SelfLimbCounter`'s 0.2s redraw timer before hiding it.
Without that the panel re-shows itself within a fraction of a second and, with the left
border reclaimed, sits on top of the game text. `AchaeaDeclutter.restore()` puts the
panel back and calls its `init()` to restart the loop.

## Field contract

`setHudData()` takes one table; every key is optional. A key **present** replaces that
section, **absent** leaves it alone, and **`false`** clears it. A cleared or never-sent
section is collapsed by the panel and costs no height, which is how the room list moved
out to its own Geyser panel without leaving a stub.

Own afflictions and defences are facts from GMCP and carry no confidence. Target
afflictions are inferences and must carry `confidence` 0–100 — the panel renders them
with a dashed border and confidence-scaled opacity, and an entry in the target section
is treated as a guess whatever confidence it supplies.

See `HUD-HANDOFF.md` for the full payload shape.
