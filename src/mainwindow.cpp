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

#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "configuration.h"
#include "ui/classictheme.h"

#include "metrics.h"
#include "misc.h"

#include <QHBoxLayout>
#include <QShortcut>
#include <QToolButton>

MainWindow *MainWindow::s_instance = nullptr;

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_processRefreshService(new OS::ProcessRefreshService(this))
    , m_processesWidget(new ProcessesWidget(this->m_processRefreshService, this))
    , m_performanceWidget(new PerformanceWidget(this))
    , m_usersWidget(new UsersWidget(this->m_processRefreshService, this))
    , m_servicesWidget(new ServicesWidget(this))
{
    s_instance = this;
    this->ui->setupUi(this);

    if (CFG->IsSuperuser)
    {
        // Make user aware of this
        this->setWindowTitle(this->windowTitle() + tr(" (superuser)"));
    }

    this->ui->processesLayout->addWidget(this->m_processesWidget);
    this->ui->performanceLayout->addWidget(this->m_performanceWidget);
    this->ui->usersLayout->addWidget(this->m_usersWidget);
    this->ui->servicesLayout->addWidget(this->m_servicesWidget);

    // Restore previous window layout
    if (!CFG->WindowGeometry.isEmpty())
        this->restoreGeometry(CFG->WindowGeometry);
    if (!CFG->WindowState.isEmpty())
        this->restoreState(CFG->WindowState);
    this->ui->tabWidget->setCurrentIndex(CFG->ActiveTab);
    this->updateTabActivity(this->ui->tabWidget->currentIndex());

    // Update-speed toggle (1x = 1 s <-> 2x = 0.5 s), graph timeline toggle
    // (1 min -> 2 min -> 5 min) plus the light / dark switch in the top-right corner of the tab bar.
    {
        QWidget *corner = new QWidget(this->ui->tabWidget);
        QHBoxLayout *cornerLayout = new QHBoxLayout(corner);
        cornerLayout->setContentsMargins(0, 0, 0, 0);
        cornerLayout->setSpacing(0);

        auto makeToggle = [corner, cornerLayout](const QString &toolTip)
        {
            QToolButton *b = new QToolButton(corner);
            b->setObjectName("cornerToggle");
            b->setCursor(Qt::PointingHandCursor);
            b->setToolTip(toolTip);
            cornerLayout->addWidget(b);
            return b;
        };

        this->m_speedToggle = makeToggle(tr("Toggle update speed: 1x (every 1 s) / 2x (every 0.5 s)"));
        connect(this->m_speedToggle, &QToolButton::clicked, this, [this]()
        {
            const int ms = (!CFG->RefreshPaused && CFG->RefreshRateMs == 1000) ? 500 : 1000;
            CFG->RefreshPaused = false;
            CFG->RefreshRateMs = ms;
            Metrics::Get()->SetInterval(ms);
            this->m_performanceWidget->applyGraphWindowSeconds();   // same time span = different number of samples
            // Only the visible tab has a running timer; the others pick the rate up when activated.
            this->m_processesWidget->ApplyRefreshRate();
            this->m_usersWidget->ApplyRefreshRate();
            this->m_servicesWidget->ApplyRefreshRate();
            this->syncCornerToggles();
        });

        cornerLayout->addSpacing(6);
        this->m_timeToggle = makeToggle(tr("Toggle graph timeline: 1 min / 2 min / 5 min"));
        connect(this->m_timeToggle, &QToolButton::clicked, this, [this]()
        {
            // Cycle 1 min -> 2 min -> 5 min -> 1 min (any other value goes to 1 min).
            const int sec = CFG->PerfGraphWindowSec;
            CFG->PerfGraphWindowSec = sec == 60 ? 120 : sec == 120 ? 300 : 60;
            this->m_performanceWidget->applyGraphWindowSeconds();
            if (this->m_performanceWidget->IsActive())
                this->m_performanceWidget->onProviderUpdated();
            this->syncCornerToggles();
        });

        this->syncCornerToggles();
        QToolButton *themeToggle = new QToolButton(corner);
        themeToggle->setObjectName("themeToggle");
        themeToggle->setCursor(Qt::PointingHandCursor);
        themeToggle->setText(ClassicTheme::IsDark() ? tr("\u2600 Light mode") : tr("\u263E Dark mode"));
        themeToggle->setToolTip(tr("Switch between light and dark mode (Ctrl+Shift+D)"));
        connect(themeToggle, &QToolButton::clicked, this, []() { ClassicTheme::SwitchAndRestart(!ClassicTheme::IsDark()); });
        cornerLayout->addSpacing(6);
        cornerLayout->addWidget(themeToggle);
        this->ui->tabWidget->setCornerWidget(corner, Qt::TopRightCorner);

        QShortcut *shortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D), this);
        connect(shortcut, &QShortcut::activated, this, []() { ClassicTheme::SwitchAndRestart(!ClassicTheme::IsDark()); });
    }

    // Keep ActiveTab in sync as the user switches tabs
    connect(this->ui->tabWidget, &QTabWidget::currentChanged, this, [this](int index)
    {
        CFG->ActiveTab = index;
        this->updateTabActivity(index);
    });

    connect(this->m_usersWidget, &UsersWidget::goToProcessRequested, this, [this](pid_t pid)
    {
        this->ui->tabWidget->setCurrentIndex(0);
        this->m_processesWidget->ClearSearchFilter();
        this->m_processesWidget->SelectProcessByPid(pid);
    });
}

MainWindow::~MainWindow()
{
    if (s_instance == this)
        s_instance = nullptr;
    delete this->ui;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    CFG->WindowGeometry = this->saveGeometry();
    CFG->WindowState    = this->saveState();
    CFG->Save();
    QMainWindow::closeEvent(event);
}

void MainWindow::SyncCornerToggles()
{
    if (s_instance)
        s_instance->syncCornerToggles();
}

void MainWindow::syncCornerToggles()
{
    // Show the real value when it was set to something else from a context menu.
    QString speed;
    if (CFG->RefreshPaused)
        speed = tr("Paused");
    else if (CFG->RefreshRateMs == 1000)
        speed = "1x";
    else if (CFG->RefreshRateMs == 500)
        speed = "2x";
    else
        speed = Misc::SimplifyTimeMS(CFG->RefreshRateMs);
    this->m_speedToggle->setText(speed);

    const int sec = CFG->PerfGraphWindowSec;
    this->m_timeToggle->setText(sec % 60 == 0 ? tr("%1 min").arg(sec / 60) : Misc::SimplifyTime(sec));
}

void MainWindow::updateTabActivity(int index)
{
    // Tabs: 0=Processes, 1=Performance, 2=Users, 3=Services
    this->m_processesWidget->SetActive(index == 0);
    this->m_performanceWidget->SetActive(index == 1);
    this->m_usersWidget->SetActive(index == 2);
    this->m_servicesWidget->SetActive(index == 3);
}
