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

#ifndef OS_PROCESSREFRESHSERVICE_H
#define OS_PROCESSREFRESHSERVICE_H

#include "process.h"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QThread>

// This provides a mechanism to refresh data from /proc using dedicated thread, so that we don't
// cause random UI stutters when in process tab by blocking UI thread for too long

namespace OS
{
    class ProcessRefreshWorker : public QObject
    {
        Q_OBJECT

        public:
            explicit ProcessRefreshWorker(QObject *parent = nullptr);

        public slots:
            void fetch(int consumer,
                       quint64 token,
                       bool includeKernelTasks,
                       bool includeOtherUsers,
                       bool collectIOMetrics,
                       bool collectMemoryDetails,
                       bool isSuperuser,
                       uint effectiveUid,
                       uint myUid);

        signals:
            void fetched(int consumer, quint64 token, const QList<Process> &processes);

        private:
            //! Counters of the previous measured snapshot, per process (PID + start time).
            struct Baseline
            {
                quint64 CPUTicks { 0 };
                quint64 IOReadBytes { 0 };
                quint64 IOWriteBytes { 0 };
                bool    IOTotalsAvailable { false };
            };
            //! Rates computed at the last measured snapshot, reused for snapshots that
            //! arrive too soon after it to be measured on their own.
            struct Rates
            {
                double CPUPercent { 0.0 };
                double IOReadBps { 0.0 };
                double IOWriteBps { 0.0 };
                bool   IORatesAvailable { false };
            };
            QHash<Process::Identity, Baseline> m_baseline;
            QHash<Process::Identity, Rates>    m_lastRates;
            quint64                            m_prevCpuTotalTicks { 0 };
            QElapsedTimer                      m_sampleTimer;
            int                                m_numCpus { 1 };
    };

    class ProcessRefreshService : public QObject
    {
        Q_OBJECT

        public:
            enum class Consumer
            {
                Processes = 0,
                Users = 1
            };

            explicit ProcessRefreshService(QObject *parent = nullptr);
            ~ProcessRefreshService() override;

            void RequestSnapshot(Consumer consumer, quint64 token, const Process::LoadOptions &options);

        signals:
            void requestRefresh(int consumer,
                                quint64 token,
                                bool includeKernelTasks,
                                bool includeOtherUsers,
                                bool collectIOMetrics,
                                bool collectMemoryDetails,
                                bool isSuperuser,
                                uint effectiveUid,
                                uint myUid);
            void snapshotReady(int consumer, quint64 token, const QList<Process> &processes);

        private:
            QThread              *m_workerThread { nullptr };
            ProcessRefreshWorker *m_worker { nullptr };
    };
}

#endif // OS_PROCESSREFRESHSERVICE_H
