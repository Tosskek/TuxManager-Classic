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

#include "process.h"

#include <QHash>
#include <QObject>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace OS;

// ── Helpers ──────────────────────────────────────────────────────────────────

namespace
{
    //! Calls sysconf once and caches the result - this can't change in runtime
    long cachedPageSize()
    {
        static long s_pageSize = sysconf(_SC_PAGESIZE);
        if (s_pageSize <= 0)
            s_pageSize = 4096L;
        return s_pageSize;
    }

    QString cachedUserNameForUid(uid_t uid)
    {
        static QHash<uid_t, QString> s_userNameByUid;

        const auto it = s_userNameByUid.constFind(uid);
        if (it != s_userNameByUid.cend())
            return it.value();

        const struct passwd *pw = getpwuid(uid);
        const QString userName = pw ? QString::fromUtf8(pw->pw_name)
                                    : QString::number(uid);
        s_userNameByUid.insert(uid, userName);
        return userName;
    }
}

QString Process::GetStateString(char state)
{
    switch (state)
    {
        case 'R': return QObject::tr("Running");
        case 'S': return QObject::tr("Sleeping");
        case 'D': return QObject::tr("Disk sleep");
        case 'Z': return QObject::tr("Zombie");
        case 'T': return QObject::tr("Stopped");
        case 't': return QObject::tr("Tracing stop");
        case 'I': return QObject::tr("Idle");
        case 'X': return QObject::tr("Dead");
        default:  return QString(state);
    }
}

Process::Identity Process::GetIdentity() const
{
    return { this->PID, this->StartTimeTicks };
}

bool Process::HasSameData(const Process &rhs) const
{
    const Process &lhs = *this;
    return lhs.PID == rhs.PID
           && lhs.PPID == rhs.PPID
           && lhs.Name == rhs.Name
           && lhs.CmdLine == rhs.CmdLine
           && lhs.State == rhs.State
           && lhs.UID == rhs.UID
           && lhs.User == rhs.User
           && lhs.Priority == rhs.Priority
           && lhs.Nice == rhs.Nice
           && lhs.Threads == rhs.Threads
           && lhs.VMRssKb == rhs.VMRssKb
           && lhs.vmSizeKb == rhs.vmSizeKb
           && lhs.SharedKb == rhs.SharedKb
           && lhs.TextKb == rhs.TextKb
           && lhs.DataKb == rhs.DataKb
           && lhs.CPUTicks == rhs.CPUTicks
           && lhs.CPUPercent == rhs.CPUPercent
           && lhs.StartTimeTicks == rhs.StartTimeTicks
           && lhs.IsKernelThread == rhs.IsKernelThread
           && lhs.IOReadBytes == rhs.IOReadBytes
           && lhs.IOWriteBytes == rhs.IOWriteBytes
           && lhs.IOReadBps == rhs.IOReadBps
           && lhs.IOWriteBps == rhs.IOWriteBps
           && lhs.IOTotalsAvailable == rhs.IOTotalsAvailable
           && lhs.IORatesAvailable == rhs.IORatesAvailable
           && lhs.IOPermissionDenied == rhs.IOPermissionDenied
           && lhs.IconName == rhs.IconName;
}

bool OS::operator<(const Process::Identity &lhs, const Process::Identity &rhs)
{
    if (lhs.PID != rhs.PID)
        return lhs.PID < rhs.PID;
    return lhs.StartTimeTicks < rhs.StartTimeTicks;
}

