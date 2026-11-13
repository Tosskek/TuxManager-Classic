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

#include "selfpriority.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDir>
#include <QProcess>
#include <QThread>

#include <algorithm>
#include <sys/resource.h>
#include <unistd.h>

namespace
{
    constexpr int kNice = -10;

    bool raiseThread(QDBusConnection &bus, qulonglong pid, qulonglong tid)
    {
        QDBusMessage msg = QDBusMessage::createMethodCall("org.freedesktop.RealtimeKit1",
                                                          "/org/freedesktop/RealtimeKit1",
                                                          "org.freedesktop.RealtimeKit1",
                                                          "MakeThreadHighPriorityWithPID");
        msg << pid << tid << kNice;
        const QDBusMessage reply = bus.call(msg, QDBus::Block, 3000);
        return reply.type() == QDBusMessage::ReplyMessage;
    }
}

void OS::BoostOwnPriority()
{
    if (qEnvironmentVariableIntValue("TUX_MANAGER_NO_BOOST") != 0)
        return;
    if (::getpriority(PRIO_PROCESS, 0) <= kNice)
        return; // already there (inherited or run as root with nice)

    // Direct way first: works for root and with a raised RLIMIT_NICE.
    if (::setpriority(PRIO_PROCESS, 0, kNice) == 0)
        return;  // threads started from now on inherit it; the few existing ones matter little

    const qulonglong pid = static_cast<qulonglong>(::getpid());
    QThread *helper = QThread::create([pid]()
    {
        // A private connection: the shared one belongs to the GUI thread, and a system
        // bus that does not answer must only ever stall this helper.
        const QString name = QStringLiteral("tux-manager-rtkit");
        {
            QDBusConnection bus = QDBusConnection::connectToBus(QDBusConnection::SystemBus, name);
            if (bus.isConnected() && raiseThread(bus, pid, pid))   // the GUI thread first
            {
                // Then the worker threads that already exist (RealtimeKit allows only a
                // handful of requests per burst, so keep it short).
                QStringList tids = QDir("/proc/self/task").entryList(QDir::Dirs | QDir::NoDotAndDotDot);
                int done = 0;
                for (const QString &tidText : std::as_const(tids))
                {
                    const qulonglong tid = tidText.toULongLong();
                    if (tid == 0 || tid == pid)
                        continue;
                    if (++done > 6 || !raiseThread(bus, pid, tid))
                        break;
                }
            }
        }
        QDBusConnection::disconnectFromBus(name);
    });
    QObject::connect(helper, &QThread::finished, helper, &QObject::deleteLater);
    helper->start();
}

bool OS::StartDetachedNormalPriority(const QString &program, const QStringList &arguments, const QString &workingDirectory)
{
    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    if (!workingDirectory.isEmpty())
        process.setWorkingDirectory(workingDirectory);
    // Only async-signal-safe calls are allowed here (runs between fork and exec).
    process.setChildProcessModifier([]() { ::setpriority(PRIO_PROCESS, 0, 0); });
    return process.startDetached();
}
