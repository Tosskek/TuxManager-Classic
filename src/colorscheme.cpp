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

#include "colorscheme.h"

#include <QApplication>
#include <QPalette>
#include <QVariant>

ColorScheme *ColorScheme::current = nullptr;

namespace
{
    const ColorScheme::ColorField kColorFields[] =
    {
        { "CpuGraphLineColor", &ColorScheme::CpuGraphLineColor },
        { "CpuGraphFillColor", &ColorScheme::CpuGraphFillColor },
        { "CpuGraphSecondaryFillColor", &ColorScheme::CpuGraphSecondaryFillColor },
        { "MemoryGraphLineColor", &ColorScheme::MemoryGraphLineColor },
        { "MemoryGraphFillColor", &ColorScheme::MemoryGraphFillColor },
        { "DiskGraphLineColor", &ColorScheme::DiskGraphLineColor },
        { "DiskGraphFillColor", &ColorScheme::DiskGraphFillColor },
        { "DiskTransferGraphLineColor", &ColorScheme::DiskTransferGraphLineColor },
        { "DiskTransferGraphFillColor", &ColorScheme::DiskTransferGraphFillColor },
        { "DiskTransferGraphSecondaryFillColor", &ColorScheme::DiskTransferGraphSecondaryFillColor },
        { "NetworkGraphLineColor", &ColorScheme::NetworkGraphLineColor },
        { "NetworkGraphFillColor", &ColorScheme::NetworkGraphFillColor },
        { "NetworkGraphSecondaryFillColor", &ColorScheme::NetworkGraphSecondaryFillColor },
        { "GpuGraphLineColor", &ColorScheme::GpuGraphLineColor },
        { "GpuGraphFillColor", &ColorScheme::GpuGraphFillColor },
        { "GpuGraphSecondaryFillColor", &ColorScheme::GpuGraphSecondaryFillColor },
        { "SwapUsageGraphLineColor", &ColorScheme::SwapUsageGraphLineColor },
        { "SwapUsageGraphFillColor", &ColorScheme::SwapUsageGraphFillColor },
        { "SwapActivityGraphLineColor", &ColorScheme::SwapActivityGraphLineColor },
        { "SwapActivityGraphFillColor", &ColorScheme::SwapActivityGraphFillColor },
        { "SwapActivityGraphSecondaryFillColor", &ColorScheme::SwapActivityGraphSecondaryFillColor },
        { "GraphGridColor", &ColorScheme::GraphGridColor },
        { "GraphOverlayTextColor", &ColorScheme::GraphOverlayTextColor },
        { "SidePanelBackgroundColor", &ColorScheme::SidePanelBackgroundColor },
        { "SidePanelItemSelectedBackgroundColor", &ColorScheme::SidePanelItemSelectedBackgroundColor },
        { "SidePanelItemHoverBackgroundColor", &ColorScheme::SidePanelItemHoverBackgroundColor },
        { "SidePanelItemBackgroundColor", &ColorScheme::SidePanelItemBackgroundColor },
        { "SidePanelItemSelectedTextColor", &ColorScheme::SidePanelItemSelectedTextColor },
        { "SidePanelItemTextColor", &ColorScheme::SidePanelItemTextColor },
        { "SidePanelItemSubtitleColor", &ColorScheme::SidePanelItemSubtitleColor },
        { "SidePanelItemSelectedBorderColor", &ColorScheme::SidePanelItemSelectedBorderColor },
        { "CpuTitleColor", &ColorScheme::CpuTitleColor },
        { "CpuHeaderValueColor", &ColorScheme::CpuHeaderValueColor },
        { "MemoryTitleColor", &ColorScheme::MemoryTitleColor },
        { "MemoryHeaderValueColor", &ColorScheme::MemoryHeaderValueColor },
        { "DiskTitleColor", &ColorScheme::DiskTitleColor },
        { "DiskHeaderValueColor", &ColorScheme::DiskHeaderValueColor },
        { "NetworkTitleColor", &ColorScheme::NetworkTitleColor },
        { "GpuTitleColor", &ColorScheme::GpuTitleColor },
        { "MutedTextColor", &ColorScheme::MutedTextColor },
        { "StatLabelColor", &ColorScheme::StatLabelColor },
        { "AxisLabelColor", &ColorScheme::AxisLabelColor },
        { "MemoryLegendTextColor", &ColorScheme::MemoryLegendTextColor },
        { "MemoryLegendUsedColor", &ColorScheme::MemoryLegendUsedColor },
        { "MemoryLegendCompressedColor", &ColorScheme::MemoryLegendCompressedColor },
        { "MemoryLegendDirtyColor", &ColorScheme::MemoryLegendDirtyColor },
        { "MemoryLegendCachedColor", &ColorScheme::MemoryLegendCachedColor },
        { "MemoryLegendFreeColor", &ColorScheme::MemoryLegendFreeColor },
        { "MemoryBarUsedColor", &ColorScheme::MemoryBarUsedColor },
        { "MemoryBarCompressedColor", &ColorScheme::MemoryBarCompressedColor },
        { "MemoryBarDirtyColor", &ColorScheme::MemoryBarDirtyColor },
        { "MemoryBarCachedColor", &ColorScheme::MemoryBarCachedColor },
        { "MemoryBarFreeColor", &ColorScheme::MemoryBarFreeColor },
        { "MemoryBarBorderColor", &ColorScheme::MemoryBarBorderColor }
    };

