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

#include "gpu.h"
#include "gpu/amdsmibackend.h"
#include "gpu/drmbackend.h"
#include "gpu/intelsysmanbackend.h"
#include "gpu/nvmlbackend.h"
#include "../configuration.h"
#include "../logger.h"

GPU::GPU() :
    m_nvmlBackend(std::make_unique<GpuNvmlBackend>()),
    m_amdSmiBackend(std::make_unique<GpuAmdSmiBackend>()),
    m_intelSysmanBackend(std::make_unique<GpuIntelSysmanBackend>()),
    m_drmBackend(std::make_unique<GpuDrmBackend>())
{
    this->detectGpuBackends();
}

GPU::~GPU() = default;

const GPU::GPUInfo &GPU::FromIndex(int gpuIndex) const
{
    if (gpuIndex < 0 || gpuIndex >= static_cast<int>(this->m_gpus.size()))
        return this->m_nullGPU;
    return *this->m_gpus.at(gpuIndex);
}

GPU::GPUEngineInfo *GPU::GPUInfo::FindEngine(const QString &key)
{
    for (const auto &engine : this->Engines)
    {
        if (engine->Key == key)
            return engine.get();
    }

    return nullptr;
}

void GPU::detectGpuBackends()
{
    LOG_DEBUG("Detecting GPU backends");

    if (CFG->ForceGpuDrm)
    {
        LOG_INFO("GPU backend detection forced to DRM via --force-drm");
        this->m_drmBackend->Detect(false, false, false);
        return;
    }

    this->m_nvmlBackend->Detect();
    this->m_amdSmiBackend->Detect();
    this->m_intelSysmanBackend->Detect();
    this->m_drmBackend->Detect(this->m_nvmlBackend->IsAvailable(),
                               this->m_amdSmiBackend->IsAvailable(),
                               this->m_intelSysmanBackend->IsAvailable());
}

void GPU::SetDetailPageVisible(bool visible)
{
    GpuNvmlBackend::SetDetailPageVisible(visible);
}

bool GPU::Sample()
{
    bool ok = false;
    if (this->m_nvmlBackend->IsAvailable())
        ok |= this->m_nvmlBackend->Sample(this->m_gpus);
    if (this->m_amdSmiBackend->IsAvailable())
        ok |= this->m_amdSmiBackend->Sample(this->m_gpus);
    if (this->m_intelSysmanBackend->IsAvailable())
        ok |= this->m_intelSysmanBackend->Sample(this->m_gpus);
    if (this->m_drmBackend->HasCards())
        ok |= this->m_drmBackend->Sample(this->m_gpus);
    return ok;
}

void GPU::unloadGpuBackends()
{
    this->m_nvmlBackend.reset();
    this->m_amdSmiBackend.reset();
    this->m_intelSysmanBackend.reset();
    this->m_drmBackend.reset();
}
