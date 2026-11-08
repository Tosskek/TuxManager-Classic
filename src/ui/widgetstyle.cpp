/*
 * Tux Manager - Linux system monitor
 * Copyright (C) 2026 Petr Bena <petr@bena.rocks>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "widgetstyle.h"

#include <QWidget>

namespace WidgetStyle
{
    QString TextStyle(const QColor &color, int fontSizePt, bool bold)
    {
        QString style = QString("color: %1; font-family: Arial;").arg(color.name(QColor::HexArgb));
        if (fontSizePt > 0)
            style += QString(" font-size: %1pt;").arg(fontSizePt);
        if (bold)
            style += " font-weight: bold;";
        return style;
    }

    void ApplyTextStyle(QWidget *widget, const QColor &color, int fontSizePt, bool bold)
    {
        if (!widget)
            return;
        widget->setStyleSheet(TextStyle(color, fontSizePt, bold));
    }
}