    QColor colorFromVariant(const QVariant &value, const QColor &fallback)
    {
        if (!value.isValid())
            return fallback;
        if (value.canConvert<QColor>())
        {
            const QColor color = value.value<QColor>();
            if (color.isValid())
                return color;
        }

        const QString text = value.toString().trimmed();
        if (text.isEmpty())
            return fallback;

        const QColor color(text);
        return color.isValid() ? color : fallback;
    }
}

ColorScheme *ColorScheme::GetCurrent()
{
    if (!ColorScheme::current)
        ColorScheme::current = new ColorScheme(ColorScheme::DefaultLight());
    return ColorScheme::current;
}

bool ColorScheme::DetectDarkMode()
{
    return QApplication::palette().color(QPalette::Window).lightness() <= 127;
}

const QVector<ColorScheme::ColorField> &ColorScheme::Fields()
{
    static const QVector<ColorScheme::ColorField> fields = []
    {
        QVector<ColorScheme::ColorField> items;
        items.reserve(static_cast<int>(sizeof(kColorFields) / sizeof(kColorFields[0])));
        for (const ColorScheme::ColorField &field : kColorFields)
            items.append(field);
        return items;
    }();
    return fields;
}

void ColorScheme::Install(ColorScheme *scheme)
{
    delete ColorScheme::current;
    ColorScheme::current = scheme;
}

ColorScheme::ColorScheme()
{}

