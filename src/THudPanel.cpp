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

#include "THudPanel.h"

#include "utils.h"

#include <QColor>
#include <QDateTime>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QScrollArea>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>

// How long a section's data stays believable, which is not the same question for
// every section.
//
// Own afflictions, defences and the room are FACTS pushed over GMCP. The game
// sends a message when they change, so silence means "still true", not "no longer
// known" - ageing them into grey would be the panel lying about its own evidence.
// They carry csmNeverStale and only ever show their age.
//
// The target's afflictions and limb counts are INFERENCES from the user's own
// attacks. Nothing confirms them and nothing retracts them, so they really do decay
// with time, and saying so is the entire point of the distinction.
//
// Vitals sit in between: they arrive with every prompt, so a long silence does mean
// the game stopped talking - but a character standing still legitimately produces no
// prompts, so the threshold is generous rather than twitchy.
static constexpr int csmNeverStale = 0;
static constexpr int csmVitalsStaleSeconds = 60;
static constexpr int csmListStaleSeconds = csmNeverStale;
static constexpr int csmTargetStaleSeconds = 30;
static constexpr int csmRoomStaleSeconds = csmNeverStale;

// Repaints are capped at this interval; the age labels tick at the slower one.
static constexpr int csmRefreshMs = 100;
static constexpr int csmTickMs = 1000;

static constexpr int csmDiagramHeight = 152;

