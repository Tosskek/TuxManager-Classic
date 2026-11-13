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

#include "graphwidget.h"
#include "../colorscheme.h"
#include "../configuration.h"
#include "../ui/classictheme.h"
#include "misc.h"

#include <cmath>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QToolTip>
#include <QtGlobal>

using namespace Perf;

GraphWidget::GraphWidget(QWidget *parent) : QWidget(parent)
{
    const ColorScheme *scheme = ColorScheme::GetCurrent();
    this->m_lineColor = scheme->CpuGraphLineColor;
    this->m_fillColor = scheme->CpuGraphFillColor;
    this->m_fillColor2 = scheme->CpuGraphSecondaryFillColor;
    this->setAutoFillBackground(false);
    this->setMouseTracking(true);
}

void GraphWidget::SetDataSource(const HistoryBuffer &data, double maxVal)
{
    this->m_data = &data;
    this->m_maxVal = (maxVal > 0.0) ? maxVal : 100.0;
    this->update();
}

void GraphWidget::SetOverlayDataSource(const HistoryBuffer &data)
{
    this->m_overlayData = &data;
    this->update();
}

void GraphWidget::SetMax(double maxVal)
{
    this->m_maxVal = (maxVal > 0.0) ? maxVal : 100.0;
}

void GraphWidget::Tick(bool requestUpdate)
{
    ++this->m_historyTick;
    if (requestUpdate)
        this->update();
}

void GraphWidget::ClearDataSource()
{
    this->m_data = nullptr;
}

void GraphWidget::ClearOverlayDataSource()
{
    this->m_overlayData = nullptr;
}

void GraphWidget::SetSeriesNames(const QString &primary, const QString &secondary)
{
    if (!primary.isEmpty())
        this->m_primaryName = primary;
    if (!secondary.isEmpty())
        this->m_secondaryName = secondary;
}

void GraphWidget::SetColor(QColor line, QColor fill, QColor fill2)
{
    this->m_lineColor = line;
    this->m_fillColor = fill;
    this->m_fillColor2 = fill2.isValid() ? fill2 : fill.darker(160);
    this->update();
}

void GraphWidget::SetSampleCapacity(int samples)
{
    this->m_sampleCapacity = qMax(2, samples);
    this->update();
}

void GraphWidget::SetOverlayText(const QString &text)
{
    if (this->m_overlayText == text)
        return;

    this->m_overlayText = text;
    this->update();
}

void GraphWidget::SetPercentTooltipAbsolute(double maxAbsoluteValue, const QString &unitLabel, int precision)
{
    this->m_percentTooltipAbsoluteEnabled = (maxAbsoluteValue > 0.0);
    this->m_percentTooltipAbsoluteMax = qMax(0.0, maxAbsoluteValue);
    this->m_percentTooltipAbsoluteUnit = unitLabel;
    this->m_percentTooltipAbsolutePrecision = qMax(0, precision);
}

