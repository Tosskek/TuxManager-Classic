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

#include "nvmlbackend.h"
#include "../../logger.h"
#include "../../misc.h"

#include <QHash>
#include <QSet>
#include <dlfcn.h>

#include <condition_variable>
#include <mutex>
#include <thread>

namespace
{
    using NvmlReturn = unsigned int;
    using NvmlDevice = void *;

    struct NvmlUtilization
    {
        unsigned int gpu;
        unsigned int memory;
    };

    struct NvmlMemory
    {
        uint64_t total;
        uint64_t free;
        uint64_t used;
    };

    struct NvmlProcessInfo_v2
    {
        unsigned int pid;
        uint64_t     usedGpuMemory;
        unsigned int gpuInstanceId;
        unsigned int computeInstanceId;
    };

    struct NvmlProcessUtilizationSample
    {
        unsigned int pid;
        uint64_t     timeStamp;
        unsigned int smUtil;
        unsigned int memUtil;
        unsigned int encUtil;
        unsigned int decUtil;
    };

    static constexpr NvmlReturn NVML_SUCCESS = 0;

    using FnNvmlInitV2 = NvmlReturn (*)();
    using FnNvmlShutdown = NvmlReturn (*)();
    using FnNvmlSystemGetDriverVersion = NvmlReturn (*)(char *, unsigned int);
    using FnNvmlDeviceGetCountV2 = NvmlReturn (*)(unsigned int *);
    using FnNvmlDeviceGetHandleByIndexV2 = NvmlReturn (*)(unsigned int, NvmlDevice *);
    using FnNvmlDeviceGetName = NvmlReturn (*)(NvmlDevice, char *, unsigned int);
    using FnNvmlDeviceGetUUID = NvmlReturn (*)(NvmlDevice, char *, unsigned int);
    using FnNvmlDeviceGetUtilizationRates = NvmlReturn (*)(NvmlDevice, NvmlUtilization *);
    using FnNvmlDeviceGetMemoryInfo = NvmlReturn (*)(NvmlDevice, NvmlMemory *);
    using FnNvmlDeviceGetEncoderUtilization = NvmlReturn (*)(NvmlDevice, unsigned int *, unsigned int *);
    using FnNvmlDeviceGetDecoderUtilization = NvmlReturn (*)(NvmlDevice, unsigned int *, unsigned int *);
    using FnNvmlDeviceGetPcieThroughput = NvmlReturn (*)(NvmlDevice, unsigned int, unsigned int *);
    using FnNvmlDeviceGetTemperature = NvmlReturn (*)(NvmlDevice, unsigned int, unsigned int *);
    using FnNvmlDeviceGetClockInfo = NvmlReturn (*)(NvmlDevice, unsigned int, unsigned int *);
    using FnNvmlDeviceGetPowerUsage = NvmlReturn (*)(NvmlDevice, unsigned int *);
    using FnNvmlDeviceGetEnforcedPowerLimit = NvmlReturn (*)(NvmlDevice, unsigned int *);
    using FnNvmlDeviceGetGraphicsRunningProcesses = NvmlReturn (*)(NvmlDevice, unsigned int *, NvmlProcessInfo_v2 *);
    using FnNvmlDeviceGetComputeRunningProcesses = NvmlReturn (*)(NvmlDevice, unsigned int *, NvmlProcessInfo_v2 *);
    using FnNvmlDeviceGetProcessUtilization = NvmlReturn (*)(NvmlDevice, NvmlProcessUtilizationSample *, unsigned int *, uint64_t);

    FnNvmlDeviceGetGraphicsRunningProcesses pNvmlDeviceGetGraphicsRunningProcesses = nullptr;
    FnNvmlDeviceGetComputeRunningProcesses pNvmlDeviceGetComputeRunningProcesses = nullptr;
    FnNvmlDeviceGetProcessUtilization pNvmlDeviceGetProcessUtilization = nullptr;

