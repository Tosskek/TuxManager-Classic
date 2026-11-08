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

#include "uihelper.h"
#include "../aboutdialog.h"
#include "../configuration.h"
#include "../metrics.h"
#include "../misc.h"
#include "../performancewidget.h"
#include "../colorscheme.h"
#include "classictheme.h"

#include <QAbstractItemModel>
#include <QAction>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QBoxLayout>
#include <QEvent>
#include <QLayout>
#include <QMenu>
#include <QStringList>
#include <QScrollBar>
#include <QTableView>
#include <QTimer>
#include <QApplication>
#include <QClipboard>

QString UIHelper::GetVisibleRowText(const QTableView *view, int row)
{
    if (!view || !view->model() || row < 0)
        return QString();

    const QAbstractItemModel *model = view->model();
    const QHeaderView *header = view->horizontalHeader();
    QStringList parts;
    parts.reserve(model->columnCount());
    for (int col = 0; col < model->columnCount(); ++col)
    {
        if (header && header->isSectionHidden(col))
            continue;

        const QModelIndex idx = model->index(row, col);
        parts << model->data(idx, Qt::DisplayRole).toString();
    }
    return parts.join('\t');
}

QString UIHelper::GetVisibleRowsText(const QTableView *view, const QList<int> &rows)
{
    QStringList lines;
    lines.reserve(rows.size());
    for (int row : rows)
        lines << GetVisibleRowText(view, row);

    return lines.join('\n');
}

UIHelper::TableSelectionSnapshot UIHelper::CaptureTableSelection(const QTableView *view, int keyColumn, const std::function<QVariant(const QModelIndex &proxyKeyIndex)> &proxyKeyResolver)
{
    TableSelectionSnapshot out;
    if (!view || !view->model())
        return out;

    if (const QScrollBar *vsb = view->verticalScrollBar())
        out.ScrollPos = vsb->value();

    QItemSelectionModel *selectionModel = view->selectionModel();
    if (!selectionModel || !proxyKeyResolver)
        return out;

    const QModelIndexList selectedRows = selectionModel->selectedRows(keyColumn);
    for (const QModelIndex &proxyIdx : selectedRows)
    {
        const QVariant key = proxyKeyResolver(proxyIdx);
        if (!key.isNull() && !out.SelectedKeys.contains(key))
            out.SelectedKeys.append(key);
    }

    const QModelIndex currentProxy = view->currentIndex();
    if (currentProxy.isValid())
        out.CurrentKey = proxyKeyResolver(currentProxy.sibling(currentProxy.row(), keyColumn));

    return out;
}

void UIHelper::RestoreTableSelection(QTableView *view,
                           int keyColumn,
                           int sourceRowCount,
                           const std::function<QModelIndex(int sourceRow)> &sourceIndexForRow,
                           const std::function<QModelIndex(const QModelIndex &sourceIndex)> &sourceToProxy,
                           const std::function<QVariant(const QModelIndex &sourceKeyIndex)> &sourceKeyResolver,
                           const UIHelper::TableSelectionSnapshot &snapshot)
{
    if (!view || !view->model())
        return;

    if (!sourceIndexForRow || !sourceToProxy || !sourceKeyResolver)
        return;

    QItemSelectionModel *selectionModel = view->selectionModel();
    if (!selectionModel)
        return;

    selectionModel->clearSelection();
    QModelIndex restoredCurrentProxy;

    for (int row = 0; row < sourceRowCount; ++row)
    {
        const QModelIndex sourceKeyIdx = sourceIndexForRow(row);
        if (!sourceKeyIdx.isValid())
            continue;

        const QModelIndex proxyIdx = sourceToProxy(sourceKeyIdx);
        if (!proxyIdx.isValid())
            continue;

        const QVariant key = sourceKeyResolver(sourceKeyIdx);
        if (snapshot.SelectedKeys.contains(key))
            selectionModel->select(proxyIdx, QItemSelectionModel::Select | QItemSelectionModel::Rows);

        if (!snapshot.CurrentKey.isNull() && key == snapshot.CurrentKey)
            restoredCurrentProxy = proxyIdx;
    }

    if (restoredCurrentProxy.isValid())
    {
        selectionModel->setCurrentIndex(restoredCurrentProxy, QItemSelectionModel::NoUpdate);
    } else
    {
        const QModelIndexList selectedRows = selectionModel->selectedRows(keyColumn);
        if (!selectedRows.isEmpty())
            selectionModel->setCurrentIndex(selectedRows.first(), QItemSelectionModel::NoUpdate);
    }

    if (QScrollBar *vsb = view->verticalScrollBar())
        vsb->setValue(snapshot.ScrollPos);
}

