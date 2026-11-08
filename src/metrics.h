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

#ifndef METRICS_H
#define METRICS_H

#include "system/cpu.h"
#include "system/gpu.h"
#include "system/memory.h"
#include "system/network.h"
#include "system/storage.h"
#include "system/swap.h"
#include "system/kernel.h"
#include <QObject>

class QTimer;

/// Periodically samples system performance subsystems.
/// All widgets that display performance data connect to the updated() signal
/// and read values through the const accessors.
class Metrics : public QObject
{
    Q_OBJECT

    public:
        static CPU      *GetCPU();
        static GPU      *GetGPU();
        static Network  *GetNetwork();
        static Memory   *GetMemory();
        static Swap     *GetSwap();
        static Storage  *GetStorage();
        static Kernel   *GetKernel();
        static Metrics  *Get();

        Metrics(QObject *parent);

        void SetInterval(int ms);
        void SetActive(bool active);
        bool IsActive() const { return this->m_active; }
        void SetProcessStatsEnabled(bool enabled) { this->m_processStatsEnabled = enabled; }
        void SetCpuSamplingEnabled(bool enabled) { this->m_cpuSamplingEnabled = enabled; }
        void SetMemorySamplingEnabled(bool enabled) { this->m_memorySamplingEnabled = enabled; }
        void SetSwapSamplingEnabled(bool enabled) { this->m_swapSamplingEnabled = enabled; }
        void SetDiskSamplingEnabled(bool enabled) { this->m_diskSamplingEnabled = enabled; }
        void SetNetworkSamplingEnabled(bool enabled) { this->m_networkSamplingEnabled = enabled; }
        void SetGpuSamplingEnabled(bool enabled) { this->m_gpuSamplingEnabled = enabled; }

    signals:
        void updated();
        void swapDevicesChanged();

    private slots:
        void onTimer();

    private:
        //! Check if sampling is enabled and if yes, runs sampleNow() and emits a signal
        void sample();
        bool sampleNow();

        QTimer *m_timer;
        int     m_intervalMs { 1000 };
        bool    m_active { true };
        bool    m_processStatsEnabled { true };
        bool    m_cpuSamplingEnabled { true };
        bool    m_memorySamplingEnabled { true };
        bool    m_swapSamplingEnabled { true };
        bool    m_diskSamplingEnabled { true };
        bool    m_networkSamplingEnabled { true };
        bool    m_gpuSamplingEnabled { true };
};

#endif // METRICS_H