bool OS::operator==(const Process::Identity &lhs, const Process::Identity &rhs)
{
    return lhs.PID == rhs.PID && lhs.StartTimeTicks == rhs.StartTimeTicks;
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
size_t OS::qHash(const Process::Identity &key, size_t seed)
#else
uint OS::qHash(const Process::Identity &key, uint seed)
#endif
{
    return ::qHash(key.StartTimeTicks, ::qHash(static_cast<quint32>(key.PID), seed));
}

// ── Private: raw /proc readers ───────────────────────────────────────────────
//
// Everything below runs on the process-refresh worker thread for every process
// on every refresh, so it avoids QFile / QString allocations: each /proc/<pid>
// directory is opened once and its files are read with openat() + read() into
// stack buffers.

namespace
{
    //! Reads a small /proc file relative to dirFd into buf (NUL terminated).
    //! Returns the number of bytes read, or -1 when the file can't be opened/read.
    ssize_t readProcFile(int dirFd, const char *name, char *buf, size_t cap)
    {
        const int fd = ::openat(dirFd, name, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return -1;
        size_t total = 0;
        bool failed = false;
        while (total < cap - 1)
        {
            const ssize_t n = ::read(fd, buf + total, cap - 1 - total);
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                failed = total == 0;
                break;
            }
            if (n == 0)
                break;
            total += static_cast<size_t>(n);
        }
        ::close(fd);
        if (failed)
            return -1;
        buf[total] = '\0';
        return static_cast<ssize_t>(total);
    }

    //! Parses the next unsigned decimal field and advances p past it.
    quint64 nextU64(const char *&p)
    {
        while (*p == ' ')
            ++p;
        char *end = nullptr;
        const quint64 v = ::strtoull(p, &end, 10);
        p = end;
        return v;
    }

    //! Parses the next signed decimal field and advances p past it.
    qint64 nextI64(const char *&p)
    {
        while (*p == ' ')
            ++p;
        char *end = nullptr;
        const qint64 v = ::strtoll(p, &end, 10);
        p = end;
        return v;
    }

    //! Skips n space-separated fields.
    void skipFields(const char *&p, int n)
    {
        for (int i = 0; i < n && *p; ++i)
        {
            while (*p == ' ')
                ++p;
            while (*p && *p != ' ')
                ++p;
        }
    }

    //! Per-thread cache of command lines, keyed by process identity. A process's
    //! command line rarely changes, so it is re-read only every few refreshes or
    //! when the process name (comm) changes, e.g. after exec().
    struct CmdlineCacheEntry
    {
        QString Name;
        QString CmdLine;
        quint64 ReadAtGeneration { 0 };
        quint64 SeenAtGeneration { 0 };
    };
    constexpr quint64 kCmdlineRefreshGenerations = 5;
    thread_local QHash<Process::Identity, CmdlineCacheEntry> t_cmdlineCache;
    thread_local quint64 t_generation = 0;
}

bool Process::loadOneStatAndUid(int dirFd, pid_t pid, Process &out)
{
    out.PID = pid;

    // ── /proc/pid/stat ──────────────────────────────────────────────────────
    // Format: pid (comm) state ppid pgrp session tty_nr tpgid flags
    //         minflt cminflt majflt cmajflt utime stime cutime cstime
    //         priority nice num_threads itrealvalue starttime vsize rss ...
    //
    // comm can contain spaces and parentheses, so we anchor on the last ')'.
    char buf[1024];
    const ssize_t len = readProcFile(dirFd, "stat", buf, sizeof(buf));
    if (len <= 0)
        return false;

    const char *commStart = static_cast<const char *>(::memchr(buf, '(', static_cast<size_t>(len)));
    const char *commEnd   = static_cast<const char *>(::memrchr(buf, ')', static_cast<size_t>(len)));
    if (!commStart || !commEnd || commEnd <= commStart || commEnd + 2 >= buf + len)
        return false;

    out.Name = QString::fromUtf8(commStart + 1, static_cast<int>(commEnd - commStart - 1));

    // Fields after ") " (0-indexed):
    //  0:state 1:ppid 2:pgrp 3:session 4:tty_nr 5:tpgid 6:flags
    //  7:minflt 8:cminflt 9:majflt 10:cmajflt
    //  11:utime 12:stime 13:cutime 14:cstime
    //  15:priority 16:nice 17:num_threads 18:itrealvalue
    //  19:starttime 20:vsize 21:rss
    const char *p = commEnd + 2;
    out.State = *p ? *p : '?';
    skipFields(p, 1);                                   // state
    out.PPID = static_cast<pid_t>(nextI64(p));          // 1
    skipFields(p, 4);                                   // 2..5
    const quint64 procFlags = nextU64(p);               // 6
    skipFields(p, 4);                                   // 7..10
    const quint64 utime = nextU64(p);                   // 11
    const quint64 stime = nextU64(p);                   // 12
    skipFields(p, 2);                                   // 13..14
    out.Priority = static_cast<int>(nextI64(p));        // 15
    out.Nice     = static_cast<int>(nextI64(p));        // 16
    out.Threads  = static_cast<int>(nextI64(p));        // 17
    skipFields(p, 1);                                   // 18
    out.StartTimeTicks = nextU64(p);                    // 19
    const quint64 vsize = nextU64(p);                   // 20
    const qint64  rss   = nextI64(p);                   // 21
    if (!*p && rss == 0 && vsize == 0 && out.StartTimeTicks == 0)
        return false; // truncated / malformed line

    // PF_KTHREAD is the authoritative kernel-thread flag (same as htop)
    static constexpr quint64 PF_KTHREAD = 0x00200000;
    out.IsKernelThread = (procFlags & PF_KTHREAD) != 0;
    out.CPUTicks       = utime + stime;
    out.vmSizeKb       = vsize / 1024ULL;
    out.VMRssKb        = static_cast<quint64>(qMax<qint64>(0, rss)) * static_cast<quint64>(cachedPageSize()) / 1024ULL;

    // ── UID via fstat() on the /proc/pid directory ───────────────────────────
    // The /proc/<pid> directory is owned by the process's real UID — far more
    // reliable than parsing /proc/<pid>/status which may use spaces or tabs.
    struct stat st{};
    if (::fstat(dirFd, &st) == 0)
        out.UID = st.st_uid;

    return true;
}

void Process::loadStatm(int dirFd, Process &proc)
{
    char buf[256];
    if (readProcFile(dirFd, "statm", buf, sizeof(buf)) <= 0)
        return;

    // size resident shared text lib data dt
    const char *p = buf;
    const quint64 size     = nextU64(p);
    const quint64 resident = nextU64(p);
    const quint64 shared   = nextU64(p);
    const quint64 text     = nextU64(p);
    nextU64(p); // lib (always 0)
    const quint64 data     = nextU64(p);

    const quint64 pageKb = static_cast<quint64>(cachedPageSize()) / 1024ULL;
    proc.vmSizeKb = size * pageKb;
    proc.VMRssKb  = resident * pageKb;
    proc.SharedKb = shared * pageKb;
    proc.TextKb   = text * pageKb;
    proc.DataKb   = data * pageKb;
}

bool Process::loadIO(int dirFd, Process &proc)
{
    char buf[512];
    if (readProcFile(dirFd, "io", buf, sizeof(buf)) <= 0)
        return false;

    // Lines: rchar, wchar, syscr, syscw, read_bytes, write_bytes, cancelled_write_bytes
    const char *readPos  = ::strstr(buf, "\nread_bytes:");
    const char *writePos = ::strstr(buf, "\nwrite_bytes:");
    if (!readPos || !writePos)
        return false;

    const char *p = readPos + sizeof("\nread_bytes:") - 1;
    proc.IOReadBytes = nextU64(p);
    p = writePos + sizeof("\nwrite_bytes:") - 1;
    proc.IOWriteBytes = nextU64(p);
    proc.IOTotalsAvailable = true;
    proc.IOPermissionDenied = false;
    return true;
}

void Process::loadUserAndCmdline(int dirFd, Process &proc)
{
    // Cache UID -> username/fallback string to avoid repeated getpwuid() lookups.
    proc.User = cachedUserNameForUid(proc.UID);

    // ── /proc/pid/cmdline ────────────────────────────────────────────────────
    // Kernel threads have no cmdline; use bracketed name as display string.
    if (proc.IsKernelThread)
    {
        proc.CmdLine = "[" + proc.Name + "]";
        return;
    }

    CmdlineCacheEntry &entry = t_cmdlineCache[proc.GetIdentity()];
    entry.SeenAtGeneration = t_generation;
    if (entry.ReadAtGeneration != 0
        && entry.Name == proc.Name
        && t_generation - entry.ReadAtGeneration < kCmdlineRefreshGenerations)
    {
        proc.CmdLine = entry.CmdLine;
        return;
    }

    QByteArray data;
    const int fd = ::openat(dirFd, "cmdline", O_RDONLY | O_CLOEXEC);
    if (fd >= 0)
    {
        char chunk[4096];
        for (;;)
        {
            const ssize_t n = ::read(fd, chunk, sizeof(chunk));
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                break;
            data.append(chunk, static_cast<int>(n));
        }
        ::close(fd);
    }
    data.replace('\0', ' ');
    proc.CmdLine = QString::fromUtf8(data).trimmed();
    if (proc.CmdLine.isEmpty())
        proc.CmdLine = proc.Name; // fallback: use comm name

    entry.Name = proc.Name;
    entry.CmdLine = proc.CmdLine;
    entry.ReadAtGeneration = t_generation;
}

// ── Public: load all processes ────────────────────────────────────────────────

QList<Process> Process::LoadAll()
{
    return Process::LoadAll(LoadOptions{});
}

QList<Process> Process::LoadAll(const LoadOptions &options)
{
    QList<Process> list;

    DIR *procDir = ::opendir("/proc");
    if (!procDir)
        return list;

    ++t_generation;
    const int procFd = ::dirfd(procDir);
    list.reserve(t_cmdlineCache.size() + 64);

    while (const struct dirent *ent = ::readdir(procDir))
    {
        // Only numeric entries are processes.
        const char *name = ent->d_name;
        if (name[0] < '1' || name[0] > '9')
            continue;
        char *end = nullptr;
        const long pidLong = ::strtol(name, &end, 10);
        if (!end || *end != '\0' || pidLong <= 0)
            continue;

        const int dirFd = ::openat(procFd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dirFd < 0)
            continue; // process exited meanwhile

        Process proc;
        if (!loadOneStatAndUid(dirFd, static_cast<pid_t>(pidLong), proc))
        {
            ::close(dirFd);
            continue;
        }

        // Fast prefilter on fields available from stat + proc dir ownership.
        if ((!options.IncludeKernelTasks && proc.IsKernelThread)
            || (!options.IncludeOtherUsers && proc.UID != options.MyUID))
        {
            ::close(dirFd);
            continue;
        }

        loadStatm(dirFd, proc);
        if (options.CollectIOMetrics)
        {
            if (options.IsSuperuser || proc.UID == options.EffectiveUID)
            {
                if (!loadIO(dirFd, proc))
                    proc.IOPermissionDenied = true;
            } else
            {
                proc.IOPermissionDenied = true;
            }
        }

        loadUserAndCmdline(dirFd, proc);
        ::close(dirFd);
        list.append(std::move(proc));
    }
    ::closedir(procDir);

    // Drop cached command lines of processes that are gone.
    for (auto it = t_cmdlineCache.begin(); it != t_cmdlineCache.end();)
    {
        if (it->SeenAtGeneration != t_generation)
            it = t_cmdlineCache.erase(it);
        else
            ++it;
    }

    return list;
}