void UIHelper::AddGlobalContextMenuItems(QMenu *menu, QWidget *parent)
{
    if (!menu)
        return;

    menu->addSeparator();
    ClassicTheme::AddToggleAction(menu);
    QAction *aboutAction = menu->addAction(QObject::tr("About TuxManager"));
    QObject::connect(aboutAction, &QAction::triggered, menu, [menu, parent]()
    {
        QWidget *dialogParent = parent ? parent : qobject_cast<QWidget *>(menu->parent());
        AboutDialog dialog(dialogParent);
        dialog.exec();
    });
}

void UIHelper::EnableCopyLabelContextMenu(QLabel *label)
{
    if (!label)
        return;

    label->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(label, &QWidget::customContextMenuRequested, label, [label](const QPoint &pos)
    {
        QString text = label->text();
        text.replace('\r', ' ');
        text.replace('\n', ' ');
        QMenu menu(label);
        QAction *copyAction = menu.addAction(QObject::tr("Copy"));
        copyAction->setEnabled(!text.isEmpty() && text != QString::fromUtf8("—"));
        QObject::connect(copyAction, &QAction::triggered, &menu, [text]()
        {
            QApplication::clipboard()->setText(text);
        });
        menu.exec(label->mapToGlobal(pos));
    });

}

QAction *UIHelper::AddCopyWidgetAction(QMenu *menu, QWidget *widget, const QString &text)
{
    if (!menu || !widget)
        return nullptr;

    QAction *copyAction = menu->addAction(text.isEmpty() ? QObject::tr("Copy graph") : text);
    QObject::connect(copyAction, &QAction::triggered, menu, [widget]()
    {
        QApplication::clipboard()->setPixmap(widget->grab());
    });
    return copyAction;
}

void UIHelper::AddRefreshIntervalContextMenu(QMenu *menu, QTimer *timer, bool timerOwnerActive)
{
    QMenu *refreshMenu = menu->addMenu(QObject::tr("Refresh interval"));

    QHash<QAction *, int> intervalActions;
    intervalActions.clear();

    for (int ms : CFG->RefreshRateAvailableIntervals)
    {
        QAction *a = refreshMenu->addAction(Misc::SimplifyTimeMS(ms));
        a->setCheckable(true);
        a->setChecked(!CFG->RefreshPaused && CFG->RefreshRateMs == ms);
        intervalActions.insert(a, ms);

        QObject::connect(a, &QAction::triggered, menu, [ a, intervalActions, timer, timerOwnerActive]()
        {          
            const int ms = intervalActions.value(a);
            CFG->RefreshPaused = false;
            CFG->RefreshRateMs = ms;
            Metrics::Get()->SetInterval(ms);
            PerformanceWidget *pw = PerformanceWidget::Get();
            if (pw && pw->IsActive())
                pw->onProviderUpdated();

            if (timer && timerOwnerActive)
                timer->start(ms);
        });
    }

    refreshMenu->addSeparator();

    QAction *pausedAction = refreshMenu->addAction(QObject::tr("Paused"));
    pausedAction->setCheckable(true);
    pausedAction->setChecked(CFG->RefreshPaused);

    QObject::connect(pausedAction, &QAction::triggered, menu, [ timer, timerOwnerActive]()
    {
        CFG->RefreshPaused = true;
        PerformanceWidget *pw = PerformanceWidget::Get();
        if (pw && pw->IsActive())
            pw->onProviderUpdated();

        if (timer && timerOwnerActive)
            timer->stop();
    });
}

