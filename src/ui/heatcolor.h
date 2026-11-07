/*
 * Tux Manager - Linux system monitor
 * Copyright (C) 2026 Petr Bena <petr@bena.rocks>
 * Copyright (C) 2026 Tosskek (classic look)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef HEATCOLOR_H
#define HEATCOLOR_H

#include <QColor>
#include <QtGlobal>
#include <cmath>
#include <unistd.h>
#include "classictheme.h"

// Pale-yellow → amber → orange cell shading for resource usage columns.
namespace HeatColor
{
    inline QColor mix(const QColor &a, const QColor &b, double t)
    {
        return QColor::fromRgbF(a.redF()   + (b.redF()   - a.redF())   * t,
                                a.greenF() + (b.greenF() - a.greenF()) * t,
                                a.blueF()  + (b.blueF()  - a.blueF())  * t);
    }

    //! fraction: 0..1 of the resource. A square-root curve keeps small values visible.
    inline QColor ForFraction(double fraction)
    {
        static const QColor l0(0xff, 0xf4, 0xc4), l1(0xff, 0xd6, 0x6b), l2(0xff, 0x9a, 0x4a);
        static const QColor d0(0x2a, 0x27, 0x1c), d1(0x5c, 0x4a, 0x1c), d2(0x8a, 0x45, 0x1c);
        const bool dark = ClassicTheme::IsDark();
        const QColor &c0 = dark ? d0 : l0;
        const QColor &c1 = dark ? d1 : l1;
        const QColor &c2 = dark ? d2 : l2;
        const double t = std::sqrt(qBound(0.0, fraction, 1.0));
        return (t < 0.6) ? mix(c0, c1, t / 0.6) : mix(c1, c2, (t - 0.6) / 0.4);
    }

    inline double TotalRamKb()
    {
        static const double kb = [] {
            const long pages = sysconf(_SC_PHYS_PAGES);
            const long size = sysconf(_SC_PAGESIZE);
            return (pages > 0 && size > 0) ? static_cast<double>(pages) * size / 1024.0 : 1.0;
        }();
        return kb;
    }

    //! Disk throughput above ~100 MB/s is shown as fully hot.
    inline double IoFraction(double bytesPerSec)
    {
        return bytesPerSec <= 0.0 ? 0.0 : bytesPerSec / (100.0 * 1024.0 * 1024.0);
    }
}

#endif // HEATCOLOR_H
