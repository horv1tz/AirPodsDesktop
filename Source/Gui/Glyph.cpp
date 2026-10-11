//
// AirPodsDesktop - AirPods Desktop User Experience Enhancement Program.
// Copyright (C) 2021-2022 SpriteOvO
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//

#include "Glyph.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include "Theme.h"

namespace Gui::Glyph {

namespace {

constexpr qreal kPi = std::numbers::pi;

// Maps the unit box (0..1 on both axes) onto a square of `side` device-independent pixels.
struct UnitBox {
    QPointF origin;
    qreal side;

    QPointF At(qreal x, qreal y) const
    {
        return QPointF{origin.x() + x * side, origin.y() + y * side};
    }

    // The bounding square of a circle given by its centre and radius, all in unit fractions.
    QRectF CircleBox(qreal centreX, qreal centreY, qreal radius) const
    {
        return QRectF{
            At(centreX - radius, centreY - radius),
            QSizeF{2.0 * radius * side, 2.0 * radius * side}};
    }
};

QPen StrokePen(const QColor &color, qreal width)
{
    return QPen{color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin};
}

void UseStroke(QPainter &painter, const QColor &color, qreal width)
{
    painter.setPen(StrokePen(color, width));
    painter.setBrush(Qt::NoBrush);
}

void UseFill(QPainter &painter, const QColor &color)
{
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
}

void DrawClose(QPainter &painter, const UnitBox &unit, const QColor &color)
{
    UseStroke(painter, color, 0.13 * unit.side);
    painter.drawLine(unit.At(0.18, 0.18), unit.At(0.82, 0.82));
    painter.drawLine(unit.At(0.82, 0.18), unit.At(0.18, 0.82));
}

// Eight tapered teeth around a round hole. The first tooth points up.
void DrawGear(QPainter &painter, const UnitBox &unit, const QColor &color)
{
    constexpr int kTeeth = 8;
    constexpr qreal kRootRadius = 0.36;
    constexpr qreal kTipRadius = 0.45;
    constexpr qreal kHoleRadius = 0.17;
    constexpr qreal kRootHalfAngle = 12.0 * kPi / 180.0;
    constexpr qreal kTipHalfAngle = 8.0 * kPi / 180.0;

    const QPointF centre = unit.At(0.5, 0.5);
    const auto polar = [&](qreal radius, qreal angle) {
        return QPointF{
            centre.x() + radius * unit.side * std::cos(angle),
            centre.y() + radius * unit.side * std::sin(angle)};
    };

    QPainterPath path;
    for (int tooth = 0; tooth < kTeeth; ++tooth) {
        const qreal angle = tooth * 2.0 * kPi / kTeeth - kPi / 2.0;
        const std::array points{
            polar(kRootRadius, angle - kRootHalfAngle), polar(kTipRadius, angle - kTipHalfAngle),
            polar(kTipRadius, angle + kTipHalfAngle), polar(kRootRadius, angle + kRootHalfAngle)};

        for (const QPointF &point : points) {
            if (path.elementCount() == 0) {
                path.moveTo(point);
            }
            else {
                path.lineTo(point);
            }
        }
    }
    path.closeSubpath();

    // The hole is a second sub-path, cut out by the odd-even fill rule.
    path.addEllipse(centre, kHoleRadius * unit.side, kHoleRadius * unit.side);
    path.setFillRule(Qt::OddEvenFill);

    painter.fillPath(path, color);
    painter.strokePath(path, StrokePen(color, 0.06 * unit.side));
}

// Two sliders: a track and a knob each.
void DrawGeneral(QPainter &painter, const UnitBox &unit, const QColor &color)
{
    UseStroke(painter, color, 0.09 * unit.side);
    painter.drawLine(unit.At(0.12, 0.30), unit.At(0.88, 0.30));
    painter.drawLine(unit.At(0.12, 0.70), unit.At(0.88, 0.70));

    UseFill(painter, color);
    const qreal knobRadius = 0.14 * unit.side;
    painter.drawEllipse(unit.At(0.35, 0.30), knobRadius, knobRadius);
    painter.drawEllipse(unit.At(0.65, 0.70), knobRadius, knobRadius);
}

// A circle whose right half is filled, the usual appearance symbol.
void DrawVisual(QPainter &painter, const UnitBox &unit, const QColor &color)
{
    const QRectF circle = unit.CircleBox(0.5, 0.5, 0.38);

    UseStroke(painter, color, 0.09 * unit.side);
    painter.drawEllipse(circle);

    QPainterPath rightHalf;
    rightHalf.moveTo(unit.At(0.5, 0.5));
    rightHalf.arcTo(circle, 90.0, -180.0);
    rightHalf.closeSubpath();

    UseFill(painter, color);
    painter.drawPath(rightHalf);
}

// A 2x2 grid of rounded squares.
void DrawFeatures(QPainter &painter, const UnitBox &unit, const QColor &color)
{
    constexpr qreal kOrigin = 0.14;
    constexpr qreal kSide = 0.30;
    constexpr qreal kGap = 0.12;

    UseFill(painter, color);
    const qreal cornerRadius = 0.07 * unit.side;
    for (int row = 0; row < 2; ++row) {
        for (int column = 0; column < 2; ++column) {
            const QRectF square{
                unit.At(kOrigin + column * (kSide + kGap), kOrigin + row * (kSide + kGap)),
                QSizeF{kSide * unit.side, kSide * unit.side}};
            painter.drawRoundedRect(square, cornerRadius, cornerRadius);
        }
    }
}

// A circled lowercase "i".
void DrawAbout(QPainter &painter, const UnitBox &unit, const QColor &color)
{
    UseStroke(painter, color, 0.09 * unit.side);
    painter.drawEllipse(unit.CircleBox(0.5, 0.5, 0.38));

    UseFill(painter, color);
    painter.drawEllipse(unit.At(0.5, 0.32), 0.055 * unit.side, 0.055 * unit.side);

    UseStroke(painter, color, 0.10 * unit.side);
    painter.drawLine(unit.At(0.5, 0.45), unit.At(0.5, 0.70));
}

// Two angle brackets, "<" and ">".
void DrawDebug(QPainter &painter, const UnitBox &unit, const QColor &color)
{
    UseStroke(painter, color, 0.09 * unit.side);

    const std::array left{unit.At(0.38, 0.28), unit.At(0.16, 0.50), unit.At(0.38, 0.72)};
    const std::array right{unit.At(0.62, 0.28), unit.At(0.84, 0.50), unit.At(0.62, 0.72)};
    painter.drawPolyline(left.data(), static_cast<int>(left.size()));
    painter.drawPolyline(right.data(), static_cast<int>(right.size()));
}

// Paints one of the glyphs in the largest centred square of the requested rectangle, in the
// accent colour of the current theme (or the disabled text colour).
class GlyphIconEngine : public QIconEngine
{
public:
    explicit GlyphIconEngine(Kind kind) : _kind{kind} {}

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State) override
    {
        if (painter == nullptr || rect.isEmpty()) {
            return;
        }

        const auto &colors = Theme::Manager::Instance().Colors();
        const QColor color = mode == QIcon::Disabled ? colors.textDisabled : colors.accent;

        const QRectF area{rect};
        const qreal side = std::min(area.width(), area.height());
        QRectF square{0.0, 0.0, side, side};
        square.moveCenter(area.center());

        Draw(*painter, _kind, square, color);
    }

