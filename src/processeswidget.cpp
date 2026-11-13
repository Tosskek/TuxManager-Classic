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

#include "processeswidget.h"
#include "ui/classictheme.h"
#include "os/processhelper.h"
#include "os/selfpriority.h"
#include "mainwindow.h"
#ifdef TM_MODEL_TEST
#include <QAbstractItemModelTester>
#endif
#include "configuration.h"
#include "globals.h"
#include "logger.h"
#include "runtaskdialog.h"
#include "ui_processeswidget.h"
#include "ui/uihelper.h"
#include "ui/classicheaderview.h"
#include "metrics.h"
#include "system/cpu.h"
#include "system/memory.h"
#include "system/storage.h"

#include <QAction>
#include <QClipboard>
#include <QGuiApplication>
#include <QHeaderView>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QScrollBar>
#include <QSortFilterProxyModel>
#include <QStandardPaths>
#include <QStyle>
#include <QTreeView>
#include <QVBoxLayout>
#include <QFileInfo>
#include <QMetaObject>

#include <algorithm>
#include <functional>
#include <signal.h>
#include <unistd.h>

ProcessesWidget::ProcessesWidget(OS::ProcessRefreshService *processRefreshService, QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::ProcessesWidget)
    , m_model(new OS::ProcessModel(this))
    , m_treeModel(new OS::ProcessTreeModel(this))
    , m_proxy(new OS::ProcessFilterProxy(this))
    , m_treeProxy(new OS::ProcessTreeProxy(this))
    , m_refreshTimer(new QTimer(this))
    , m_processRefreshService(processRefreshService)
{
    this->ui->setupUi(this);

    this->setupTable();

    connect(this->ui->searchEdit, &QLineEdit::textChanged, this, [this](const QString &text)
    {
        this->m_proxy->setFilterFixedString(text);
        this->m_treeProxy->setFilterFixedString(text);
        // While searching, show every match (also inside collapsed program rows);
        // clearing the search collapses them again.
        if (!text.isEmpty())
            this->m_treeView->expandAll();
        else
        {
            this->m_treeView->collapseAll();
            this->m_treeDefaultExpansionDone = false;
            this->applyDefaultTreeExpansion();   // back to: Apps open, the rest closed
        }
    });
    // A match that turns up later (a program started while the search is active)
    // arrives as new, collapsed rows: open them too. Merged into one pass per refresh.
    connect(this->m_treeProxy, &QAbstractItemModel::rowsInserted, this, [this]()
    {
        if (this->ui->searchEdit->text().isEmpty() || this->m_searchExpandQueued)
            return;
        this->m_searchExpandQueued = true;
        QTimer::singleShot(0, this, [this]()
        {
            this->m_searchExpandQueued = false;
            if (!this->ui->searchEdit->text().isEmpty() && this->m_treeView)
                this->m_treeView->expandAll();
        });
    });
    connect(this->ui->runNewTaskButton, &QPushButton::clicked, this, &ProcessesWidget::runNewTask);
    connect(this->ui->openTerminalButton, &QPushButton::clicked, this, &ProcessesWidget::openTerminal);
    connect(this->m_refreshTimer, &QTimer::timeout, this, &ProcessesWidget::onTimerTick);
    connect(this->m_processRefreshService, &OS::ProcessRefreshService::snapshotReady, this, &ProcessesWidget::onRefreshFinished);

    // Update status bar whenever the proxy's visible row count changes
    // (model reset after refresh, or filter toggle)
    connect(this->m_proxy, &QAbstractItemModel::modelReset, this, &ProcessesWidget::updateStatusBar);
    connect(this->m_proxy, &QAbstractItemModel::rowsInserted, this, &ProcessesWidget::updateStatusBar);
    connect(this->m_proxy, &QAbstractItemModel::rowsRemoved, this, &ProcessesWidget::updateStatusBar);
    connect(this->m_treeProxy, &QAbstractItemModel::modelReset, this, &ProcessesWidget::updateStatusBar);
    connect(this->m_treeProxy, &QAbstractItemModel::rowsInserted, this, &ProcessesWidget::updateStatusBar);
    connect(this->m_treeProxy, &QAbstractItemModel::rowsRemoved, this, &ProcessesWidget::updateStatusBar);

    // MainWindow controls active/inactive state based on current top tab.
}

ProcessesWidget::~ProcessesWidget()
{
    delete this->ui;
}

void ProcessesWidget::ClearSearchFilter()
{
    this->ui->searchEdit->clear();
}

bool ProcessesWidget::SelectProcessByPid(pid_t pid)
{
    return this->m_treeViewMode ? this->selectProcessInTree(pid) : this->selectProcessInTable(pid);
}

