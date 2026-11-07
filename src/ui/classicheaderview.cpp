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

#include "classicheaderview.h"
#include "classictheme.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

ClassicHeaderView::ClassicHeaderView(QWidget *parent) : QHeaderView(Qt::Horizontal, parent)
{
    this->setMouseTracking(true);
    this->setSectionsClickable(true);
    this->setHighlightSections(false);
    this->setDefaultAlignment(Qt::AlignLeft | Qt::AlignBottom);
}

QFont ClassicHeaderView::totalFont() const
{
    QFont f = this->font();
    f.setPointSizeF(f.pointSizeF() * 1.6);
    return f;
}

QSize ClassicHeaderView::sizeHint() const
{
    QSize s = QHeaderView::sizeHint();
    const int h = QFontMetrics(this->totalFont()).height() + this->fontMetrics().height() + 14;
    s.setHeight(qMax(s.height(), h));
    return s;
}

void ClassicHeaderView::paintSection(QPainter *p, const QRect &rect, int logical) const
{
    if (!rect.isValid() || !this->model())
        return;

    p->save();
    p->setRenderHint(QPainter::Antialiasing, false);
    const bool dark = ClassicTheme::IsDark();
    const QColor bg = dark ? QColor(0x1a, 0x1a, 0x1a) : QColor(0xff, 0xff, 0xff);
    const QColor hoverBg = dark ? QColor(0x2a, 0x3b, 0x4d) : QColor(0xd9, 0xeb, 0xf9);
    const QColor captionColor = dark ? QColor(0x9f, 0xb4, 0xcc) : QColor(0x4c, 0x60, 0x7a);
    const QColor totalColor = dark ? QColor(0xf0, 0xf0, 0xf0) : QColor(0x00, 0x00, 0x00);
    p->fillRect(rect, logical == this->m_hover ? hoverBg : bg);

    p->setPen(dark ? QColor(0x33, 0x33, 0x33) : QColor(0xe5, 0xe5, 0xe5));
    p->drawLine(rect.topRight(), rect.bottomRight());
    p->setPen(dark ? QColor(0x40, 0x40, 0x40) : QColor(0xd0, 0xd0, 0xd0));
    p->drawLine(rect.bottomLeft(), rect.bottomRight());

    const QRect inner = rect.adjusted(7, 3, -7, -5);
    const bool right = this->m_rightAligned.contains(logical);
    const Qt::Alignment hAlign = right ? Qt::AlignRight : Qt::AlignLeft;

    // Caption on the bottom line.
    const QString caption = this->model()->headerData(logical, this->orientation(), Qt::DisplayRole).toString();
    p->setFont(this->font());
    p->setPen(captionColor);
    p->drawText(inner, hAlign | Qt::AlignBottom,
                this->fontMetrics().elidedText(caption, Qt::ElideRight, inner.width()));

    // Large summary value above it.
    const QString total = this->m_totalProvider ? this->m_totalProvider(logical) : QString();
    if (!total.isEmpty())
    {
        const QFont tf = this->totalFont();
        const QFontMetrics tfm(tf);
        QRect totalRect = inner;
        totalRect.setBottom(inner.bottom() - this->fontMetrics().height());
        p->setFont(tf);
        p->setPen(totalColor);
        p->drawText(totalRect, hAlign | Qt::AlignBottom, tfm.elidedText(total, Qt::ElideRight, inner.width()));
    }

    // Sort chevron, centred at the top edge.
    if (this->isSortIndicatorShown() && this->sortIndicatorSection() == logical)
    {
        p->setRenderHint(QPainter::Antialiasing, true);
        const qreal cx = rect.center().x() + 0.5;
        const qreal top = rect.top() + 3.5;
        const qreal w = 4.5, h = 3.0;
        QPainterPath chevron;
        if (this->sortIndicatorOrder() == Qt::DescendingOrder)
        {
            chevron.moveTo(cx - w, top);
            chevron.lineTo(cx, top + h);
            chevron.lineTo(cx + w, top);
        } else
        {
            chevron.moveTo(cx - w, top + h);
            chevron.lineTo(cx, top);
            chevron.lineTo(cx + w, top + h);
        }
        p->setPen(QPen(QColor(0x80, 0x80, 0x80), 1.0));
        p->setBrush(Qt::NoBrush);
        p->drawPath(chevron);
    }

    p->restore();
}

void ClassicHeaderView::mouseMoveEvent(QMouseEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const int idx = this->logicalIndexAt(event->position().toPoint());
#else
    const int idx = this->logicalIndexAt(event->pos());
#endif
    if (idx != this->m_hover)
    {
        this->m_hover = idx;
        this->viewport()->update();
    }
    QHeaderView::mouseMoveEvent(event);
}

void ClassicHeaderView::leaveEvent(QEvent *event)
{
    this->m_hover = -1;
    this->viewport()->update();
    QHeaderView::leaveEvent(event);
}