    FnNvmlInitV2 pNvmlInitV2 = nullptr;
    FnNvmlShutdown pNvmlShutdown = nullptr;
    FnNvmlSystemGetDriverVersion pNvmlSystemGetDriverVersion = nullptr;
    FnNvmlDeviceGetCountV2 pNvmlDeviceGetCountV2 = nullptr;
    FnNvmlDeviceGetHandleByIndexV2 pNvmlDeviceGetHandleByIndexV2 = nullptr;
    FnNvmlDeviceGetName pNvmlDeviceGetName = nullptr;
    FnNvmlDeviceGetUUID pNvmlDeviceGetUUID = nullptr;
    FnNvmlDeviceGetUtilizationRates pNvmlDeviceGetUtilizationRates = nullptr;
    FnNvmlDeviceGetMemoryInfo pNvmlDeviceGetMemoryInfo = nullptr;
    FnNvmlDeviceGetEncoderUtilization pNvmlDeviceGetEncoderUtilization = nullptr;
    FnNvmlDeviceGetDecoderUtilization pNvmlDeviceGetDecoderUtilization = nullptr;
    FnNvmlDeviceGetPcieThroughput pNvmlDeviceGetPcieThroughput = nullptr;
    FnNvmlDeviceGetTemperature pNvmlDeviceGetTemperature = nullptr;
    FnNvmlDeviceGetClockInfo pNvmlDeviceGetClockInfo = nullptr;
    FnNvmlDeviceGetPowerUsage pNvmlDeviceGetPowerUsage = nullptr;
    FnNvmlDeviceGetEnforcedPowerLimit pNvmlDeviceGetEnforcedPowerLimit = nullptr;

    GPU::GPUInfo *findOrCreateGpu(std::vector<std::unique_ptr<GPU::GPUInfo>> &gpus, const QString &id)
    {
        for (const auto &gpu : gpus)
        {
            if (gpu->ID == id)
                return gpu.get();
        }

        auto gpu = std::make_unique<GPU::GPUInfo>();
        gpu->ID = id;
        gpus.push_back(std::move(gpu));
        return gpus.back().get();
    }

    void zeroMissingEngines(GPU::GPUInfo &gpu, const QSet<QString> &seenEngineKeys)
    {
        for (const auto &engine : gpu.Engines)
        {
            if (seenEngineKeys.contains(engine->Key))
                continue;

            engine->Pct = 0.0;
            engine->History.Push(0.0);
        }
    }
}

struct GpuNvmlBackend::PcieWorker
{
    std::mutex              mutex;
    std::condition_variable cv;
    std::thread             thread;
    bool                    stop { false };
    bool                    requested { false };
    std::vector<NvmlDevice> devices;                       //!< devices to query on next request
    QHash<NvmlDevice, std::pair<unsigned int, unsigned int>> results; //!< device -> (tx KB/s, rx KB/s)

    void run()
    {
        static constexpr unsigned int kNvmlPcieTxBytes = 0;
        static constexpr unsigned int kNvmlPcieRxBytes = 1;

        std::unique_lock<std::mutex> lock(this->mutex);
        for (;;)
        {
            this->cv.wait(lock, [this] { return this->stop || this->requested; });
            if (this->stop)
                return;
            this->requested = false;
            const std::vector<NvmlDevice> todo = this->devices;
            lock.unlock();

            QHash<NvmlDevice, std::pair<unsigned int, unsigned int>> fresh;
            for (NvmlDevice dev : todo)
            {
                unsigned int tx = 0;
                unsigned int rx = 0;
                if (pNvmlDeviceGetPcieThroughput(dev, kNvmlPcieTxBytes, &tx) != NVML_SUCCESS)
                    tx = 0;
                if (pNvmlDeviceGetPcieThroughput(dev, kNvmlPcieRxBytes, &rx) != NVML_SUCCESS)
                    rx = 0;
                fresh.insert(dev, { tx, rx });
            }

            lock.lock();
            this->results = fresh;
        }
    }
};

GpuNvmlBackend::GpuNvmlBackend() = default;

GpuNvmlBackend::~GpuNvmlBackend()
{
    this->unload();
}