static double epochNow()
{
    return static_cast<double>(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
}

static QColor blendColours(const QColor& from, const QColor& to, qreal amount)
{
    const qreal t = qBound(0.0, amount, 1.0);
    return QColor::fromRgbF(from.redF() + (to.redF() - from.redF()) * t, from.greenF() + (to.greenF() - from.greenF()) * t, from.blueF() + (to.blueF() - from.blueF()) * t);
}

// Stale evidence must not look as convincing as fresh evidence, so everything a
// stale section draws is pulled most of the way to grey.
static QColor fadeColour(const QColor& colour, bool dimmed)
{
    return dimmed ? blendColours(colour, QColor(128, 128, 128), 0.62) : colour;
}

// FNV-1a rather than qHash: qHash is salted per process, so the same class would get
// a different colour on every launch and the "same class, same colour" promise would
// only hold within one session.
static quint32 stableHash(const QString& text)
{
    quint32 hash = 2166136261u;
    const QByteArray bytes = text.toLower().toUtf8();
    for (const char byte : bytes) {
        hash ^= static_cast<quint32>(static_cast<unsigned char>(byte));
        hash *= 16777619u;
    }
    return hash;
}

static QColor classColour(const QString& className)
{
    if (className.isEmpty()) {
        return QColor(122, 130, 140);
    }
    return QColor::fromHsv(static_cast<int>(stableHash(className) % 360u), 145, 195);
}

// Neutral while untouched, amber through the middle, red as a break approaches, and
// something else entirely once broken - a broken limb is a different fact, not just
// a worse number.
static QColor limbColour(int damage, bool broken)
{
    if (broken) {
        return QColor(176, 96, 208);
    }
    if (damage < 0) {
        return QColor(96, 102, 112);
    }
    const int bounded = qBound(0, damage, 100);
    if (bounded <= 50) {
        return blendColours(QColor(86, 104, 126), QColor(214, 166, 62), bounded / 50.0);
    }
    return blendColours(QColor(214, 166, 62), QColor(206, 68, 58), (bounded - 50) / 50.0);
}

static QColor contrastInk(const QColor& background)
{
    return background.lightnessF() > 0.55 ? QColor(20, 22, 26) : QColor(238, 240, 244);
}

static QFont smallerFont(const QFont& base, qreal delta)
{
    QFont font = base;
    if (font.pointSizeF() > 0.0) {
        font.setPointSizeF(qMax(6.0, font.pointSizeF() - delta));
    }
    return font;
}


// A single labelled bar: caption on the left, a value string on the right, and a fill
// behind both. A percent of -1 means unknown and draws an empty track - never a
// zero-length fill, which would read as "dead" rather than as "we do not know".
class THudPanel::BarWidget : public QWidget
{
public:
    explicit BarWidget(QWidget* parent = nullptr)
    : QWidget(parent)
    {
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    }

    void setBar(const QString& caption, int percent, const QString& valueText, const QColor& colour, bool dimmed)
    {
        if (caption == mCaption && percent == mPercent && valueText == mValueText && colour == mColour && dimmed == mDimmed) {
            return;
        }
        mCaption = caption;
        mPercent = percent;
        mValueText = valueText;
        mColour = colour;
        mDimmed = dimmed;
        update();
    }

    QSize sizeHint() const override { return QSize(160, QFontMetrics(font()).height() + 6); }

    QSize minimumSizeHint() const override { return QSize(70, QFontMetrics(font()).height() + 6); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QColor ink = fadeColour(palette().color(QPalette::WindowText), mDimmed);
        const QColor track = blendColours(palette().color(QPalette::Base), palette().color(QPalette::WindowText), 0.14);

        const QRectF bar(0.0, 0.0, static_cast<qreal>(width()), static_cast<qreal>(height()));
        painter.setPen(Qt::NoPen);
        painter.setBrush(track);
        painter.drawRoundedRect(bar, 3.0, 3.0);

        if (mPercent >= 0) {
            QColor fill = fadeColour(mColour, mDimmed);
            fill.setAlpha(mDimmed ? 105 : 170);
            QPainterPath clip;
            clip.addRoundedRect(bar, 3.0, 3.0);
            painter.save();
            painter.setClipPath(clip);
            painter.setBrush(fill);
            painter.drawRect(QRectF(0.0, 0.0, bar.width() * qBound(0, mPercent, 100) / 100.0, bar.height()));
            painter.restore();
        }

        painter.setPen(ink);
        const QRectF textArea = bar.adjusted(5.0, 0.0, -5.0, 0.0);
        painter.drawText(textArea, Qt::AlignLeft | Qt::AlignVCenter, mCaption);
        painter.drawText(textArea, Qt::AlignRight | Qt::AlignVCenter, mValueText);
    }

private:
    QString mCaption;
    QString mValueText;
    QColor mColour;
    int mPercent = -1;
    bool mDimmed = false;
};


// The balance and equilibrium indicators. Tri-state, and the unknown state is drawn
// hollow rather than as "off": not knowing whether you have balance is not the same
// as knowing that you do not.
class THudPanel::PipsWidget : public QWidget
{
public:
    explicit PipsWidget(QWidget* parent = nullptr)
    : QWidget(parent)
    {
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    }

    void setPips(int balance, int equilibrium, const QString& balanceCaption, const QString& equilibriumCaption, bool dimmed)
    {
        if (balance == mBalance && equilibrium == mEquilibrium && balanceCaption == mBalanceCaption && equilibriumCaption == mEquilibriumCaption && dimmed == mDimmed) {
            return;
        }
        mBalance = balance;
        mEquilibrium = equilibrium;
        mBalanceCaption = balanceCaption;
        mEquilibriumCaption = equilibriumCaption;
        mDimmed = dimmed;
        update();
    }

    QSize sizeHint() const override { return QSize(160, QFontMetrics(font()).height() + 4); }

    QSize minimumSizeHint() const override { return QSize(80, QFontMetrics(font()).height() + 4); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QFontMetrics metrics(font());
        const qreal afterBalance = drawPip(painter, metrics, 1.0, mBalance, mBalanceCaption);
        drawPip(painter, metrics, afterBalance + 14.0, mEquilibrium, mEquilibriumCaption);
    }

private:
    qreal drawPip(QPainter& painter, const QFontMetrics& metrics, qreal x, int state, const QString& caption)
    {
        const qreal radius = 4.5;
        const qreal centreY = height() / 2.0;
        const QRectF dot(x, centreY - radius, radius * 2.0, radius * 2.0);

        const QColor ink = fadeColour(palette().color(QPalette::WindowText), mDimmed);
        if (state < 0) {
            QColor outline = ink;
            outline.setAlpha(110);
            painter.setPen(QPen(outline, 1.0));
            painter.setBrush(Qt::NoBrush);
        } else {
            painter.setPen(Qt::NoPen);
            painter.setBrush(fadeColour(state > 0 ? QColor(92, 176, 112) : QColor(198, 84, 74), mDimmed));
        }
        painter.drawEllipse(dot);

        QColor textInk = ink;
        if (state < 0) {
            textInk.setAlpha(140);
        }
        painter.setPen(textInk);
        const qreal textX = dot.right() + 4.0;
        const qreal textWidth = metrics.horizontalAdvance(caption);
        painter.drawText(QRectF(textX, 0.0, textWidth + 2.0, height()), Qt::AlignLeft | Qt::AlignVCenter, caption);
        return textX + textWidth;
    }

    QString mBalanceCaption;
    QString mEquilibriumCaption;
    int mBalance = -1;
    int mEquilibrium = -1;
    bool mDimmed = false;
};


// A wrapping row of small chips, painted rather than built out of child widgets: one
// widget and one paint pass beats a pool of QLabels that has to be grown, shrunk and
// restyled every time an affliction list changes.
//
// It carries the "fact versus guess" distinction. A solid chip is something the game
// told us; a dashed, confidence-faded chip is something one of our own attacks merely
// implies.
class THudPanel::ChipFlow : public QWidget
{
public:
    struct Chip
    {
        QString id;
        QString text;
        QColor colour;
        // Inferred rather than known - drawn with a dashed border.
        bool dashed = false;
        // Hostile - drawn with a heavier warning border on top of its own colour.
        bool flagged = false;
        int alpha = 200;
    };

    ChipFlow(THudPanel* panel, const QString& kind, QWidget* parent = nullptr)
    : QWidget(parent)
    , mpPanel(panel)
    , mKind(kind)
    {
        QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Minimum);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
        setCursor(Qt::PointingHandCursor);
    }

    void setChips(const QVector<Chip>& chips, bool dimmed)
    {
        const int previousHeight = layoutChips(width());
        mChips = chips;
        mDimmed = dimmed;
        mPressedIndex = -1;
        if (layoutChips(width()) != previousHeight) {
            updateGeometry();
        }
        update();
    }

    // Shown when the section has data and that data is an empty list. "none" is a
    // fact, and must not be confused with the section's "no data" placeholder.
    void setEmptyText(const QString& text)
    {
        mEmptyText = text;
        update();
    }

    static QVector<Chip> afflictionChips(const QVector<hud::Affliction>& entries, const QColor& factColour, double now, bool showAge)
    {
        QVector<Chip> chips;
        chips.reserve(entries.size());
        for (const hud::Affliction& entry : entries) {
            Chip chip;
            chip.id = entry.name;
            chip.dashed = entry.inferred();
            chip.colour = factColour;
            // Confidence drives opacity as well as the printed number: a 25% guess
            // must not sit on the screen as boldly as a 95% one.
            chip.alpha = chip.dashed ? qBound(55, 30 + qBound(0, entry.confidence, 100) * 2, 235) : 205;

            const QString age = (showAge && entry.since > 0.0) ? THudPanel::formatAge(now - entry.since) : QString();
            if (chip.dashed) {
                const QString confidence = entry.confidence >= 0 ? qsl("%1%").arg(entry.confidence) : qsl("?");
                chip.text = age.isEmpty() ? qsl("%1  %2").arg(entry.name, confidence) : qsl("%1  %2  %3").arg(entry.name, confidence, age);
            } else {
                chip.text = age.isEmpty() ? entry.name : qsl("%1  %2").arg(entry.name, age);
            }
            chips.append(chip);
        }
        return chips;
    }

    static QVector<Chip> occupantChips(const QVector<hud::Occupant>& occupants)
    {
        QVector<Chip> chips;
        chips.reserve(occupants.size());
        for (const hud::Occupant& occupant : occupants) {
            Chip chip;
            chip.id = occupant.name;
            chip.colour = classColour(occupant.className);
            chip.flagged = occupant.enemy;
            chip.alpha = 190;
            // The enemy marker is spelled out as well as coloured, because a border
            // colour on its own is not a flag everyone can see.
            const QString name = occupant.enemy ? qsl("! %1").arg(occupant.name) : occupant.name;
            chip.text = occupant.className.isEmpty() ? name : qsl("%1  (%2)").arg(name, occupant.className);
            chips.append(chip);
        }
        return chips;
    }

    bool hasHeightForWidth() const override { return true; }

    int heightForWidth(int width) const override { return layoutChips(width); }

    QSize sizeHint() const override
    {
        const int usable = width() > 0 ? width() : 280;
        return QSize(usable, layoutChips(usable));
    }

    QSize minimumSizeHint() const override { return QSize(70, rowHeight()); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QColor ink = fadeColour(palette().color(QPalette::WindowText), mDimmed);

        if (mChips.isEmpty()) {
            QColor faint = ink;
            faint.setAlpha(120);
            painter.setPen(faint);
            painter.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter, mEmptyText);
            return;
        }

        layoutChips(width());
        const QFontMetrics metrics(font());
        for (int index = 0; index < mChips.size() && index < mRects.size(); ++index) {
            const Chip& chip = mChips.at(index);
            const QRect& box = mRects.at(index);

            QColor fill = fadeColour(chip.colour, mDimmed);
            fill.setAlpha(mDimmed ? qMin(chip.alpha, 105) : chip.alpha);

            QPen pen(chip.flagged ? fadeColour(QColor(214, 78, 70), mDimmed) : fadeColour(chip.colour, mDimmed).darker(145));
            pen.setWidthF(chip.flagged ? 1.8 : 1.0);
            pen.setStyle(chip.dashed ? Qt::DashLine : Qt::SolidLine);
            painter.setPen(pen);
            painter.setBrush(fill);
            painter.drawRoundedRect(QRectF(box).adjusted(0.5, 0.5, -0.5, -0.5), 4.0, 4.0);

            QColor textInk = ink;
            textInk.setAlpha(mDimmed ? 150 : qBound(130, chip.alpha + 55, 255));
            painter.setPen(textInk);
            const QRect textBox = box.adjusted(csmChipPadding, 0, -csmChipPadding, 0);
            painter.drawText(textBox, Qt::AlignLeft | Qt::AlignVCenter, metrics.elidedText(chip.text, Qt::ElideRight, textBox.width()));
        }
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        mPressedIndex = event->button() == Qt::LeftButton ? chipIndexAt(event->position().toPoint()) : -1;
        QWidget::mousePressEvent(event);
    }

    // One of exactly two places in this file that reach elementActivated, and it is
    // reached only by a left press and a left release landing on the same chip.
    void mouseReleaseEvent(QMouseEvent* event) override
    {
        const int pressed = mPressedIndex;
        mPressedIndex = -1;
        QWidget::mouseReleaseEvent(event);
        if (!mpPanel || event->button() != Qt::LeftButton || pressed < 0 || pressed >= mChips.size()) {
            return;
        }
        if (chipIndexAt(event->position().toPoint()) != pressed) {
            return;
        }
        mpPanel->activateElement(mKind, mChips.at(pressed).id);
    }

    void resizeEvent(QResizeEvent* event) override
    {
        QWidget::resizeEvent(event);
        updateGeometry();
    }

    bool event(QEvent* incoming) override
    {
        if (incoming->type() == QEvent::ToolTip) {
            auto* help = static_cast<QHelpEvent*>(incoming);
            const int index = chipIndexAt(help->pos());
            if (index >= 0) {
                QToolTip::showText(help->globalPos(), mChips.at(index).id, this);
            } else {
                QToolTip::hideText();
                incoming->ignore();
            }
            return true;
        }
        return QWidget::event(incoming);
    }