void GraphWidget::paintEvent(QPaintEvent * /*event*/)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRect r = this->rect();
    const int   w = r.width();
    const int   h = r.height();
    const qreal px = 1.0 / qMax<qreal>(0.01, this->devicePixelRatioF());
    const double contentLeft = px;
    const double contentTop = px;
    const double contentRight = qMax(contentLeft, static_cast<double>(w) - px);
    const double contentBottom = qMax(contentTop, static_cast<double>(h) - px);
    const double contentWidth = qMax(0.0, contentRight - contentLeft);
    const double contentHeight = qMax(0.0, contentBottom - contentTop);

    ////////////////////////////////////////////////////////////////////////////////////////////////////
    // Background
    ////////////////////////////////////////////////////////////////////////////////////////////////////
    const QColor bg = this->palette().color(QPalette::Base);
    const ColorScheme *scheme = ColorScheme::GetCurrent();
    p.fillRect(r, bg);

    // Fixed time axis slot geometry.
    const int sampleCount = qMax(2, this->m_sampleCapacity);
    const double stepX = contentWidth / static_cast<double>(sampleCount - 1);

    ////////////////////////////////////////////////////////////////////////////////////////////////////
    // Grid
    ////////////////////////////////////////////////////////////////////////////////////////////////////
    if (this->m_gridEnabled)
    {
        // Grid lines are a pale tint of the graph's own line colour.
        QColor grid = this->m_lineColor;
        grid.setAlpha(scheme->DarkMode ? 90 : 38);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(QPen(grid, 0));

        // A one-pixel grid line drawn as a filled rectangle is several times cheaper
        // than QPainter::drawLine(). The rectangle covers exactly the pixels a thin
        // (cosmetic, non-antialiased) line would: device coordinates snapped to 1/64
        // and rounded down, both end points included.
        const QTransform gridTransform = p.deviceTransform();
        const bool gridAsRects = gridTransform.type() <= QTransform::TxScale && gridTransform.m11() > 0.0 && gridTransform.m22() > 0.0;
        auto gridLine = [&](const QPointF &from, const QPointF &to)
        {
            if (!gridAsRects)
            {
                p.drawLine(from, to);
                return;
            }
            auto pixel = [](double deviceCoord) { return static_cast<qint64>(std::floor(std::round(deviceCoord * 64.0) / 64.0)); };
            const qint64 x0 = pixel(from.x() * gridTransform.m11() + gridTransform.dx());
            const qint64 y0 = pixel(from.y() * gridTransform.m22() + gridTransform.dy());
            const qint64 x1 = pixel(to.x() * gridTransform.m11() + gridTransform.dx()) + 1;
            const qint64 y1 = pixel(to.y() * gridTransform.m22() + gridTransform.dy()) + 1;
            p.fillRect(QRectF(QPointF((x0 - gridTransform.dx()) / gridTransform.m11(), (y0 - gridTransform.dy()) / gridTransform.m22()),
                              QPointF((x1 - gridTransform.dx()) / gridTransform.m11(), (y1 - gridTransform.dy()) / gridTransform.m22())), grid);
        };

        // Denser grid on larger widgets while keeping existing configured minimum.
        const int targetGridPxX = 80;
        const int targetGridPxY = 55;
        const int gridCols = qMax(this->m_gridCols, qMax(1, w / targetGridPxX));
        const int gridRows = qMax(this->m_gridRows, qMax(1, h / targetGridPxY));
        const qreal dpr = qMax<qreal>(0.01, this->devicePixelRatioF());

        // Horizontal lines snapped to physical pixels
        for (int i = 1; i < gridRows; ++i)
        {
            const double y = std::round((contentTop + contentHeight * i / gridRows) * dpr) / dpr;
            gridLine(QPointF(contentLeft, y), QPointF(contentRight, y));
        }

        // Vertical lines snap to time slots and phase-shift with sample updates.
        // This keeps graph points aligned to the same grid columns as data scrolls.
        const int gridSlotStep = qMax(1, sampleCount / gridCols);
        const int phase = this->m_historyTick % gridSlotStep;
        int lastX = -1;
        for (int slot = 1; slot < sampleCount - 1; ++slot)
        {
            if (((slot + phase) % gridSlotStep) != 0)
                continue;

            const double rawX = contentLeft + slot * stepX;
            const int physX = static_cast<int>(std::round(rawX * dpr));
            if (physX <= static_cast<int>(std::round(contentLeft * dpr)) ||
                physX >= static_cast<int>(std::round(contentRight * dpr)) ||
                physX == lastX)
                continue;
            const double x = physX / dpr;
            gridLine(QPointF(x, contentTop), QPointF(x, contentBottom));
            lastX = physX;
        }
        p.setRenderHint(QPainter::Antialiasing, true);
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////
    // Data
    ////////////////////////////////////////////////////////////////////////////////////////////////////
    // An empty graph still gets its frame and overlay text below.
    if (this->m_data && !this->m_data->IsEmpty())
    {

    // Keep a fixed-width time axis:
    // - when history is short, right-align it (empty area on the left)
    // - once full, new samples push older ones off the left edge
    //
    // Points for both series — left to right, newest sample on the right.
    // Slot s sits at x = contentLeft + s * stepX.
    struct Series
    {
        QPolygonF points;
        int       firstSlot { 0 };
        int lastSlot() const { return this->firstSlot + static_cast<int>(this->points.size()) - 1; }
    };
    auto buildSeries = [&](const HistoryBuffer *data) -> Series
    {
        Series s;
        if (!data || data->IsEmpty())
            return s;
        const int size = data->Size();
        const int visibleStart = qMax(0, size - sampleCount);
        const int visibleCount = size - visibleStart;
        s.firstSlot = qMax(0, sampleCount - visibleCount);
        s.points.reserve(visibleCount);
        for (int i = 0; i < visibleCount; ++i)
        {
            const double val = qBound(0.0, data->At(visibleStart + i), this->m_maxVal);
            s.points.append(QPointF(contentLeft + (s.firstSlot + i) * stepX,
                                    contentBottom - (val / this->m_maxVal) * contentHeight));
        }
        return s;
    };
    const Series primary = buildSeries(this->m_data);
    const Series overlay = buildSeries(this->m_overlayData);   // kernel time / send rate: darker fill on top

    // Fills the area below points[a..b]; `scratch` is reused between calls.
    QPolygonF scratch;
    auto fillBelow = [&](const Series &s, int a, int b, const QColor &color)
    {
        if (b < a)
            return;
        scratch.resize(0);
        scratch.reserve(b - a + 3);
        for (int i = a; i <= b; ++i)
            scratch.append(s.points.at(i));
        scratch.append(QPointF(s.points.at(b).x(), contentBottom));
        scratch.append(QPointF(s.points.at(a).x(), contentBottom));
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawPolygon(scratch);
    };
    const QPen linePen(this->m_lineColor, 1.0);
    // Paints fill, overlay fill and line for slots [slotA..slotB] of both series.
    auto drawRange = [&](int slotA, int slotB)
    {
        const int a1 = qMax(0, slotA - primary.firstSlot);
        const int b1 = qMin(static_cast<int>(primary.points.size()) - 1, slotB - primary.firstSlot);
        fillBelow(primary, a1, b1, this->m_fillColor);
        if (!overlay.points.isEmpty())
        {
            const int a2 = qMax(0, slotA - overlay.firstSlot);
            const int b2 = qMin(static_cast<int>(overlay.points.size()) - 1, slotB - overlay.firstSlot);
            fillBelow(overlay, a2, b2, this->m_fillColor2);
        }
        if (b1 >= a1)
        {
            p.setPen(linePen);
            p.setBrush(Qt::NoBrush);
            p.drawPolyline(primary.points.constData() + a1, b1 - a1 + 1);
        }
    };

    // Qt's antialiased rasteriser gets much slower than linearly as a shape gains
    // vertices (a 600-sample graph costs ~10x a 120-sample one). So long series are
    // painted in column chunks, each clipped to whole device pixels and overlapping
    // its neighbours by a few pixels of data: every pixel gets exactly the same
    // coverage as when painting the whole shape at once, at a fraction of the cost.
    static constexpr int kChunkMinPoints = 64;      // below this, one pass is as fast
    static constexpr int kChunkSlots = 32;
    static constexpr double kChunkMinPx = 24.0;     // don't make chunks narrower than this
    static constexpr double kOverlapPx = 4.0;       // > line half-width + antialiasing reach
    const QTransform deviceTransform = p.deviceTransform();
    const double scaleX = deviceTransform.m11();
    const bool canChunk = primary.points.size() >= kChunkMinPoints
                          && deviceTransform.type() <= QTransform::TxScale
                          && scaleX > 0.0 && stepX > 0.0;
    const int firstSlot = overlay.points.isEmpty() ? primary.firstSlot : qMin(primary.firstSlot, overlay.firstSlot);
    const int lastSlot = overlay.points.isEmpty() ? primary.lastSlot() : qMax(primary.lastSlot(), overlay.lastSlot());
    if (!canChunk)
    {
        drawRange(firstSlot, lastSlot);
    } else
    {
        const double stepPx = stepX * scaleX;
        const int chunkSlots = qMax(kChunkSlots, static_cast<int>(std::ceil(kChunkMinPx / stepPx)));
        const int overlapSlots = static_cast<int>(std::ceil(kOverlapPx / stepPx)) + 1;
        auto deviceColumn = [&](double x) { return x * scaleX + deviceTransform.dx(); };
        auto logicalX = [&](qint64 column) { return (static_cast<double>(column) - deviceTransform.dx()) / scaleX; };

        qint64 clipLeft = static_cast<qint64>(std::floor(deviceColumn(-2.0)));
        const qint64 clipEnd = static_cast<qint64>(std::ceil(deviceColumn(w + 2.0)));
        for (int s = firstSlot; s < lastSlot; s += chunkSlots)
        {
            const int e = qMin(lastSlot, s + chunkSlots);
            const qint64 clipRight = (e >= lastSlot) ? clipEnd
                                                     : static_cast<qint64>(std::floor(deviceColumn(contentLeft + e * stepX)));
            if (clipRight <= clipLeft)
                continue;
            p.save();
            p.setClipRect(QRectF(QPointF(logicalX(clipLeft), -2.0), QPointF(logicalX(clipRight), h + 2.0)), Qt::IntersectClip);
            drawRange(s - overlapSlots, e + overlapSlots);
            p.restore();
            clipLeft = clipRight;
        }
    }
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////
    // Border
    ////////////////////////////////////////////////////////////////////////////////////////////////////
    p.setRenderHint(QPainter::Antialiasing, false);
    {
        // Align the frame to physical pixels: with a fractional UI scale an
        // integer logical rect lands between device pixels and the top/right
        // edges get clipped away.
        const qreal px = 1.0 / qMax<qreal>(0.01, this->devicePixelRatioF());
        p.setPen(QPen(this->m_lineColor, px));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(px / 2.0, px / 2.0, qMax<qreal>(0.0, w - px), qMax<qreal>(0.0, h - px)));
    }
    p.setRenderHint(QPainter::Antialiasing, true);

    if (!this->m_overlayText.isEmpty())
    {
        p.setRenderHint(QPainter::TextAntialiasing, false);
        p.setFont(ClassicTheme::SnapCrispFont(QStringLiteral("Arial"), 9.0));
        p.setPen(scheme->GraphOverlayTextColor);
        p.drawText(r.adjusted(4, 2, -4, -2), Qt::AlignLeft | Qt::AlignTop, this->m_overlayText);
    }

    if (this->m_hoverLineEnabled && this->m_hoverSlot >= 0 && this->m_hoverSlot < sampleCount)
    {
        const int x = static_cast<int>(contentLeft + this->m_hoverSlot * stepX + 0.5);
        QColor hover = this->m_lineColor;
        hover.setAlpha(170);
        p.setPen(QPen(hover, 1));
        p.drawLine(QPointF(x, contentTop), QPointF(x, contentBottom));
    }
}

