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

#include "kernel.h"
#include <QDir>
#include <QFile>

Kernel::Kernel()
{
    this->sampleProcessStats();
}

bool Kernel::Sample()
{
    this->sampleProcessStats();
    return true;
}

void Kernel::sampleProcessStats()
{
    int procs = 0, threads = 0;
    const QDir procDir("/proc");
    for (const QString &entry : procDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
    {
        bool ok;
        entry.toInt(&ok);
        if (!ok)
            continue;
        ++procs;
        // Each thread appears as a subdirectory entry under /proc/<pid>/task/
        const QDir taskDir(QString("/proc/%1/task").arg(entry));
        // count() includes "." and ".." so subtract 2
        threads += static_cast<int>(taskDir.count()) - 2;
    }
    this->m_processCount = procs;
    this->m_threadCount  = qMax(0, threads);
}

