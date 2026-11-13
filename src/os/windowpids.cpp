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

#include "windowpids.h"

#include <QGuiApplication>
#include <QVector>

#include <xcb/xcb.h>

#include <cstdlib>
#include <cstring>

using namespace OS;

namespace
{
    xcb_atom_t internAtom(xcb_connection_t *c, const char *name)
    {
        xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c, xcb_intern_atom(c, 1, static_cast<uint16_t>(std::strlen(name)), name), nullptr);
        const xcb_atom_t atom = r ? r->atom : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
        std::free(r);
        return atom;
    }
}

WindowPids::WindowPids() = default;
WindowPids::~WindowPids() = default;

bool WindowPids::init()
{
    this->m_initTried = true;
    auto *x11 = qGuiApp ? qGuiApp->nativeInterface<QNativeInterface::QX11Application>() : nullptr;
    xcb_connection_t *c = x11 ? x11->connection() : nullptr;
    if (!c)
        return false;
    const xcb_setup_t *setup = xcb_get_setup(c);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    if (!it.rem)
        return false;
    this->m_conn = c;
    this->m_root = it.data->root;
    this->m_atomClientList = internAtom(c, "_NET_CLIENT_LIST");
    this->m_atomPid = internAtom(c, "_NET_WM_PID");
    this->m_atomType = internAtom(c, "_NET_WM_WINDOW_TYPE");
    this->m_atomState = internAtom(c, "_NET_WM_STATE");
    this->m_atomSkipTaskbar = internAtom(c, "_NET_WM_STATE_SKIP_TASKBAR");
    for (const char *t : { "_NET_WM_WINDOW_TYPE_DOCK", "_NET_WM_WINDOW_TYPE_DESKTOP", "_NET_WM_WINDOW_TYPE_SPLASH",
                           "_NET_WM_WINDOW_TYPE_TOOLBAR", "_NET_WM_WINDOW_TYPE_MENU", "_NET_WM_WINDOW_TYPE_UTILITY",
                           "_NET_WM_WINDOW_TYPE_NOTIFICATION", "_NET_WM_WINDOW_TYPE_TOOLTIP",
                           "_NET_WM_WINDOW_TYPE_POPUP_MENU", "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU" })
    {
        const xcb_atom_t a = internAtom(c, t);
        if (a != XCB_ATOM_NONE)
            this->m_notOnTaskbarTypes.insert(a);
    }
    return this->m_atomClientList != XCB_ATOM_NONE && this->m_atomPid != XCB_ATOM_NONE;
}

void WindowPids::Refresh()
{
    if (!this->m_initTried && !this->init())
        this->m_conn = nullptr;
    auto *c = static_cast<xcb_connection_t *>(this->m_conn);
    if (!c)
    {
        this->m_available = false;
        return;
    }

    xcb_get_property_reply_t *list = xcb_get_property_reply(
        c, xcb_get_property(c, 0, this->m_root, this->m_atomClientList, XCB_ATOM_WINDOW, 0, 1 << 16), nullptr);
    if (!list || list->type == XCB_ATOM_NONE)
    {
        // No EWMH window manager (or it is gone): nothing to go by.
        std::free(list);
        this->m_available = false;
        this->m_windows.clear();
        this->m_pids.clear();
        return;
    }
    const int count = xcb_get_property_value_length(list) / 4;
    const xcb_window_t *wins = static_cast<const xcb_window_t *>(xcb_get_property_value(list));

    // Ask about new windows only, all requests first, then the replies (one round trip).
    struct Pending
    {
        xcb_window_t win;
        xcb_get_property_cookie_t pid, type, state;
    };
    QVector<Pending> pending;
    QSet<quint32> current;
    current.reserve(count);
    for (int i = 0; i < count; ++i)
    {
        current.insert(wins[i]);
        if (this->m_windows.contains(wins[i]))
            continue;
        pending.append({ wins[i],
                         xcb_get_property(c, 0, wins[i], this->m_atomPid, XCB_ATOM_CARDINAL, 0, 1),
                         xcb_get_property(c, 0, wins[i], this->m_atomType, XCB_ATOM_ATOM, 0, 16),
                         xcb_get_property(c, 0, wins[i], this->m_atomState, XCB_ATOM_ATOM, 0, 32) });
    }
    std::free(list);

    for (const Pending &p : std::as_const(pending))
    {
        Info info;
        if (xcb_get_property_reply_t *r = xcb_get_property_reply(c, p.pid, nullptr))
        {
            if (xcb_get_property_value_length(r) >= 4)
                info.pid = static_cast<pid_t>(*static_cast<const uint32_t *>(xcb_get_property_value(r)));
            std::free(r);
        }
        info.onTaskbar = true;
        if (xcb_get_property_reply_t *r = xcb_get_property_reply(c, p.type, nullptr))
        {
            const int n = xcb_get_property_value_length(r) / 4;
            const xcb_atom_t *types = static_cast<const xcb_atom_t *>(xcb_get_property_value(r));
            // The first type is the one the window manager uses.
            if (n > 0 && this->m_notOnTaskbarTypes.contains(types[0]))
                info.onTaskbar = false;
            std::free(r);
        }
        if (xcb_get_property_reply_t *r = xcb_get_property_reply(c, p.state, nullptr))
        {
            const int n = xcb_get_property_value_length(r) / 4;
            const xcb_atom_t *states = static_cast<const xcb_atom_t *>(xcb_get_property_value(r));
            for (int i = 0; i < n; ++i)
            {
                if (states[i] == this->m_atomSkipTaskbar)
                    info.onTaskbar = false;
            }
            std::free(r);
        }
        this->m_windows.insert(p.win, info);
    }

    // Forget closed windows, rebuild the PID set.
    for (auto it = this->m_windows.begin(); it != this->m_windows.end();)
    {
        if (current.contains(it.key()))
            ++it;
        else
            it = this->m_windows.erase(it);
    }
    this->m_pids.clear();
    for (const Info &info : std::as_const(this->m_windows))
    {
        if (info.onTaskbar && info.pid > 0)
            this->m_pids.insert(info.pid);
    }
    this->m_available = true;
}
