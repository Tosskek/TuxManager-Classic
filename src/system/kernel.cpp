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
#include <dirent.h>
#include <stdio.h>
#include <algorithm>

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
    int procs = 0;
    DIR *procDir = ::opendir("/proc");
    if (procDir)
    {
        while (struct dirent *entry = ::readdir(procDir))
        {
            if (entry->d_name[0] >= '0' && entry->d_name[0] <= '9')
            {
                bool isPid = true;
                for (const char *p = entry->d_name + 1; *p; ++p)
                {
                    if (*p < '0' || *p > '9')
                    {
                        isPid = false;
                        break;
                    }
                }
                if (isPid)
                    ++procs;
            }
        }
        ::closedir(procDir);
    }

    int threads = 0;
    FILE *f = ::fopen("/proc/loadavg", "r");
    if (f)
    {
        int running = 0;
        // Use %*s for the three load averages so locale decimal points (e.g. uk_UA comma) don't break parsing
        if (::fscanf(f, "%*s %*s %*s %d/%d", &running, &threads) != 2)
            threads = 0;
        ::fclose(f);
    }

    this->m_processCount = procs;
    this->m_threadCount  = std::max(0, threads);
}

