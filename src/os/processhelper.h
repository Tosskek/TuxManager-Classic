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

#ifndef OS_PROCESSHELPER_H
#define OS_PROCESSHELPER_H

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <sys/types.h>

namespace OS
{
    class ProcessHelper
    {
        public:
            /// Send an arbitrary signal number to a process.
            /// Returns true on success, sets errorMsg on failure.
            static bool SendSignal(pid_t pid, int signal, QString &errorMsg);

            /// Change the nice value of a process.
            /// Returns true on success, sets errorMsg on failure.
            static bool Renice(pid_t pid, int nice, QString &errorMsg);

            //! A process to signal, pinned by PID + start time so a reused PID is never hit.
            struct Target
            {
                pid_t   PID { 0 };
                quint64 StartTimeTicks { 0 };   ///< 0 = unknown, the PID alone is trusted
                uid_t   UID { 0 };
                QString Name;
            };

            struct SignalResult
            {
                int         Sent { 0 };      ///< processes that got the signal
                int         Gone { 0 };      ///< already exited (or the PID now belongs to another process)
                int         Swept { 0 };     ///< extra processes found and included by KillGroup
                QStringList Errors;          ///< real failures (permission, ...)
            };

            //! Start time (jiffies since boot) of a live process, 0 when it does not exist.
            static quint64 StartTimeOf(pid_t pid);

            //! Sends the signal to every target that is still the same process.
            static SignalResult SendSignalChecked(const QList<Target> &targets, int signal);

            /*!
             * Kills a whole group so that a program that keeps starting copies of itself
             * (fork bomb) cannot outrun it: everything is stopped first, then /proc is
             * rescanned for children of stopped members and, for the names in
             * \a sweepNames, for same-named processes of the same user that did not exist
             * in \a knownPids (= appeared after the list was read). Repeats until nothing
             * new shows up, then sends SIGKILL to all.
             */
            static SignalResult KillGroup(const QList<Target> &targets, const QSet<QString> &sweepNames, const QSet<pid_t> &knownPids);

            /// Human-readable description of a signal number.
            static QString GetSignalName(int signal);

        private:
            ProcessHelper() = delete;
    };
} // namespace Os

#endif // OS_PROCESSHELPER_H