private:
    static constexpr int csmChipPadding = 7;
    static constexpr int csmChipGap = 4;

    int rowHeight() const { return QFontMetrics(font()).height() + 6; }

    // Lays the chips out for a given width, caches the rectangles for painting and
    // hit testing, and returns the total height the flow needs.
    int layoutChips(int width) const
    {
        mRects.clear();
        const int height = rowHeight();
        if (mChips.isEmpty()) {
            return height;
        }

        mRects.reserve(mChips.size());
        const QFontMetrics metrics(font());
        const int available = qMax(40, width);
        int x = 0;
        int y = 0;
        for (const Chip& chip : mChips) {
            const int chipWidth = qMin(metrics.horizontalAdvance(chip.text) + 2 * csmChipPadding, available);
            if (x > 0 && x + chipWidth > available) {
                x = 0;
                y += height + csmChipGap;
            }
            mRects.append(QRect(x, y, chipWidth, height));
            x += chipWidth + csmChipGap;
        }
        return y + height;
    }

    int chipIndexAt(const QPoint& position) const
    {
        layoutChips(width());
        for (int index = 0; index < mRects.size(); ++index) {
            if (mRects.at(index).contains(position)) {
                return index;
            }
        }
        return -1;
    }

    THudPanel* mpPanel = nullptr;
    QString mKind;
    QString mEmptyText;
    QVector<Chip> mChips;
    mutable QVector<QRect> mRects;
    bool mDimmed = false;
    int mPressedIndex = -1;
};


