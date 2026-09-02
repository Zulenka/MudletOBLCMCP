#ifndef MUDLET_THUDDATA_H
#define MUDLET_THUDDATA_H

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

#include <QString>
#include <QStringList>
#include <QVector>

// The whole of what the HUD renders, and nothing about where it came from.
//
// The C++ side deliberately knows about *fields*, not about Legacy, AK or Ltracker:
// a small Lua adapter in the profile reads those and calls setHudData(). When a
// third-party system changes its table layout only the adapter moves.
namespace hud
{
// A payload can carry any subset of the HUD. Absent is not the same as empty:
// "the adapter did not mention the target this time" must not read as "there is
// no target", and "the target is gone" must not read as stale target state.
enum class SectionState {
    // Not in this payload - keep whatever is already displayed.
    Absent = 0,
    // In this payload - replace what is displayed.
    Present,
    // Explicitly cleared by the adapter (section = false) - show "no data".
    Cleared
};

// Common to every section so each one can be aged independently. Vitals arriving
// every prompt says nothing about whether a limb count from 40 seconds ago is
// still worth believing.
struct Section
{
    SectionState state = SectionState::Absent;
    // Epoch seconds, as Mudlet's getEpoch() reports them, of when the adapter
    // last had reason to believe this section. 0 means the adapter did not say.
    double updated = 0.0;
};

// Health/mana/endurance/willpower plus the two balances. -1 means "unknown", which
// renders as "--" rather than as zero: a zero-health bar is a lie about a dead
// character, an empty one is an honest admission.
struct Vitals : Section
{
    int hp = -1;
    int maxHp = -1;
    int mp = -1;
    int maxMp = -1;
    int ep = -1;
    int maxEp = -1;
    int wp = -1;
    int maxWp = -1;
    // Tri-state as well: -1 unknown, 0 off, 1 on.
    int balance = -1;
    int equilibrium = -1;
};

// One affliction or defence. confidence separates fact from inference and is the
// single most important field in this header: own afflictions come from GMCP and
// carry -1 (a fact), target afflictions are guesses the user's own attacks imply
// and carry 0-100.
struct Affliction
{
    QString name;
    // Epoch seconds when it was first believed; 0 if the adapter did not say.
    double since = 0.0;
    // -1 = a fact from the game. 0-100 = an inference, at that confidence.
    int confidence = -1;

    bool inferred() const { return confidence >= 0; }
};

// Per-limb damage on the target. Ltracker-style counters run 0-100 as a percentage
// of the way to a break, and can exceed 100 once broken.
struct Limb
{
    // "head", "torso", "left arm", "right arm", "left leg", "right leg"
    QString name;
    // -1 unknown, otherwise percent-to-break; >= 100 usually means broken.
    int damage = -1;
    bool broken = false;
};

// A player or denizen in the room.
struct Occupant
{
    QString name;
    // Empty unless the adapter knows it, e.g. from Legacy.CT.Enemies.
    QString className;
    bool player = false;
    bool enemy = false;
};

struct AfflictionList : Section
{
    QVector<Affliction> entries;
};

struct Target : Section
{
    QString name;
    QString className;
    // Percent, -1 unknown.
    int health = -1;
    // Inferred - every entry here should carry a confidence.
    QVector<Affliction> afflictions;
    QVector<Limb> limbs;
};

struct Room : Section
{
    QString name;
    QString area;
    QStringList exits;
    QVector<Occupant> occupants;
};

// One setHudData() call, or the accumulated state of the panel. The same type
// serves both: a payload carries SectionState::Present on the sections it
// mentions, and merging is per-section replacement.
struct Snapshot
{
    Vitals vitals;
    // Own afflictions - facts, from GMCP via Legacy.
    AfflictionList afflictions;
    // Own defences - facts.
    AfflictionList defences;
    Target target;
    Room room;

    // Free-form identifier the adapter sets, shown in the panel footer so a
    // stale adapter version is visible rather than merely suspected.
    QString adapter;
    // Epoch seconds the adapter stamped the payload with.
    double generated = 0.0;

    // Per-section replacement. Sections the update leaves Absent are untouched;
    // Cleared wipes to "no data" rather than leaving the last good values up.
    void mergeFrom(const Snapshot& update);
};
} // namespace hud

#endif // MUDLET_THUDDATA_H