void UIHelper::AddGraphWindowContextMenu(QMenu *menu)
{
    QMenu *timeMenu = menu->addMenu(QObject::tr("Graph time"));
    QVector<int> intervals = CFG->DataWindowAvailableIntervals;
    if (intervals.isEmpty())
        intervals.append(CFG->PerfGraphWindowSec);
    QHash<QAction *, int> graphWindowActions;

    for (int sec : intervals)
    {
        QAction *a = timeMenu->addAction(Misc::SimplifyTime(sec));
        a->setCheckable(true);
        a->setChecked(CFG->PerfGraphWindowSec == sec);
        graphWindowActions.insert(a, sec);

        QObject::connect(a, &QAction::triggered, menu, [=]()
        {          
            const int requestedWindow = graphWindowActions.value(a);
            CFG->PerfGraphWindowSec = requestedWindow;
            PerformanceWidget *pw = PerformanceWidget::Get();
            if (pw)
            {
                pw->applyGraphWindowSeconds();
                if (pw->IsActive())
                    pw->onProviderUpdated();
            }
        });
    }
}

void UIHelper::AddGraphContextMenuItems(QMenu *menu, QWidget *graphArea)
{
    AddRefreshIntervalContextMenu(menu);
    AddGraphWindowContextMenu(menu);

    menu->addSeparator();
    AddCopyWidgetAction(menu, graphArea, QObject::tr("Copy graph"));
}

void UIHelper::EnableGraphContextMenu(QWidget *widget)
{
    if (!widget)
        return;

    widget->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(widget, &QWidget::customContextMenuRequested, widget, [widget](const QPoint &pos)
    {
        QMenu menu(widget);
        AddGraphContextMenuItems(&menu, widget);
        menu.exec(widget->mapToGlobal(pos));
    });
}

void UIHelper::ApplyClassicStats(QGridLayout *grid, const QList<QLabel *> &bigValues, int bigColumns)
{
    if (!grid)
        return;

    // Collect label/value pairs in reading order.
    QList<QPair<QLabel *, QLabel *>> pairs;
    for (int row = 0; row < grid->rowCount(); ++row)
    {
        for (int col = 0; col + 1 < grid->columnCount(); col += 2)
        {
            QLayoutItem *li = grid->itemAtPosition(row, col);
            QLayoutItem *vi = grid->itemAtPosition(row, col + 1);
            QLabel *label = li ? qobject_cast<QLabel *>(li->widget()) : nullptr;
            QLabel *value = vi ? qobject_cast<QLabel *>(vi->widget()) : nullptr;
            if (label && value)
                pairs.append(qMakePair(label, value));
        }
    }
    if (pairs.isEmpty())
        return;

    for (const auto &pair : pairs)
    {
        grid->removeWidget(pair.first);
        grid->removeWidget(pair.second);
    }

    // Typography modelled on the classic layout: captions and table text share
    // one comfortable size in grey, big values are large and light, rows breathe.
    QGridLayout *big = new QGridLayout();
    big->setHorizontalSpacing(30);
    big->setVerticalSpacing(4);
    QGridLayout *small = new QGridLayout();
    small->setHorizontalSpacing(28);
    small->setVerticalSpacing(7);

    const ColorScheme *scheme = ColorScheme::GetCurrent();
    const QString valueColor = scheme->CpuTitleColor.name();
    const QString captionStyle = QString("color: %1; font-family: Arial; font-size: 10pt;").arg(scheme->StatLabelColor.name());
    const QString bigValueStyle = QString("color: %1; font-family: Arial; font-size: 16pt;").arg(valueColor);
    const QString tableValueStyle = QString("color: %1; font-family: Arial; font-size: 10pt;").arg(valueColor);
    int bigIndex = 0;
    int smallRow = 0;
    for (const auto &pair : pairs)
    {
        if (bigValues.contains(pair.second))
        {
            const int r = (bigIndex / bigColumns) * 3;
            const int c = bigIndex % bigColumns;
            pair.first->setStyleSheet(captionStyle);
            pair.second->setStyleSheet(bigValueStyle);
            big->addWidget(pair.first, r, c, Qt::AlignLeft | Qt::AlignBottom);
            big->addWidget(pair.second, r + 1, c, Qt::AlignLeft | Qt::AlignTop);
            big->setRowMinimumHeight(r + 2, 12);
            ++bigIndex;
        } else
        {
            QString text = pair.first->text().trimmed();
            if (!text.isEmpty() && !text.endsWith(':'))
                pair.first->setText(text + ':');
            pair.first->setStyleSheet(captionStyle);
            pair.second->setStyleSheet(tableValueStyle);
            small->addWidget(pair.first, smallRow, 0, Qt::AlignLeft | Qt::AlignVCenter);
            small->addWidget(pair.second, smallRow, 1, Qt::AlignLeft | Qt::AlignVCenter);
            ++smallRow;
        }
    }
    big->setRowStretch(big->rowCount(), 1);
    small->setRowStretch(small->rowCount(), 1);

    grid->setHorizontalSpacing(56);
    grid->setContentsMargins(0, 8, 0, 0);
    grid->addLayout(big, 0, 0, Qt::AlignLeft | Qt::AlignTop);
    grid->addLayout(small, 0, 1, Qt::AlignLeft | Qt::AlignTop);
    grid->setColumnStretch(0, 0);
    grid->setColumnStretch(1, 0);
    grid->setColumnStretch(2, 1);
}

