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

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

struct GPU::Worker
{
    std::mutex              mutex;
    std::condition_variable wake;
    std::condition_variable done;
    bool requested { false };
    bool busy { false };
    bool quit { false };
    bool lastOk { false };
    std::thread thread;
};

GPU::GPU() :
    m_nvmlBackend(std::make_unique<GpuNvmlBackend>()),
    m_amdSmiBackend(std::make_unique<GpuAmdSmiBackend>()),
    m_intelSysmanBackend(std::make_unique<GpuIntelSysmanBackend>()),
    m_drmBackend(std::make_unique<GpuDrmBackend>())
{
    this->detectGpuBackends();

    this->m_worker = std::make_unique<Worker>();
    Worker *w = this->m_worker.get();
    w->thread = std::thread([this, w]()
    {
        std::unique_lock<std::mutex> lock(w->mutex);
        for (;;)
        {
            w->wake.wait(lock, [w]() { return w->requested || w->quit; });
            if (w->quit)
                return;
            w->requested = false;
            w->busy = true;
            lock.unlock();
            const bool ok = this->sampleBackends();
            lock.lock();
            w->busy = false;
            w->lastOk = ok;
            w->done.notify_all();
        }
    });
}

GPU::~GPU()
{
    Worker *w = this->m_worker.get();
    bool idle;
    {
        std::unique_lock<std::mutex> lock(w->mutex);
        w->quit = true;
        w->wake.notify_all();
        idle = w->done.wait_for(lock, std::chrono::milliseconds(300), [w]() { return !w->busy; });
    }
    if (idle)
    {
        w->thread.join();
        return;
    }
    // The helper is stuck inside the driver. Leave it and everything it uses alone
    // (the process is exiting anyway); freeing it now would crash instead.
    w->thread.detach();
    (void) this->m_worker.release();
    (void) this->m_nvmlBackend.release();
    (void) this->m_amdSmiBackend.release();
    (void) this->m_intelSysmanBackend.release();
    (void) this->m_drmBackend.release();
    for (auto &gpu : this->m_shadow)
        (void) gpu.release();
}

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

bool GPU::sampleBackends()
{
    // Helper thread.
    // Test hook: TUX_MANAGER_TEST_GPU_HANG_MS=N makes the 5th sample take N ms, to
    // check that a hung driver does not freeze the window.
    static const int hangMs = qEnvironmentVariableIntValue("TUX_MANAGER_TEST_GPU_HANG_MS");
    static int sampleNumber = 0;
    if (hangMs > 0 && ++sampleNumber == 5)
        std::this_thread::sleep_for(std::chrono::milliseconds(hangMs));
    bool ok = false;
    ok |= this->m_nvmlBackend->Sample(this->m_shadow);   // also retries detection after a driver loss
    if (this->m_amdSmiBackend->IsAvailable())
        ok |= this->m_amdSmiBackend->Sample(this->m_shadow);
    if (this->m_intelSysmanBackend->IsAvailable())
        ok |= this->m_intelSysmanBackend->Sample(this->m_shadow);
    if (this->m_drmBackend->HasCards())
        ok |= this->m_drmBackend->Sample(this->m_shadow);
    return ok;
}

bool GPU::Sample()
{
    Worker *w = this->m_worker.get();
    std::unique_lock<std::mutex> lock(w->mutex);

    // A sample normally takes a few milliseconds. The very first one decides which
    // GPU panels exist, so it gets more time.
    const auto patience = std::chrono::milliseconds(this->m_firstSample ? 1500 : 120);
    this->m_firstSample = false;

    if (this->m_stalledTicks > 0)
    {
        // The helper did not come back last time. Never wait for it again until it
        // has; just look whether it is done by now.
        if (w->busy || w->requested)
        {
            if (++this->m_stalledTicks == 4)
                this->markStale();
            return false;
        }
        this->m_stalledTicks = 0;
        this->publish();   // the late result
    }

    w->requested = true;
    w->wake.notify_one();
    if (!w->done.wait_for(lock, patience, [w]() { return !w->busy && !w->requested; }))
    {
        this->m_stalledTicks = 1;
        return false;
    }
    this->publish();
    return w->lastOk;
}

void GPU::publish()
{
    // GUI thread, helper idle (mutex held): copy what it measured into the objects
    // the UI reads. Those keep their addresses; graphs hold pointers to the histories.
    this->m_stale = false;
    for (size_t i = 0; i < this->m_shadow.size(); ++i)
    {
        if (i >= this->m_gpus.size())
            this->m_gpus.push_back(std::make_unique<GPUInfo>());
        const GPUInfo &src = *this->m_shadow[i];
        GPUInfo &dst = *this->m_gpus[i];
        dst.ID = src.ID;
        dst.Name = src.Name;
        dst.DriverVersion = src.DriverVersion;
        dst.Backend = src.Backend;
        dst.UtilPct = src.UtilPct;
        dst.TemperatureC = src.TemperatureC;
        dst.CoreClockMHz = src.CoreClockMHz;
        dst.PowerUsageW = src.PowerUsageW;
        dst.PowerLimitW = src.PowerLimitW;
        dst.MemUsedMiB = src.MemUsedMiB;
        dst.MemTotalMiB = src.MemTotalMiB;
        dst.SharedMemUsedMiB = src.SharedMemUsedMiB;
        dst.SharedMemTotalMiB = src.SharedMemTotalMiB;
        dst.CopyTxBps = src.CopyTxBps;
        dst.CopyRxBps = src.CopyRxBps;
        dst.MaxCopyBps = src.MaxCopyBps;
        dst.UtilHistory = src.UtilHistory;
        dst.PowerHistory = src.PowerHistory;
        dst.MemUsageHistory = src.MemUsageHistory;
        dst.SharedMemHistory = src.SharedMemHistory;
        dst.CopyTxHistory = src.CopyTxHistory;
        dst.CopyRxHistory = src.CopyRxHistory;
        for (size_t e = 0; e < src.Engines.size(); ++e)
        {
            if (e >= dst.Engines.size())
                dst.Engines.push_back(std::make_unique<GPUEngineInfo>());
            dst.Engines[e]->Key = src.Engines[e]->Key;
            dst.Engines[e]->Label = src.Engines[e]->Label;
            dst.Engines[e]->Pct = src.Engines[e]->Pct;
            dst.Engines[e]->History = src.Engines[e]->History;
        }
    }
}

void GPU::markStale()
{
    // The driver has not answered for several samples: stop showing the last
    // readings as if they were current.
    if (this->m_stale)
        return;
    this->m_stale = true;
    LOG_WARN("GPU driver is not answering; GPU readings are on hold");
    for (auto &gpu : this->m_gpus)
    {
        gpu->UtilPct = 0.0;
        gpu->TemperatureC = -1;
        gpu->CoreClockMHz = -1;
        gpu->PowerUsageW = -1.0;
        gpu->CopyTxBps = 0.0;
        gpu->CopyRxBps = 0.0;
        for (auto &engine : gpu->Engines)
            engine->Pct = 0.0;
    }
}

void GPU::unloadGpuBackends()
{
    this->m_nvmlBackend.reset();
    this->m_amdSmiBackend.reset();
    this->m_intelSysmanBackend.reset();
    this->m_drmBackend.reset();
}