ColorScheme ColorScheme::DefaultDark()
{
    ColorScheme scheme;
    scheme.DarkMode = true;

    // Classic flat look, dark variant: same hues as the light scheme, lifted
    // for contrast, over deep tinted fills; neutral grey panels, light text.
    const QColor cpuBlue(0x3f, 0xa9, 0xf5);
    const QColor memPurple(0xc0, 0x6e, 0xe0);
    const QColor diskGreen(0x6c, 0xc6, 0x44);
    const QColor netOrange(0xe0, 0x8a, 0x3c);
    const QColor text(0xe8, 0xe8, 0xe8);
    const QColor grey(0x9a, 0x9a, 0x9a);

    scheme.CpuGraphLineColor = cpuBlue;
    scheme.CpuGraphFillColor = QColor(0x17, 0x2a, 0x3b);
    scheme.CpuGraphSecondaryFillColor = QColor(0x1f, 0x45, 0x66);
    scheme.MemoryGraphLineColor = memPurple;
    scheme.MemoryGraphFillColor = QColor(0x2c, 0x1d, 0x35);
    scheme.DiskGraphLineColor = diskGreen;
    scheme.DiskGraphFillColor = QColor(0x1c, 0x2e, 0x16);
    scheme.DiskTransferGraphLineColor = diskGreen;
    scheme.DiskTransferGraphFillColor = QColor(0x1c, 0x2e, 0x16);
    scheme.DiskTransferGraphSecondaryFillColor = QColor(0x2b, 0x4a, 0x20);
    scheme.NetworkGraphLineColor = netOrange;
    scheme.NetworkGraphFillColor = QColor(0x35, 0x24, 0x14);
    scheme.NetworkGraphSecondaryFillColor = QColor(0x55, 0x37, 0x1b);
    scheme.GpuGraphLineColor = diskGreen;
    scheme.GpuGraphFillColor = QColor(0x1c, 0x2e, 0x16);
    scheme.GpuGraphSecondaryFillColor = QColor(0x2b, 0x4a, 0x20);
    scheme.SwapUsageGraphLineColor = memPurple;
    scheme.SwapUsageGraphFillColor = QColor(0x2c, 0x1d, 0x35);
    scheme.SwapActivityGraphLineColor = memPurple;
    scheme.SwapActivityGraphFillColor = QColor(0x2c, 0x1d, 0x35);
    scheme.SwapActivityGraphSecondaryFillColor = QColor(0x45, 0x2a, 0x55);
    scheme.GraphGridColor = QColor(0x33, 0x3d, 0x47);
    scheme.GraphOverlayTextColor = QColor(0xc8, 0xc8, 0xc8);
    scheme.SidePanelBackgroundColor = QColor(0x20, 0x20, 0x20);
    scheme.SidePanelItemSelectedBackgroundColor = QColor(0x26, 0x4f, 0x78);
    scheme.SidePanelItemHoverBackgroundColor = QColor(0x2a, 0x3b, 0x4d);
    scheme.SidePanelItemBackgroundColor = QColor(0x20, 0x20, 0x20);
    scheme.SidePanelItemSelectedTextColor = QColor(0xff, 0xff, 0xff);
    scheme.SidePanelItemTextColor = text;
    scheme.SidePanelItemSubtitleColor = QColor(0xc8, 0xc8, 0xc8);
    scheme.SidePanelItemSelectedBorderColor = QColor(0x26, 0x4f, 0x78);
    scheme.CpuTitleColor = text;
    scheme.CpuHeaderValueColor = text;
    scheme.MemoryTitleColor = text;
    scheme.MemoryHeaderValueColor = text;
    scheme.DiskTitleColor = text;
    scheme.DiskHeaderValueColor = text;
    scheme.NetworkTitleColor = text;
    scheme.GpuTitleColor = text;
    scheme.MutedTextColor = grey;
    scheme.StatLabelColor = grey;
    scheme.AxisLabelColor = grey;
    scheme.MemoryLegendTextColor = grey;
    scheme.MemoryLegendUsedColor = memPurple;
    scheme.MemoryLegendCompressedColor = QColor(0x9a, 0x5c, 0xb8);
    scheme.MemoryLegendDirtyColor = QColor(0xc8, 0x9a, 0x3a);
    scheme.MemoryLegendCachedColor = QColor(0x45, 0x2a, 0x55);
    scheme.MemoryLegendFreeColor = QColor(0x1a, 0x1a, 0x1a);
    scheme.MemoryBarUsedColor = QColor(0x45, 0x2a, 0x55);
    scheme.MemoryBarCompressedColor = QColor(0x5a, 0x37, 0x70);
    scheme.MemoryBarDirtyColor = QColor(0x6a, 0x52, 0x20);
    scheme.MemoryBarCachedColor = QColor(0x2c, 0x1d, 0x35);
    scheme.MemoryBarFreeColor = QColor(0x1a, 0x1a, 0x1a);
    scheme.MemoryBarBorderColor = memPurple;
    return scheme;
}