// A stick figure of the target, drawn from behind so that the target's left limb sits
// on the viewer's left and no mental mirroring is needed mid-fight. Hovering a limb
// names it, so that convention never has to be guessed at.
class THudPanel::BodyDiagram : public QWidget
{
public:
    explicit BodyDiagram(THudPanel* panel, QWidget* parent = nullptr)
    : QWidget(parent)
    , mpPanel(panel)
    {
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        setCursor(Qt::PointingHandCursor);
        setFixedHeight(csmDiagramHeight);
        static const char* const names[] = {"head", "torso", "left arm", "right arm", "left leg", "right leg"};
        for (const char* const name : names) {
            Part part;
            part.name = QString::fromLatin1(name);
            mParts.append(part);
        }
        rebuildGeometry();
    }

    void setLimbs(const QVector<hud::Limb>& limbs, bool dimmed)
    {
        // Anything the payload did not mention goes back to unknown rather than
        // keeping the number it had a moment ago.
        for (Part& part : mParts) {
            part.damage = -1;
            part.broken = false;
        }
        for (const hud::Limb& limb : limbs) {
            const QString wanted = limb.name.trimmed().toLower();
            for (Part& part : mParts) {
                if (part.name == wanted) {
                    part.damage = limb.damage;
                    part.broken = limb.broken;
                    break;
                }
            }
        }
        mDimmed = dimmed;
        update();
    }

    QSize sizeHint() const override { return QSize(200, csmDiagramHeight); }

    QSize minimumSizeHint() const override { return QSize(130, csmDiagramHeight); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setFont(smallerFont(font(), 1.0));
        const QFontMetrics metrics(painter.font());
        const QColor ink = fadeColour(palette().color(QPalette::WindowText), mDimmed);

        for (const Part& part : mParts) {
            const QColor fill = fadeColour(limbColour(part.damage, part.broken), mDimmed);
            painter.setPen(QPen(fill.darker(150), 1.0));
            painter.setBrush(fill);
            painter.drawPath(part.path);

            painter.setPen(part.labelOutside ? ink : contrastInk(fill));
            const QString label = metrics.elidedText(labelFor(part), Qt::ElideRight, qRound(part.labelRect.width()));
            painter.drawText(part.labelRect, part.labelAlign | Qt::AlignVCenter, label);
        }
    }

    void resizeEvent(QResizeEvent* event) override
    {
        QWidget::resizeEvent(event);
        rebuildGeometry();
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        mPressedIndex = event->button() == Qt::LeftButton ? partIndexAt(event->position().toPoint()) : -1;
        QWidget::mousePressEvent(event);
    }

    // The second and last place in this file that reaches elementActivated.
    void mouseReleaseEvent(QMouseEvent* event) override
    {
        const int pressed = mPressedIndex;
        mPressedIndex = -1;
        QWidget::mouseReleaseEvent(event);
        if (!mpPanel || event->button() != Qt::LeftButton || pressed < 0 || pressed >= mParts.size()) {
            return;
        }
        if (partIndexAt(event->position().toPoint()) != pressed) {
            return;
        }
        mpPanel->activateElement(qsl("limb"), mParts.at(pressed).name);
    }

    bool event(QEvent* incoming) override
    {
        if (incoming->type() == QEvent::ToolTip) {
            auto* help = static_cast<QHelpEvent*>(incoming);
            const int index = partIndexAt(help->pos());
            if (index >= 0) {
                QToolTip::showText(help->globalPos(), mParts.at(index).name, this);
            } else {
                QToolTip::hideText();
                incoming->ignore();
            }
            return true;
        }
        return QWidget::event(incoming);
    }

private:
    struct Part
    {
        QString name;
        QPainterPath path;
        QRectF labelRect;
        int labelAlign = Qt::AlignHCenter;
        // Arms are too narrow to hold a number, so theirs is printed beside them and
        // has to take the panel's ink colour rather than the limb's contrast colour.
        bool labelOutside = false;
        int damage = -1;
        bool broken = false;
    };

    static QString labelFor(const Part& part)
    {
        if (part.damage < 0) {
            return part.broken ? qsl("!") : qsl("?");
        }
        return part.broken ? qsl("%1!").arg(part.damage) : QString::number(part.damage);
    }

