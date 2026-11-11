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

#ifndef GPUNVMLBACKEND_H
#define GPUNVMLBACKEND_H

#include "../gpu.h"

#include <memory>
#include <vector>

class GpuNvmlBackend
{
    public:
        GpuNvmlBackend();
        ~GpuNvmlBackend();

        void Detect();
        bool Sample(std::vector<std::unique_ptr<GPU::GPUInfo>> &gpus);
        bool IsAvailable() const { return this->m_available; }
        static void SetDetailPageVisible(bool visible);

    private:
        void unload();
        void stopPcieWorker();

        //! nvmlDeviceGetPcieThroughput() blocks ~20 ms per call (it measures over a
        //! 20 ms window), so it runs on a helper thread; Sample() uses the latest result.
        struct PcieWorker;
        std::unique_ptr<PcieWorker> m_pcie;

        bool    m_available { false };
        void   *m_libHandle { nullptr };
};

#endif // GPUNVMLBACKEND_H
