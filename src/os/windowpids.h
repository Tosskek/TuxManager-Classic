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

#ifndef OS_WINDOWPIDS_H
#define OS_WINDOWPIDS_H

#include <QHash>
#include <QSet>

#include <sys/types.h>

namespace OS
{
    //! PIDs that own a window shown on the taskbar, read from the window manager's
    //! EWMH client list on X11 (_NET_CLIENT_LIST + _NET_WM_PID). Docks, desktops,
    //! splash screens and skip-taskbar windows don't count. This is how Windows Task
    //! Manager decides "Apps", and it works on every desktop (Plasma, XFCE, ...),
    //! unlike systemd app units, which only some desktops create.
    class WindowPids
    {
        public:
            WindowPids();
            ~WindowPids();

            //! Re-reads the client list; only windows not seen before are queried.
            void Refresh();
            //! False when there is no X11 connection or the window manager doesn't
            //! publish a client list (e.g. Wayland, bare Xvfb).
            bool IsAvailable() const { return this->m_available; }
            const QSet<pid_t> &Pids() const { return this->m_pids; }

        private:
            struct Info
            {
                pid_t pid { 0 };
                bool  onTaskbar { false };
            };

            bool init();

            bool m_initTried { false };
            bool m_available { false };
            void *m_conn { nullptr };      //!< xcb_connection_t* owned by Qt
            quint32 m_root { 0 };
            quint32 m_atomClientList { 0 };
            quint32 m_atomPid { 0 };
            quint32 m_atomType { 0 };
            quint32 m_atomState { 0 };
            quint32 m_atomSkipTaskbar { 0 };
            QSet<quint32> m_notOnTaskbarTypes;
            QHash<quint32, Info> m_windows;
            QSet<pid_t> m_pids;
    };
}

#endif // OS_WINDOWPIDS_H
