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

#include "cpudetailwidget.h"
#include "ui_cpudetailwidget.h"
#include "configuration.h"
#include "metrics.h"
#include "../colorscheme.h"
#include "../ui/uihelper.h"
#include "../ui/widgetstyle.h"

#include <QAction>
#include <QFile>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QVBoxLayout>

using namespace Perf;

CpuDetailWidget::CpuDetailWidget(QWidget *parent) : QWidget(parent), ui(new Ui::CpuDetailWidget)
{
    this->ui->setupUi(this);
    {
        // Content sits ~25 screen pixels further right and ~20 lower (15 / 12 px at the default UI scale).
        QMargins m = this->ui->verticalLayout->contentsMargins();
        m.setLeft(m.left() + 15);
        m.setTop(m.top() + 12);
        this->ui->verticalLayout->setContentsMargins(m);
    }
    UIHelper::ApplyClassicStats(this->ui->statsGrid, { this->ui->statUtilValue, this->ui->statSpeedValue, this->ui->statProcessesValue, this->ui->statThreadsValue, this->ui->statUptimeValue });
    const ColorScheme *scheme = ColorScheme::GetCurrent();

    WidgetStyle::ApplyTextStyle(this->ui->titleLabel, scheme->CpuTitleColor, 22);
    WidgetStyle::ApplyTextStyle(this->ui->modelNameLabel, scheme->CpuHeaderValueColor, 13);
    WidgetStyle::ApplyTextStyle(this->ui->utilizationLabel, scheme->CpuHeaderValueColor, 18);
    WidgetStyle::ApplyTextStyle(this->ui->timeLeftLabel, scheme->AxisLabelColor, 9);
    WidgetStyle::ApplyTextStyle(this->ui->timeRightLabel, scheme->AxisLabelColor, 9);

    if (QGridLayout *statsGrid = this->findChild<QGridLayout *>("statsGrid"))
    {
        for (int row = 0; row < statsGrid->rowCount(); ++row)
        {
            for (int column = 0; column < statsGrid->columnCount(); column += 2)
            {
                if (QLayoutItem *item = statsGrid->itemAtPosition(row, column))
                {
                    if (QLabel *label = qobject_cast<QLabel *>(item->widget()))
                        WidgetStyle::ApplyTextStyle(label, scheme->StatLabelColor);
                }
            }
        }
    }

    // Classic header: "CPU" on the left, model name on the right, baseline-aligned.
    this->ui->headerLabelLayout->removeWidget(this->ui->modelNameLabel);
    this->ui->modelNameLabel->setWordWrap(false);
    this->ui->modelNameLabel->setAlignment(Qt::AlignRight | Qt::AlignBottom);
    this->ui->headerLayout->insertWidget(1, this->ui->modelNameLabel, 0, Qt::AlignRight | Qt::AlignBottom);
    this->ui->utilizationLabel->hide();

    // "% Utilization ............ 100%" caption above the graph.
    {
        QHBoxLayout *caption = new QHBoxLayout();
        caption->setContentsMargins(0, 6, 0, 0);
        this->m_graphCaptionLeft = new QLabel(tr("% Utilization"), this);
        this->m_graphCaptionRight = new QLabel(tr("100%"), this);
        caption->addWidget(this->m_graphCaptionLeft);
        caption->addStretch(1);
        caption->addWidget(this->m_graphCaptionRight);
        WidgetStyle::ApplyTextStyle(this->m_graphCaptionLeft, scheme->AxisLabelColor, 9);
        WidgetStyle::ApplyTextStyle(this->m_graphCaptionRight, scheme->AxisLabelColor, 9);
        const int graphIndex = this->ui->verticalLayout->indexOf(this->ui->graphAreaContainer);
        this->ui->verticalLayout->insertLayout(graphIndex, caption);
        this->ui->verticalLayout->setStretch(graphIndex, 0);
        // Graph keeps the classic proportion (height ~0.42 x width), stats sit
        // right below it, and any spare height collects at the very bottom.
        this->ui->verticalLayout->setStretch(graphIndex + 1, 1000);
        this->ui->verticalLayout->addStretch(1);
        UIHelper::ApplyGraphRowsNarrower(this, this->ui->verticalLayout, { this->ui->graphAreaContainer },
                                         { this->ui->headerLayout, caption, this->ui->timeAxisLayout }, 0.05);
        UIHelper::ApplyClassicPageGeometry(this, { qMakePair(static_cast<QWidget *>(this->ui->graphAreaContainer), 0.42) });
    }

    // Embed CpuGraphArea into the plain container widget from the .ui
    this->m_graphArea = new CpuGraphArea(this->ui->graphAreaContainer);
    QVBoxLayout *lay  = new QVBoxLayout(this->ui->graphAreaContainer);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(this->m_graphArea);
    this->ui->graphAreaContainer->setLayout(lay);

    connect(this->m_graphArea, &CpuGraphArea::contextMenuRequested, this, &CpuDetailWidget::onContextMenuRequested);

    this->m_graphArea->SetMode(
                CFG->CpuGraphMode == 1
                ? CpuGraphArea::GraphMode::PerCore
                : CpuGraphArea::GraphMode::Overall);
    this->m_graphArea->SetShowKernelTime(CFG->CpuShowKernelTimes);
    UIHelper::EnableCopyLabelContextMenu(this->ui->statUtilValue);
    UIHelper::EnableCopyLabelContextMenu(this->ui->statSpeedValue);
    UIHelper::EnableCopyLabelContextMenu(this->ui->statProcessesValue);
    UIHelper::EnableCopyLabelContextMenu(this->ui->statThreadsValue);
    UIHelper::EnableCopyLabelContextMenu(this->ui->statUptimeValue);
    UIHelper::EnableCopyLabelContextMenu(this->ui->statLogicalCpusValue);
    UIHelper::EnableCopyLabelContextMenu(this->ui->statVmValue);
    UIHelper::EnableCopyLabelContextMenu(this->ui->statTempValue);
}

