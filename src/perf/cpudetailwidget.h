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

#ifndef PERF_CPUDETAILWIDGET_H
#define PERF_CPUDETAILWIDGET_H

#include "cpugrapharea.h"

#include <QMenu>
#include <QWidget>

QT_BEGIN_NAMESPACE
namespace Ui { class CpuDetailWidget; }
QT_END_NAMESPACE

class QLabel;

namespace Perf
{
    class CpuDetailWidget : public QWidget
    {
        Q_OBJECT

        public:
            explicit CpuDetailWidget(QWidget *parent = nullptr);
            ~CpuDetailWidget();

            void Init();
            void ApplyColorScheme();

        private slots:
            void onUpdated();
            void onContextMenuRequested(const QPoint &globalPos);

        private:
            Ui::CpuDetailWidget  *ui;
            QLabel *m_graphCaptionLeft { nullptr };
            QLabel *m_graphCaptionRight { nullptr };
            CpuGraphArea         *m_graphArea  { nullptr };
    };
} // namespace Perf

#endif // PERF_CPUDETAILWIDGET_H
