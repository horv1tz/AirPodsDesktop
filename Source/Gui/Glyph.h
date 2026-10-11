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

#pragma once

#include <QColor>
#include <QIcon>
#include <QRectF>

class QPainter;

namespace Gui::Glyph {

//
// Small vector glyphs that are drawn with `QPainter`, so they stay sharp at every device pixel
// ratio and can take any colour. All of them are original line art and filled shapes.
//
enum class Kind { Close, Gear, General, Visual, Features, About, Debug };

// Draws `kind` into `box` with `color`. Every shape, including its stroke, stays within half of
// the box width from the centre of the box. The painter state is restored before returning.
void Draw(QPainter &painter, Kind kind, const QRectF &box, const QColor &color);

// An icon that paints the glyph at paint time: the accent colour of the current theme, or the
// disabled text colour for `QIcon::Disabled`. It follows theme changes without being rebuilt.
QIcon Icon(Kind kind);

} // namespace Gui::Glyph
