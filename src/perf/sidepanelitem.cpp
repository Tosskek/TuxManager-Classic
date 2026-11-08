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

#include "sidepanelitem.h"
#include "../configuration.h"
#include "../colorscheme.h"
#include "../ui/classictheme.h"
#include "globals.h"

#include <QPainter>
#include <QPaintEvent>
#include <QHBoxLayout>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QEnterEvent>
#endif

using namespace Perf;

SidePanelItem::SidePanelItem(const QString &title, QWidget *parent) : QWidget(parent), m_title(title), m_graph(new GraphWidget(this))
{
    this->setCursor(Qt::PointingHandCursor);
    this->setMouseTracking(true);

    // Layout: small framed thumbnail graph on the left; title/subtitle are
    // painted to its right in paintEvent.
    QHBoxLayout *lay = new QHBoxLayout(this);
    lay->setContentsMargins(kPadX, kPadY, 0, kPadY);
    lay->setSpacing(0);
    this->m_graph->setFixedSize(kGraphW, kGraphH);
    this->m_graph->SetSampleCapacity(TUX_MANAGER_HISTORY_SIZE);
    this->m_graph->SetGridEnabled(CFG->SidePanelGridEnabled);
    this->m_graph->SetHoverLineEnabled(false);
    this->m_graph->SetHoverTooltipEnabled(false);
    lay->addWidget(this->m_graph, 0, Qt::AlignVCenter);
    lay->addStretch(1);
    this->setLayout(lay);
}

void SidePanelItem::SetGraphSource(const HistoryBuffer &history, double maxVal)
{
    this->m_graph->SetDataSource(history, maxVal);
}

void SidePanelItem::Update(const QString &subtitle, double maxVal)
{
    this->m_subtitle = subtitle;
    this->m_graph->SetMax(maxVal);
    this->m_graph->Tick();
    this->update();
}

void SidePanelItem::SetSelected(bool selected)
{
    if (this->m_selected == selected)
        return;
    this->m_selected = selected;
    this->update();
}

void SidePanelItem::SetGraphColor(QColor line, QColor fill)
{
    this->m_graph->SetColor(line, fill);
}

void SidePanelItem::SetGraphGridEnabled(bool enabled)
{
    this->m_graph->SetGridEnabled(enabled);
}

// ── Paint ─────────────────────────────────────────────────────────────────────

void SidePanelItem::paintEvent(QPaintEvent *event)
{
    QWidget::paintEvent(event);   // draw children (the graph widget)

    QPainter p(this);
    p.setRenderHint(QPainter::TextAntialiasing, false);
    const ColorScheme *scheme = ColorScheme::GetCurrent();

    const QRect r = this->rect();

    // Background
    QColor bg;
    if (this->m_selected)
    {
        bg = scheme->SidePanelItemSelectedBackgroundColor;
    } else if (this->m_hovered)
    {
        bg = scheme->SidePanelItemHoverBackgroundColor;
    } else
    {
        bg = scheme->SidePanelItemBackgroundColor;
    }

    p.fillRect(r, bg);

    // Title (large) and subtitle (one or more lines) to the right of the graph.
    QFont titleFont = ClassicTheme::SnapCrispFont(QStringLiteral("Arial"), 14.5);
    QFont subFont = ClassicTheme::SnapCrispFont(QStringLiteral("Arial"), 10.0);
    const QFontMetrics titleFm(titleFont);
    const QFontMetrics subFm(subFont);

    const int textLeft = kPadX + kGraphW + 12;
    const int textW = qMax(0, r.width() - textLeft - 6);
    const QStringList subLines = this->m_subtitle.split('\n');
    const int blockH = titleFm.height() + 2 + subLines.size() * subFm.height();
    int y = qMax(2, (r.height() - blockH) / 2);

    p.setFont(titleFont);
    p.setPen(this->m_selected ? scheme->SidePanelItemSelectedTextColor
                              : scheme->SidePanelItemTextColor);
    p.drawText(QRect(textLeft, y, textW, titleFm.height()), Qt::AlignLeft | Qt::AlignVCenter,
               titleFm.elidedText(this->m_title, Qt::ElideRight, textW));
    y += titleFm.height() + 2;

    p.setFont(subFont);
    p.setPen(scheme->SidePanelItemSubtitleColor);
    // A tab splits a line into two columns: the second one starts at a fixed
    // position (width of "100%" plus a small gap) so it does not shift when the
    // first value gains or loses a digit.
    const int secondColumn = subFm.horizontalAdvance(QStringLiteral("100%")) + subFm.horizontalAdvance(QStringLiteral(" "));
    for (const QString &line : subLines)
    {
        const int tab = line.indexOf('\t');
        if (tab >= 0)
        {
            p.drawText(QRect(textLeft, y, secondColumn, subFm.height()), Qt::AlignLeft | Qt::AlignVCenter, line.left(tab));
            const int restW = qMax(0, textW - secondColumn);
            p.drawText(QRect(textLeft + secondColumn, y, restW, subFm.height()), Qt::AlignLeft | Qt::AlignVCenter,
                       subFm.elidedText(line.mid(tab + 1), Qt::ElideRight, restW));
        } else
        {
            p.drawText(QRect(textLeft, y, textW, subFm.height()), Qt::AlignLeft | Qt::AlignVCenter,
                       subFm.elidedText(line, Qt::ElideRight, textW));
        }
        y += subFm.height();
    }
}

// ── Events ────────────────────────────────────────────────────────────────────

void SidePanelItem::mousePressEvent(QMouseEvent *event)
{
    QWidget::mousePressEvent(event);
    if (event->button() == Qt::LeftButton)
        emit this->clicked();
    else if (event->button() == Qt::RightButton)
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        emit this->contextMenuRequested(event->globalPosition().toPoint());
#else
        emit this->contextMenuRequested(event->globalPos());
#endif
    }
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
void SidePanelItem::enterEvent(QEnterEvent *event)
#else
void SidePanelItem::enterEvent(QEvent *event)
#endif
{
    QWidget::enterEvent(event);
    this->m_hovered = true;
    this->update();
}

void SidePanelItem::leaveEvent(QEvent *event)
{
    QWidget::leaveEvent(event);
    this->m_hovered = false;
    this->update();
}
