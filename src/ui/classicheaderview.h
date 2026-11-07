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

#ifndef CLASSICHEADERVIEW_H
#define CLASSICHEADERVIEW_H

#include <QHeaderView>
#include <QSet>
#include <functional>

//! Flat two-line table header: an optional large summary value (e.g. total CPU %)
//! above the column caption, sort chevron centred at the top, thin separators.
class ClassicHeaderView : public QHeaderView
{
    public:
        explicit ClassicHeaderView(QWidget *parent = nullptr);

        //! Returns the large summary text for a logical column, or an empty string.
        void SetTotalProvider(const std::function<QString(int)> &provider) { this->m_totalProvider = provider; }
        //! Logical columns whose caption is right-aligned (numeric columns).
        void SetRightAlignedSections(const QSet<int> &sections) { this->m_rightAligned = sections; }

        QSize sizeHint() const override;

    protected:
        void paintSection(QPainter *painter, const QRect &rect, int logicalIndex) const override;
        void mouseMoveEvent(QMouseEvent *event) override;
        void leaveEvent(QEvent *event) override;

    private:
        QFont totalFont() const;

        std::function<QString(int)> m_totalProvider;
        QSet<int> m_rightAligned;
        int m_hover { -1 };
};

#endif // CLASSICHEADERVIEW_H