    QIconEngine *clone() const override
    {
        return new GlyphIconEngine{_kind};
    }

private:
    Kind _kind;
};

} // namespace

void Draw(QPainter &painter, Kind kind, const QRectF &box, const QColor &color)
{
    // The glyphs are square; a box of another shape holds them centred.
    const qreal side = std::min(box.width(), box.height());
    if (!(side > 0.0)) {
        return;
    }

    const UnitBox unit{
        box.topLeft() + QPointF{(box.width() - side) / 2.0, (box.height() - side) / 2.0}, side};

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);

    switch (kind) {
    case Kind::Close:
        DrawClose(painter, unit, color);
        break;
    case Kind::Gear:
        DrawGear(painter, unit, color);
        break;
    case Kind::General:
        DrawGeneral(painter, unit, color);
        break;
    case Kind::Visual:
        DrawVisual(painter, unit, color);
        break;
    case Kind::Features:
        DrawFeatures(painter, unit, color);
        break;
    case Kind::About:
        DrawAbout(painter, unit, color);
        break;
    case Kind::Debug:
        DrawDebug(painter, unit, color);
        break;
    }

    painter.restore();
}

QIcon Icon(Kind kind)
{
    return QIcon{new GlyphIconEngine{kind}};
}

} // namespace Gui::Glyph