bool ProcessesWidget::selectProcessInTable(pid_t pid)
{
    if (pid <= 0)
        return false;

    for (int row = 0; row < this->m_model->rowCount(); ++row)
    {
        const QModelIndex sourceIdx = this->m_model->index(row, OS::ProcessModel::ColPid);
        if (!sourceIdx.isValid())
            continue;
        if (static_cast<pid_t>(this->m_model->data(sourceIdx, Qt::UserRole).toLongLong()) != pid)
            continue;

        const QModelIndex proxyIdx = this->m_proxy->mapFromSource(sourceIdx);
        if (!proxyIdx.isValid())
            return false;

        QItemSelectionModel *selectionModel = this->ui->tableView->selectionModel();
        if (!selectionModel)
            return false;

        selectionModel->clearSelection();
        selectionModel->select(proxyIdx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        selectionModel->setCurrentIndex(proxyIdx, QItemSelectionModel::NoUpdate);
        this->ui->tableView->scrollTo(proxyIdx, QAbstractItemView::PositionAtCenter);
        return true;
    }

    return false;
}

bool ProcessesWidget::selectProcessInTree(pid_t pid)
{
    if (pid <= 0)
        return false;

    const QModelIndex sourceIdx = this->m_treeModel->IndexForPid(pid);
    if (!sourceIdx.isValid())
        return false;

    const QModelIndex proxyIdx = this->m_treeProxy->mapFromSource(sourceIdx);
    if (!proxyIdx.isValid())
        return false;

    QItemSelectionModel *selectionModel = this->m_treeView->selectionModel();
    if (!selectionModel)
        return false;

    selectionModel->clearSelection();
    selectionModel->select(proxyIdx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    selectionModel->setCurrentIndex(proxyIdx, QItemSelectionModel::NoUpdate);
    this->m_treeView->scrollTo(proxyIdx, QAbstractItemView::PositionAtCenter);
    this->m_treeView->expand(proxyIdx.parent());
    return true;
}

// ── Private setup ─────────────────────────────────────────────────────────────

void ProcessesWidget::setupTable()
{
    static constexpr int PROCESS_COLUMN_SCHEMA_VERSION = 4;
    const int savedProcessColumnSchemaVersion = CFG->ProcessColumnSchemaVersion;
    const bool resetProcessHeaderState = savedProcessColumnSchemaVersion < 1;
    const bool migrateProcessNameFirstLayout = savedProcessColumnSchemaVersion < 2;
    // v3: classic column set (Name, Status, CPU, Memory, Disk) plus the new Disk column.
    // v4: re-applied once for the Windows-style grouped list (one row per program).
    const bool applyClassicColumns = savedProcessColumnSchemaVersion < 4;
    const auto classicLayout = [](QHeaderView *header)
    {
        using M = OS::ProcessModel;
        for (int col = 0; col < M::ColCount; ++col)
            header->setSectionHidden(col, true);
        const int order[] = { M::ColName, M::ColState, M::ColCpu, M::ColMemRss, M::ColDisk };
        int visual = 0;
        for (int col : order)
        {
            header->setSectionHidden(col, false);
            header->moveSection(header->visualIndex(col), visual++);
        }
        header->resizeSection(M::ColName, 280);
        header->resizeSection(M::ColState, 110);
        header->resizeSection(M::ColCpu, 80);
        header->resizeSection(M::ColMemRss, 95);
        header->resizeSection(M::ColDisk, 95);
        header->setSortIndicator(M::ColCpu, Qt::DescendingOrder);
    };
    if (resetProcessHeaderState && CFG->ProcessListSortColumn > OS::ProcessModel::ColMemVirt)
        CFG->ProcessListSortColumn += 3;

#ifdef TM_MODEL_TEST
    new QAbstractItemModelTester(this->m_treeModel, QAbstractItemModelTester::FailureReportingMode::Fatal, this);
    new QAbstractItemModelTester(this->m_model, QAbstractItemModelTester::FailureReportingMode::Fatal, this);
#endif
    this->m_proxy->setSourceModel(this->m_model);
    this->m_treeProxy->setSourceModel(this->m_treeModel);
    this->m_treeProxy->setSortRole(Qt::UserRole);
    this->m_treeProxy->setFilterRole(Qt::DisplayRole);
    this->m_treeProxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    this->m_treeProxy->setFilterKeyColumn(-1);
    this->m_treeProxy->setRecursiveFilteringEnabled(true);

    // Load persisted view toggles
    this->m_proxy->ShowKernelTasks     = CFG->ShowKernelTasks;
    this->m_proxy->ShowOtherUsersProcs = CFG->ShowOtherUsersProcs;
    this->m_model->SetShowKernelTasks(CFG->ShowKernelTasks);
    this->m_model->SetShowOtherUsersProcs(CFG->ShowOtherUsersProcs);

    QTableView *tv = this->ui->tableView;
    this->m_treeView = new QTreeView(this);

    // The process list uses a larger font, icons and rows than the rest of the
    // window, for readability (like Windows Task Manager's list).
    const QFont listFont = ClassicTheme::SnapCrispFont(QStringLiteral("Arial"), 12.0);
    tv->setFont(listFont);
    this->m_treeView->setFont(listFont);
    this->m_treeModel->SetBaseFont(listFont);
    tv->setIconSize(QSize(20, 20));
    this->m_treeView->setIconSize(QSize(20, 20));
    this->m_treeView->setStyleSheet(QStringLiteral("QTreeView::item { padding-top: 3px; padding-bottom: 3px; }"));
    this->applyIconSetting();

    if (QVBoxLayout *vl = qobject_cast<QVBoxLayout *>(this->layout()))
    {
        const int tablePos = vl->indexOf(tv);
        if (tablePos >= 0)
            vl->insertWidget(tablePos + 1, this->m_treeView);
    }

    // Classic two-line headers: totals for CPU / Memory above the captions.
    {
        const auto totalProvider = [this](int logical) -> QString
        {
            // Sum of the rows: always current and consistent with them (the
            // system-wide CPU meter only runs while the Performance tab is open).
            if (logical == OS::ProcessModel::ColCpu)
                return QString::number(qMin(100.0, this->m_snapshotCpuTotal), 'f', 0) + "%";
            if (logical == OS::ProcessModel::ColMemRss)
            {
                const qint64 total = Metrics::GetMemory()->MemTotalKb();
                const qint64 used = Metrics::GetMemory()->MemUsedKb();
                return QString::number(total > 0 ? qRound(100.0 * used / total) : 0) + "%";
            }
            if (logical == OS::ProcessModel::ColDisk)
            {
                double busiest = 0.0;
                Storage *storage = Metrics::GetStorage();
                for (int i = 0; i < storage->DiskCount(); ++i)
                    busiest = qMax(busiest, storage->FromIndex(i).ActivePct);
                return QString::number(busiest, 'f', 0) + "%";
            }
            return QString();
        };
        const QSet<int> numeric = { OS::ProcessModel::ColPid, OS::ProcessModel::ColCpu, OS::ProcessModel::ColMemRss,
                                    OS::ProcessModel::ColMemVirt, OS::ProcessModel::ColMemShared, OS::ProcessModel::ColMemText,
                                    OS::ProcessModel::ColMemData, OS::ProcessModel::ColIoReads, OS::ProcessModel::ColIoWrites,
                                    OS::ProcessModel::ColIoReadsPerSec, OS::ProcessModel::ColIoWritesPerSec,
                                    OS::ProcessModel::ColThreads, OS::ProcessModel::ColPriority, OS::ProcessModel::ColNice,
                                    OS::ProcessModel::ColDisk };

        ClassicHeaderView *tableHeader = new ClassicHeaderView(tv);
        tableHeader->SetTotalProvider(totalProvider);
        tableHeader->SetRightAlignedSections(numeric);
        tv->setHorizontalHeader(tableHeader);

        ClassicHeaderView *treeHeader = new ClassicHeaderView(this->m_treeView);
        treeHeader->SetTotalProvider(totalProvider);
        treeHeader->SetRightAlignedSections(numeric);
        this->m_treeView->setHeader(treeHeader);

        connect(Metrics::Get(), &Metrics::updated, this, [tableHeader, treeHeader]()
        {
            tableHeader->viewport()->update();
            treeHeader->viewport()->update();
        });
    }

    // Flat rows: one line per process, no zebra striping, a little breathing room.
    tv->setWordWrap(false);
    tv->setAlternatingRowColors(false);
    tv->setTextElideMode(Qt::ElideRight);
    tv->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    tv->verticalHeader()->setDefaultSectionSize(qMax(tv->fontMetrics().height(), tv->iconSize().height()) + 10);

    tv->setModel(this->m_proxy);
    tv->setSortingEnabled(true);
    tv->sortByColumn(CFG->ProcessListSortColumn, static_cast<Qt::SortOrder>(CFG->ProcessListSortOrder));

    QHeaderView *hv = tv->horizontalHeader();
    hv->setSectionsMovable(true);
    hv->setStretchLastSection(false);
    hv->setContextMenuPolicy(Qt::CustomContextMenu);
    hv->setSectionResizeMode(QHeaderView::Interactive);
    hv->setSectionResizeMode(OS::ProcessModel::ColCmdline, QHeaderView::Stretch);
    connect(hv, &QHeaderView::customContextMenuRequested, this, [this, hv](const QPoint &pos)
    {
        this->showHeaderContextMenu(hv, OS::ProcessModel::ColCount, [this](int col)
        {
            return this->m_model->headerData(col, Qt::Horizontal).toString();
        }, pos);
    });
    connect(hv, &QHeaderView::sectionMoved, this, [this]() { this->saveTableHeaderState(); });
    connect(hv, &QHeaderView::sectionResized, this, [this]() { this->saveTableHeaderState(); });

    connect(hv, &QHeaderView::sortIndicatorChanged, this, [](int column, Qt::SortOrder order)
    {
        CFG->ProcessListSortColumn = column;
        CFG->ProcessListSortOrder  = static_cast<int>(order);
    });

    tv->verticalHeader()->hide();
    tv->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tv->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tv->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(tv, &QTableView::customContextMenuRequested, this, &ProcessesWidget::onTableContextMenu);

    // Reasonable default column widths
    tv->setColumnWidth(OS::ProcessModel::ColPid,      60);
    tv->setColumnWidth(OS::ProcessModel::ColName,    160);
    tv->setColumnWidth(OS::ProcessModel::ColUser,     90);
    tv->setColumnWidth(OS::ProcessModel::ColState,    90);
    tv->setColumnWidth(OS::ProcessModel::ColCpu,      65);
    tv->setColumnWidth(OS::ProcessModel::ColMemRss,   80);
    tv->setColumnWidth(OS::ProcessModel::ColMemVirt,  80);
    tv->setColumnWidth(OS::ProcessModel::ColMemShared, 80);
    tv->setColumnWidth(OS::ProcessModel::ColMemText,  80);
    tv->setColumnWidth(OS::ProcessModel::ColMemData,  80);
    tv->setColumnWidth(OS::ProcessModel::ColIoReads,  90);
    tv->setColumnWidth(OS::ProcessModel::ColIoWrites, 90);
    tv->setColumnWidth(OS::ProcessModel::ColIoReadsPerSec, 100);
    tv->setColumnWidth(OS::ProcessModel::ColIoWritesPerSec, 100);
    tv->setColumnWidth(OS::ProcessModel::ColThreads,  65);
    tv->setColumnWidth(OS::ProcessModel::ColPriority, 65);
    tv->setColumnWidth(OS::ProcessModel::ColNice,     50);
    // Hide less-common columns by default
    hv->hideSection(OS::ProcessModel::ColMemVirt);
    hv->hideSection(OS::ProcessModel::ColMemShared);
    hv->hideSection(OS::ProcessModel::ColMemText);
    hv->hideSection(OS::ProcessModel::ColMemData);
    hv->hideSection(OS::ProcessModel::ColPriority);
    hv->hideSection(OS::ProcessModel::ColNice);
    hv->hideSection(OS::ProcessModel::ColIoReads);
    hv->hideSection(OS::ProcessModel::ColIoWrites);
    hv->hideSection(OS::ProcessModel::ColIoReadsPerSec);
    hv->hideSection(OS::ProcessModel::ColIoWritesPerSec);
    if (!resetProcessHeaderState && !CFG->ProcessListHeaderState.isEmpty())
    {
        hv->restoreState(CFG->ProcessListHeaderState);
    }
    if (migrateProcessNameFirstLayout)
    {
        // Through 1.0.8, PID was the first column. Migrate it once so names and icons
        // lead both process views, then preserve any column order chosen by the user.
        hv->moveSection(hv->visualIndex(OS::ProcessModel::ColName), 0);
    }
    if (applyClassicColumns)
    {
        classicLayout(hv);
        tv->sortByColumn(OS::ProcessModel::ColCpu, Qt::DescendingOrder);
        CFG->ProcessListSortColumn = OS::ProcessModel::ColCpu;
        CFG->ProcessListSortOrder = static_cast<int>(Qt::DescendingOrder);
    }
    this->m_tableHeaderPersistenceEnabled = true;

    this->m_treeView->setModel(this->m_treeProxy);
    this->m_treeView->setSortingEnabled(true);
    this->m_treeView->sortByColumn(CFG->ProcessListSortColumn, static_cast<Qt::SortOrder>(CFG->ProcessListSortOrder));
    this->m_treeView->setAlternatingRowColors(false);
    this->m_treeView->setWordWrap(false);
    this->m_treeView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    this->m_treeView->setSelectionBehavior(QAbstractItemView::SelectRows);
    this->m_treeView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    this->m_treeView->setContextMenuPolicy(Qt::CustomContextMenu);
    this->m_treeView->setRootIsDecorated(true);
    // In the past we had PID for the original tree column, it looked odd since icons were added so now we show the hierarchy beside names and icons instead.
    this->m_treeView->setTreePosition(OS::ProcessTreeModel::ColName);
    this->m_treeView->setItemsExpandable(true);
    this->m_treeView->setAnimated(false);
    this->m_treeView->setUniformRowHeights(true);
    QHeaderView *treeHeader = this->m_treeView->header();
    treeHeader->setSectionsMovable(true);
    treeHeader->setContextMenuPolicy(Qt::CustomContextMenu);
    treeHeader->setSectionResizeMode(QHeaderView::Interactive);
    treeHeader->setStretchLastSection(false);
    treeHeader->setSectionResizeMode(OS::ProcessTreeModel::ColCmdline, QHeaderView::Stretch);
    connect(treeHeader, &QHeaderView::customContextMenuRequested, this, [this, treeHeader](const QPoint &pos)
    {
        this->showHeaderContextMenu(treeHeader, OS::ProcessTreeModel::ColCount, [this](int col)
        {
            return this->m_treeModel->headerData(col, Qt::Horizontal).toString();
        }, pos);
    });
    connect(treeHeader, &QHeaderView::sectionMoved, this, [this]() { this->saveTreeHeaderState(); });
    connect(treeHeader, &QHeaderView::sectionResized, this, [this]() { this->saveTreeHeaderState(); });
    this->m_treeView->setColumnWidth(OS::ProcessTreeModel::ColName, 220);
    this->m_treeView->setColumnWidth(OS::ProcessTreeModel::ColUser, 90);
    this->m_treeView->setColumnWidth(OS::ProcessTreeModel::ColState, 90);
    this->m_treeView->setColumnWidth(OS::ProcessTreeModel::ColCpu, 65);
    this->m_treeView->setColumnWidth(OS::ProcessTreeModel::ColMemRss, 80);
    this->m_treeView->setColumnWidth(OS::ProcessTreeModel::ColMemVirt, 80);
    this->m_treeView->setColumnWidth(OS::ProcessTreeModel::ColMemShared, 80);
    this->m_treeView->setColumnWidth(OS::ProcessTreeModel::ColMemText, 80);
    this->m_treeView->setColumnWidth(OS::ProcessTreeModel::ColMemData, 80);
    this->m_treeView->setColumnWidth(OS::ProcessTreeModel::ColIoReads, 90);
    this->m_treeView->setColumnWidth(OS::ProcessTreeModel::ColIoWrites, 90);
    this->m_treeView->setColumnWidth(OS::ProcessTreeModel::ColIoReadsPerSec, 100);
    this->m_treeView->setColumnWidth(OS::ProcessTreeModel::ColIoWritesPerSec, 100);
    this->m_treeView->setColumnWidth(OS::ProcessTreeModel::ColThreads, 65);
    this->m_treeView->setColumnWidth(OS::ProcessTreeModel::ColPriority, 65);
    this->m_treeView->setColumnWidth(OS::ProcessTreeModel::ColNice, 50);
    this->m_treeView->setColumnHidden(OS::ProcessTreeModel::ColMemVirt, true);
    this->m_treeView->setColumnHidden(OS::ProcessTreeModel::ColMemShared, true);
    this->m_treeView->setColumnHidden(OS::ProcessTreeModel::ColMemText, true);
    this->m_treeView->setColumnHidden(OS::ProcessTreeModel::ColMemData, true);
    this->m_treeView->setColumnHidden(OS::ProcessTreeModel::ColPriority, true);
    this->m_treeView->setColumnHidden(OS::ProcessTreeModel::ColNice, true);
    this->m_treeView->setColumnHidden(OS::ProcessTreeModel::ColIoReads, true);
    this->m_treeView->setColumnHidden(OS::ProcessTreeModel::ColIoWrites, true);
    this->m_treeView->setColumnHidden(OS::ProcessTreeModel::ColIoReadsPerSec, true);
    this->m_treeView->setColumnHidden(OS::ProcessTreeModel::ColIoWritesPerSec, true);
    connect(this->m_treeView, &QTreeView::expanded, this, [this]() { this->ensureNameColumnFits(); });
    if (!resetProcessHeaderState && !CFG->ProcessTreeHeaderState.isEmpty())
    {
        treeHeader->restoreState(CFG->ProcessTreeHeaderState);
    }
    if (migrateProcessNameFirstLayout)
        treeHeader->moveSection(treeHeader->visualIndex(OS::ProcessTreeModel::ColName), 0);
    this->m_treeView->setColumnWidth(OS::ProcessTreeModel::ColPid, 65);
    if (applyClassicColumns)
    {
        classicLayout(treeHeader);
        this->m_treeView->sortByColumn(OS::ProcessTreeModel::ColCpu, Qt::DescendingOrder);
        // Grouped (tree) view by default so kernel workers fold under kthreadd.
        CFG->ProcessTreeView = true;
    }
    this->syncAllProcessColumnVisibility();
    this->updateStretchColumns();
    this->m_treeHeaderPersistenceEnabled = true;
    if (savedProcessColumnSchemaVersion < PROCESS_COLUMN_SCHEMA_VERSION)
    {
        CFG->ProcessColumnSchemaVersion = PROCESS_COLUMN_SCHEMA_VERSION;
        this->saveTableHeaderState();
        this->saveTreeHeaderState();
    }
    connect(this->m_treeView, &QTreeView::customContextMenuRequested, this, &ProcessesWidget::onTreeContextMenu);

    // Del keyboard shortcut to terminate process
    QAction *terminate_action = new QAction(tr("Terminate  (SIGTERM)"), this);
    terminate_action->setShortcut(QKeySequence::Delete);
    terminate_action->setShortcutContext(Qt::WidgetShortcut);
    connect(terminate_action, &QAction::triggered, this, &ProcessesWidget::terminateSelectedProcesses);
    tv->addAction(terminate_action);
    this->m_treeView->addAction(terminate_action);

    this->updateIOMetricsEnabledState(false);

    this->setTreeViewMode(CFG->ProcessTreeView);
}

void ProcessesWidget::setTreeViewMode(bool enabled)
{
    this->m_treeViewMode = enabled;
    CFG->ProcessTreeView = enabled;

    if (enabled)
    {
        this->m_treeModel->SetProcesses(this->m_lastProcessSnapshot.isEmpty() ? this->m_model->GetProcesses() : this->m_lastProcessSnapshot);
        this->applyDefaultTreeExpansion();
    } else if (!this->m_lastProcessSnapshot.isEmpty())
    {
        // The flat model is not updated while the tree is shown; catch it up now.
        this->m_model->SetProcesses(this->m_lastProcessSnapshot);
    }

    if (!this->m_treeView || !this->ui->tableView)
        return;

    this->ui->tableView->setVisible(!enabled);
    this->m_treeView->setVisible(enabled);
}

void ProcessesWidget::ApplyRefreshRate()
{
    if (this->m_active && !CFG->RefreshPaused)
        this->m_refreshTimer->start(CFG->RefreshRateMs);
}

void ProcessesWidget::SetActive(bool active)
{
    if (this->m_active == active)
        return;

    this->m_active = active;
    if (active)
    {
        // Refresh immediately when user switches to Processes tab.
        this->startRefresh();
        this->m_refreshTimer->start(CFG->RefreshRateMs);
    } else
    {
        this->m_refreshTimer->stop();
        this->m_refreshPending = false;
    }
}

// ── Slots ─────────────────────────────────────────────────────────────────────

void ProcessesWidget::onTimerTick()
{
    if (CFG->RefreshPaused)
        return;
    this->startRefresh();
}

void ProcessesWidget::startRefresh()
{
    if (CFG->RefreshPaused)
    {
        this->m_refreshPending = false;
        return;
    }

    if (this->m_tableContextMenuOpen)
    {
        this->m_refreshPending = true;
        return;
    }

    if (this->m_refreshInFlight)
    {
        this->m_refreshPending = true;
        // The reader normally answers in milliseconds. When it does not (the kernel is
        // not handing out process data: out of memory, a hung mount...), say so
        // instead of silently showing an old list.
        if (this->m_refreshClock.isValid() && this->m_refreshClock.elapsed() > 5000)
            this->ui->statusLabel->setText(tr("The system is not answering - this list is %1 s old").arg(this->m_refreshClock.elapsed() / 1000));
        return;
    }

    this->m_refreshClock.start();
    this->m_refreshInFlight = true;
    this->m_refreshPending = false;
    ++this->m_refreshToken;

    OS::Process::LoadOptions options;
    options.IncludeKernelTasks = this->m_proxy->ShowKernelTasks;
    options.IncludeOtherUsers = this->m_proxy->ShowOtherUsersProcs;
    options.MyUID = ::getuid();
    options.CollectIOMetrics = CFG->IOMetricsEnabled;
    // statm is only needed for the MEM SHR / TEXT / DATA columns (hidden by default).
    {
        const QHeaderView *hv = this->ui->tableView->horizontalHeader();
        options.CollectMemoryDetails = !hv->isSectionHidden(OS::ProcessModel::ColMemShared)
                                       || !hv->isSectionHidden(OS::ProcessModel::ColMemText)
                                       || !hv->isSectionHidden(OS::ProcessModel::ColMemData);
    }
    options.IsSuperuser = CFG->IsSuperuser;
    options.EffectiveUID = CFG->EUID;

    this->m_processRefreshService->RequestSnapshot(OS::ProcessRefreshService::Consumer::Processes,
                                                   this->m_refreshToken,
                                                   options);
}

void ProcessesWidget::onRefreshFinished(int consumer, quint64 token, const QList<OS::Process> &processes)
{
    if (consumer != static_cast<int>(OS::ProcessRefreshService::Consumer::Processes) || token != this->m_refreshToken)
        return;

    this->m_refreshInFlight = false;

    if (!this->m_active)
        return;

    if (this->m_tableContextMenuOpen)
    {
        this->m_refreshPending = true;
        return;
    }

    UIHelper::TableSelectionSnapshot tableSnapshot;
    QList<pid_t> treeSelection;
    QSet<pid_t> expandedPids;
    pid_t treeCurrentPid = 0;
    int treeScroll = 0;

    if (!this->m_treeViewMode)
    {
        tableSnapshot = UIHelper::CaptureTableSelection(this->ui->tableView, OS::ProcessModel::ColPid, std::bind(&ProcessesWidget::tableSelectionKeyFromProxy, this, std::placeholders::_1));
    }
    // Tree mode needs no capture: the tree model updates rows in place, so the view
    // keeps expansion, selection and scroll position by itself.
    this->m_lastProcessSnapshot = processes;
    this->m_snapshotCpuTotal = 0.0;
    this->m_snapshotThreadTotal = 0;
    for (const OS::Process &proc : std::as_const(this->m_lastProcessSnapshot))
    {
        this->m_snapshotCpuTotal += proc.CPUPercent;
        this->m_snapshotThreadTotal += qMax(0, proc.Threads);
    }
    // Memory and disk totals in the header come from the system meters, which only
    // sample while the Performance tab is open; refresh the cheap ones here.
    if (!Metrics::Get()->IsActive())
    {
        Metrics::GetMemory()->Sample();
        Metrics::GetStorage()->Sample();
    }
    if (QHeaderView *h = this->m_treeView->header())
        h->viewport()->update();
    this->ui->tableView->horizontalHeader()->viewport()->update();
    if (this->m_appRegistry)
        this->m_appRegistry->Annotate(this->m_lastProcessSnapshot);

    if (this->m_treeViewMode)
    {
        // The flat table is hidden in tree mode, so it is not updated here (it is
        // refilled from m_lastProcessSnapshot when switching back to table mode).
        this->m_windowPids.Refresh();
        this->m_treeModel->SetWindowPids(this->m_windowPids.Pids(), this->m_windowPids.IsAvailable());
        if (this->m_treeModel->SetProcesses(this->m_lastProcessSnapshot))
            this->restoreTreeStateDeferred(expandedPids, treeSelection, treeCurrentPid, treeScroll);
        this->applyDefaultTreeExpansion();
        this->updateStatusBar();
    } else
    {
        this->m_model->SetProcesses(this->m_lastProcessSnapshot);
        UIHelper::RestoreTableSelection(
            this->ui->tableView,
            OS::ProcessModel::ColPid,
            this->m_model->rowCount(),
            [this](int row) -> QModelIndex { return this->m_model->index(row, OS::ProcessModel::ColPid); },
            [this](const QModelIndex &sourceIndex) -> QModelIndex { return this->m_proxy->mapFromSource(sourceIndex); },
            [this](const QModelIndex &sourceKeyIndex) -> QVariant { return this->m_model->data(sourceKeyIndex, Qt::UserRole); },
            tableSnapshot);
    }

    if (this->m_active && this->m_refreshPending && !CFG->RefreshPaused)
    {
        this->m_refreshPending = false;
        QMetaObject::invokeMethod(this, &ProcessesWidget::startRefresh, Qt::QueuedConnection);
    }
}

void ProcessesWidget::onHeaderContextMenu(const QPoint &pos)
{
    QHeaderView *hv = this->ui->tableView->horizontalHeader();
    this->showHeaderContextMenu(hv, OS::ProcessModel::ColCount, [this](int col)
    {
        return this->m_model->headerData(col, Qt::Horizontal).toString();
    }, pos);
}

void ProcessesWidget::showHeaderContextMenu(QHeaderView *header, int columnCount, const std::function<QString(int)> &titleForColumn, const QPoint &pos)
{
    if (!header)
        return;

    QMenu menu(this);

    for (int col = 0; col < columnCount; ++col)
    {
        QAction *action = menu.addAction(titleForColumn(col));
        action->setCheckable(true);
        action->setChecked(!header->isSectionHidden(col));
        action->setData(col);
    }

    UIHelper::AddGlobalContextMenuItems(&menu, this);

    const QAction *chosen = menu.exec(header->mapToGlobal(pos));
    if (!chosen)
        return;

    // Only the column check boxes; the shared menu items below them handle themselves
    // (they used to fall through here as "column 0" and hide it).
    if (!chosen->isCheckable() || !chosen->data().isValid())
        return;
    const int col = chosen->data().toInt();
    if (col < 0 || col >= columnCount)
        return;
    if (!chosen->isChecked())
    {
        // Keep Name, and at least one column: with none left there is no header to
        // right-click to get them back.
        int visible = 0;
        for (int c = 0; c < columnCount; ++c)
            visible += header->isSectionHidden(c) ? 0 : 1;
        if (col == OS::ProcessModel::ColName || visible <= 1)
            return;
    }
    this->syncProcessColumnVisibility(col, !chosen->isChecked());
    this->updateStretchColumns();
    this->saveTableHeaderState();
    this->saveTreeHeaderState();
    this->updateIOMetricsEnabledState();
}

void ProcessesWidget::saveTableHeaderState() const
{
    if (!this->m_tableHeaderPersistenceEnabled)
        return;

    if (QHeaderView *header = this->ui->tableView ? this->ui->tableView->horizontalHeader() : nullptr)
    {
        CFG->ProcessListHeaderState = header->saveState();
        MainWindow::ScheduleSave();
    }
}

void ProcessesWidget::saveTreeHeaderState() const
{
    if (!this->m_treeHeaderPersistenceEnabled)
        return;

    if (this->m_treeView && this->m_treeView->header())
    {
        CFG->ProcessTreeHeaderState = this->m_treeView->header()->saveState();
        MainWindow::ScheduleSave();
    }
}

void ProcessesWidget::syncProcessColumnVisibility(int column, bool hidden)
{
    if (column < 0 || column >= OS::ProcessModel::ColCount)
        return;

    if (QHeaderView *tableHeader = this->ui && this->ui->tableView ? this->ui->tableView->horizontalHeader() : nullptr)
        tableHeader->setSectionHidden(column, hidden);
    if (this->m_treeView)
        this->m_treeView->setColumnHidden(column, hidden);
}

void ProcessesWidget::updateStretchColumns()
{
    // The Name column takes whatever width the other columns leave, so the list
    // always fills the window and follows it when it is resized. When the command
    // line column is shown, that one stretches instead and Name keeps its width.
    const auto apply = [](QHeaderView *header)
    {
        if (!header)
            return;
        const bool cmdlineShown = !header->isSectionHidden(OS::ProcessModel::ColCmdline);
        header->setStretchLastSection(false);
        header->setSectionResizeMode(OS::ProcessModel::ColCmdline, QHeaderView::Stretch);
        header->setSectionResizeMode(OS::ProcessModel::ColName, cmdlineShown ? QHeaderView::Interactive : QHeaderView::Stretch);
    };
    apply(this->ui->tableView ? this->ui->tableView->horizontalHeader() : nullptr);
    apply(this->m_treeView ? this->m_treeView->header() : nullptr);
}

void ProcessesWidget::syncAllProcessColumnVisibility()
{
    if (!this->ui || !this->ui->tableView || !this->m_treeView)
        return;

    QHeaderView *tableHeader = this->ui->tableView->horizontalHeader();
    QHeaderView *treeHeader = this->m_treeView->header();
    if (!tableHeader || !treeHeader)
        return;

    for (int col = 0; col < OS::ProcessModel::ColCount; ++col)
    {
        const bool visible = !tableHeader->isSectionHidden(col) || !treeHeader->isSectionHidden(col);
        this->syncProcessColumnVisibility(col, !visible);
    }
}

bool ProcessesWidget::anyIOMetricsColumnVisible() const
{
    const QHeaderView *tableHeader = this->ui && this->ui->tableView ? this->ui->tableView->horizontalHeader() : nullptr;
    if (!tableHeader)
        return false;

    return !tableHeader->isSectionHidden(OS::ProcessModel::ColIoReads)
           || !tableHeader->isSectionHidden(OS::ProcessModel::ColIoWrites)
           || !tableHeader->isSectionHidden(OS::ProcessModel::ColIoReadsPerSec)
           || !tableHeader->isSectionHidden(OS::ProcessModel::ColIoWritesPerSec)
           || !tableHeader->isSectionHidden(OS::ProcessModel::ColDisk);
}

void ProcessesWidget::updateIOMetricsEnabledState(bool triggerImmediateRefresh)
{
    const bool enabled = this->anyIOMetricsColumnVisible();
    const bool changed = (CFG->IOMetricsEnabled != enabled);

    CFG->IOMetricsEnabled = enabled;

    if (changed && enabled && triggerImmediateRefresh && this->m_active && !CFG->RefreshPaused && !this->m_tableContextMenuOpen)
        this->startRefresh();
}

void ProcessesWidget::onTableContextMenu(const QPoint &pos)
{
    // Pause refresh during context menu so we don't loose the selection
    this->m_tableContextMenuOpen = true;

    const QModelIndex clickedIndex = this->ui->tableView->indexAt(pos);
    const QModelIndex targetIndex = clickedIndex.isValid() ? clickedIndex : this->ui->tableView->currentIndex();
    this->m_contextMenuTargetIndex = targetIndex;
    const bool hasTargetCell = targetIndex.isValid();
    const QModelIndexList selectedRowsIdx = this->ui->tableView->selectionModel()->selectedRows();
    QList<int> selectedRows;
    selectedRows.reserve(selectedRowsIdx.size());
    for (const QModelIndex &idx : selectedRowsIdx)
        selectedRows.append(idx.row());
    std::sort(selectedRows.begin(), selectedRows.end());
    const bool hasSelectedRows = !selectedRows.isEmpty();
    const bool multipleRowsSelected = selectedRows.size() > 1;
    const bool hasRowTarget = hasTargetCell || hasSelectedRows;

    const QList<pid_t> pids = this->selectedPids();
    const bool hasSelection = !pids.isEmpty();

    QMenu menu(this);

    // ── Copy submenu ────────────────────────────────────────────────────────
    QMenu *copyMenu = menu.addMenu(tr("Copy"));

    QAction *copyRowAct = copyMenu->addAction(tr("Entire row"));
    copyRowAct->setEnabled(hasRowTarget);
    connect(copyRowAct, &QAction::triggered, this, &ProcessesWidget::copyRowSelectionToClipboard);

    QAction *copyCellAct = copyMenu->addAction(tr("Selected cell"));
    copyCellAct->setEnabled(hasTargetCell && !multipleRowsSelected);
    connect(copyCellAct, &QAction::triggered, this, &ProcessesWidget::copyCellSelectionToClipboard);

    QString copyPIDLabel;
    if (multipleRowsSelected)
        copyPIDLabel = tr("PIDs");
    else
        copyPIDLabel = tr("PID");

    QAction *copyPidAct = copyMenu->addAction(copyPIDLabel);
    copyPidAct->setEnabled(hasSelection);
    connect(copyPidAct, &QAction::triggered, this, &ProcessesWidget::copySelectedPidsToClipboard);

    menu.addSeparator();

    // ── View submenu — always visible ────────────────────────────────────────
    QMenu *viewMenu = menu.addMenu(tr("View"));

    QAction *kernelAct = viewMenu->addAction(tr("Kernel tasks"));
    kernelAct->setCheckable(true);
    kernelAct->setChecked(this->m_proxy->ShowKernelTasks);
    connect(kernelAct, &QAction::toggled, this, &ProcessesWidget::setShowKernelTasks);

    QAction *otherUsersAct = viewMenu->addAction(tr("Processes of other users"));
    otherUsersAct->setCheckable(true);
    otherUsersAct->setChecked(this->m_proxy->ShowOtherUsersProcs);
    connect(otherUsersAct, &QAction::toggled, this, &ProcessesWidget::setShowOtherUsersProcesses);

    QAction *iconsAct = viewMenu->addAction(tr("Show icons"));
    iconsAct->setCheckable(true);
    iconsAct->setChecked(CFG->ShowProcessIcons);
    connect(iconsAct, &QAction::toggled, this, &ProcessesWidget::setShowIcons);

    viewMenu->addSeparator();
    QAction *tableModeAct = viewMenu->addAction(tr("Table view"));
    tableModeAct->setCheckable(true);
    tableModeAct->setChecked(!this->m_treeViewMode);
    connect(tableModeAct, &QAction::triggered, this, [this]() { this->setTreeViewMode(false); });

    QAction *treeModeAct = viewMenu->addAction(tr("Tree view"));
    treeModeAct->setCheckable(true);
    treeModeAct->setChecked(this->m_treeViewMode);
    connect(treeModeAct, &QAction::triggered, this, [this]() { this->setTreeViewMode(true); });
    // ── Send signal submenu — requires selection ────────────────────────────
    menu.addSeparator();
    QMenu *signalMenu = menu.addMenu(tr("Send signal"));
    signalMenu->setEnabled(hasSelection);

    struct { const char *label; int sig; } commonSignals[] =
    {
        { "SIGTERM  (15) — Terminate",   SIGTERM  },
        { "SIGKILL   (9) — Kill (force)", SIGKILL  },
        { "SIGHUP    (1) — Hangup",       SIGHUP   },
        { "SIGSTOP  (19) — Stop",         SIGSTOP  },
        { "SIGCONT  (18) — Continue",     SIGCONT  },
        { "SIGINT    (2) — Interrupt",    SIGINT   },
        { "SIGUSR1  (10) — User 1",       SIGUSR1  },
        { "SIGUSR2  (12) — User 2",       SIGUSR2  },
    };
    for (const auto &s : commonSignals)
    {
        QAction *a = signalMenu->addAction(tr(s.label));
        a->setData(s.sig);
        connect(a, &QAction::triggered, this, [this, s]()
        {
            this->sendSignalToSelected(s.sig);
        });
    }

    signalMenu->addSeparator();
    QAction *customSig = signalMenu->addAction(tr("Custom signal..."));
    connect(customSig, &QAction::triggered, this, &ProcessesWidget::promptAndSendCustomSignal);

    // ── Quick-access kill / term — requires selection ───────────────────────
    menu.addSeparator();
    QAction *termAction = menu.addAction(tr("Terminate  (SIGTERM)"));
    termAction->setEnabled(hasSelection);
    termAction->setShortcut(QKeySequence::Delete);
    termAction->setShortcutVisibleInContextMenu(true);
    connect(termAction, &QAction::triggered, this, &ProcessesWidget::terminateSelectedProcesses);

    QAction *killAction = menu.addAction(tr("Kill  (SIGKILL)"));
    killAction->setEnabled(hasSelection);
    killAction->setIcon(style()->standardIcon(QStyle::SP_MessageBoxCritical));
    connect(killAction, &QAction::triggered, this, &ProcessesWidget::killSelectedProcesses);

    // ── Priority ─────────────────────────────────────────────────────────────
    menu.addSeparator();
    QAction *reniceAction = menu.addAction(tr("Change priority (renice)..."));
    reniceAction->setEnabled(hasSelection);
    connect(reniceAction, &QAction::triggered, this, &ProcessesWidget::reniceSelected);

    menu.addSeparator();
    UIHelper::AddRefreshIntervalContextMenu(&menu, this->m_refreshTimer, this->m_active);
    UIHelper::AddGlobalContextMenuItems(&menu, this);

    menu.exec(this->ui->tableView->viewport()->mapToGlobal(pos));

    this->m_contextMenuTargetIndex = QModelIndex();
    this->m_tableContextMenuOpen = false;
    if (this->m_active && this->m_refreshPending && !CFG->RefreshPaused)
        this->startRefresh();
}

void ProcessesWidget::onTreeContextMenu(const QPoint &pos)
{
    this->m_tableContextMenuOpen = true;
    QMenu menu(this);

    QMenu *viewMenu = menu.addMenu(tr("View"));
    QAction *kernelAct = viewMenu->addAction(tr("Kernel tasks"));
    kernelAct->setCheckable(true);
    kernelAct->setChecked(this->m_proxy->ShowKernelTasks);
    connect(kernelAct, &QAction::toggled, this, &ProcessesWidget::setShowKernelTasks);

    QAction *otherUsersAct = viewMenu->addAction(tr("Processes of other users"));
    otherUsersAct->setCheckable(true);
    otherUsersAct->setChecked(this->m_proxy->ShowOtherUsersProcs);
    connect(otherUsersAct, &QAction::toggled, this, &ProcessesWidget::setShowOtherUsersProcesses);

    QAction *iconsAct = viewMenu->addAction(tr("Show icons"));
    iconsAct->setCheckable(true);
    iconsAct->setChecked(CFG->ShowProcessIcons);
    connect(iconsAct, &QAction::toggled, this, &ProcessesWidget::setShowIcons);

    viewMenu->addSeparator();
    QAction *tableModeAct = viewMenu->addAction(tr("Table view"));
    tableModeAct->setCheckable(true);
    tableModeAct->setChecked(!this->m_treeViewMode);
    connect(tableModeAct, &QAction::triggered, this, [this]() { this->setTreeViewMode(false); });

    QAction *treeModeAct = viewMenu->addAction(tr("Tree view"));
    treeModeAct->setCheckable(true);
    treeModeAct->setChecked(this->m_treeViewMode);
    connect(treeModeAct, &QAction::triggered, this, [this]() { this->setTreeViewMode(true); });

    const QList<pid_t> pids = this->selectedPids();
    const bool hasSelection = !pids.isEmpty();
    menu.addSeparator();
    QAction *termAction = menu.addAction(tr("Terminate  (SIGTERM)"));
    termAction->setEnabled(hasSelection);
    termAction->setShortcut(QKeySequence::Delete);
    termAction->setShortcutVisibleInContextMenu(true);
    connect(termAction, &QAction::triggered, this, &ProcessesWidget::terminateSelectedProcesses);

    QAction *killAction = menu.addAction(tr("Kill  (SIGKILL)"));
    killAction->setEnabled(hasSelection);
    killAction->setIcon(style()->standardIcon(QStyle::SP_MessageBoxCritical));
    connect(killAction, &QAction::triggered, this, &ProcessesWidget::killSelectedProcesses);

    QAction *reniceAction = menu.addAction(tr("Change priority (renice)..."));
    reniceAction->setEnabled(hasSelection);
    connect(reniceAction, &QAction::triggered, this, &ProcessesWidget::reniceSelected);

    menu.addSeparator();
    UIHelper::AddRefreshIntervalContextMenu(&menu, this->m_refreshTimer, this->m_active);
    UIHelper::AddGlobalContextMenuItems(&menu, this);

    menu.exec(this->m_treeView->viewport()->mapToGlobal(pos));

    this->m_tableContextMenuOpen = false;
    if (this->m_active && this->m_refreshPending && !CFG->RefreshPaused)
        this->startRefresh();
}

// ── Helpers ───────────────────────────────────────────────────────────────────

void ProcessesWidget::updateStatusBar()
{
    // Called for every inserted / removed row; the totals are computed once per snapshot.
    qlonglong totalThreads = this->m_snapshotThreadTotal;
    int totalTasks = static_cast<int>(this->m_lastProcessSnapshot.size());
    if (this->m_lastProcessSnapshot.isEmpty())
    {
        const QList<OS::Process> &all = this->m_model->GetProcesses();
        totalTasks = static_cast<int>(all.size());
        totalThreads = 0;
        for (const OS::Process &p : all)
            totalThreads += qMax(0, p.Threads);
    }
    const QString text = tr("Tasks: %1  Threads: %2").arg(totalTasks).arg(totalThreads);
    if (this->ui->statusLabel->text() != text)
        this->ui->statusLabel->setText(text);
}

void ProcessesWidget::copyRowSelectionToClipboard()
{
    if (!this->ui->tableView || !this->ui->tableView->model())
        return;

    QModelIndexList selectedRowsIdx = this->ui->tableView->selectionModel()
                                      ? this->ui->tableView->selectionModel()->selectedRows()
                                      : QModelIndexList();
    QList<int> selectedRows;
    selectedRows.reserve(selectedRowsIdx.size());
    for (const QModelIndex &idx : selectedRowsIdx)
        selectedRows.append(idx.row());
    std::sort(selectedRows.begin(), selectedRows.end());

    const QModelIndex targetIndex = this->m_contextMenuTargetIndex.isValid()
                                    ? this->m_contextMenuTargetIndex
                                    : this->ui->tableView->currentIndex();

    if (selectedRows.size() > 1)
    {
        QGuiApplication::clipboard()->setText(UIHelper::GetVisibleRowsText(this->ui->tableView, selectedRows));
        return;
    }

    const int row = targetIndex.isValid() ? targetIndex.row() : selectedRows.value(0, -1);
    QGuiApplication::clipboard()->setText(UIHelper::GetVisibleRowText(this->ui->tableView, row));
}

QVariant ProcessesWidget::tableSelectionKeyFromProxy(const QModelIndex &proxyKeyIndex) const
{
    const QModelIndex srcIdx = this->m_proxy->mapToSource(proxyKeyIndex);
    if (!srcIdx.isValid())
        return {};
    return this->m_model->data(srcIdx, Qt::UserRole);
}

void ProcessesWidget::copyCellSelectionToClipboard()
{
    if (!this->ui->tableView || !this->ui->tableView->model())
        return;

    const QModelIndex targetIndex = this->m_contextMenuTargetIndex.isValid()
                                    ? this->m_contextMenuTargetIndex
                                    : this->ui->tableView->currentIndex();
    if (!targetIndex.isValid())
        return;

    QGuiApplication::clipboard()->setText(this->ui->tableView->model()->data(targetIndex, Qt::DisplayRole).toString());
}

void ProcessesWidget::copySelectedPidsToClipboard()
{
    const QList<pid_t> pids = this->selectedPids();
    if (pids.isEmpty())
        return;

    QStringList pidStrings;
    pidStrings.reserve(pids.size());
    for (pid_t pid : pids)
        pidStrings << QString::number(pid);

    QGuiApplication::clipboard()->setText(pidStrings.join(' '));
}

void ProcessesWidget::terminateSelectedProcesses()
{
    this->sendSignalToSelected(SIGTERM);
}

void ProcessesWidget::killSelectedProcesses()
{
    this->sendSignalToSelected(SIGKILL);
}

void ProcessesWidget::promptAndSendCustomSignal()
{
    bool ok = false;
    const int sig = QInputDialog::getInt(
        this,
        tr("Send custom signal"),
        tr("Signal number:"),
        1, 1, 64, 1, &ok);
    if (ok)
        this->sendSignalToSelected(sig);
}

void ProcessesWidget::runNewTask()
{
    RunTaskDialog(this).exec();
}

void ProcessesWidget::openTerminal()
{
    const QString terminal = findTerminalExecutable();
    if (terminal.isEmpty())
    {
        QMessageBox::warning(this, tr("Open terminal failed"), tr("No supported terminal emulator was found on this system."));
        return;
    }

    if (!OS::StartDetachedNormalPriority(terminal, {}))
    {
        QMessageBox::warning(this, tr("Open terminal failed"), tr("Failed to start terminal: %1").arg(terminal));
        return;
    }

    LOG_INFO(QString("Opened terminal via detached process: %1").arg(terminal));
}

void ProcessesWidget::setShowKernelTasks(bool checked)
{
    this->m_proxy->ShowKernelTasks = checked;
    this->m_model->SetShowKernelTasks(checked);
    CFG->ShowKernelTasks = checked;
    this->m_proxy->ApplyFilters();
    this->onTimerTick();
    LOG_DEBUG(QString("ShowKernelTasks = %1").arg(checked));
}

void ProcessesWidget::setShowOtherUsersProcesses(bool checked)
{
    this->m_proxy->ShowOtherUsersProcs = checked;
    this->m_model->SetShowOtherUsersProcs(checked);
    CFG->ShowOtherUsersProcs = checked;
    this->m_proxy->ApplyFilters();
    this->onTimerTick();
    LOG_DEBUG(QString("ShowOtherUsersProcs = %1").arg(checked));
}

void ProcessesWidget::setShowIcons(bool checked)
{
    CFG->ShowProcessIcons = checked;
    this->applyIconSetting();
    this->onTimerTick();
    LOG_DEBUG(QString("ShowProcessIcons = %1").arg(checked));
}

// Creates or drops the registry according to the setting and points both models at it.
void ProcessesWidget::applyIconSetting()
{
    if (CFG->ShowProcessIcons && !this->m_appRegistry)
    {
        this->m_appRegistry = new OS::AppRegistry(this);
    } else if (!CFG->ShowProcessIcons && this->m_appRegistry)
    {
        delete this->m_appRegistry;
        this->m_appRegistry = nullptr;
    }
    this->m_model->SetAppRegistry(this->m_appRegistry);
    this->m_treeModel->SetAppRegistry(this->m_appRegistry);
    // Rows whose data did not change emit no dataChanged, so repaint to add or drop their icons.
    this->ui->tableView->viewport()->update();
    this->m_treeView->viewport()->update();
}

void ProcessesWidget::captureExpandedTreePids(const QModelIndex &parentProxy, QSet<pid_t> &expandedPids) const
{
    const int rows = this->m_treeProxy->rowCount(parentProxy);
    for (int r = 0; r < rows; ++r)
    {
        const QModelIndex proxyIdx = this->m_treeProxy->index(r, OS::ProcessTreeModel::ColPid, parentProxy);
        if (!proxyIdx.isValid() || !this->m_treeView->isExpanded(proxyIdx))
            continue;

        const QModelIndex srcIdx = this->m_treeProxy->mapToSource(proxyIdx);
        if (srcIdx.isValid())
            expandedPids.insert(static_cast<pid_t>(this->m_treeModel->data(srcIdx, Qt::UserRole).toLongLong()));

        this->captureExpandedTreePids(proxyIdx, expandedPids);
    }
}

void ProcessesWidget::restoreExpandedTreePids(const QModelIndex &sourceParent, const QSet<pid_t> &expandedPids)
{
    const int rows = this->m_treeModel->rowCount(sourceParent);
    for (int r = 0; r < rows; ++r)
    {
        const QModelIndex srcIdx = this->m_treeModel->index(r, OS::ProcessTreeModel::ColPid, sourceParent);
        if (!srcIdx.isValid())
            continue;

        const pid_t pid = static_cast<pid_t>(this->m_treeModel->data(srcIdx, Qt::UserRole).toLongLong());
        const QModelIndex proxyIdx = this->m_treeProxy->mapFromSource(srcIdx);
        if (proxyIdx.isValid() && expandedPids.contains(pid))
            this->m_treeView->expand(proxyIdx);

        this->restoreExpandedTreePids(srcIdx, expandedPids);
    }
}

void ProcessesWidget::ensureNameColumnFits()
{
    // Widen (never narrow) the Name column so the section titles and the visible
    // program names are not cut off. Measured by hand: the view's own measurement
    // ignores the bold section font.
    QHeaderView *header = this->m_treeView ? this->m_treeView->header() : nullptr;
    if (!header || header->sectionResizeMode(OS::ProcessTreeModel::ColName) == QHeaderView::Stretch)
        return; // the column fills the window by itself
    const QFont base = this->m_treeView->font();
    QFont bold = base;
    bold.setBold(true);
    const QFontMetrics fm(base), fmBold(bold);
    const int indent = this->m_treeView->indentation();
    const int icon = this->m_treeView->iconSize().width();
    int needed = 0;
    std::function<void(const QModelIndex &, int)> measure = [&](const QModelIndex &parent, int depth)
    {
        for (int r = 0; r < this->m_treeProxy->rowCount(parent); ++r)
        {
            const QModelIndex idx = this->m_treeProxy->index(r, OS::ProcessTreeModel::ColName, parent);
            const QString text = idx.data(Qt::DisplayRole).toString();
            const bool isBold = idx.data(Qt::FontRole).value<QFont>().bold();
            const int width = indent * (depth + 1) + icon + 12 + (isBold ? fmBold : fm).horizontalAdvance(text);
            needed = qMax(needed, width);
            if (this->m_treeView->isExpanded(idx) && depth < 2)
                measure(idx, depth + 1);
        }
    };
    measure(QModelIndex(), 0);
    if (header->sectionSize(OS::ProcessTreeModel::ColName) < needed)
        header->resizeSection(OS::ProcessTreeModel::ColName, needed);
}

void ProcessesWidget::applyDefaultTreeExpansion()
{
    // Apps open, Background & system closed — once, so the user's own
    // expanding / collapsing afterwards is kept.
    if (this->m_treeDefaultExpansionDone || !this->m_treeView || this->m_treeModel->rowCount() == 0)
        return;
    const QModelIndex apps = this->m_treeProxy->mapFromSource(this->m_treeModel->SectionIndex(OS::ProcessTreeModel::SectionApps));
    if (!apps.isValid())
        return;
    this->m_treeView->expand(apps);
    this->m_treeDefaultExpansionDone = true;

    // Make room for "Background & system (123)" and program names (never narrower
    // than the width the user chose). Deferred: the view lays out new rows later.
    QTimer::singleShot(0, this, [this]()
    {
        QHeaderView *header = this->m_treeView ? this->m_treeView->header() : nullptr;
        if (!header)
            return;
        this->ensureNameColumnFits();
    });
}

void ProcessesWidget::restoreTreeStateDeferred(const QSet<pid_t> &expandedPids,
                                               const QList<pid_t> &treeSelection,
                                               pid_t treeCurrentPid,
                                               int treeScroll)
{
    // Defer restoration until the proxy/view settle after model reset/sort.
    QTimer::singleShot(0, this, [this, expandedPids, treeSelection, treeCurrentPid, treeScroll]()
    {
        this->restoreExpandedTreePids(QModelIndex(), expandedPids);
        if (!this->m_treeDefaultExpansionDone && this->m_treeModel->rowCount() > 0)
        {
            // Groups start collapsed, as in the classic task list.
            this->m_treeDefaultExpansionDone = true;
        }

        if (QItemSelectionModel *sel = this->m_treeView->selectionModel())
        {
            sel->clearSelection();
            for (pid_t pid : treeSelection)
            {
                const QModelIndex src = this->m_treeModel->IndexForPid(pid);
                const QModelIndex proxy = this->m_treeProxy->mapFromSource(src);
                if (proxy.isValid())
                    sel->select(proxy, QItemSelectionModel::Select | QItemSelectionModel::Rows);
            }

            if (treeCurrentPid > 0)
            {
                const QModelIndex src = this->m_treeModel->IndexForPid(treeCurrentPid);
                const QModelIndex proxy = this->m_treeProxy->mapFromSource(src);
                if (proxy.isValid())
                    sel->setCurrentIndex(proxy, QItemSelectionModel::NoUpdate);
            }
        }

        if (QScrollBar *sb = this->m_treeView->verticalScrollBar())
            sb->setValue(treeScroll);
    });
}

QList<pid_t> ProcessesWidget::selectedPids() const
{
    QList<pid_t> pids;
    if (!this->m_treeViewMode)
    {
        const QModelIndexList rows = this->ui->tableView->selectionModel()->selectedRows(OS::ProcessModel::ColPid);
        pids.reserve(rows.size());
        for (const QModelIndex &proxyIdx : rows)
        {
            const QModelIndex srcIdx = this->m_proxy->mapToSource(proxyIdx);
            const QVariant v = this->m_model->data(srcIdx, Qt::UserRole);
            pids.append(static_cast<pid_t>(v.toLongLong()));
        }
    } else if (this->m_treeView && this->m_treeView->selectionModel())
    {
        const QModelIndexList rows = this->m_treeView->selectionModel()->selectedRows(OS::ProcessTreeModel::ColPid);
        pids.reserve(rows.size());
        QSet<pid_t> seen;
        const bool searching = !this->ui->searchEdit->text().isEmpty();
        for (const QModelIndex &proxyIdx : rows)
        {
            // A group row stands for all of its members (End task ends them all).
            // While a search is active only for the members that are shown: a row
            // "bash (20)" with one match under it must not take the other 19 along.
            QList<pid_t> members;
            if (searching && this->m_treeProxy->rowCount(proxyIdx.siblingAtColumn(0)) > 0)
            {
                std::function<void(const QModelIndex &)> collect = [&](const QModelIndex &parent)
                {
                    for (int r = 0; r < this->m_treeProxy->rowCount(parent); ++r)
                    {
                        const QModelIndex child = this->m_treeProxy->index(r, 0, parent);
                        if (this->m_treeProxy->rowCount(child) > 0)
                            collect(child);
                        else
                            members.append(this->m_treeModel->PidsForIndex(this->m_treeProxy->mapToSource(child)));
                    }
                };
                collect(proxyIdx.siblingAtColumn(0));
            } else
            {
                members = this->m_treeModel->PidsForIndex(this->m_treeProxy->mapToSource(proxyIdx));
            }
            for (pid_t pid : std::as_const(members))
            {
                if (pid > 0 && !seen.contains(pid))
                {
                    seen.insert(pid);
                    pids.append(pid);
                }
            }
        }
    }
    return pids;
}

QList<OS::Process> ProcessesWidget::selectedProcesses() const
{
    const QList<pid_t> pids = this->selectedPids();
    if (pids.isEmpty())
        return {};

    const QList<OS::Process> &all = this->m_lastProcessSnapshot.isEmpty()
                                    ? this->m_model->GetProcesses()
                                    : this->m_lastProcessSnapshot;
    QHash<pid_t, const OS::Process *> byPid;
    byPid.reserve(all.size());
    for (const OS::Process &proc : all)
        byPid.insert(proc.PID, &proc);

    QList<OS::Process> selected;
    selected.reserve(pids.size());
    for (pid_t pid : pids)
    {
        const auto it = byPid.constFind(pid);
        if (it != byPid.cend() && it.value())
            selected.append(*it.value());
    }
    return selected;
}

bool ProcessesWidget::selectionHasGroupRow() const
{
    if (!this->m_treeViewMode || !this->m_treeView || !this->m_treeView->selectionModel())
        return false;
    const QModelIndexList rows = this->m_treeView->selectionModel()->selectedRows(OS::ProcessTreeModel::ColPid);
    for (const QModelIndex &proxyIdx : rows)
    {
        if (this->m_treeModel->rowCount(this->m_treeProxy->mapToSource(proxyIdx).siblingAtColumn(0)) > 0)
            return true;
    }
    return false;
}

void ProcessesWidget::sendSignalToSelected(int signal)
{
    const QList<OS::Process> selected = this->selectedProcesses();
    if (selected.isEmpty())
        return;
    const bool groupSelected = this->selectionHasGroupRow();

    // Pin every target by PID + start time: the confirmation can stay open for a
    // long time and a PID may be reused meanwhile.
    QList<OS::ProcessHelper::Target> targets;
    targets.reserve(selected.size());
    QHash<QString, int> nameCounts;
    QStringList nameOrder;
    QStringList otherUsers;
    bool hitsSelf = false, hitsSession = false;
    static const QSet<QString> sessionCritical {
        "Xorg", "X", "Xwayland", "xfce4-session", "lightdm", "gdm", "sddm", "dbus-daemon", "dbus-broker",
        "gnome-shell", "gnome-session-b", "kwin_wayland", "sway", "Hyprland", "systemd", "systemd-logind"
    };
    const pid_t self = ::getpid();
    for (const OS::Process &proc : selected)
    {
        OS::ProcessHelper::Target t;
        t.PID = proc.PID;
        t.StartTimeTicks = proc.StartTimeTicks;
        t.UID = proc.UID;
        t.Name = proc.Name;
        targets.append(t);
        if (++nameCounts[proc.Name] == 1)
            nameOrder.append(proc.Name);
        if (proc.PID == self)
            hitsSelf = true;
        if (proc.PID == 1 || sessionCritical.contains(proc.Name))
            hitsSession = true;
        if (!CFG->IsSuperuser && proc.UID != CFG->EUID && !proc.User.isEmpty() && !otherUsers.contains(proc.User))
            otherUsers.append(proc.User);
    }

    const QString signalLabel = OS::ProcessHelper::GetSignalName(signal);
    const QString signalText = signalLabel.isEmpty() ? QString::number(signal) : signalLabel;

    QString body;
    if (targets.size() == 1)
    {
        body = tr("Send %1 to \"%2\" (PID %3)?").arg(signalText, targets.first().Name).arg(targets.first().PID);
    } else
    {
        // Names with counts, largest first; never a wall of PIDs.
        std::sort(nameOrder.begin(), nameOrder.end(), [&nameCounts](const QString &l, const QString &r)
        {
            return nameCounts.value(l) > nameCounts.value(r);
        });
        QStringList parts;
        for (int i = 0; i < nameOrder.size() && i < 8; ++i)
        {
            const int n = nameCounts.value(nameOrder.at(i));
            parts << (n > 1 ? QString("%1 × %2").arg(nameOrder.at(i)).arg(n) : nameOrder.at(i));
        }
        if (nameOrder.size() > 8)
            parts << tr("and %1 more programs").arg(nameOrder.size() - 8);
        body = tr("Send %1 to %2 processes?\n\n%3").arg(signalText).arg(targets.size()).arg(parts.join(", "));
    }
    if (groupSelected && !this->ui->searchEdit->text().isEmpty())
        body += tr("\n\nOnly the processes shown by the current search are included.");
    if (hitsSession)
        body += tr("\n\nWarning: this includes a part of the desktop session or the system itself. "
                   "Ending it can close every open program and log you out.");
    if (hitsSelf)
        body += tr("\n\nThis includes Task Manager itself.");
    if (!otherUsers.isEmpty())
        body += tr("\n\nSome of these belong to another user (%1). Without superuser rights the signal will most likely be rejected.")
                    .arg(otherUsers.join(", "));

    const QMessageBox::StandardButton answer = QMessageBox::warning(this, tr("Confirm Signal"), body, QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    OS::ProcessHelper::SignalResult result;
    if (signal == SIGKILL && groupSelected)
    {
        // Whole group: also catch what it starts while we work (fork bombs). The
        // "same name, appeared just now" rule is only used for names that are
        // already there in large numbers.
        QSet<QString> sweepNames;
        for (auto it = nameCounts.cbegin(); it != nameCounts.cend(); ++it)
        {
            if (it.value() >= 50)
                sweepNames.insert(it.key());
        }
        QSet<pid_t> known;
        known.reserve(this->m_lastProcessSnapshot.size());
        for (const OS::Process &proc : std::as_const(this->m_lastProcessSnapshot))
            known.insert(proc.PID);
        // Task Manager itself goes last (KillGroup skips it), so the rest is done first.
        result = OS::ProcessHelper::KillGroup(targets, sweepNames, known);
        if (hitsSelf)
            ::kill(self, SIGKILL);
    } else
    {
        // Our own process last, or the others would never get their signal.
        QList<OS::ProcessHelper::Target> ordered;
        ordered.reserve(targets.size());
        for (const OS::ProcessHelper::Target &t : std::as_const(targets))
            if (t.PID != self)
                ordered.append(t);
        result = OS::ProcessHelper::SendSignalChecked(ordered, signal);
        if (hitsSelf)
            ::kill(self, signal);
    }
    LOG_INFO(QString("%1: sent to %2, already gone %3, extra found %4, failed %5")
                 .arg(signalText).arg(result.Sent).arg(result.Gone).arg(result.Swept).arg(result.Errors.size()));

    // Show the result right away instead of at the next tick.
    this->startRefresh();

    if (!result.Errors.isEmpty())
        QMessageBox::warning(this, tr("Signal failed"), result.Errors.join('\n'));
}

void ProcessesWidget::reniceSelected()
{
    const QList<OS::Process> selected = this->selectedProcesses();
    if (selected.isEmpty())
        return;

    bool ok;
    const int nice = QInputDialog::getInt(this, tr("Change priority"), tr("Nice value (-20 = highest priority, 19 = lowest):"), 0, -20, 19, 1, &ok);
    if (!ok)
        return;

    QStringList errors;
    for (const OS::Process &proc : selected)
    {
        // Skip a PID that now belongs to another process.
        if (proc.StartTimeTicks != 0 && OS::ProcessHelper::StartTimeOf(proc.PID) != proc.StartTimeTicks)
            continue;
        QString err;
        if (!OS::ProcessHelper::Renice(proc.PID, nice, err))
        {
            LOG_WARN(err);
            if (errors.size() < 12)
                errors << err;
        } else
        {
            LOG_INFO(QString("Reniced PID %1 to nice=%2").arg(proc.PID).arg(nice));
        }
    }

    if (!errors.isEmpty())
    {
        QMessageBox::warning(this, tr("Renice failed"), errors.join('\n'));
    }
}

QString ProcessesWidget::findTerminalExecutable()
{
    static const QStringList terminalCandidates {
        "xdg-terminal-exec",
        "x-terminal-emulator",
        "kgx",
        "gnome-terminal",
        "konsole",
        "xfce4-terminal",
        "mate-terminal",
        "lxterminal",
        "tilix",
        "kitty",
        "alacritty",
        "urxvt",
        "terminator",
        "xterm"
    };
    static const QStringList trustedPrefixes {
        // FHS:
        "/usr/bin/",
        "/bin/",
        "/sbin/",
        "/usr/sbin/",
        // NixOS/Home-manager:
        "/nix/store/"
    };

    // The desktop's own terminal first: x-terminal-emulator is one system-wide choice
    // (e.g. Konsole), which is the wrong one when the same machine also runs XFCE.
    QStringList candidates;
    const QString desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP").toUpper();
    if (desktop.contains(QLatin1String("XFCE")))
        candidates << "xfce4-terminal";
    else if (desktop.contains(QLatin1String("KDE")))
        candidates << "konsole";
    else if (desktop.contains(QLatin1String("GNOME")))
        candidates << "kgx" << "gnome-terminal";
    else if (desktop.contains(QLatin1String("MATE")))
        candidates << "mate-terminal";
    else if (desktop.contains(QLatin1String("LXQT")))
        candidates << "qterminal";
    else if (desktop.contains(QLatin1String("LXDE")))
        candidates << "lxterminal";
    candidates << terminalCandidates;

    for (const QString &candidate : std::as_const(candidates))
    {
        const QString exe = QStandardPaths::findExecutable(candidate);
        if (!exe.isEmpty())
        {
            const QString canonical = QFileInfo(exe).canonicalFilePath();
            if (canonical.isEmpty())
                continue;

            for (const QString &prefix : trustedPrefixes)
            {
                if (canonical.startsWith(prefix))
                    return canonical;
            }
        }
    }

    return {};
}
