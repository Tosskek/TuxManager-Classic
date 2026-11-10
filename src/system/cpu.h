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

#ifndef CPU_H
#define CPU_H

#include "../globals.h"
#include "../historybuffer.h"

#include <memory>
#include <vector>

class CPU
{
    public:
        CPU();
        ~CPU();
        CPU(const CPU &) = delete;
        CPU &operator=(const CPU &) = delete;

        /// Sample /proc/stat aggregate + per-core jiffies and append utilization histories.
        bool Sample();

        // ── Aggregate CPU ─────────────────────────────────────────────────────
        double CpuPercent()  const { return this->m_cpuHistory.IsEmpty() ? 0.0 : this->m_cpuHistory.Back(); }
        const HistoryBuffer &CpuHistory()       const { return this->m_cpuHistory; }
        const HistoryBuffer &CpuKernelHistory() const { return this->m_cpuKernelHistory; }

        // ── Per-core CPU ──────────────────────────────────────────────────────
        int    CoreCount()                          const { return static_cast<int>(this->m_cores.size()); }
        double CorePercent(int i)                   const;
        const  HistoryBuffer &CoreHistory(int i)       const;
        const  HistoryBuffer &CoreKernelHistory(int i) const;

        // ── CPU metadata (read once at startup) ───────────────────────────────
        const QString &CpuModelName() const { return this->m_cpuModelName; }
        double CpuBaseMhz()           const { return this->m_cpuBaseMhz;   }
        double CpuCurrentMhz()        const { return this->m_cpuCurrentMhz; }
        double CoreCurrentMhz(int i)  const;
        int    CpuLogicalCount()      const { return this->m_cpuLogicalCount; }
        bool   CpuIsVirtualMachine()  const { return this->m_cpuIsVirtualMachine; }
        const QString &CpuVmVendor()  const { return this->m_cpuVmVendor; }
        int    CpuTemperatureC()      const { return this->m_cpuTemperatureC; }

    private:
        /// Per-core rolling sample state.
        struct CoreSample
        {
            quint64         PrevIdle     { 0 };
            quint64         PrevTotal    { 0 };
            quint64         PrevKernel   { 0 };
            HistoryBuffer   History       { TUX_MANAGER_HISTORY_SIZE };
            HistoryBuffer   KernelHistory { TUX_MANAGER_HISTORY_SIZE };
        };

        static quint64 parseCpuLine(const char *p, quint64 &outIdle, quint64 &outKernel);

        void readCpuMetadata();
        void detectCpuFreqSource();
        void readCurrentFreq();
        void detectCpuTemperatureSensor();
        void sampleCpuTemperature();
        void readHardwareMetadata();

        // Aggregate CPU state
        quint64          m_prevCpuIdle   { 0 };
        quint64          m_prevCpuTotal  { 0 };
        quint64          m_prevCpuKernel { 0 };
        HistoryBuffer    m_cpuHistory       { TUX_MANAGER_HISTORY_SIZE };
        HistoryBuffer    m_cpuKernelHistory { TUX_MANAGER_HISTORY_SIZE };

        // Per-core state
        std::vector<std::unique_ptr<CoreSample>> m_cores;

        // CPU metadata
        QString  m_cpuModelName;
        double   m_cpuBaseMhz       { 0.0 };
        double   m_cpuCurrentMhz    { 0.0 };
        QVector<double> m_coreCurrentMhz;
        int      m_cpuLogicalCount  { 0 };
        bool     m_cpuIsVirtualMachine { false };
        QString  m_cpuVmVendor;
        int      m_cpuTemperatureC { -1 };
        QString  m_cpuTempInputPath;
        bool     m_cpuFreqSourceDetected { false };
        bool     m_cpuFreqUseSysfs { false };
        QVector<QString> m_cpuFreqPaths;
        std::vector<int> m_cpuFreqFds;   //!< kept open; re-read with pread() each sample
};

#endif // CPU_H