void GraphWidget::mouseMoveEvent(QMouseEvent *event)
{
    const int sampleCount = qMax(2, this->m_sampleCapacity);
    const double stepX = static_cast<double>(qMax(1, this->width())) / static_cast<double>(sampleCount - 1);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const double mouseX = event->position().x();
#else
    const double mouseX = event->pos().x();
#endif
    const int slot = qBound(0, static_cast<int>(std::lround(mouseX / stepX)), sampleCount - 1);
    const bool slotChanged = (slot != this->m_hoverSlot);

    if (slotChanged)
    {
        const QRect oldRect = this->hoverLineRect(this->m_hoverSlot);
        this->m_hoverSlot = slot;
        this->update(oldRect.united(this->hoverLineRect(this->m_hoverSlot)));
    }

    if (this->m_hoverTooltipEnabled && slotChanged)
    {
        const int idx1 = sampleIndexForSlot(this->m_data ? this->m_data->Size() : 0, slot, sampleCount);
        const int idx2 = sampleIndexForSlot(this->m_overlayData ? this->m_overlayData->Size() : 0, slot, sampleCount);

        if (idx1 >= 0 || idx2 >= 0)
        {
            QString tip;
            if (idx1 >= 0)
                tip += tr("%1: %2").arg(this->m_primaryName, this->formatValue(this->m_data->At(idx1)));
            if (idx2 >= 0)
            {
                if (!tip.isEmpty())
                    tip += "\n";
                tip += tr("%1: %2").arg(this->m_secondaryName, this->formatValue(this->m_overlayData->At(idx2)));
            }
            // One slot is one refresh interval, which is not always one second.
            const int intervalMs = qMax(1, CFG->RefreshRateMs);
            const double secAgo = static_cast<double>(sampleCount - 1 - slot) * intervalMs / 1000.0;
            if (!tip.isEmpty())
                tip += tr("\n%1 s ago").arg(QString::number(secAgo, 'f', (intervalMs % 1000) == 0 ? 0 : 1));
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            QToolTip::showText(event->globalPosition().toPoint(), tip, this);
#else
            QToolTip::showText(event->globalPos(), tip, this);
#endif
        } else
        {
            QToolTip::hideText();
        }
    }

    QWidget::mouseMoveEvent(event);
}