CpuDetailWidget::~CpuDetailWidget()
{
    delete this->ui;
}

void CpuDetailWidget::Init()
{
    // Populate one-time static labels from metadata
    this->ui->modelNameLabel->setText(Metrics::GetCPU()->CpuModelName());
    this->ui->statLogicalCpusValue->setText(QString::number(Metrics::GetCPU()->CpuLogicalCount()));
    this->m_graphArea->Init();

    connect(Metrics::Get(), &Metrics::updated, this, &CpuDetailWidget::onUpdated);
    this->onUpdated();
}

void CpuDetailWidget::ApplyColorScheme()
{
    const ColorScheme *scheme = ColorScheme::GetCurrent();
    WidgetStyle::ApplyTextStyle(this->ui->titleLabel, scheme->CpuTitleColor, 22);
    WidgetStyle::ApplyTextStyle(this->ui->modelNameLabel, scheme->CpuHeaderValueColor, 13);
    WidgetStyle::ApplyTextStyle(this->ui->utilizationLabel, scheme->CpuHeaderValueColor, 18);
    WidgetStyle::ApplyTextStyle(this->ui->timeLeftLabel, scheme->AxisLabelColor, 9);
    WidgetStyle::ApplyTextStyle(this->ui->timeRightLabel, scheme->AxisLabelColor, 9);

    if (QGridLayout *statsGrid = this->findChild<QGridLayout *>("statsGrid"))
    {
        for (int row = 0; row < statsGrid->rowCount(); ++row)
        {
            for (int column = 0; column < statsGrid->columnCount(); column += 2)
            {
                if (QLayoutItem *item = statsGrid->itemAtPosition(row, column))
                {
                    if (QLabel *label = qobject_cast<QLabel *>(item->widget()))
                        WidgetStyle::ApplyTextStyle(label, scheme->StatLabelColor);
                }
            }
        }
    }

    this->m_graphArea->ApplyColorScheme();
    this->update();
}

// ── Private slots ─────────────────────────────────────────────────────────────

