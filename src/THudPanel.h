#ifndef MUDLET_THUDPANEL_H
#define MUDLET_THUDPANEL_H

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

#include <QWidget>

class QColor;
class QLabel;
class QTimer;

// The Achaea HUD dock: one always-visible column of vitals, own afflictions and
// defences, target (inferred afflictions and limb damage), and room.
//
// Three things about this widget are contractual rather than stylistic:
//
//  - It never talks to the game. The only thing it tells the outside world is
//    elementActivated(), and the only code that emits that is the two mouse-release
//    handlers. Nothing on a timer, nothing in applyUpdate(), nothing driven by a
//    change of state may fire it, so the panel can never send a command on the
//    player's behalf.
//  - It never shows data it cannot vouch for. A section that was cleared, or that
//    was never set, renders an explicit "no data" placeholder; a section past its
//    staleness threshold is greyed and marked; inferred target afflictions are
//    drawn dashed and faded by confidence so they can never be mistaken for the
//    facts that own afflictions are.
//  - It is cheap. applyUpdate() runs on the same thread as the Lua engine and may
//    be called on every prompt, so it does the merge and arms a coalescing timer;
//    every repaint happens from that timer, at no more than 10Hz.
class THudPanel : public QWidget
{
    Q_OBJECT

public:
    Q_DISABLE_COPY(THudPanel)
    explicit THudPanel(QWidget* parent = nullptr);
    ~THudPanel();

    // Merge a (possibly partial) payload and schedule a repaint. Called from the
    // Lua binding, potentially on every game prompt. Must be cheap.
    void applyUpdate(const hud::Snapshot& update);
    // Wipe every section to "no data".
    void clearAll();
    const hud::Snapshot& snapshot() const;

    QSize sizeHint() const override;

signals:
    // Emitted ONLY from a real mouse click on a HUD element. kind is one of
    // "limb", "target-affliction", "own-affliction", "defence", "occupant";
    // id is the element's name, e.g. "left leg" or "asthma".
    void elementActivated(const QString& kind, const QString& id);

private:
    class BarWidget;
    class PipsWidget;
    class ChipFlow;
    class BodyDiagram;

    // One titled block: heading row (title plus age), the real content, and the
    // placeholder that stands in for it when there is nothing to show.
    struct SectionBox
    {
        QWidget* container = nullptr;
        QLabel* title = nullptr;
        QLabel* age = nullptr;
        QWidget* body = nullptr;
        QLabel* placeholder = nullptr;
        bool stale = false;
    };

    // The single emission point for elementActivated. Only ChipFlow's and
    // BodyDiagram's mouse-release handlers call it - see the class comment.
    void activateElement(const QString& kind, const QString& id);

    void buildUi();
    SectionBox makeSection(QWidget* parent, const QString& title);
    void scheduleRefresh();
    void refresh();
    void tick();

    void refreshVitals(double now);
    void refreshOwnLists(double now);
    void refreshTarget(double now);
    void refreshRoom(double now);
    void refreshFooter(double now);
    void setBarValues(BarWidget* bar, const QString& caption, int current, int maximum, const QColor& colour, bool dimmed);

    // Static so the nested chip flow, which has no Q_OBJECT of its own, can borrow
    // THudPanel's translation context for the ages it prints on affliction chips.
    static QString formatAge(double seconds);

    // Rewrites the age label and returns true when the staleness verdict flipped,
    // which is the only thing the one-second tick can change that needs a repaint.
    bool refreshHeading(SectionBox& box, const hud::Section& section, int staleAfterSeconds, double now);
    static void showBody(SectionBox& box, bool haveData);

    hud::Snapshot mSnapshot;

    QTimer* mpRefreshTimer = nullptr;
    QTimer* mpTickTimer = nullptr;

    SectionBox mVitalsBox;
    SectionBox mAfflictionsBox;
    SectionBox mDefencesBox;
    SectionBox mTargetBox;
    SectionBox mRoomBox;

    BarWidget* mpHpBar = nullptr;
    BarWidget* mpMpBar = nullptr;
    BarWidget* mpEpBar = nullptr;
    BarWidget* mpWpBar = nullptr;
    PipsWidget* mpPips = nullptr;

    ChipFlow* mpOwnAfflictions = nullptr;
    ChipFlow* mpDefences = nullptr;

    QLabel* mpTargetSummary = nullptr;
    BarWidget* mpTargetHealth = nullptr;
    QLabel* mpInferredHeading = nullptr;
    ChipFlow* mpTargetAfflictions = nullptr;
    BodyDiagram* mpBodyDiagram = nullptr;

    QLabel* mpRoomSummary = nullptr;
    QLabel* mpExits = nullptr;
    QLabel* mpPlayersHeading = nullptr;
    ChipFlow* mpPlayers = nullptr;
    QLabel* mpDenizensHeading = nullptr;
    ChipFlow* mpDenizens = nullptr;

    QLabel* mpFooter = nullptr;
};

#endif // MUDLET_THUDPANEL_H