void GraphWidget::leaveEvent(QEvent *event)
{
    const QRect oldRect = this->hoverLineRect(this->m_hoverSlot);
    this->m_hoverSlot = -1;
    if (this->m_hoverTooltipEnabled)
        QToolTip::hideText();
    this->update(oldRect);
    QWidget::leaveEvent(event);
}

QRect GraphWidget::hoverLineRect(int slot) const
{
    if (!this->m_hoverLineEnabled || slot < 0)
        return {};

    const int sampleCount = qMax(2, this->m_sampleCapacity);
    const int width = qMax(1, this->width());
    const double contentLeft = (width > 2) ? 1.0 : 0.0;
    const double contentRight = qMax(contentLeft, static_cast<double>(width - 2));
    const double stepX = (contentRight - contentLeft) / static_cast<double>(sampleCount - 1);
    const int x = static_cast<int>(contentLeft + slot * stepX + 0.5);
    return QRect(x - 3, 0, 7, this->height()).intersected(this->rect());
}

int GraphWidget::sampleIndexForSlot(int size, int slot, int sampleCount)
{
    if (size <= 0 || slot < 0 || sampleCount < 2)
        return -1;
    const int visibleStart = qMax(0, size - sampleCount);
    const int visibleCount = size - visibleStart;
    const int slotOffset = qMax(0, sampleCount - visibleCount);
    if (slot < slotOffset || slot >= slotOffset + visibleCount)
        return -1;
    return visibleStart + (slot - slotOffset);
}