void CpuDetailWidget::onUpdated()
{
    const double pct = Metrics::GetCPU()->CpuPercent();

    // Header utilisation
    this->ui->utilizationLabel->setText(QString::number(pct, 'f', 0) + "%");

    // Stats panel
    this->ui->statUtilValue->setText(QString::number(pct, 'f', 1) + "%");

    const double curMhz = Metrics::GetCPU()->CpuCurrentMhz();
    if (curMhz > 0.0)
        this->ui->statSpeedValue->setText(
                tr("%1 GHz").arg(curMhz / 1000.0, 0, 'f', 2));
    else
        this->ui->statSpeedValue->setText(tr("—"));

    const int cpuTempC = Metrics::GetCPU()->CpuTemperatureC();
    this->ui->statTempValue->setText(cpuTempC >= 0 ? tr("%1 C").arg(cpuTempC) : tr("—"));

    this->ui->statProcessesValue->setText(QString::number(Metrics::GetKernel()->ProcessCount()));
    this->ui->statThreadsValue->setText(QString::number(Metrics::GetKernel()->ThreadCount()));

    // Uptime from /proc/uptime
    QFile f("/proc/uptime");
    if (f.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        const double uptimeSec = f.readAll().simplified().split(' ').value(0).toDouble();
        f.close();
        const int days    = static_cast<int>(uptimeSec / 86400);
        const int hours   = static_cast<int>(uptimeSec / 3600)  % 24;
        const int minutes = static_cast<int>(uptimeSec / 60)    % 60;
        const int seconds = static_cast<int>(uptimeSec)         % 60;
        QString upStr;
        if (days > 0)
            upStr = tr("%1d %2:%3:%4")
                    .arg(days)
                    .arg(hours,   2, 10, QChar('0'))
                    .arg(minutes, 2, 10, QChar('0'))
                    .arg(seconds, 2, 10, QChar('0'));
        else
            upStr = tr("%1:%2:%3")
                    .arg(hours,   2, 10, QChar('0'))
                    .arg(minutes, 2, 10, QChar('0'))
                    .arg(seconds, 2, 10, QChar('0'));
        this->ui->statUptimeValue->setText(upStr);
    }

    // Update the graph area
    this->m_graphArea->UpdateData();

    if (Metrics::GetCPU()->CpuIsVirtualMachine())
    {
        const QString vendor = Metrics::GetCPU()->CpuVmVendor();
        if (vendor.isEmpty())
            this->ui->statVmValue->setText(tr("Yes"));
        else
            this->ui->statVmValue->setText(tr("Yes (%1)").arg(vendor));
    } else
    {
        this->ui->statVmValue->setText(tr("No"));
    }
}

void CpuDetailWidget::onContextMenuRequested(const QPoint &globalPos)
{
    QMenu menu(this);
    menu.setTitle(tr("CPU graph options"));

    // ── Change graph to ───────────────────────────────────────────────────────
    QMenu *graphMenu = menu.addMenu(tr("Change graph to"));

    QAction *actOverall  = graphMenu->addAction(tr("Overall utilization"));
    QAction *actPerCore  = graphMenu->addAction(tr("Logical processors"));
    actOverall->setCheckable(true);
    actPerCore->setCheckable(true);

    const bool isOverall = (this->m_graphArea->GetMode() == CpuGraphArea::GraphMode::Overall);
    actOverall->setChecked( isOverall);
    actPerCore->setChecked(!isOverall);

    connect(actOverall, &QAction::triggered, this, [this]()
    {
        this->m_graphArea->SetMode(CpuGraphArea::GraphMode::Overall);
        CFG->CpuGraphMode = 0;
    });
    connect(actPerCore, &QAction::triggered, this, [this]()
    {
        this->m_graphArea->SetMode(CpuGraphArea::GraphMode::PerCore);
        CFG->CpuGraphMode = 1;
    });

    menu.addSeparator();

    // ── Show kernel times ─────────────────────────────────────────────────────
    QAction *actKernel = menu.addAction(tr("Show kernel times"));
    actKernel->setCheckable(true);
    actKernel->setChecked(this->m_graphArea->GetShowKernelTime());
    connect(actKernel, &QAction::triggered, this, [this](bool checked)
    {
        this->m_graphArea->SetShowKernelTime(checked);
        CFG->CpuShowKernelTimes = checked;
    });

    menu.addSeparator();

    // ── Copy ─────────────────────────────────────────────────────────────────
    UIHelper::AddGraphContextMenuItems(&menu, this->m_graphArea);

    menu.exec(globalPos);
}
