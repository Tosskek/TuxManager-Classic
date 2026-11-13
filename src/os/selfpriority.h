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

#ifndef OS_SELFPRIORITY_H
#define OS_SELFPRIORITY_H

#include <QString>
#include <QStringList>

namespace OS
{
    /*!
     * A task manager is needed most when the machine is overloaded, so it must get
     * CPU time ahead of whatever is eating it. Asks RealtimeKit (the same service
     * audio servers use; no root needed) to raise this process to nice -10, like
     * Windows runs its Task Manager at High priority. Never blocks: the request is
     * made from a helper thread and silently does nothing when unavailable.
     * Disabled with TUX_MANAGER_NO_BOOST=1.
     */
    void BoostOwnPriority();

    //! Starts a program detached at normal priority (not our raised one).
    bool StartDetachedNormalPriority(const QString &program, const QStringList &arguments, const QString &workingDirectory = QString());
}

#endif // OS_SELFPRIORITY_H