QString GraphWidget::formatValue(double v) const
{
    switch (this->m_valueFormat)
    {
        case ValueFormat::Percent:
        {
            const QString percent = QString::number(v, 'f', 1) + tr("%");
            if (!this->m_percentTooltipAbsoluteEnabled || this->m_percentTooltipAbsoluteMax <= 0.0)
                return percent;

            const double absolute = (v / 100.0) * this->m_percentTooltipAbsoluteMax;
            return tr("%1 (%2 %3)", "%1=percent text %2=absolute value %3=absolute unit")
                    .arg(percent,
                         QString::number(absolute, 'f', this->m_percentTooltipAbsolutePrecision),
                         this->m_percentTooltipAbsoluteUnit);
        }
        case ValueFormat::BytesPerSec:
            return Misc::FormatBytesPerSecond(v);
        case ValueFormat::BitsPerSec:
            return Misc::FormatBitsPerSecond(v);
        case ValueFormat::MegabytesPerSec:
            return Misc::FormatMegabytesPerSecond(v);
        case ValueFormat::Raw:
            return QString::number(v, 'f', 2);
        case ValueFormat::Auto:
        default:
            if (this->m_maxVal <= 100.0)
                return QString::number(v, 'f', 1) + tr("%");
            return QString::number(v, 'f', 2);
    }
}
