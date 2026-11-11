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

#include "processrefreshservice.h"

#include "proc.h"

#include <unistd.h>
#include <utility>

using namespace OS;

ProcessRefreshWorker::ProcessRefreshWorker(QObject *parent) : QObject(parent)
{
    this->m_numCpus = static_cast<int>(::sysconf(_SC_NPROCESSORS_ONLN));
    if (this->m_numCpus < 1)
        this->m_numCpus = 1;
}

void ProcessRefreshWorker::fetch(int consumer,
                                 quint64 token,
                                 bool includeKernelTasks,
                                 bool includeOtherUsers,
                                 bool collectIOMetrics,
                                 bool collectMemoryDetails,
                                 bool isSuperuser,
                                 uint effectiveUid,
                                 uint myUid)
{
    Process::LoadOptions opts;
    opts.IncludeKernelTasks = includeKernelTasks;
    opts.IncludeOtherUsers = includeOtherUsers;
    opts.MyUID = static_cast<uid_t>(myUid);
    opts.CollectIOMetrics = collectIOMetrics;
    opts.CollectMemoryDetails = collectMemoryDetails;
    opts.IsSuperuser = isSuperuser;
    opts.EffectiveUID = static_cast<uid_t>(effectiveUid);

    // Reading every process takes tens of milliseconds, so the CPU-time total that
    // is compared against them is taken as the average of a reading before and after.
    const quint64 totalBefore = Proc::ReadTotalCpuJiffies();
    QList<Process> fresh = Process::LoadAll(opts);
    const quint64 totalAfter = Proc::ReadTotalCpuJiffies();
    const quint64 totalJiffies = (totalBefore + totalAfter) / 2;

    // Rates are measured between this snapshot and the baseline. Process CPU time is
    // counted in whole ticks (10 ms), so over a very short window a single tick reads
    // as an absurd percentage (it hit the 100% x cores cap). Snapshots that come too
    // soon after the baseline (tab switches, the Users tab sharing this worker) reuse
    // the last measured values and keep the baseline.
    static constexpr qint64 kMinSampleMs = 250;
    const bool haveBaseline = this->m_sampleTimer.isValid() && this->m_prevCpuTotalTicks > 0;
    const qint64 elapsedMs = haveBaseline ? this->m_sampleTimer.elapsed() : 0;
    const bool tooSoon = haveBaseline && elapsedMs < kMinSampleMs;

    if (tooSoon)
    {
        for (Process &proc : fresh)
        {
            const auto it = this->m_lastRates.constFind(proc.GetIdentity());
            if (it == this->m_lastRates.cend())
                continue;
            proc.CPUPercent = it->CPUPercent;
            if (collectIOMetrics && proc.IOTotalsAvailable && it->IORatesAvailable)
            {
                proc.IOReadBps = it->IOReadBps;
                proc.IOWriteBps = it->IOWriteBps;
                proc.IORatesAvailable = true;
            }
        }
        emit fetched(consumer, token, fresh);
        return;
    }

    const quint64 periodJiffies = (haveBaseline && totalJiffies > this->m_prevCpuTotalTicks)
                                  ? (totalJiffies - this->m_prevCpuTotalTicks) : 0;
    const double elapsedSec = static_cast<double>(elapsedMs) / 1000.0;
    // CPU % is a share of the whole CPU (all cores together = 100 %), like Windows
    // Task Manager, so rows add up to the header total and never exceed 100 %.
    const double periodAllCpus = static_cast<double>(periodJiffies);

    QHash<Process::Identity, Baseline> baseline;
    baseline.reserve(fresh.size());
    this->m_lastRates.clear();
    this->m_lastRates.reserve(fresh.size());
    for (Process &proc : fresh)
    {
        const Process::Identity id = proc.GetIdentity();   // PID + start time: a reused PID is a new process
        const auto prev = this->m_baseline.constFind(id);
        if (prev != this->m_baseline.cend())
        {
            if (periodAllCpus > 0.0 && proc.CPUTicks >= prev->CPUTicks)
            {
                const double pct = static_cast<double>(proc.CPUTicks - prev->CPUTicks) / periodAllCpus * 100.0;
                proc.CPUPercent = qMin(pct, 100.0);
            }
            if (collectIOMetrics && elapsedSec > 0.0 && proc.IOTotalsAvailable && prev->IOTotalsAvailable
                && proc.IOReadBytes >= prev->IOReadBytes && proc.IOWriteBytes >= prev->IOWriteBytes)
            {
                proc.IOReadBps = static_cast<double>(proc.IOReadBytes - prev->IOReadBytes) / elapsedSec;
                proc.IOWriteBps = static_cast<double>(proc.IOWriteBytes - prev->IOWriteBytes) / elapsedSec;
                proc.IORatesAvailable = true;
            }
        }

        baseline.insert(id, Baseline { proc.CPUTicks, proc.IOReadBytes, proc.IOWriteBytes, proc.IOTotalsAvailable });
        this->m_lastRates.insert(id, Rates { proc.CPUPercent, proc.IOReadBps, proc.IOWriteBps, proc.IORatesAvailable });
    }

    this->m_baseline = std::move(baseline);
    this->m_prevCpuTotalTicks = totalJiffies;
    this->m_sampleTimer.start();

    emit fetched(consumer, token, fresh);
}

ProcessRefreshService::ProcessRefreshService(QObject *parent)
    : QObject(parent)
    , m_workerThread(new QThread(this))
    , m_worker(new ProcessRefreshWorker())
{
    qRegisterMetaType<OS::Process>("OS::Process");
    qRegisterMetaType<QList<OS::Process>>("QList<OS::Process>");

    this->m_worker->moveToThread(this->m_workerThread);
    connect(this->m_workerThread, &QThread::finished, this->m_worker, &QObject::deleteLater);
    connect(this,
            &ProcessRefreshService::requestRefresh,
            this->m_worker,
            &ProcessRefreshWorker::fetch,
            Qt::QueuedConnection);
    connect(this->m_worker,
            &ProcessRefreshWorker::fetched,
            this,
            &ProcessRefreshService::snapshotReady,
            Qt::QueuedConnection);
    this->m_workerThread->start();
}

ProcessRefreshService::~ProcessRefreshService()
{
    this->m_workerThread->quit();
    this->m_workerThread->wait(1000);
}

void ProcessRefreshService::RequestSnapshot(Consumer consumer, quint64 token, const Process::LoadOptions &options)
{
    emit requestRefresh(static_cast<int>(consumer),
                        token,
                        options.IncludeKernelTasks,
                        options.IncludeOtherUsers,
                        options.CollectIOMetrics,
                        options.CollectMemoryDetails,
                        options.IsSuperuser,
                        static_cast<uint>(options.EffectiveUID),
                        static_cast<uint>(options.MyUID));
}