void GpuNvmlBackend::stopPcieWorker()
{
    if (!this->m_pcie)
        return;
    {
        std::lock_guard<std::mutex> lock(this->m_pcie->mutex);
        this->m_pcie->stop = true;
    }
    this->m_pcie->cv.notify_all();
    if (this->m_pcie->thread.joinable())
        this->m_pcie->thread.join();
    this->m_pcie.reset();
}

void GpuNvmlBackend::Detect()
{
    this->unload();

    this->m_libHandle = ::dlopen("libnvidia-ml.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!this->m_libHandle)
    {
        LOG_DEBUG("NVML scan: unable to locate libnvidia-ml.so.1, trying fallback to libnvidia-ml.so");
        this->m_libHandle = ::dlopen("libnvidia-ml.so", RTLD_LAZY | RTLD_LOCAL);
    }

    if (!this->m_libHandle)
    {
        LOG_DEBUG("NVML scan: no nvidia driver libs detected");
        return;
    }

    pNvmlInitV2 = reinterpret_cast<FnNvmlInitV2>(::dlsym(this->m_libHandle, "nvmlInit_v2"));
    pNvmlShutdown = reinterpret_cast<FnNvmlShutdown>(::dlsym(this->m_libHandle, "nvmlShutdown"));
    pNvmlSystemGetDriverVersion = reinterpret_cast<FnNvmlSystemGetDriverVersion>(::dlsym(this->m_libHandle, "nvmlSystemGetDriverVersion"));
    pNvmlDeviceGetCountV2 = reinterpret_cast<FnNvmlDeviceGetCountV2>(::dlsym(this->m_libHandle, "nvmlDeviceGetCount_v2"));
    pNvmlDeviceGetHandleByIndexV2 = reinterpret_cast<FnNvmlDeviceGetHandleByIndexV2>(::dlsym(this->m_libHandle, "nvmlDeviceGetHandleByIndex_v2"));
    pNvmlDeviceGetName = reinterpret_cast<FnNvmlDeviceGetName>(::dlsym(this->m_libHandle, "nvmlDeviceGetName"));
    pNvmlDeviceGetUUID = reinterpret_cast<FnNvmlDeviceGetUUID>(::dlsym(this->m_libHandle, "nvmlDeviceGetUUID"));
    pNvmlDeviceGetUtilizationRates = reinterpret_cast<FnNvmlDeviceGetUtilizationRates>(::dlsym(this->m_libHandle, "nvmlDeviceGetUtilizationRates"));
    pNvmlDeviceGetMemoryInfo = reinterpret_cast<FnNvmlDeviceGetMemoryInfo>(::dlsym(this->m_libHandle, "nvmlDeviceGetMemoryInfo"));
    pNvmlDeviceGetEncoderUtilization = reinterpret_cast<FnNvmlDeviceGetEncoderUtilization>(::dlsym(this->m_libHandle, "nvmlDeviceGetEncoderUtilization"));
    pNvmlDeviceGetDecoderUtilization = reinterpret_cast<FnNvmlDeviceGetDecoderUtilization>(::dlsym(this->m_libHandle, "nvmlDeviceGetDecoderUtilization"));
    pNvmlDeviceGetPcieThroughput = reinterpret_cast<FnNvmlDeviceGetPcieThroughput>(::dlsym(this->m_libHandle, "nvmlDeviceGetPcieThroughput"));
    pNvmlDeviceGetTemperature = reinterpret_cast<FnNvmlDeviceGetTemperature>(::dlsym(this->m_libHandle, "nvmlDeviceGetTemperature"));
    pNvmlDeviceGetClockInfo = reinterpret_cast<FnNvmlDeviceGetClockInfo>(::dlsym(this->m_libHandle, "nvmlDeviceGetClockInfo"));
    pNvmlDeviceGetPowerUsage = reinterpret_cast<FnNvmlDeviceGetPowerUsage>(::dlsym(this->m_libHandle, "nvmlDeviceGetPowerUsage"));
    pNvmlDeviceGetEnforcedPowerLimit = reinterpret_cast<FnNvmlDeviceGetEnforcedPowerLimit>(::dlsym(this->m_libHandle, "nvmlDeviceGetEnforcedPowerLimit"));
    pNvmlDeviceGetGraphicsRunningProcesses = reinterpret_cast<FnNvmlDeviceGetGraphicsRunningProcesses>(::dlsym(this->m_libHandle, "nvmlDeviceGetGraphicsRunningProcesses_v3"));
    pNvmlDeviceGetComputeRunningProcesses = reinterpret_cast<FnNvmlDeviceGetComputeRunningProcesses>(::dlsym(this->m_libHandle, "nvmlDeviceGetComputeRunningProcesses_v3"));
    pNvmlDeviceGetProcessUtilization = reinterpret_cast<FnNvmlDeviceGetProcessUtilization>(::dlsym(this->m_libHandle, "nvmlDeviceGetProcessUtilization"));

    const bool symbolsOk = pNvmlInitV2 && pNvmlShutdown && pNvmlDeviceGetCountV2
                           && pNvmlDeviceGetHandleByIndexV2 && pNvmlDeviceGetName
                           && pNvmlDeviceGetUUID && pNvmlDeviceGetUtilizationRates
                           && pNvmlDeviceGetMemoryInfo;
    if (!symbolsOk)
    {
        LOG_DEBUG("NVML: required symbols not found, unloading");
        this->unload();
        return;
    }

    if (pNvmlInitV2() != NVML_SUCCESS)
    {
        LOG_DEBUG("NVML: nvmlInit_v2 failed, unloading");
        this->unload();
        return;
    }

    unsigned int count = 0;
    if (pNvmlDeviceGetCountV2(&count) != NVML_SUCCESS || count == 0)
    {
        LOG_DEBUG("NVML: no devices found, shutting down");
        this->unload();
        return;
    }

    this->m_available = true;
}

bool GpuNvmlBackend::Sample(std::vector<std::unique_ptr<GPU::GPUInfo>> &gpus)
{
    if (!this->m_available || !pNvmlDeviceGetCountV2 || !pNvmlDeviceGetHandleByIndexV2)
        return false;

    unsigned int count = 0;
    if (pNvmlDeviceGetCountV2(&count) != NVML_SUCCESS)
        return false;

    char driverVer[96] = {};
    if (pNvmlSystemGetDriverVersion)
        pNvmlSystemGetDriverVersion(driverVer, sizeof(driverVer));
    const QString driverVersion = QString::fromLatin1(driverVer).trimmed();

    QSet<QString> seenIds;
    std::vector<NvmlDevice> pcieDevices;

    for (unsigned int i = 0; i < count; ++i)
    {
        static constexpr unsigned int kNvmlTemperatureGpu = 0;
        static constexpr unsigned int kNvmlClockGraphics = 0;

        NvmlDevice dev = nullptr;
        if (pNvmlDeviceGetHandleByIndexV2(i, &dev) != NVML_SUCCESS || !dev)
            continue;

        char nameBuf[128] = {};
        char uuidBuf[96] = {};
        pNvmlDeviceGetName(dev, nameBuf, sizeof(nameBuf));
        pNvmlDeviceGetUUID(dev, uuidBuf, sizeof(uuidBuf));

        const QString name = QString::fromLatin1(nameBuf).trimmed();
        QString id = QString::fromLatin1(uuidBuf).trimmed();
        if (id.isEmpty())
            id = QString("gpu-%1").arg(i);

        NvmlUtilization util{};
        NvmlMemory mem{};
        const bool hasUtil = pNvmlDeviceGetUtilizationRates(dev, &util) == NVML_SUCCESS;
        const bool hasMem = pNvmlDeviceGetMemoryInfo(dev, &mem) == NVML_SUCCESS;
        unsigned int txKBps = 0;
        unsigned int rxKBps = 0;
        bool hasTx = false;
        bool hasRx = false;
        if (this->m_pcie)
        {
            // Result of the previous asynchronous query (one sample behind).
            std::lock_guard<std::mutex> lock(this->m_pcie->mutex);
            const auto it = this->m_pcie->results.constFind(dev);
            if (it != this->m_pcie->results.cend())
            {
                txKBps = it->first;
                rxKBps = it->second;
                hasTx = hasRx = true;
            }
        }
        pcieDevices.push_back(dev);
        unsigned int tempC = 0;
        const bool hasTemp = pNvmlDeviceGetTemperature
                             && pNvmlDeviceGetTemperature(dev, kNvmlTemperatureGpu, &tempC) == NVML_SUCCESS;
        unsigned int coreClockMHz = 0;
        const bool hasCoreClock = pNvmlDeviceGetClockInfo
                                  && pNvmlDeviceGetClockInfo(dev, kNvmlClockGraphics, &coreClockMHz) == NVML_SUCCESS;
        unsigned int powerMilliW = 0;
        const bool hasPower = pNvmlDeviceGetPowerUsage
                              && pNvmlDeviceGetPowerUsage(dev, &powerMilliW) == NVML_SUCCESS;
        unsigned int powerLimitMilliW = 0;
        const bool hasPowerLimit = pNvmlDeviceGetEnforcedPowerLimit
                                   && pNvmlDeviceGetEnforcedPowerLimit(dev, &powerLimitMilliW) == NVML_SUCCESS
                                   && powerLimitMilliW > 0;

        GPU::GPUInfo &gpu = *findOrCreateGpu(gpus, id);
        gpu.ID = id;
        gpu.Name = name;
        gpu.DriverVersion = driverVersion;
        gpu.Backend = QStringLiteral("NVML");
        gpu.UtilPct = hasUtil ? qBound(0.0, static_cast<double>(util.gpu), 100.0) : 0.0;
        gpu.TemperatureC = hasTemp ? static_cast<int>(tempC) : -1;
        gpu.CoreClockMHz = hasCoreClock ? static_cast<int>(coreClockMHz) : -1;
        gpu.PowerUsageW = hasPower ? static_cast<double>(powerMilliW) / 1000.0 : -1.0;
        gpu.PowerLimitW = hasPowerLimit ? static_cast<double>(powerLimitMilliW) / 1000.0 : -1.0;
        gpu.MemUsedMiB = hasMem ? static_cast<qint64>(mem.used / (1024ULL * 1024ULL)) : 0;
        gpu.MemTotalMiB = hasMem ? static_cast<qint64>(mem.total / (1024ULL * 1024ULL)) : 0;
        gpu.SharedMemUsedMiB = 0;
        gpu.SharedMemTotalMiB = 0;
        gpu.CopyTxBps = hasTx ? static_cast<double>(txKBps) * 1024.0 : 0.0;
        gpu.CopyRxBps = hasRx ? static_cast<double>(rxKBps) * 1024.0 : 0.0;
        gpu.UtilHistory.Push(gpu.UtilPct);
        const double powerPct = (gpu.PowerLimitW > 0.0 && gpu.PowerUsageW >= 0.0)
                                    ? (gpu.PowerUsageW / gpu.PowerLimitW * 100.0)
                                    : 0.0;
        gpu.PowerHistory.Push(powerPct);
        const double memPct = gpu.MemTotalMiB > 0
                                  ? static_cast<double>(gpu.MemUsedMiB) / static_cast<double>(gpu.MemTotalMiB) * 100.0
                                  : 0.0;
        gpu.MemUsageHistory.Push(memPct);
        gpu.SharedMemHistory.Push(0.0);
        Misc::PushHistoryAndUpdateMax(gpu.CopyTxHistory, gpu.CopyTxBps, gpu.MaxCopyBps);
        Misc::PushHistoryAndUpdateMax(gpu.CopyRxHistory, gpu.CopyRxBps, gpu.MaxCopyBps);

        QSet<QString> seenEngineKeys;
        auto addEngine = [&](const QString &key, const QString &label, double pct)
        {
            GPU::GPUEngineInfo *engine = gpu.FindEngine(key);
            if (!engine)
            {
                auto newEngine = std::make_unique<GPU::GPUEngineInfo>();
                newEngine->Key = key;
                newEngine->Label = label;
                gpu.Engines.push_back(std::move(newEngine));
                engine = gpu.Engines.back().get();
            }
            engine->Pct = qBound(0.0, pct, 100.0);
            engine->History.Push(engine->Pct);
            seenEngineKeys.insert(key);
        };

        if (hasUtil)
        {
            double graphicsUtil = util.gpu;
            double computeUtil = 0.0;

            if (pNvmlDeviceGetGraphicsRunningProcesses
                && pNvmlDeviceGetComputeRunningProcesses
                && pNvmlDeviceGetProcessUtilization)
            {
                auto collectPids = [&dev](auto fn) -> QSet<unsigned int>
                {
                    unsigned int count = 0;
                    fn(dev, &count, nullptr);
                    QSet<unsigned int> pids;
                    if (count > 0)
                    {
                        QVector<NvmlProcessInfo_v2> procs(static_cast<int>(count));
                        if (fn(dev, &count, procs.data()) == NVML_SUCCESS)
                        {
                            for (unsigned int p = 0; p < count; ++p)
                                pids.insert(procs[static_cast<int>(p)].pid);
                        }
                    }
                    return pids;
                };

                const QSet<unsigned int> gfxPids = collectPids(pNvmlDeviceGetGraphicsRunningProcesses);
                const QSet<unsigned int> compPids = collectPids(pNvmlDeviceGetComputeRunningProcesses);

                unsigned int procUtilCount = 64;
                QVector<NvmlProcessUtilizationSample> procUtil(static_cast<int>(procUtilCount));
                NvmlReturn ret = pNvmlDeviceGetProcessUtilization(dev, procUtil.data(), &procUtilCount, 0);
                if (ret != NVML_SUCCESS && procUtilCount > 64 && procUtilCount <= 1024)
                {
                    procUtil.resize(static_cast<int>(procUtilCount));
                    ret = pNvmlDeviceGetProcessUtilization(dev, procUtil.data(), &procUtilCount, 0);
                }
                if (ret == NVML_SUCCESS && procUtilCount > 0)
                {
                    uint64_t maxTs = 0;
                    for (unsigned int p = 0; p < procUtilCount; ++p)
                    {
                        if (procUtil[static_cast<int>(p)].timeStamp > maxTs)
                            maxTs = procUtil[static_cast<int>(p)].timeStamp;
                    }

                    // Only consider fresh samples within 1.5s of the newest sample
                    const uint64_t kMaxAgeUs = 1500000;
                    double sumComp = 0.0;
                    double sumGfx = 0.0;
                    for (unsigned int p = 0; p < procUtilCount; ++p)
                    {
                        const auto &sample = procUtil[static_cast<int>(p)];
                        if (maxTs > sample.timeStamp && (maxTs - sample.timeStamp) > kMaxAgeUs)
                            continue;

                        if (compPids.contains(sample.pid) && !gfxPids.contains(sample.pid))
                            sumComp += sample.smUtil;
                        else
                            sumGfx += sample.smUtil;
                    }

                    if (sumComp > 0.0)
                    {
                        // Dedicated compute workload active: apportion hardware SM util
                        const double total = sumGfx + sumComp;
                        if (total > 0.0)
                        {
                            graphicsUtil = qBound(0.0, util.gpu * (sumGfx / total), 100.0);
                            computeUtil = qBound(0.0, util.gpu * (sumComp / total), 100.0);
                        }
                    }
                    else
                    {
                        // Standard graphics load: 3D engine directly reflects hardware SM utilization
                        graphicsUtil = util.gpu;
                        computeUtil = 0.0;
                    }
                }
            }

            addEngine(QStringLiteral("3d"), QStringLiteral("3D"), graphicsUtil);
            addEngine(QStringLiteral("cuda"), QStringLiteral("CUDA"), computeUtil);
            addEngine(QStringLiteral("copy"), QStringLiteral("Copy"), util.memory);
        }
        if (pNvmlDeviceGetEncoderUtilization)
        {
            unsigned int enc = 0;
            unsigned int sampling = 0;
            if (pNvmlDeviceGetEncoderUtilization(dev, &enc, &sampling) == NVML_SUCCESS)
                addEngine(QStringLiteral("video-encode"), QStringLiteral("Video Encode"), enc);
        }
        if (pNvmlDeviceGetDecoderUtilization)
        {
            unsigned int dec = 0;
            unsigned int sampling = 0;
            if (pNvmlDeviceGetDecoderUtilization(dev, &dec, &sampling) == NVML_SUCCESS)
                addEngine(QStringLiteral("video-decode"), QStringLiteral("Video Decode"), dec);
        }

        // Board power as a selectable "engine": percent of the enforced power limit.
        if (hasPower && hasPowerLimit)
            addEngine(QStringLiteral("power"), QStringLiteral("Power usage"), gpu.PowerUsageW / gpu.PowerLimitW * 100.0);

        zeroMissingEngines(gpu, seenEngineKeys);
        seenIds.insert(id);
    }

    // Kick off the next (slow) PCIe throughput query in the background.
    if (pNvmlDeviceGetPcieThroughput && !pcieDevices.empty())
    {
        if (!this->m_pcie)
        {
            this->m_pcie = std::make_unique<PcieWorker>();
            this->m_pcie->thread = std::thread([w = this->m_pcie.get()] { w->run(); });
        }
        {
            std::lock_guard<std::mutex> lock(this->m_pcie->mutex);
            this->m_pcie->devices = pcieDevices;
            this->m_pcie->requested = true;
        }
        this->m_pcie->cv.notify_one();
    }

    for (const auto &gpu : gpus)
    {
        if (gpu->Backend != QLatin1String("NVML") || seenIds.contains(gpu->ID))
            continue;

        gpu->UtilPct = 0.0;
        gpu->TemperatureC = -1;
        gpu->CoreClockMHz = -1;
        gpu->PowerUsageW = -1.0;
        gpu->MemUsedMiB = 0;
        gpu->MemTotalMiB = 0;
        gpu->SharedMemUsedMiB = 0;
        gpu->SharedMemTotalMiB = 0;
        gpu->CopyTxBps = 0.0;
        gpu->CopyRxBps = 0.0;
        gpu->UtilHistory.Push(0.0);
        gpu->PowerHistory.Push(0.0);
        gpu->MemUsageHistory.Push(0.0);
        gpu->SharedMemHistory.Push(0.0);
        Misc::PushHistoryAndUpdateMax(gpu->CopyTxHistory, 0.0, gpu->MaxCopyBps);
        Misc::PushHistoryAndUpdateMax(gpu->CopyRxHistory, 0.0, gpu->MaxCopyBps);
        for (const auto &engine : gpu->Engines)
        {
            engine->Pct = 0.0;
            engine->History.Push(0.0);
        }
    }

    return true;
}

void GpuNvmlBackend::unload()
{
    // The helper thread calls into NVML, so it must stop before NVML is shut down.
    this->stopPcieWorker();

    if (this->m_available && pNvmlShutdown)
        pNvmlShutdown();

    this->m_available = false;

    pNvmlInitV2 = nullptr;
    pNvmlShutdown = nullptr;
    pNvmlSystemGetDriverVersion = nullptr;
    pNvmlDeviceGetCountV2 = nullptr;
    pNvmlDeviceGetHandleByIndexV2 = nullptr;
    pNvmlDeviceGetName = nullptr;
    pNvmlDeviceGetUUID = nullptr;
    pNvmlDeviceGetUtilizationRates = nullptr;
    pNvmlDeviceGetMemoryInfo = nullptr;
    pNvmlDeviceGetEncoderUtilization = nullptr;
    pNvmlDeviceGetDecoderUtilization = nullptr;
    pNvmlDeviceGetPcieThroughput = nullptr;
    pNvmlDeviceGetTemperature = nullptr;
    pNvmlDeviceGetClockInfo = nullptr;
    pNvmlDeviceGetPowerUsage = nullptr;
    pNvmlDeviceGetGraphicsRunningProcesses = nullptr;
    pNvmlDeviceGetComputeRunningProcesses = nullptr;
    pNvmlDeviceGetProcessUtilization = nullptr;

    if (this->m_libHandle)
    {
        ::dlclose(this->m_libHandle);
        this->m_libHandle = nullptr;
    }
}