void UIHelper::ApplyClassicPageProportions(QBoxLayout *layout, const QList<QPair<QWidget *, int>> &graphWeights, int bottomWeight)
{
    if (!layout)
        return;
    for (int i = 0; i < layout->count(); ++i)
        layout->setStretch(i, 0);
    for (const auto &gw : graphWeights)
    {
        const int idx = layout->indexOf(gw.first);
        if (idx >= 0)
            layout->setStretch(idx, gw.second);
        if (gw.first)
            gw.first->setMinimumHeight(qMin(gw.first->minimumHeight(), 60));
    }
    if (bottomWeight > 0)
        layout->addStretch(bottomWeight);

    // Height of each graph follows the page width; the main graph gets the
    // ~0.42 ratio the CPU page uses, smaller ones scale by their weight.
    int maxWeight = 1;
    for (const auto &gw : graphWeights)
        maxWeight = qMax(maxWeight, gw.second);
    QList<QPair<QWidget *, double>> ratios;
    for (const auto &gw : graphWeights)
        ratios.append(qMakePair(gw.first, 0.42 * gw.second / maxWeight));
    if (QWidget *page = layout->parentWidget())
        ApplyClassicPageGeometry(page, ratios);
}


namespace
{
    class ClassicGeometryFilter : public QObject
    {
        public:
            ClassicGeometryFilter(QWidget *page, int baseRightMargin, const QList<QPair<QWidget *, double>> &ratios)
                : QObject(page), m_baseRight(baseRightMargin), m_ratios(ratios) {}

