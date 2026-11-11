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

#ifndef GPU_H
#define GPU_H

#include "../globals.h"
#include "../historybuffer.h"

#include <QHash>
#include <memory>
#include <vector>

class GpuNvmlBackend;
class GpuAmdSmiBackend;
class GpuIntelSysmanBackend;
class GpuDrmBackend;

class GPU
{
    public:
        struct GPUEngineInfo
        {
            QString         Key;
            QString         Label;
            double          Pct { 0.0 };
            HistoryBuffer   History { TUX_MANAGER_HISTORY_SIZE };
        };

        struct GPUInfo
        {
            GPUEngineInfo  *FindEngine(const QString &key);

            QString         ID;
            QString         Name;
            QString         DriverVersion;
            QString         Backend;
            double          UtilPct { 0.0 };
            int             TemperatureC { -1 };
            int             CoreClockMHz { -1 };
            double          PowerUsageW { -1.0 };
            double          PowerLimitW { -1.0 };   ///< Enforced board power limit, scales the power graph.
            qint64          MemUsedMiB { 0 };
            qint64          MemTotalMiB { 0 };
            qint64          SharedMemUsedMiB { 0 };
            qint64          SharedMemTotalMiB { 0 };
            double          CopyTxBps { 0.0 };
            double          CopyRxBps { 0.0 };
            double          MaxCopyBps { 0.0 };
            HistoryBuffer   UtilHistory { TUX_MANAGER_HISTORY_SIZE };
            HistoryBuffer   PowerHistory { TUX_MANAGER_HISTORY_SIZE };
            HistoryBuffer   MemUsageHistory { TUX_MANAGER_HISTORY_SIZE };
            HistoryBuffer   SharedMemHistory { TUX_MANAGER_HISTORY_SIZE };
            HistoryBuffer   CopyTxHistory { TUX_MANAGER_HISTORY_SIZE };
            HistoryBuffer   CopyRxHistory { TUX_MANAGER_HISTORY_SIZE };
            std::vector<std::unique_ptr<GPUEngineInfo>>    Engines;
            QHash<QString, qint64>      PrevFDInfoEngineNs;
        };

        GPU();
        ~GPU();

        /// Sample GPU backends (NVML when available) and update utilization/memory/engine histories.
        bool Sample();
        //! The GPU detail page shows PCIe copy bandwidth; while it is hidden the (costly)
        //! PCIe counter read runs on every 5th sample only.
        void SetDetailPageVisible(bool visible);

        int GpuCount() const { return static_cast<int>(this->m_gpus.size()); }
        const GPUInfo &FromIndex(int gpuIndex) const;

    private:
        void detectGpuBackends();
        void unloadGpuBackends();
        //! Returned if index of requested GPU doesn't exist
        GPUInfo m_nullGPU;

        std::vector<std::unique_ptr<GPUInfo>>    m_gpus;
        std::unique_ptr<GpuNvmlBackend>  m_nvmlBackend;
        std::unique_ptr<GpuAmdSmiBackend> m_amdSmiBackend;
        std::unique_ptr<GpuIntelSysmanBackend> m_intelSysmanBackend;
        std::unique_ptr<GpuDrmBackend>   m_drmBackend;
};

#endif // GPU_H