    void rebuildGeometry()
    {
        if (mParts.size() < 6) {
            return;
        }
        const qreal centreX = width() / 2.0;
        const qreal top = 8.0;

        QPainterPath head;
        head.addEllipse(QPointF(centreX, top + 14.0), 12.0, 13.0);
        setPart(0, head, QRectF(centreX - 13.0, top + 4.0, 26.0, 20.0), Qt::AlignHCenter, false);

        QPainterPath torso;
        torso.addRoundedRect(QRectF(centreX - 17.0, top + 30.0, 34.0, 46.0), 4.0, 4.0);
        setPart(1, torso, QRectF(centreX - 17.0, top + 44.0, 34.0, 18.0), Qt::AlignHCenter, false);

        QPainterPath leftArm;
        leftArm.addRoundedRect(QRectF(centreX - 32.0, top + 32.0, 12.0, 44.0), 4.0, 4.0);
        setPart(2, leftArm, QRectF(centreX - 78.0, top + 46.0, 42.0, 16.0), Qt::AlignRight, true);

        QPainterPath rightArm;
        rightArm.addRoundedRect(QRectF(centreX + 20.0, top + 32.0, 12.0, 44.0), 4.0, 4.0);
        setPart(3, rightArm, QRectF(centreX + 36.0, top + 46.0, 42.0, 16.0), Qt::AlignLeft, true);

        QPainterPath leftLeg;
        leftLeg.addRoundedRect(QRectF(centreX - 18.0, top + 78.0, 15.0, 52.0), 4.0, 4.0);
        setPart(4, leftLeg, QRectF(centreX - 19.0, top + 96.0, 17.0, 16.0), Qt::AlignHCenter, false);

        QPainterPath rightLeg;
        rightLeg.addRoundedRect(QRectF(centreX + 3.0, top + 78.0, 15.0, 52.0), 4.0, 4.0);
        setPart(5, rightLeg, QRectF(centreX + 2.0, top + 96.0, 17.0, 16.0), Qt::AlignHCenter, false);
    }

    void setPart(int index, const QPainterPath& path, const QRectF& labelRect, int align, bool outside)
    {
        Part& part = mParts[index];
        part.path = path;
        part.labelRect = labelRect;
        part.labelAlign = align;
        part.labelOutside = outside;
    }

    int partIndexAt(const QPoint& position) const
    {
        for (int index = 0; index < mParts.size(); ++index) {
            if (mParts.at(index).path.contains(QPointF(position))) {
                return index;
            }
        }
        return -1;
    }

    THudPanel* mpPanel = nullptr;
    QVector<Part> mParts;
    bool mDimmed = false;
    int mPressedIndex = -1;
};


THudPanel::THudPanel(QWidget* parent)
: QWidget(parent)
, mpRefreshTimer(new QTimer(this))
, mpTickTimer(new QTimer(this))
{
    setObjectName(qsl("hudPanel"));
    setMinimumWidth(200);
    setFont(smallerFont(font(), 1.0));

    buildUi();

    mpRefreshTimer->setSingleShot(true);
    mpRefreshTimer->setInterval(csmRefreshMs);
    connect(mpRefreshTimer, &QTimer::timeout, this, &THudPanel::refresh);

    // Ages have to keep counting up while nothing is arriving - that silence is
    // exactly when the user most needs to see how old the numbers are.
    mpTickTimer->setInterval(csmTickMs);
    connect(mpTickTimer, &QTimer::timeout, this, &THudPanel::tick);
    mpTickTimer->start();

    refresh();
}

THudPanel::~THudPanel() = default;

QSize THudPanel::sizeHint() const
{
    return QSize(320, 620);
}

void THudPanel::applyUpdate(const hud::Snapshot& update)
{
    mSnapshot.mergeFrom(update);
    scheduleRefresh();
}

void THudPanel::clearAll()
{
    const QString adapter = mSnapshot.adapter;
    mSnapshot = hud::Snapshot{};
    // Cleared rather than Absent: the panel is saying "there is nothing here", not
    // "nobody has told us yet". Both draw the placeholder, but only one is true.
    // updated stays zero, because no adapter vouched for this.
    mSnapshot.vitals.state = hud::SectionState::Cleared;
    mSnapshot.afflictions.state = hud::SectionState::Cleared;
    mSnapshot.defences.state = hud::SectionState::Cleared;
    mSnapshot.target.state = hud::SectionState::Cleared;
    mSnapshot.room.state = hud::SectionState::Cleared;
    // The adapter's identity is not one of the sections, and which adapter emptied
    // the panel is still worth showing in the footer.
    mSnapshot.adapter = adapter;

    mpRefreshTimer->stop();
    refresh();
}

const hud::Snapshot& THudPanel::snapshot() const
{
    return mSnapshot;
}

void THudPanel::activateElement(const QString& kind, const QString& id)
{
    if (id.isEmpty()) {
        return;
    }
    emit elementActivated(kind, id);
}

void THudPanel::scheduleRefresh()
{
    // Deliberately not a restart: under a flood of prompts a restarted timer would
    // keep pushing the repaint further out and the panel would never draw. Leaving
    // an already-armed timer alone caps repaints at one per interval instead.
    if (!mpRefreshTimer->isActive()) {
        mpRefreshTimer->start();
    }
}

