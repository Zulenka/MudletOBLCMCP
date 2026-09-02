/***************************************************************************
 *   Copyright (C) 2026 by the Mudlet Achaea HUD contributors              *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

// The Achaea HUD's Lua API, split out to keep TLuaInterpreter.cpp's size down.
//
// This file is the whole of what C++ knows about the game: field names. It does
// not read Legacy, AK or Ltracker globals - a small adapter script in the profile
// does that and hands the result to setHudData(), so a third-party system changing
// its table layout costs an adapter edit rather than a rebuild.

#include "Host.h"
#include "THudPanel.h"
#include "TLuaInterpreter.h"

namespace
{
// Achaea sends most GMCP numbers as strings, so accept anything Lua will convert.
// Returns fallback when the field is missing or is not number-like.
double numberField(lua_State* L, const int table, const char* key, const double fallback)
{
    lua_getfield(L, table, key);
    const double value = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : fallback;
    lua_pop(L, 1);
    return value;
}

int intField(lua_State* L, const int table, const char* key, const int fallback)
{
    return static_cast<int>(numberField(L, table, key, static_cast<double>(fallback)));
}

// Tri-state: -1 when the field is absent, otherwise 0 or 1. A boolean, a number and
// the strings the game sends ("1"/"0") all count.
int triStateField(lua_State* L, const int table, const char* key)
{
    lua_getfield(L, table, key);
    int value = -1;
    if (lua_isboolean(L, -1)) {
        value = lua_toboolean(L, -1) ? 1 : 0;
    } else if (lua_isnumber(L, -1)) {
        value = (lua_tonumber(L, -1) != 0.0) ? 1 : 0;
    }
    lua_pop(L, 1);
    return value;
}

bool boolField(lua_State* L, const int table, const char* key)
{
    return triStateField(L, table, key) == 1;
}

QString stringField(lua_State* L, const int table, const char* key)
{
    lua_getfield(L, table, key);
    QString value;
    if (lua_type(L, -1) == LUA_TSTRING) {
        value = QString::fromUtf8(lua_tostring(L, -1));
    }
    lua_pop(L, 1);
    return value;
}

// Which of the three things a top-level section key can mean. Leaves the value on
// the stack when it returns Present, and pops it otherwise.
hud::SectionState openSection(lua_State* L, const int payload, const char* key)
{
    lua_getfield(L, payload, key);
    if (lua_istable(L, -1)) {
        return hud::SectionState::Present;
    }
    // A section set to false is the adapter saying "this is gone", which has to blank
    // the panel rather than leave the last good values up. A nil - much the commoner
    // case, since a vitals tick sends vitals alone - means "I did not look".
    const bool cleared = lua_isboolean(L, -1) && !lua_toboolean(L, -1);
    lua_pop(L, 1);
    return cleared ? hud::SectionState::Cleared : hud::SectionState::Absent;
}

// One entry of an afflictions or defences array. An entry with no name is dropped:
// an unnamed chip is worse than no chip.
bool readAffliction(lua_State* L, const int entry, hud::Affliction& affliction)
{
    affliction.name = stringField(L, entry, "name");
    if (affliction.name.isEmpty()) {
        return false;
    }
    affliction.since = numberField(L, entry, "since", 0.0);
    affliction.confidence = intField(L, entry, "confidence", -1);
    if (affliction.confidence > 100) {
        affliction.confidence = 100;
    }
    return true;
}

void readAfflictionArray(lua_State* L, const int array, QVector<hud::Affliction>& into)
{
    const int count = static_cast<int>(lua_objlen(L, array));
    into.reserve(count);
    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(L, array, i);
        if (lua_istable(L, -1)) {
            hud::Affliction affliction;
            if (readAffliction(L, lua_gettop(L), affliction)) {
                into.append(affliction);
            }
        }
        lua_pop(L, 1);
    }
}

void readLimbArray(lua_State* L, const int array, QVector<hud::Limb>& into)
{
    const int count = static_cast<int>(lua_objlen(L, array));
    into.reserve(count);
    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(L, array, i);
        if (lua_istable(L, -1)) {
            const int entry = lua_gettop(L);
            hud::Limb limb;
            limb.name = stringField(L, entry, "name");
            if (!limb.name.isEmpty()) {
                limb.damage = intField(L, entry, "damage", -1);
                // A counter at or past 100 is a break whether or not the adapter
                // bothered to say so.
                limb.broken = boolField(L, entry, "broken") || (limb.damage >= 100);
                into.append(limb);
            }
        }
        lua_pop(L, 1);
    }
}

void readOccupantArray(lua_State* L, const int array, QVector<hud::Occupant>& into)
{
    const int count = static_cast<int>(lua_objlen(L, array));
    into.reserve(count);
    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(L, array, i);
        if (lua_istable(L, -1)) {
            const int entry = lua_gettop(L);
            hud::Occupant occupant;
            occupant.name = stringField(L, entry, "name");
            if (!occupant.name.isEmpty()) {
                occupant.className = stringField(L, entry, "class");
                occupant.player = boolField(L, entry, "player");
                occupant.enemy = boolField(L, entry, "enemy");
                into.append(occupant);
            }
        }
        lua_pop(L, 1);
    }
}

void readStringArray(lua_State* L, const int array, QStringList& into)
{
    const int count = static_cast<int>(lua_objlen(L, array));
    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(L, array, i);
        if (lua_type(L, -1) == LUA_TSTRING) {
            into.append(QString::fromUtf8(lua_tostring(L, -1)));
        }
        lua_pop(L, 1);
    }
}

void readVitals(lua_State* L, const int section, hud::Vitals& vitals)
{
    vitals.hp = intField(L, section, "hp", -1);
    vitals.maxHp = intField(L, section, "maxhp", -1);
    vitals.mp = intField(L, section, "mp", -1);
    vitals.maxMp = intField(L, section, "maxmp", -1);
    vitals.ep = intField(L, section, "ep", -1);
    vitals.maxEp = intField(L, section, "maxep", -1);
    vitals.wp = intField(L, section, "wp", -1);
    vitals.maxWp = intField(L, section, "maxwp", -1);
    vitals.balance = triStateField(L, section, "balance");
    vitals.equilibrium = triStateField(L, section, "equilibrium");
}

void readTarget(lua_State* L, const int section, hud::Target& target)
{
    target.name = stringField(L, section, "name");
    target.className = stringField(L, section, "class");
    target.health = intField(L, section, "health", -1);

    lua_getfield(L, section, "afflictions");
    if (lua_istable(L, -1)) {
        readAfflictionArray(L, lua_gettop(L), target.afflictions);
    }
    lua_pop(L, 1);

    lua_getfield(L, section, "limbs");
    if (lua_istable(L, -1)) {
        readLimbArray(L, lua_gettop(L), target.limbs);
    }
    lua_pop(L, 1);
}

void readRoom(lua_State* L, const int section, hud::Room& room)
{
    room.name = stringField(L, section, "name");
    room.area = stringField(L, section, "area");

    lua_getfield(L, section, "exits");
    if (lua_istable(L, -1)) {
        readStringArray(L, lua_gettop(L), room.exits);
    }
    lua_pop(L, 1);

    lua_getfield(L, section, "occupants");
    if (lua_istable(L, -1)) {
        readOccupantArray(L, lua_gettop(L), room.occupants);
    }
    lua_pop(L, 1);
}
} // namespace

// Documentation: https://wiki.mudlet.org/w/Manual:Achaea_HUD#setHudData
int TLuaInterpreter::setHudData(lua_State* L)
{
    static const char* sFunc = "setHudData";
    if (!lua_istable(L, 1)) {
        lua_pushfstring(L, "setHudData: bad argument #1 type (data table expected, got %s!)", luaL_typename(L, 1));
        return lua_error(L);
    }

    Host& host = getHostFromLua(L);
    auto* pPanel = host.achaeaHud();
    if (!pPanel) {
        return warnArgumentValue(L, sFunc, "the HUD panel could not be created");
    }

    const int payload = 1;
    hud::Snapshot update;
    update.adapter = stringField(L, payload, "adapter");
    update.generated = numberField(L, payload, "generated", 0.0);

    update.vitals.state = openSection(L, payload, "vitals");
    if (update.vitals.state == hud::SectionState::Present) {
        const int section = lua_gettop(L);
        readVitals(L, section, update.vitals);
        update.vitals.updated = numberField(L, section, "updated", update.generated);
        lua_pop(L, 1);
    }

    update.afflictions.state = openSection(L, payload, "afflictions");
    if (update.afflictions.state == hud::SectionState::Present) {
        const int section = lua_gettop(L);
        readAfflictionArray(L, section, update.afflictions.entries);
        update.afflictions.updated = numberField(L, section, "updated", update.generated);
        lua_pop(L, 1);
    }

    update.defences.state = openSection(L, payload, "defences");
    if (update.defences.state == hud::SectionState::Present) {
        const int section = lua_gettop(L);
        readAfflictionArray(L, section, update.defences.entries);
        update.defences.updated = numberField(L, section, "updated", update.generated);
        lua_pop(L, 1);
    }

    update.target.state = openSection(L, payload, "target");
    if (update.target.state == hud::SectionState::Present) {
        const int section = lua_gettop(L);
        readTarget(L, section, update.target);
        update.target.updated = numberField(L, section, "updated", update.generated);
        lua_pop(L, 1);
    }

    update.room.state = openSection(L, payload, "room");
    if (update.room.state == hud::SectionState::Present) {
        const int section = lua_gettop(L);
        readRoom(L, section, update.room);
        update.room.updated = numberField(L, section, "updated", update.generated);
        lua_pop(L, 1);
    }

    pPanel->applyUpdate(update);
    lua_pushboolean(L, true);
    return 1;
}

// Documentation: https://wiki.mudlet.org/w/Manual:Achaea_HUD#clearHudData
int TLuaInterpreter::clearHudData(lua_State* L)
{
    Host& host = getHostFromLua(L);
    if (auto* pPanel = host.achaeaHudIfPresent()) {
        pPanel->clearAll();
    }
    lua_pushboolean(L, true);
    return 1;
}

// Documentation: https://wiki.mudlet.org/w/Manual:Achaea_HUD#hudVisible
int TLuaInterpreter::hudVisible(lua_State* L)
{
    Host& host = getHostFromLua(L);
    if (lua_gettop(L) < 1) {
        lua_pushboolean(L, host.achaeaHudVisible());
        return 1;
    }

    const bool visible = getVerifiedBool(L, "hudVisible", 1, "visible");
    host.setAchaeaHudVisible(visible);
    lua_pushboolean(L, true);
    return 1;
}