            bool eventFilter(QObject *watched, QEvent *event) override
            {
                if (event->type() == QEvent::Resize || event->type() == QEvent::Show)
                {
                    QWidget *page = static_cast<QWidget *>(watched);
                    QLayout *lay = page->layout();
                    if (!lay)
                        return false;
                    // No extra right margin: pages that want narrower graphs use
                    // ApplyGraphRowsNarrower, everything else uses the full width.
                    const QMargins m = lay->contentsMargins();
                    const int contentWidth = page->width() - m.left() - m.right();

                    // Ideal height of every graph follows the width. If the page is
                    // too short for all of them, scale them down by one common factor
                    // so the page keeps its proportions instead of squashing one graph.
                    int idealSum = 0, graphMinSum = 0;
                    QList<int> ideals;
                    for (const auto &gr : this->m_ratios)
                    {
                        const int extra = gr.first ? gr.first->property("classicExtraHeight").toInt() : 0;
                        // A page may draw its graphs narrower than the content
                        // (classicGraphWidthFactor); heights follow that width.
                        const double widthFactor = page->property("classicGraphWidthFactor").isValid()
                                                   ? page->property("classicGraphWidthFactor").toDouble() : 1.0;
                        const int ideal = gr.first ? qMax(48, static_cast<int>(contentWidth * widthFactor * gr.second)) + extra : 0;
                        ideals.append(ideal);
                        idealSum += ideal;
                        if (gr.first)
                            graphMinSum += gr.first->minimumHeight();
                    }
                    const int others = qMax(0, lay->minimumSize().height() - graphMinSum);
                    const int available = page->height() - others;
                    const double scale = (idealSum > 0) ? qBound(0.25, double(available) / idealSum, 1.0) : 1.0;
                    for (int i = 0; i < this->m_ratios.size(); ++i)
                    {
                        QWidget *graph = this->m_ratios.at(i).first;
                        if (!graph)
                            continue;
                        const int extra = graph->property("classicExtraHeight").toInt();
                        graph->setMinimumHeight(30 + extra);
                        graph->setMaximumHeight(qMax(30 + extra, int((ideals.at(i) - extra) * scale) + extra));
                    }
                }
                return QObject::eventFilter(watched, event);
            }

        private:
            int m_baseRight;
            QList<QPair<QWidget *, double>> m_ratios;
    };
}

void UIHelper::ApplyClassicPageGeometry(QWidget *page, const QList<QPair<QWidget *, double>> &graphRatios)
{
    if (!page || !page->layout())
        return;
    page->installEventFilter(new ClassicGeometryFilter(page, page->layout()->contentsMargins().right(), graphRatios));
}

namespace
{
    class GraphRowsNarrower : public QObject
    {
        public:
            GraphRowsNarrower(QWidget *page, QBoxLayout *pageLayout, const QList<QWidget *> &graphs,
                              const QList<QLayout *> &rows, double fraction, int extraInsetPx = 0)
                : QObject(page), m_layout(pageLayout), m_graphs(graphs), m_rows(rows), m_fraction(fraction), m_extraInset(extraInsetPx) {}

            bool eventFilter(QObject *watched, QEvent *event) override
            {
                if (event->type() == QEvent::Resize || event->type() == QEvent::Show)
                {
                    QWidget *page = static_cast<QWidget *>(watched);
                    const QMargins m = this->m_layout->contentsMargins();
                    const int contentWidth = qMax(0, page->width() - m.left() - m.right());
                    const int inset = static_cast<int>(contentWidth * this->m_fraction) + this->m_extraInset;
                    for (QWidget *graph : this->m_graphs)
                        graph->setMaximumWidth(qMax(60, contentWidth - inset));
                    for (QLayout *row : this->m_rows)
                    {
                        QMargins rm = row->contentsMargins();
                        if (rm.right() != inset)
                        {
                            rm.setRight(inset);
                            row->setContentsMargins(rm);
                        }
                    }
                }
                return QObject::eventFilter(watched, event);
            }

        private:
            QBoxLayout *m_layout;
            QList<QWidget *> m_graphs;
            QList<QLayout *> m_rows;
            double m_fraction;
            int m_extraInset;
    };
}

void UIHelper::ApplyGraphRowsNarrower(QWidget *page, QBoxLayout *pageLayout, const QList<QWidget *> &graphs,
                                      const QList<QLayout *> &rows, double fraction, int extraInsetPx)
{
    if (!page || !pageLayout)
        return;
    page->setProperty("classicGraphWidthFactor", 1.0 - fraction);
    page->installEventFilter(new GraphRowsNarrower(page, pageLayout, graphs, rows, fraction, extraInsetPx));
}