THudPanel::SectionBox THudPanel::makeSection(QWidget* parent, const QString& title)
{
    SectionBox box;
    box.container = new QWidget(parent);
    auto* boxLayout = new QVBoxLayout(box.container);
    boxLayout->setContentsMargins(0, 0, 0, 0);
    boxLayout->setSpacing(3);

    auto* headingRow = new QWidget(box.container);
    auto* headingLayout = new QHBoxLayout(headingRow);
    headingLayout->setContentsMargins(0, 0, 0, 0);
    headingLayout->setSpacing(6);

    box.title = new QLabel(title, headingRow);
    QFont titleFont = box.title->font();
    titleFont.setBold(true);
    box.title->setFont(titleFont);
    headingLayout->addWidget(box.title);
    headingLayout->addStretch(1);

    box.age = new QLabel(headingRow);
    box.age->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    box.age->setFont(smallerFont(font(), 0.5));
    headingLayout->addWidget(box.age);
    boxLayout->addWidget(headingRow);

    box.body = new QWidget(box.container);
    auto* bodyLayout = new QVBoxLayout(box.body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(4);
    boxLayout->addWidget(box.body);

    //: Stands in for a HUD section that has nothing behind it - the adapter cleared
    //: it, or never sent it. Shown instead of the last known values, which would be
    //: a lie about what the panel currently knows.
    box.placeholder = new QLabel(tr("no data"), box.container);
    box.placeholder->setEnabled(false);
    box.placeholder->hide();
    boxLayout->addWidget(box.placeholder);

    return box;
}

void THudPanel::buildUi()
{
    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(0);

    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    // The chip flows size themselves by height-for-width, which only works while the
    // viewport dictates their width.
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    outerLayout->addWidget(scrollArea);

    auto* column = new QWidget(scrollArea);
    auto* columnLayout = new QVBoxLayout(column);
    columnLayout->setContentsMargins(8, 8, 8, 8);
    columnLayout->setSpacing(10);
    scrollArea->setWidget(column);

    //: Heading of the HUD section carrying health, mana, endurance, willpower and the
    //: balance indicators.
    mVitalsBox = makeSection(column, tr("Vitals"));
    columnLayout->addWidget(mVitalsBox.container);
    mpHpBar = new BarWidget(mVitalsBox.body);
    mpMpBar = new BarWidget(mVitalsBox.body);
    mpEpBar = new BarWidget(mVitalsBox.body);
    mpWpBar = new BarWidget(mVitalsBox.body);
    mpPips = new PipsWidget(mVitalsBox.body);
    mVitalsBox.body->layout()->addWidget(mpHpBar);
    mVitalsBox.body->layout()->addWidget(mpMpBar);
    mVitalsBox.body->layout()->addWidget(mpEpBar);
    mVitalsBox.body->layout()->addWidget(mpWpBar);
    mVitalsBox.body->layout()->addWidget(mpPips);

    //: Heading of the HUD section listing the player's own afflictions, which come
    //: from the game and are therefore facts rather than guesses.
    mAfflictionsBox = makeSection(column, tr("Afflictions"));
    columnLayout->addWidget(mAfflictionsBox.container);
    mpOwnAfflictions = new ChipFlow(this, qsl("own-affliction"), mAfflictionsBox.body);
    //: Shown in place of the affliction chips when the game says there are none.
    //: Different from "no data": this is knowing that there are none.
    mpOwnAfflictions->setEmptyText(tr("none"));
    mAfflictionsBox.body->layout()->addWidget(mpOwnAfflictions);

    //: Heading of the HUD section listing the player's own defences, which come from
    //: the game and are therefore facts rather than guesses.
    mDefencesBox = makeSection(column, tr("Defences"));
    columnLayout->addWidget(mDefencesBox.container);
    mpDefences = new ChipFlow(this, qsl("defence"), mDefencesBox.body);
    //: Shown in place of the defence chips when the game says there are none.
    mpDefences->setEmptyText(tr("none"));
    mDefencesBox.body->layout()->addWidget(mpDefences);

    //: Heading of the HUD section describing the current target.
    mTargetBox = makeSection(column, tr("Target"));
    columnLayout->addWidget(mTargetBox.container);
    mpTargetSummary = new QLabel(mTargetBox.body);
    mpTargetSummary->setWordWrap(true);
    mpTargetHealth = new BarWidget(mTargetBox.body);
    //: Heading above the target's afflictions. These are not facts: they are what the
    //: player's own attacks imply landed. The word "inferred" has to survive
    //: translation - it is the whole warning.
    mpInferredHeading = new QLabel(tr("Inferred afflictions - guesses, not facts"), mTargetBox.body);
    mpInferredHeading->setWordWrap(true);
    mpInferredHeading->setFont(smallerFont(font(), 0.5));
    mpTargetAfflictions = new ChipFlow(this, qsl("target-affliction"), mTargetBox.body);
    //: Shown in place of the inferred affliction chips when nothing has been inferred.
    mpTargetAfflictions->setEmptyText(tr("nothing inferred"));
    //: Caption above the target's stick-figure limb diagram, explaining that the
    //: numbers drawn on it are percentages of the way to a break.
    auto* limbCaption = new QLabel(tr("Limb damage, percent to break"), mTargetBox.body);
    limbCaption->setWordWrap(true);
    limbCaption->setFont(smallerFont(font(), 0.5));
    mpBodyDiagram = new BodyDiagram(this, mTargetBox.body);
    mTargetBox.body->layout()->addWidget(mpTargetSummary);
    mTargetBox.body->layout()->addWidget(mpTargetHealth);
    mTargetBox.body->layout()->addWidget(mpInferredHeading);
    mTargetBox.body->layout()->addWidget(mpTargetAfflictions);
    mTargetBox.body->layout()->addWidget(limbCaption);
    mTargetBox.body->layout()->addWidget(mpBodyDiagram);

    //: Heading of the HUD section describing the room the player is in.
    mRoomBox = makeSection(column, tr("Room"));
    columnLayout->addWidget(mRoomBox.container);
    mpRoomSummary = new QLabel(mRoomBox.body);
    mpRoomSummary->setWordWrap(true);
    mpExits = new QLabel(mRoomBox.body);
    mpExits->setWordWrap(true);
    //: Heading above the chips for the players standing in the room.
    mpPlayersHeading = new QLabel(tr("Players"), mRoomBox.body);
    mpPlayersHeading->setFont(smallerFont(font(), 0.5));
    mpPlayers = new ChipFlow(this, qsl("occupant"), mRoomBox.body);
    //: Shown in place of the player chips when the room holds no other players.
    mpPlayers->setEmptyText(tr("none"));
    //: Heading above the chips for the non-player creatures standing in the room.
    mpDenizensHeading = new QLabel(tr("Denizens"), mRoomBox.body);
    mpDenizensHeading->setFont(smallerFont(font(), 0.5));
    mpDenizens = new ChipFlow(this, qsl("occupant"), mRoomBox.body);
    //: Shown in place of the denizen chips when the room holds no denizens.
    mpDenizens->setEmptyText(tr("none"));
    mRoomBox.body->layout()->addWidget(mpRoomSummary);
    mRoomBox.body->layout()->addWidget(mpExits);
    mRoomBox.body->layout()->addWidget(mpPlayersHeading);
    mRoomBox.body->layout()->addWidget(mpPlayers);
    mRoomBox.body->layout()->addWidget(mpDenizensHeading);
    mRoomBox.body->layout()->addWidget(mpDenizens);

    columnLayout->addStretch(1);

    mpFooter = new QLabel(column);
    mpFooter->setWordWrap(true);
    mpFooter->setEnabled(false);
    mpFooter->setFont(smallerFont(font(), 0.5));
    columnLayout->addWidget(mpFooter);
}

QString THudPanel::formatAge(double seconds)
{
    const qint64 whole = static_cast<qint64>(qMax(0.0, seconds));
    if (whole < 60) {
        //: Age of HUD data in seconds, e.g. "12s". It sits in a narrow dock heading,
        //: so keep it as short as the language allows.
        return tr("%1s").arg(whole);
    }
    if (whole < 3600) {
        //: Age of HUD data in minutes and seconds, e.g. "2m 05s".
        return tr("%1m %2s").arg(whole / 60).arg(whole % 60, 2, 10, QLatin1Char('0'));
    }
    //: Age of HUD data in hours and minutes, e.g. "3h 07m".
    return tr("%1h %2m").arg(whole / 3600).arg((whole % 3600) / 60, 2, 10, QLatin1Char('0'));
}

void THudPanel::showBody(SectionBox& box, bool haveData)
{
    box.body->setVisible(haveData);
    box.placeholder->setVisible(!haveData);
}

bool THudPanel::refreshHeading(SectionBox& box, const hud::Section& section, int staleAfterSeconds, double now)
{
    const bool present = section.state == hud::SectionState::Present;
    // A section with no timestamp cannot be aged, and inventing one would be worse
    // than admitting that: it reports the age as unknown and is never marked stale.
    const double age = section.updated > 0.0 ? now - section.updated : -1.0;
    // csmNeverStale marks a section whose data does not decay just because nothing
    // has happened to it. Its age is still shown; it is simply never condemned.
    const bool ages = staleAfterSeconds > csmNeverStale;
    const bool stale = present && ages && age >= static_cast<double>(staleAfterSeconds);

    QString text;
    if (!present) {
        text.clear();
    } else if (age < 0.0) {
        //: Shown where a HUD section's age would be when the adapter sent no
        //: timestamp with it.
        text = tr("age unknown");
    } else if (stale) {
        //: A HUD section's age once it is past its staleness threshold, e.g.
        //: "42s - stale". %1 is the age.
        text = tr("%1 - stale").arg(formatAge(age));
    } else {
        text = formatAge(age);
    }

    box.age->setText(text);
    box.age->setEnabled(!stale);
    box.title->setEnabled(!stale);

    const bool changed = stale != box.stale;
    box.stale = stale;
    return changed;
}

void THudPanel::setBarValues(BarWidget* bar, const QString& caption, int current, int maximum, const QColor& colour, bool dimmed)
{
    const bool known = current >= 0 && maximum > 0;
    const int percent = known ? qBound(0, qRound(100.0 * current / maximum), 100) : -1;
    // An unknown value gets an empty bar and a dash. Rendering it as zero would claim
    // the character is dead, or out of mana, on no evidence at all.
    const QString text = known ? qsl("%1/%2  %3%").arg(current).arg(maximum).arg(percent) : qsl("--");
    bar->setBar(caption, percent, text, colour, dimmed);
}

void THudPanel::refreshVitals(double now)
{
    const hud::Vitals& vitals = mSnapshot.vitals;
    refreshHeading(mVitalsBox, vitals, csmVitalsStaleSeconds, now);
    const bool haveData = vitals.state == hud::SectionState::Present;
    showBody(mVitalsBox, haveData);
    if (!haveData) {
        return;
    }

    const bool dimmed = mVitalsBox.stale;
    //: Very short label for the health bar in the HUD vitals section.
    setBarValues(mpHpBar, tr("HP"), vitals.hp, vitals.maxHp, QColor(198, 72, 72), dimmed);
    //: Very short label for the mana bar in the HUD vitals section.
    setBarValues(mpMpBar, tr("MP"), vitals.mp, vitals.maxMp, QColor(72, 128, 200), dimmed);
    //: Very short label for the endurance bar in the HUD vitals section.
    setBarValues(mpEpBar, tr("EP"), vitals.ep, vitals.maxEp, QColor(196, 148, 60), dimmed);
    //: Very short label for the willpower bar in the HUD vitals section.
    setBarValues(mpWpBar, tr("WP"), vitals.wp, vitals.maxWp, QColor(146, 100, 190), dimmed);
    mpPips->setPips(vitals.balance,
                    vitals.equilibrium,
                    //: Very short label beside the balance indicator pip in the HUD.
                    tr("bal"),
                    //: Very short label beside the equilibrium indicator pip in the HUD.
                    tr("eq"),
                    dimmed);
}

void THudPanel::refreshOwnLists(double now)
{
    refreshHeading(mAfflictionsBox, mSnapshot.afflictions, csmListStaleSeconds, now);
    const bool haveAfflictions = mSnapshot.afflictions.state == hud::SectionState::Present;
    showBody(mAfflictionsBox, haveAfflictions);
    if (haveAfflictions) {
        mpOwnAfflictions->setChips(ChipFlow::afflictionChips(mSnapshot.afflictions.entries, QColor(196, 84, 74), now, true), mAfflictionsBox.stale);
    }

    refreshHeading(mDefencesBox, mSnapshot.defences, csmListStaleSeconds, now);
    const bool haveDefences = mSnapshot.defences.state == hud::SectionState::Present;
    showBody(mDefencesBox, haveDefences);
    if (haveDefences) {
        // Defences carry no age: how long a defence has been up says little, whereas
        // how long an affliction has been on you says a great deal.
        mpDefences->setChips(ChipFlow::afflictionChips(mSnapshot.defences.entries, QColor(86, 156, 116), now, false), mDefencesBox.stale);
    }
}

void THudPanel::refreshTarget(double now)
{
    const hud::Target& target = mSnapshot.target;
    refreshHeading(mTargetBox, target, csmTargetStaleSeconds, now);
    const bool haveData = target.state == hud::SectionState::Present;
    showBody(mTargetBox, haveData);
    if (!haveData) {
        return;
    }

    const bool dimmed = mTargetBox.stale;
    //: Shown where the target's name would be when the adapter sent a target without
    //: naming it.
    QString summary = target.name.isEmpty() ? tr("unnamed target") : target.name;
    if (!target.className.isEmpty()) {
        summary = qsl("%1  (%2)").arg(summary, target.className);
    }
    mpTargetSummary->setText(summary);
    mpTargetSummary->setEnabled(!dimmed);

    const bool healthKnown = target.health >= 0;
    //: Label of the target's health bar in the HUD.
    mpTargetHealth->setBar(tr("health"), healthKnown ? qBound(0, target.health, 100) : -1, healthKnown ? qsl("%1%").arg(target.health) : qsl("--"), QColor(198, 72, 72), dimmed);

    mpInferredHeading->setEnabled(!dimmed);
    mpTargetAfflictions->setChips(ChipFlow::afflictionChips(target.afflictions, QColor(206, 152, 62), now, true), dimmed);
    mpBodyDiagram->setLimbs(target.limbs, dimmed);
}

void THudPanel::refreshRoom(double now)
{
    const hud::Room& room = mSnapshot.room;
    refreshHeading(mRoomBox, room, csmRoomStaleSeconds, now);
    const bool haveData = room.state == hud::SectionState::Present;
    showBody(mRoomBox, haveData);
    if (!haveData) {
        return;
    }

    const bool dimmed = mRoomBox.stale;
    //: Shown where the room's name would be when the adapter sent a room without
    //: naming it.
    QString summary = room.name.isEmpty() ? tr("unnamed room") : room.name;
    if (!room.area.isEmpty()) {
        summary = qsl("%1  (%2)").arg(summary, room.area);
    }
    mpRoomSummary->setText(summary);
    mpRoomSummary->setEnabled(!dimmed);

    //: Shown in the HUD room section when the room has no exits at all.
    const QString exitsNone = tr("Exits: none");
    //: The room's exits, listed on one line. %1 is a comma-separated list.
    mpExits->setText(room.exits.isEmpty() ? exitsNone : tr("Exits: %1").arg(room.exits.join(qsl(", "))));
    mpExits->setEnabled(!dimmed);

    QVector<hud::Occupant> players;
    QVector<hud::Occupant> denizens;
    for (const hud::Occupant& occupant : room.occupants) {
        if (occupant.player) {
            players.append(occupant);
        } else {
            denizens.append(occupant);
        }
    }

    mpPlayersHeading->setEnabled(!dimmed);
    mpDenizensHeading->setEnabled(!dimmed);
    mpPlayers->setChips(ChipFlow::occupantChips(players), dimmed);
    mpDenizens->setChips(ChipFlow::occupantChips(denizens), dimmed);
}

void THudPanel::refreshFooter(double now)
{
    //: Shown in the HUD footer in place of the adapter's name when no adapter has
    //: identified itself.
    const QString adapter = mSnapshot.adapter.isEmpty() ? tr("no adapter") : mSnapshot.adapter;
    //: Shown in the HUD footer in place of a payload age when nothing has arrived yet.
    const QString noPayload = tr("nothing yet");
    const QString age = mSnapshot.generated > 0.0 ? formatAge(now - mSnapshot.generated) : noPayload;
    //: HUD footer line: %1 is the adapter's name, %2 how old its most recent payload
    //: is. A stale adapter should be visible rather than merely suspected.
    mpFooter->setText(tr("%1 - last payload %2").arg(adapter, age));
}

void THudPanel::refresh()
{
    const double now = epochNow();
    refreshVitals(now);
    refreshOwnLists(now);
    refreshTarget(now);
    refreshRoom(now);
    refreshFooter(now);
}

// Once a second, and touching only the age labels - unless a section has just crossed
// its staleness threshold, which changes how that section's contents have to be drawn
// and so needs a real repaint.
void THudPanel::tick()
{
    const double now = epochNow();
    bool stalenessChanged = false;
    stalenessChanged |= refreshHeading(mVitalsBox, mSnapshot.vitals, csmVitalsStaleSeconds, now);
    stalenessChanged |= refreshHeading(mAfflictionsBox, mSnapshot.afflictions, csmListStaleSeconds, now);
    stalenessChanged |= refreshHeading(mDefencesBox, mSnapshot.defences, csmListStaleSeconds, now);
    stalenessChanged |= refreshHeading(mTargetBox, mSnapshot.target, csmTargetStaleSeconds, now);
    stalenessChanged |= refreshHeading(mRoomBox, mSnapshot.room, csmRoomStaleSeconds, now);
    refreshFooter(now);

    if (stalenessChanged) {
        scheduleRefresh();
    }
}