ColorScheme ColorScheme::DefaultLight()
{
    ColorScheme scheme;
    scheme.DarkMode = false;

    // Classic flat look: one strong line colour per resource, a very pale
    // fill underneath, white panels and black text.
    const QColor cpuBlue(0x11, 0x7d, 0xbb);
    const QColor memPurple(0x8b, 0x12, 0xae);
    const QColor diskGreen(0x4d, 0xa6, 0x0c);
    const QColor netBrown(0xa7, 0x4f, 0x01);
    const QColor black(0x00, 0x00, 0x00);
    const QColor grey(0x70, 0x70, 0x70);

    scheme.CpuGraphLineColor = cpuBlue;
    scheme.CpuGraphFillColor = QColor(0xf1, 0xf6, 0xfa);
    scheme.CpuGraphSecondaryFillColor = QColor(0xc4, 0xdc, 0xec);
    scheme.MemoryGraphLineColor = memPurple;
    scheme.MemoryGraphFillColor = QColor(0xf4, 0xf2, 0xf4);
    scheme.DiskGraphLineColor = diskGreen;
    scheme.DiskGraphFillColor = QColor(0xef, 0xf7, 0xe9);
    scheme.DiskTransferGraphLineColor = diskGreen;
    scheme.DiskTransferGraphFillColor = QColor(0xef, 0xf7, 0xe9);
    scheme.DiskTransferGraphSecondaryFillColor = QColor(0xd2, 0xe9, 0xc2);
    scheme.NetworkGraphLineColor = netBrown;
    scheme.NetworkGraphFillColor = QColor(0xfc, 0xf3, 0xeb);
    scheme.NetworkGraphSecondaryFillColor = QColor(0xf0, 0xd6, 0xbd);
    scheme.GpuGraphLineColor = diskGreen;
    scheme.GpuGraphFillColor = QColor(0xef, 0xf7, 0xe9);
    scheme.GpuGraphSecondaryFillColor = QColor(0xd2, 0xe9, 0xc2);
    scheme.SwapUsageGraphLineColor = memPurple;
    scheme.SwapUsageGraphFillColor = QColor(0xf4, 0xf2, 0xf4);
    scheme.SwapActivityGraphLineColor = memPurple;
    scheme.SwapActivityGraphFillColor = QColor(0xf4, 0xf2, 0xf4);
    scheme.SwapActivityGraphSecondaryFillColor = QColor(0xdc, 0xc4, 0xe4);
    scheme.GraphGridColor = QColor(0xd9, 0xea, 0xf4);
    scheme.GraphOverlayTextColor = QColor(0x50, 0x50, 0x50);
    scheme.SidePanelBackgroundColor = QColor(0xff, 0xff, 0xff);
    scheme.SidePanelItemSelectedBackgroundColor = QColor(0xcc, 0xe8, 0xff);
    scheme.SidePanelItemHoverBackgroundColor = QColor(0xe5, 0xf3, 0xff);
    scheme.SidePanelItemBackgroundColor = QColor(0xff, 0xff, 0xff);
    scheme.SidePanelItemSelectedTextColor = black;
    scheme.SidePanelItemTextColor = black;
    scheme.SidePanelItemSubtitleColor = black;
    scheme.SidePanelItemSelectedBorderColor = QColor(0xcc, 0xe8, 0xff);
    scheme.CpuTitleColor = black;
    scheme.CpuHeaderValueColor = black;
    scheme.MemoryTitleColor = black;
    scheme.MemoryHeaderValueColor = black;
    scheme.DiskTitleColor = black;
    scheme.DiskHeaderValueColor = black;
    scheme.NetworkTitleColor = black;
    scheme.GpuTitleColor = black;
    scheme.MutedTextColor = grey;
    scheme.StatLabelColor = grey;
    scheme.AxisLabelColor = grey;
    scheme.MemoryLegendTextColor = grey;
    scheme.MemoryLegendUsedColor = memPurple;
    scheme.MemoryLegendCompressedColor = QColor(0xb9, 0x6f, 0xcf);
    scheme.MemoryLegendDirtyColor = QColor(0xb0, 0x85, 0x23);
    scheme.MemoryLegendCachedColor = QColor(0xdc, 0xc4, 0xe4);
    scheme.MemoryLegendFreeColor = QColor(0xff, 0xff, 0xff);
    scheme.MemoryBarUsedColor = QColor(0xdc, 0xc4, 0xe4);
    scheme.MemoryBarCompressedColor = QColor(0xc9, 0xa6, 0xd6);
    scheme.MemoryBarDirtyColor = QColor(0xe0, 0xc0, 0x7a);
    scheme.MemoryBarCachedColor = QColor(0xf4, 0xf2, 0xf4);
    scheme.MemoryBarFreeColor = QColor(0xff, 0xff, 0xff);
    scheme.MemoryBarBorderColor = memPurple;
    return scheme;
}

QVariantMap ColorScheme::ToVariantMap() const
{
    QVariantMap map;
    map.insert("DarkMode", this->DarkMode);
    for (const ColorScheme::ColorField &field : ColorScheme::Fields())
        map.insert(field.Name, (this->*field.Member).name(QColor::HexArgb));
    return map;
}

void ColorScheme::ApplyVariantMap(const QVariantMap &map)
{
    this->DarkMode = map.value("DarkMode", this->DarkMode).toBool();
    for (const ColorScheme::ColorField &field : ColorScheme::Fields())
        this->*field.Member = colorFromVariant(map.value(field.Name), this->*field.Member);
}
