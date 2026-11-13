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

#include "processhelper.h"

#include <cerrno>
#include <cstring>

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace
{
    struct StatInfo
    {
        quint64 startTicks { 0 };
        pid_t   ppid { 0 };
        char    comm[32] { 0 };
        bool    ok { false };
    };

    // Reads /proc/<pid>/stat (relative to procFd) without any allocation.
    StatInfo readStat(int procFd, pid_t pid)
    {
        StatInfo info;
        char path[32];
        snprintf(path, sizeof(path), "%d/stat", static_cast<int>(pid));
        const int fd = ::openat(procFd, path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return info;
        char buf[1024];
        const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
        ::close(fd);
        if (n <= 0)
            return info;
        buf[n] = '\0';
        // "pid (comm) S ppid ..." - comm may contain spaces and brackets, so find the last ')'.
        const char *open = strchr(buf, '(');
        const char *close = strrchr(buf, ')');
        if (!open || !close || close < open)
            return info;
        const size_t commLen = qMin<size_t>(static_cast<size_t>(close - open - 1), sizeof(info.comm) - 1);
        memcpy(info.comm, open + 1, commLen);
        info.comm[commLen] = '\0';
        // Fields after ')': state(3) ppid(4) ... starttime(22)
        const char *p = close + 1;
        int field = 2;
        while (*p)
        {
            while (*p == ' ')
                ++p;
            if (!*p)
                break;
            ++field;
            if (field == 4)
                info.ppid = static_cast<pid_t>(strtol(p, nullptr, 10));
            else if (field == 22)
            {
                info.startTicks = strtoull(p, nullptr, 10);
                info.ok = true;
                break;
            }
            while (*p && *p != ' ')
                ++p;
        }
        return info;
    }

    int openProc()
    {
        return ::open("/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    }

    // Signals one process only if it is still the one that was listed.
    // Returns 0 = sent, 1 = gone / other process, -1 = error (errno set).
    int signalChecked(int procFd, pid_t pid, quint64 startTicks, int signal)
    {
        if (pid <= 0)
            return 1;
        // A pidfd pins the process: once the start time is verified through it, the
        // signal cannot land on a later process that got the same PID.
        const int pidfd = static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
        if (pidfd < 0 && errno == ESRCH)
            return 1;
        if (startTicks != 0)
        {
            const StatInfo now = readStat(procFd, pid);
            if (!now.ok || now.startTicks != startTicks)
            {
                if (pidfd >= 0)
                    ::close(pidfd);
                return 1;
            }
        }
        int rc;
        if (pidfd >= 0)
        {
            rc = static_cast<int>(::syscall(SYS_pidfd_send_signal, pidfd, signal, nullptr, 0));
            const int saved = errno;
            ::close(pidfd);
            errno = saved;
        } else
        {
            rc = ::kill(pid, signal);   // kernel without pidfd support
        }
        if (rc == 0)
            return 0;
        return errno == ESRCH ? 1 : -1;
    }

    void addError(OS::ProcessHelper::SignalResult &result, pid_t pid, const QString &name, int signal)
    {
        const int err = errno;
        if (result.Errors.size() < 12)
            result.Errors << QString("%1 (PID %2): %3").arg(name.isEmpty() ? QString("?") : name).arg(pid).arg(strerror(err));
        else if (result.Errors.size() == 12)
            result.Errors << QString("...");
        Q_UNUSED(signal);
    }
}

using namespace OS;

bool ProcessHelper::SendSignal(pid_t pid, int signal, QString &errorMsg)
{
    if (::kill(pid, signal) == 0)
        return true;
    errorMsg = QString("kill(%1, %2) failed: %3").arg(pid).arg(signal).arg(strerror(errno));
    return false;
}

bool ProcessHelper::Renice(pid_t pid, int nice, QString &errorMsg)
{
    errno = 0;
    if (setpriority(PRIO_PROCESS, static_cast<id_t>(pid), nice) == 0)
        return true;
    errorMsg = QString("setpriority(%1, %2) failed: %3").arg(pid).arg(nice).arg(strerror(errno));
    return false;
}

quint64 ProcessHelper::StartTimeOf(pid_t pid)
{
    const int procFd = openProc();
    if (procFd < 0)
        return 0;
    const StatInfo info = readStat(procFd, pid);
    ::close(procFd);
    return info.ok ? info.startTicks : 0;
}

ProcessHelper::SignalResult ProcessHelper::SendSignalChecked(const QList<Target> &targets, int signal)
{
    SignalResult result;
    const int procFd = openProc();
    for (const Target &t : targets)
    {
        const int rc = signalChecked(procFd, t.PID, t.StartTimeTicks, signal);
        if (rc == 0)
            ++result.Sent;
        else if (rc == 1)
            ++result.Gone;
        else
            addError(result, t.PID, t.Name, signal);
    }
    if (procFd >= 0)
        ::close(procFd);
    return result;
}

ProcessHelper::SignalResult ProcessHelper::KillGroup(const QList<Target> &targets, const QSet<QString> &sweepNames, const QSet<pid_t> &knownPids)
{
    SignalResult result;
    const int procFd = openProc();
    if (procFd < 0)
        return SendSignalChecked(targets, SIGKILL);

    const pid_t self = ::getpid();
    QHash<pid_t, Target> all;
    QSet<uid_t> uids;
    QSet<QByteArray> names;
    for (const QString &n : sweepNames)
        names.insert(n.toUtf8().left(15));   // comm is at most 15 bytes
    QList<Target> fresh = targets;
    for (const Target &t : targets)
        uids.insert(t.UID);

    // Stop, look for what the stopped ones had already started, stop those too...
    // A stopped process cannot start anything, so this converges.
    for (int round = 0; round < 12 && !fresh.isEmpty(); ++round)
    {
        for (const Target &t : std::as_const(fresh))
        {
            if (t.PID == self || t.PID <= 1 || all.contains(t.PID))
                continue;
            all.insert(t.PID, t);
            signalChecked(procFd, t.PID, t.StartTimeTicks, SIGSTOP);
        }
        fresh.clear();

        DIR *dir = ::fdopendir(::dup(procFd));
        if (!dir)
            break;
        ::rewinddir(dir);
        while (const struct dirent *entry = ::readdir(dir))
        {
            if (entry->d_name[0] < '1' || entry->d_name[0] > '9')
                continue;
            const pid_t pid = static_cast<pid_t>(strtol(entry->d_name, nullptr, 10));
            if (pid <= 1 || pid == self || all.contains(pid))
                continue;
            const StatInfo info = readStat(procFd, pid);
            if (!info.ok)
                continue;
            const bool child = all.contains(info.ppid);
            const bool sameNameNew = !child && !names.isEmpty() && !knownPids.contains(pid) && names.contains(QByteArray(info.comm));
            if (!child && !sameNameNew)
                continue;
            struct stat st{};
            if (::fstatat(procFd, entry->d_name, &st, 0) != 0)
                continue;
            if (sameNameNew && !uids.contains(st.st_uid))
                continue;
            Target t;
            t.PID = pid;
            t.StartTimeTicks = info.startTicks;
            t.UID = st.st_uid;
            t.Name = QString::fromUtf8(info.comm);
            fresh.append(t);
        }
        ::closedir(dir);
        result.Swept += fresh.size();
    }

    for (auto it = all.cbegin(); it != all.cend(); ++it)
    {
        const int rc = signalChecked(procFd, it->PID, it->StartTimeTicks, SIGKILL);
        if (rc == 0)
            ++result.Sent;
        else if (rc == 1)
            ++result.Gone;
        else
        {
            addError(result, it->PID, it->Name, SIGKILL);
            // Could not kill it: at least do not leave it frozen.
            const int saved = errno;
            signalChecked(procFd, it->PID, it->StartTimeTicks, SIGCONT);
            errno = saved;
        }
    }
    ::close(procFd);
    return result;
}

QString ProcessHelper::GetSignalName(int signal)
{
    switch (signal)
    {
        case SIGHUP:    return "SIGHUP (1)";
        case SIGINT:    return "SIGINT (2)";
        case SIGQUIT:   return "SIGQUIT (3)";
        case SIGILL:    return "SIGILL (4)";
        case SIGTRAP:   return "SIGTRAP (5)";
        case SIGABRT:   return "SIGABRT (6)";
        case SIGBUS:    return "SIGBUS (7)";
        case SIGFPE:    return "SIGFPE (8)";
        case SIGKILL:   return "SIGKILL (9)";
        case SIGUSR1:   return "SIGUSR1 (10)";
        case SIGSEGV:   return "SIGSEGV (11)";
        case SIGUSR2:   return "SIGUSR2 (12)";
        case SIGPIPE:   return "SIGPIPE (13)";
        case SIGALRM:   return "SIGALRM (14)";
        case SIGTERM:   return "SIGTERM (15)";
#ifdef SIGSTKFLT
        case SIGSTKFLT: return "SIGSTKFLT (16)";
#endif
        case SIGCHLD:   return "SIGCHLD (17)";
        case SIGCONT:   return "SIGCONT (18)";
        case SIGSTOP:   return "SIGSTOP (19)";
        case SIGTSTP:   return "SIGTSTP (20)";
        case SIGTTIN:   return "SIGTTIN (21)";
        case SIGTTOU:   return "SIGTTOU (22)";
        case SIGURG:    return "SIGURG (23)";
        case SIGXCPU:   return "SIGXCPU (24)";
        case SIGXFSZ:   return "SIGXFSZ (25)";
        case SIGVTALRM: return "SIGVTALRM (26)";
        case SIGPROF:   return "SIGPROF (27)";
        case SIGWINCH:  return "SIGWINCH (28)";
        case SIGIO:     return "SIGIO (29)";
#ifdef SIGPWR
        case SIGPWR:    return "SIGPWR (30)";
#endif
        case SIGSYS:    return "SIGSYS (31)";
        default:
            break;
    }

#if (defined SIGRTMIN) and (defined SIGRTMAX)
    if (signal >= SIGRTMIN && signal <= SIGRTMAX)
    {
        if (signal == SIGRTMIN)
            return QString("SIGRTMIN (%1)").arg(signal);
        if (signal == SIGRTMAX)
            return QString("SIGRTMAX (%1)").arg(signal);

        const int minOffset = signal - SIGRTMIN;
        const int maxOffset = SIGRTMAX - signal;
        if (minOffset <= maxOffset)
            return QString("SIGRTMIN+%1 (%2)").arg(minOffset).arg(signal);
        return QString("SIGRTMAX-%1 (%2)").arg(maxOffset).arg(signal);
    }
#endif

    return QString("Signal %1").arg(signal);
}
