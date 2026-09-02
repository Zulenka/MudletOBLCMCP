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

#include "THudData.h"

namespace hud
{
// Absent leaves a section alone, Present replaces it, Cleared resets it to empty
// while remembering it was deliberately emptied - so the panel can say "no data"
// instead of leaving the last good values on screen.
void Snapshot::mergeFrom(const Snapshot& update)
{
    switch (update.vitals.state) {
    case SectionState::Absent:
        break;
    case SectionState::Present:
        vitals = update.vitals;
        break;
    case SectionState::Cleared:
        vitals = Vitals{};
        vitals.state = SectionState::Cleared;
        vitals.updated = update.vitals.updated;
        break;
    }

    switch (update.afflictions.state) {
    case SectionState::Absent:
        break;
    case SectionState::Present:
        afflictions = update.afflictions;
        break;
    case SectionState::Cleared:
        afflictions = AfflictionList{};
        afflictions.state = SectionState::Cleared;
        afflictions.updated = update.afflictions.updated;
        break;
    }

    switch (update.defences.state) {
    case SectionState::Absent:
        break;
    case SectionState::Present:
        defences = update.defences;
        break;
    case SectionState::Cleared:
        defences = AfflictionList{};
        defences.state = SectionState::Cleared;
        defences.updated = update.defences.updated;
        break;
    }

    switch (update.target.state) {
    case SectionState::Absent:
        break;
    case SectionState::Present:
        target = update.target;
        break;
    case SectionState::Cleared:
        target = Target{};
        target.state = SectionState::Cleared;
        target.updated = update.target.updated;
        break;
    }

    switch (update.room.state) {
    case SectionState::Absent:
        break;
    case SectionState::Present:
        room = update.room;
        break;
    case SectionState::Cleared:
        room = Room{};
        room.state = SectionState::Cleared;
        room.updated = update.room.updated;
        break;
    }

    if (!update.adapter.isEmpty()) {
        adapter = update.adapter;
    }
    if (update.generated > 0.0) {
        generated = update.generated;
    }
}
} // namespace hud
