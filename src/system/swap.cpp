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

#include "swap.h"
#include "../misc.h"

#include <QFile>
#include <map>
#include <unistd.h>
#include <utility>

Swap::Swap()
{
}

const Swap::SwapInfo &Swap::FromIndex(int i) const
{
    if (i < 0 || i >= static_cast<int>(this->m_swaps.size()))
        return this->m_nullSwap;
    return *this->m_swaps.at(i);
}

bool Swap::Sample(bool &devicesChanged)
{
    devicesChanged = false;

    QFile f("/proc/swaps");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    struct ParsedSwap
    {
        QString name;
        QString type;
        qint64 totalKb { 0 };
        qint64 usedKb { 0 };
        qint64 freeKb { 0 };
        int priority { 0 };
    };

    std::vector<ParsedSwap> parsedSwaps;
    QStringList parsedNames;
    bool header = true;
    for (;;)
    {
        const QByteArray line = f.readLine();
        if (line.isNull())
            break;
        if (header)
        {
            header = false;
            continue;
        }

        const QList<QByteArray> parts = line.simplified().split(' ');
        if (parts.size() < 5)
            continue;

        ParsedSwap parsed;
        parsed.name = QString::fromUtf8(parts.at(0));
        parsed.type = QString::fromUtf8(parts.at(1));
        parsed.totalKb = parts.at(2).toLongLong();
        parsed.usedKb = parts.at(3).toLongLong();
        parsed.freeKb = qMax<qint64>(0, parsed.totalKb - parsed.usedKb);
        parsed.priority = parts.at(4).toInt();
        parsedSwaps.push_back(parsed);
        parsedNames.append(parsed.name);
    }
    f.close();

    QStringList existingNames;
    existingNames.reserve(static_cast<int>(this->m_swaps.size()));
    for (const auto &existing : std::as_const(this->m_swaps))
    {
        if (existing)
            existingNames.append(existing->Name);
    }
    devicesChanged = (existingNames != parsedNames);

    if (devicesChanged)
    {
        std::map<QString, std::unique_ptr<SwapInfo>> existingByName;
        for (auto &existing : this->m_swaps)
        {
            if (existing)
                existingByName.emplace(existing->Name, std::move(existing));
        }

        std::vector<std::unique_ptr<SwapInfo>> refreshedSwaps;
        refreshedSwaps.reserve(parsedSwaps.size());

        for (const ParsedSwap &parsed : parsedSwaps)
        {
            std::unique_ptr<SwapInfo> swapInfo;
            auto it = existingByName.find(parsed.name);
            if (it != existingByName.end())
            {
                swapInfo = std::move(it->second);
                existingByName.erase(it);
            }

            if (!swapInfo)
            {
                swapInfo = std::make_unique<SwapInfo>();
                swapInfo->Name = parsed.name;
            }
            refreshedSwaps.push_back(std::move(swapInfo));
        }

        this->m_swaps = std::move(refreshedSwaps);
    }

    for (int i = 0; i < static_cast<int>(parsedSwaps.size()) && i < static_cast<int>(this->m_swaps.size()); ++i)
    {
        const ParsedSwap &parsed = parsedSwaps.at(i);
        SwapInfo &swapInfo = *this->m_swaps.at(i);
        swapInfo.Name = parsed.name;
        swapInfo.Type = parsed.type;
        swapInfo.TotalKb = parsed.totalKb;
        swapInfo.UsedKb = parsed.usedKb;
        swapInfo.FreeKb = parsed.freeKb;
        swapInfo.Priority = parsed.priority;

        const double frac = (parsed.totalKb > 0)
                            ? static_cast<double>(parsed.usedKb) / static_cast<double>(parsed.totalKb)
                            : 0.0;
        swapInfo.UsageHistory.Push(frac * 100.0);
    }

    this->m_swapTotalKb = 0;
    this->m_swapUsedKb = 0;
    for (const auto &swapInfo : std::as_const(this->m_swaps))
    {
        this->m_swapTotalKb += qMax<qint64>(0, swapInfo->TotalKb);
        this->m_swapUsedKb += qMax<qint64>(0, swapInfo->UsedKb);
    }
    this->m_swapFreeKb = qMax<qint64>(0, this->m_swapTotalKb - this->m_swapUsedKb);

    const double swapFrac = (this->m_swapTotalKb > 0)
                            ? static_cast<double>(this->m_swapUsedKb) / static_cast<double>(this->m_swapTotalKb)
                            : 0.0;
    this->m_swapUsageHistory.Push(swapFrac * 100.0);

    quint64 pswpin = 0;
    quint64 pswpout = 0;
    QFile vmf("/proc/vmstat");
    if (vmf.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        for (;;)
        {
            const QByteArray line = vmf.readLine();
            if (line.isNull())
                break;
            const QList<QByteArray> parts = line.simplified().split(' ');
            if (parts.size() < 2)
                continue;
            if (parts.at(0) == "pswpin")
                pswpin = parts.at(1).toULongLong();
            else if (parts.at(0) == "pswpout")
                pswpout = parts.at(1).toULongLong();
        }
        vmf.close();
    }

    if (!this->m_swapTimer.isValid())
        this->m_swapTimer.start();
    const qint64 nowMs = this->m_swapTimer.elapsed();
    const qint64 dtMs = (this->m_prevSwapSampleMs > 0) ? (nowMs - this->m_prevSwapSampleMs) : 0;
    this->m_prevSwapSampleMs = nowMs;

    if (dtMs > 0)
    {
        const quint64 dInPages = (pswpin >= this->m_prevSwapInPages) ? (pswpin - this->m_prevSwapInPages) : 0;
        const quint64 dOutPages = (pswpout >= this->m_prevSwapOutPages) ? (pswpout - this->m_prevSwapOutPages) : 0;
        const long pageSize = ::sysconf(_SC_PAGESIZE);
        const double bytesPerPage = (pageSize > 0) ? static_cast<double>(pageSize) : 4096.0;
        this->m_swapInBps = static_cast<double>(dInPages) * bytesPerPage * 1000.0 / static_cast<double>(dtMs);
        this->m_swapOutBps = static_cast<double>(dOutPages) * bytesPerPage * 1000.0 / static_cast<double>(dtMs);
    } else
    {
        this->m_swapInBps = 0.0;
        this->m_swapOutBps = 0.0;
    }

    this->m_prevSwapInPages = pswpin;
    this->m_prevSwapOutPages = pswpout;
    Misc::PushHistoryPairAndUpdateMax(this->m_swapInHistory, this->m_swapInBps, this->m_swapOutHistory, this->m_swapOutBps, this->m_swapMaxActivityBps);
    return true;
}
