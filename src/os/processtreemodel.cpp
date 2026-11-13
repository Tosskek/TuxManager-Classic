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

#include "../ui/heatcolor.h"
#include "processtreemodel.h"

#include <QFont>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

#include <unistd.h>

#include "appregistry.h"
#include "../misc.h"
using namespace OS;

namespace
{
    //! Friendlier name for a process row:
    //!  - the kernel cuts names to 15 characters ("xdg-desktop-por"); use the full
    //!    executable name from the command line when it starts with the short name;
    //!  - for script interpreters show the script ("python3 win7startmenu.py" -> "win7startmenu.py").
    const QStringList &interpreterNames()
    {
        static const QStringList names = { "python", "python3", "bash", "sh", "dash", "zsh", "perl", "ruby", "node", "env" };
        return names;
    }

    //! "python3.12" -> "python"
    QString withoutVersion(const QString &name)
    {
        qsizetype end = name.size();
        while (end > 0 && (name.at(end - 1).isDigit() || name.at(end - 1) == '.'))
            --end;
        return name.left(end);
    }

    QString displayName(const Process &proc)
    {
        if (proc.IsKernelThread || proc.CmdLine.isEmpty())
            return proc.Name;
        // Called for every paint, sort comparison and total: most processes need no
        // look at the command line at all, so decide that before splitting it.
        if (proc.Name.size() < 15 && !interpreterNames().contains(proc.Name) && !interpreterNames().contains(withoutVersion(proc.Name)))
            return proc.Name;
        const QStringList args = proc.CmdLine.split(' ', Qt::SkipEmptyParts);
        if (args.isEmpty())
            return proc.Name;
        QString name = proc.Name;
        if (name.size() >= 15)
        {
            // The full name is the executable, or the script for "python3 /usr/bin/networkd-dispatcher".
            for (int i = 0; i < qMin<qsizetype>(2, args.size()); ++i)
            {
                const QString exe = args.at(i).section('/', -1);
                if (exe.startsWith(name))
                {
                    name = exe;
                    break;
                }
            }
        }

        const QStringList &interpreters = interpreterNames();
        if (interpreters.contains(name) || interpreters.contains(withoutVersion(name)))
        {
            for (int i = 1; i < args.size(); ++i)
            {
                const QString &arg = args.at(i);
                if (arg.startsWith('-'))
                {
                    if (arg == QLatin1String("-c") || arg == QLatin1String("-m"))
                        break;   // inline code / module: keep the interpreter name
                    continue;
                }
                QString script = arg.section('/', -1);
                // Generic entry points say nothing: "…/win10explorer/run.py" -> "win10explorer".
                static const QStringList generic = { "run.py", "main.py", "app.py", "__main__.py", "start.sh", "run.sh" };
                if (generic.contains(script) && arg.count('/') >= 2)
                    script = arg.section('/', -2, -2);
                if (!script.isEmpty() && (arg.contains('/') || script.contains('.')))
                    return script;
                break;
            }
        }
        return name;
    }
}


ProcessTreeModel::ProcessTreeModel(QObject *parent) : QAbstractItemModel(parent), m_root(new Node())
{
    for (int s = 0; s < SectionCount; ++s)
    {
        Node *section = new Node();
        section->kind = Kind::Section;
        section->parent = this->m_root;
        section->row = s;
        this->m_root->children.append(section);
        this->m_sections[s] = section;
    }
}

ProcessTreeModel::~ProcessTreeModel()
{
    freeNode(this->m_root);
}

QModelIndex ProcessTreeModel::index(int row, int column, const QModelIndex &parent) const
{
    if (row < 0 || column < 0 || column >= ColCount)
        return {};
    if (parent.isValid() && parent.column() != 0)
        return {};   // only column 0 has children (Qt item-model convention)

    Node *parentNode = nodeFromIndex(parent);
    if (!parentNode)
        parentNode = this->m_root;
    if (row >= parentNode->children.size())
        return {};

    return createIndex(row, column, parentNode->children.at(row));
}

QModelIndex ProcessTreeModel::parent(const QModelIndex &child) const
{
    if (!child.isValid())
        return {};

    Node *node = nodeFromIndex(child);
    if (!node || !node->parent || node->parent == this->m_root)
        return {};

    Node *parentNode = node->parent;
    return createIndex(parentNode->row, 0, parentNode);
}

int ProcessTreeModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid() && parent.column() != 0)
        return 0;
    Node *parentNode = nodeFromIndex(parent);
    if (!parentNode)
        parentNode = this->m_root;
    return parentNode->children.size();
}

int ProcessTreeModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return ColCount;
}

QVariant ProcessTreeModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.column() < 0 || index.column() >= ColCount)
        return {};

    Node *node = nodeFromIndex(index);
    if (!node)
        return {};
    const Process &proc = node->process;
    const Column column = static_cast<Column>(index.column());

    // ── Section header rows ("Apps (12)") ───────────────────────────────────
    if (node->kind == Kind::Section)
    {
        if (role == Qt::DisplayRole && column == ColName)
        {
            const QString title = node == this->m_sections[SectionApps] ? tr("Apps") : tr("Background & system");
            return QString("%1 (%2)").arg(title).arg(node->memberCount);
        }
        if (role == Qt::FontRole && column == ColName)
        {
            QFont font = this->m_baseFont;   // same size as the list, just bold
            font.setBold(true);
            return font;
        }
        if (role == Qt::UserRole && column == ColName)
            return node->row;   // not sortable; ProcessTreeProxy keeps the order
        if (role == Qt::DisplayRole || role == Qt::UserRole)
        {
            // Totals for CPU / memory / disk, nothing for per-process fields.
            switch (column)
            {
                case ColCpu: case ColMemRss: case ColMemVirt: case ColMemShared: case ColMemText: case ColMemData:
                case ColIoReads: case ColIoWrites: case ColIoReadsPerSec: case ColIoWritesPerSec: case ColThreads: case ColDisk:
                    break;
                default:
                    return {};
            }
        } else if (role != Qt::TextAlignmentRole)
        {
            return {};
        }
    }

    // ── App rows ("Claude (40)") and name groups ("claude (13)"): totals ─────
    if (node->kind == Kind::Group || node->kind == Kind::App)
    {
        switch (column)
        {
            case ColName:
                if (role == Qt::DisplayRole)
                {
                    const QString name = node->kind == Kind::App ? proc.Name : node->key;
                    const int members = node->kind == Kind::App ? node->memberCount : static_cast<int>(node->children.size());
                    return QString("%1 (%2)").arg(name).arg(members);
                }
                if (role == Qt::UserRole)
                    return node->kind == Kind::App ? proc.Name : node->key;
                break;
            case ColPid:
            case ColState:
            case ColPriority:
            case ColNice:
                if (role == Qt::DisplayRole)
                    return {};
                if (role == Qt::UserRole && column == ColPid)
                {
                    const QList<pid_t> pids = this->PidsForIndex(index);
                    return pids.isEmpty() ? 0LL : static_cast<qlonglong>(*std::min_element(pids.cbegin(), pids.cend()));
                }
                break;
            default:
                break;
        }
    }

    if (role == Qt::DecorationRole)
    {
        if (index.column() == ColName && this->m_appRegistry)
            return this->m_appRegistry->IconFor(proc.IconName);
        return {};
    }

    if (role == Qt::DisplayRole)
    {
        switch (static_cast<Column>(index.column()))
        {
            case ColPid:      return proc.PID;
            case ColName:     return displayName(proc);
            case ColUser:     return proc.User;
            case ColState:
                // Like Windows' Status column: blank while normal, only unusual states.
                switch (proc.State)
                {
                    case 'T': case 't': return tr("Suspended");
                    case 'Z':           return tr("Zombie");
                    case 'D':           return tr("Waiting for disk");
                    default:            return QString();
                }
            case ColCpu:      return proc.CPUPercent < 0.05 ? QString("0%") : QString::number(proc.CPUPercent, 'f', 1) + "%";
            case ColMemRss:   return Misc::FormatKiB(proc.VMRssKb, 0);
            case ColMemVirt:  return Misc::FormatKiB(proc.vmSizeKb, 0);
            case ColMemShared:return Misc::FormatKiB(proc.SharedKb, 0);
            case ColMemText:  return Misc::FormatKiB(proc.TextKb, 0);
            case ColMemData:  return Misc::FormatKiB(proc.DataKb, 0);
            case ColIoReads:  return proc.IOTotalsAvailable ? Misc::FormatBytes(proc.IOReadBytes, 0) : QString("?");
            case ColIoWrites: return proc.IOTotalsAvailable ? Misc::FormatBytes(proc.IOWriteBytes, 0) : QString("?");
            case ColIoReadsPerSec:
                if (proc.IOPermissionDenied)
                    return QString("?");
                return proc.IORatesAvailable ? Misc::FormatBytesPerSecond(proc.IOReadBps) : tr("measuring...");
            case ColIoWritesPerSec:
                if (proc.IOPermissionDenied)
                    return QString("?");
                return proc.IORatesAvailable ? Misc::FormatBytesPerSecond(proc.IOWriteBps) : tr("measuring...");
            case ColThreads:  return proc.Threads;
            case ColPriority: return proc.Priority;
            case ColNice:     return proc.Nice;
            case ColCmdline:  return proc.CmdLine;
            case ColDisk:
            {
                if (proc.IOPermissionDenied || !proc.IORatesAvailable)
                    return QString("0 MB/s");
                const double mb = (proc.IOReadBps + proc.IOWriteBps) / (1024.0 * 1024.0);
                return mb < 0.05 ? QString("0 MB/s") : QString::number(mb, 'f', 1) + " MB/s";
            }
            default: break;
        }
    }

    if (role == Qt::UserRole)
    {
        switch (static_cast<Column>(index.column()))
        {
            case ColPid:      return static_cast<qlonglong>(proc.PID);
            case ColCpu:      return proc.CPUPercent;
            case ColMemRss:   return static_cast<qulonglong>(proc.VMRssKb);
            case ColMemVirt:  return static_cast<qulonglong>(proc.vmSizeKb);
            case ColMemShared:return static_cast<qulonglong>(proc.SharedKb);
            case ColMemText:  return static_cast<qulonglong>(proc.TextKb);
            case ColMemData:  return static_cast<qulonglong>(proc.DataKb);
            case ColIoReads:
                return proc.IOTotalsAvailable
                       ? QVariant::fromValue(static_cast<qlonglong>(proc.IOReadBytes))
                       : QVariant::fromValue(static_cast<qlonglong>(-1));
            case ColIoWrites:
                return proc.IOTotalsAvailable
                       ? QVariant::fromValue(static_cast<qlonglong>(proc.IOWriteBytes))
                       : QVariant::fromValue(static_cast<qlonglong>(-1));
            case ColIoReadsPerSec:
                return proc.IORatesAvailable
                       ? QVariant::fromValue(proc.IOReadBps)
                       : QVariant::fromValue(-1.0);
            case ColIoWritesPerSec:
                return proc.IORatesAvailable
                       ? QVariant::fromValue(proc.IOWriteBps)
                       : QVariant::fromValue(-1.0);
            case ColDisk:
                return proc.IORatesAvailable ? proc.IOReadBps + proc.IOWriteBps : 0.0;
            case ColThreads:  return proc.Threads;
            case ColPriority: return proc.Priority;
            case ColNice:     return proc.Nice;
            default:          return data(index, Qt::DisplayRole);
        }
    }

    if (role == Qt::BackgroundRole)
    {
        static constexpr double cpuCapacity = 100.0;   // CPU % is already a share of the whole CPU
        switch (static_cast<Column>(index.column()))
        {
            case ColCpu:
                return HeatColor::ForFraction(proc.CPUPercent / cpuCapacity);
            case ColMemRss:
                return HeatColor::ForFraction(static_cast<double>(proc.VMRssKb) / HeatColor::TotalRamKb());
            case ColIoReadsPerSec:
                return HeatColor::ForFraction(proc.IORatesAvailable ? HeatColor::IoFraction(proc.IOReadBps) : 0.0);
            case ColIoWritesPerSec:
                return HeatColor::ForFraction(proc.IORatesAvailable ? HeatColor::IoFraction(proc.IOWriteBps) : 0.0);
            case ColDisk:
                return HeatColor::ForFraction(proc.IORatesAvailable ? HeatColor::IoFraction(proc.IOReadBps + proc.IOWriteBps) : 0.0);
            default:
                return {};
        }
    }

    if (role == Qt::TextAlignmentRole)
    {
        switch (static_cast<Column>(index.column()))
        {
            case ColCpu:
            case ColMemRss:
            case ColMemVirt:
            case ColMemShared:
            case ColMemText:
            case ColMemData:
            case ColIoReads:
            case ColIoWrites:
            case ColIoReadsPerSec:
            case ColIoWritesPerSec:
            case ColThreads:
            case ColPriority:
            case ColNice:
            case ColDisk:
                return QVariant(Qt::AlignRight | Qt::AlignVCenter);
            default:
                return QVariant(Qt::AlignLeft | Qt::AlignVCenter);
        }
    }

    return {};
}

QVariant ProcessTreeModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    if (section < 0 || section >= ColCount)
        return {};
    return columnHeader(static_cast<Column>(section));
}

Qt::ItemFlags ProcessTreeModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    if (IsUnselectableIndex(index))
        return Qt::ItemIsEnabled;   // "Linux kernel": can be expanded but not selected / ended
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

namespace
{
    //! A process the user started: their own, launched as a desktop app or from a
    //! terminal/launcher, which systemd places in an app-*.scope / app-*@*.service unit
    //! under app.slice. Desktop services (session.slice, background.slice), autostart
    //! helpers (app-*@autostart.service), other users and kernel threads are "system".
    bool isUserApp(const Process &proc, uid_t myUid)
    {
        if (proc.IsKernelThread || proc.UID != myUid)
            return false;
        const int at = proc.CGroup.indexOf(QLatin1String("/app.slice/app-"));
        if (at < 0)
            return false;
        const int unitStart = at + static_cast<int>(sizeof("/app.slice/") - 1);
        const int slash = proc.CGroup.indexOf('/', unitStart);
        const QStringView unit = QStringView(proc.CGroup).mid(unitStart, slash < 0 ? -1 : slash - unitStart);
        return !unit.endsWith(QLatin1String("@autostart.service"));
    }

    //! Name rows are grouped by. Kernel threads carry per-CPU / per-device suffixes
    //! ("kworker/3:1-events", "irq/45-nvidia"), so they group by the part before '/'.
    QString groupName(const Process &proc)
    {
        if (proc.IsKernelThread)
        {
            const int slash = proc.Name.indexOf('/');
            if (slash > 0)
                return proc.Name.left(slash);
        }
        return proc.Name;
    }
}

bool ProcessTreeModel::SetProcesses(const QList<Process> &processes)
{
    const uid_t myUid = ::getuid();
    const int count = static_cast<int>(processes.size());

    // ── Which program row every process belongs to ───────────────────────────
    // Like Windows Task Manager: one row per program, its processes inside it.
    //  - Plasma / GNOME put every launched app in a systemd app unit: one row per app,
    //    named after its desktop entry ("A:<app>").
    //  - Elsewhere (XFCE, or programs started by the Win7 start menu / Explorer clone)
    //    processes stay in the session or the launcher's unit, so the program is found by
    //    walking up to whatever launched it ("R:<root pid>:<start>"); named after the
    //    first member with a desktop entry.
    //  - Apps = app units plus programs that own a window on the taskbar (Windows'
    //    rule; read from the window manager), everything else is Background & system.
    //  - The session's own pieces (Xorg, xfwm4, panel, desktop...) form "Desktop session";
    //    all kernel threads form "Linux kernel".
    // Inside a program row, same-name processes fold into one "name (N)" row.
    QVector<QString> containerOf(count);   // "A:<app>", "K", "D:<session>", "R:<pid>:<start>"
    QVector<QString> nameOf(count);
    QHash<QString, QString> titles;        // container -> fixed title (apps, kernel, session)
    QHash<QString, QString> icons;
    QHash<QString, int> membersOf;         // container -> process count
    QHash<QString, int> membersByGroup;    // "<container>\n<name>" -> count
    QHash<QString, bool> appContainer;     // container -> belongs in the Apps section
    QHash<QString, QVector<int>> rootMembers; // "R:" container -> member indexes
    QHash<QString, int> rootOfKey;         // "R:" container -> index of its root process
    QHash<pid_t, int> incomingByPid;
    incomingByPid.reserve(count);
    for (int i = 0; i < count; ++i)
        incomingByPid.insert(processes.at(i).PID, i);

    static const QRegularExpression sessionScope(QStringLiteral("/session-[^/]+\\.scope$"));
    // The Win10 taskbar (~/Claude/win10taskbar/taskbar.py): part of the session, and it
    // starts programs (taskbar buttons, Win+1..9), so their children are programs of their own.
    auto isWin10Taskbar = [](const Process &p)
    {
        return p.CmdLine.contains(QLatin1String("/win10taskbar/taskbar.py"));
    };
    // Processes that start programs: their children are programs of their own.
    auto isLauncher = [&isWin10Taskbar](const Process &p)
    {
        static const QSet<QString> names = {
            "xfce4-session", "xfce4-panel", "wrapper-2.0", "xfdesktop", "xfsettingsd", "xfce4-appfinder",
            "Thunar", "thunar", "sddm-helper", "sddm-helper-sta", "lightdm", "startplasma-x11",
            "startplasma-wayl", "ksmserver", "systemd", "mate-session", "mate-panel", "lxsession", "lxpanel",
            "openbox", "i3", "sway", "win10taskbar" };
        if (names.contains(p.Name))
            return true;
        // The user's own launchers (python): Win7 start menu, Explorer clone, Win10 taskbar.
        return p.Name.startsWith(QLatin1String("python"))
               && (p.CmdLine.contains(QLatin1String("win7startmenu.py")) || p.CmdLine.contains(QLatin1String("/win10explorer/"))
                   || isWin10Taskbar(p));
    };
    // Parts of the desktop session itself (grouped as "Desktop session").
    auto isSessionPart = [&isWin10Taskbar](const Process &p)
    {
        static const QSet<QString> names = {
            "xfce4-session", "xfwm4", "xfsettingsd", "xfce4-panel", "wrapper-2.0", "xfdesktop", "xfconfd",
            "Xorg", "X", "sddm-helper", "sddm-helper-sta", "startplasma-x11", "startplasma-wayl", "ssh-agent",
            "gpg-agent", "im-launch", "at-spi-bus-laun", "at-spi2-registr", "xfce4-notifyd", "xfce4-power-man",
            "xfce4-screensav", "light-locker", "xiccd", "xfce-polkit", "polkit-gnome-au", "dbus-daemon",
            "dbus-launch", "xscreensaver", "kwin_x11", "plasmashell", "ksmserver",
            // Win10 taskbar (replaced xfce4-panel under Xfce): launcher script + its tray proxy.
            "win10taskbar", "xembedsniproxy" };
        return names.contains(p.Name) || (p.Name.startsWith(QLatin1String("python")) && isWin10Taskbar(p));
    };

    // Root of a process: walk up while the parent is in the same unit and is not a launcher.
    QVector<int> rootIdx(count, -1);
    auto rootOf = [&](int i)
    {
        QVector<int> path;
        int j = i;
        while (rootIdx[j] < 0)
        {
            path.append(j);
            const Process &pj = processes.at(j);
            const int k = (pj.PPID > 1 && pj.PPID != pj.PID) ? incomingByPid.value(pj.PPID, -1) : -1;
            if (k < 0 || path.size() > 64)
                break;
            const Process &pk = processes.at(k);
            if (pk.IsKernelThread || pk.CGroup != pj.CGroup || isLauncher(pk) || isSessionPart(pk))
                break;
            j = k;
        }
        const int root = rootIdx[j] >= 0 ? rootIdx[j] : j;
        for (int p : std::as_const(path))
            rootIdx[p] = root;
        return root;
    };

    // Container of a process in a systemd app unit: one row per app, named after its desktop entry.
    auto appKey = [&](const Process &proc) -> QString
    {
        auto info = this->m_appInfoCache.constFind(proc.CGroup);
        if (info == this->m_appInfoCache.cend())
        {
            QString name, icon;
            if (!this->m_appRegistry || !this->m_appRegistry->AppInfoForCGroup(proc.CGroup, &name, &icon) || name.isEmpty())
            {
                // No desktop entry: name the app after its unit ("app-<id>-1234.scope").
                name = AppRegistry::AppIdFromCGroup(proc.CGroup);
                if (name.isEmpty())
                    name = proc.Name;
                name = name.section('.', -1);
            }
            // Launcher names often carry notes: "Claude (focus/open)" -> "Claude".
            static const QRegularExpression trailingNote(QStringLiteral("\\s*\\([^)]*\\)\\s*$"));
            name.remove(trailingNote);
            info = this->m_appInfoCache.insert(proc.CGroup, { name, icon });
        }
        const QString key = QStringLiteral("A:") + info->first;
        titles.insert(key, info->first);
        appContainer.insert(key, true);
        if (!info->second.isEmpty() && !icons.contains(key))
            icons.insert(key, info->second);
        return key;
    };

    for (int i = 0; i < count; ++i)
    {
        const Process &proc = processes.at(i);
        nameOf[i] = groupName(proc);
        if (proc.IsKernelThread)
        {
            containerOf[i] = QStringLiteral("K");
            titles.insert(containerOf[i], tr("Linux kernel"));
        } else if (isUserApp(proc, myUid))
        {
            containerOf[i] = appKey(proc);
        } else if (sessionScope.match(proc.CGroup).hasMatch() && isSessionPart(proc))
        {
            containerOf[i] = QStringLiteral("D:") + proc.CGroup;
            titles.insert(containerOf[i], tr("Desktop session"));
        } else
        {
            const int root = rootOf(i);
            const Process &r = processes.at(root);
            // Chrome and Electron apps move their main process into an app unit after
            // starting; helpers forked before that stay behind. They belong to that app.
            const int parent = (r.PPID > 1 && r.PPID != r.PID) ? incomingByPid.value(r.PPID, -1) : -1;
            if (parent >= 0 && isUserApp(processes.at(parent), myUid))
            {
                containerOf[i] = appKey(processes.at(parent));
            } else
            {
                containerOf[i] = QStringLiteral("R:%1:%2").arg(r.PID).arg(r.StartTimeTicks);
                rootMembers[containerOf[i]].append(i);
                rootOfKey.insert(containerOf[i], root);
            }
        }
        ++membersOf[containerOf[i]];
        ++membersByGroup[containerOf[i] + '\n' + nameOf[i]];
    }

    // Programs found by the walk: Apps when one of their processes has a taskbar window.
    // Without window information (no EWMH window manager), programs started directly by
    // a launcher count as apps.
    for (auto it = rootMembers.cbegin(); it != rootMembers.cend(); ++it)
    {
        bool isApp = false;
        if (this->m_windowsKnown)
        {
            for (int i : it.value())
            {
                if (this->m_windowPids.contains(processes.at(i).PID))
                {
                    isApp = true;
                    break;
                }
            }
        } else
        {
            const Process &root = processes.at(rootOfKey.value(it.key()));
            const int parent = incomingByPid.value(root.PPID, -1);
            isApp = parent >= 0 && isLauncher(processes.at(parent)) && !isSessionPart(root)
                    && root.UID == myUid && !processes.at(parent).Name.startsWith(QLatin1String("systemd"))
                    && processes.at(parent).Name != QLatin1String("xfce4-session");
        }
        // Without window information a launched program still needs a desktop entry
        // (decided below) to count as an app; helpers like thumbnail loaders have none.
        const bool needsEntry = isApp && !this->m_windowsKnown;
        if (isApp && !needsEntry)
            appContainer.insert(it.key(), true);
        else if (!isApp && it.value().size() < 2)
            continue;   // a single background process is shown as itself

        // Title: desktop entry of the earliest member that has one, else the root's name.
        QVector<int> members = it.value();
        std::sort(members.begin(), members.end(), [&](int a, int b) { return processes.at(a).StartTimeTicks < processes.at(b).StartTimeTicks; });
        QString title, icon;
        for (int i : std::as_const(members))
        {
            const Process &p = processes.at(i);
            const QString cacheKey = QStringLiteral("p:%1:%2").arg(p.PID).arg(p.StartTimeTicks);
            auto info = this->m_appInfoCache.constFind(cacheKey);
            if (info == this->m_appInfoCache.cend())
            {
                QString name, ic;
                if (!this->m_appRegistry || !this->m_appRegistry->AppInfoForProcess(p, &name, &ic))
                    name.clear();
                static const QRegularExpression trailingNote(QStringLiteral("\\s*\\([^)]*\\)\\s*$"));
                name.remove(trailingNote);
                info = this->m_appInfoCache.insert(cacheKey, { name, ic });
            }
            if (!info->first.isEmpty())
            {
                title = info->first;
                icon = info->second;
                break;
            }
        }
        if (needsEntry && !title.isEmpty())
            appContainer.insert(it.key(), true);
        else if (needsEntry)
            isApp = false;
        if (title.isEmpty() && isApp)
            title = displayName(processes.at(rootOfKey.value(it.key())));
        if (!title.isEmpty())
            titles.insert(it.key(), title);
        if (!icon.isEmpty())
            icons.insert(it.key(), icon);
    }
    if (this->m_appInfoCache.size() > qMax<qsizetype>(4096, 3 * count))
    {
        // Forget only processes that are gone. (Clearing everything made a machine
        // with more than 4096 processes redo every lookup on every refresh.)
        QSet<QString> live;
        live.reserve(count);
        for (const Process &p : processes)
        {
            live.insert(QStringLiteral("p:%1:%2").arg(p.PID).arg(p.StartTimeTicks));
            live.insert(p.CGroup);
        }
        for (auto it = this->m_appInfoCache.begin(); it != this->m_appInfoCache.end();)
            it = live.contains(it.key()) ? std::next(it) : this->m_appInfoCache.erase(it);
    }

    // App, kernel and session rows always exist; other programs only with 2+ processes.
    auto containerNeeded = [&](const QString &key)
    {
        return titles.contains(key) || membersOf.value(key) >= 2;
    };
    auto targetGroup = [&](int i) -> QString
    {
        return membersByGroup.value(containerOf[i] + '\n' + nameOf[i]) >= 2 ? nameOf[i] : QString();
    };
    auto sectionFor = [&](const QString &key) { return this->m_sections[appContainer.value(key, false) ? SectionApps : SectionSystem]; };
    auto targetParent = [&](int i) -> Node *   // program row or section; nullptr = program row not created yet
    {
        return containerNeeded(containerOf[i]) ? this->m_containers.value(containerOf[i], nullptr) : sectionFor(containerOf[i]);
    };

    // ── 0. Program rows that changed section (a window opened / all closed) move
    for (Node *container : QList<Node *>(this->m_containers.values()))
    {
        if (membersOf.contains(container->key) && container->parent != sectionFor(container->key))
            this->removeNode(container);   // its processes are re-added below
    }

    // ── 1. Remove processes that exited or whose place changed ───────────────
    QList<Node *> stale;
    for (auto it = this->m_byPid.cbegin(); it != this->m_byPid.cend(); ++it)
    {
        Node *node = it.value();
        const int i = incomingByPid.value(node->process.PID, -1);
        bool keep = false;
        if (i >= 0 && processes.at(i).StartTimeTicks == node->process.StartTimeTicks)
        {
            Node *container = targetParent(i);
            const QString group = (container && container->kind == Kind::App) ? targetGroup(i) : QString();
            keep = container && (group.isEmpty() ? node->parent == container
                                                 : (node->parent->kind == Kind::Group && node->parent->parent == container && node->parent->key == group));
        }
        if (!keep)
            stale.append(node);
    }
    // Removed in runs of neighbouring rows: when a big group dies at once (a fork bomb
    // being killed) that is a handful of signals instead of one per process, each of
    // which costs the sort proxy and the view work proportional to the group size.
    std::sort(stale.begin(), stale.end(), [](const Node *l, const Node *r)
    {
        return l->parent != r->parent ? std::less<const Node *>()(l->parent, r->parent) : l->row > r->row;
    });
    for (int i = 0; i < stale.size();)
    {
        Node *parent = stale.at(i)->parent;
        const int last = stale.at(i)->row;
        int first = last;
        int j = i + 1;
        while (j < stale.size() && stale.at(j)->parent == parent && stale.at(j)->row == first - 1)
        {
            first = stale.at(j)->row;
            ++j;
        }
        this->removeLeafRange(parent, first, last);
        i = j;
    }

    // ── 2. Drop name groups and program rows that became empty or unneeded ───
    for (Node *container : QList<Node *>(this->m_containers.values()))
    {
        for (Node *child : QList<Node *>(container->children))
        {
            if (child->kind == Kind::Group && (child->children.isEmpty() || membersByGroup.value(container->key + '\n' + child->key) < 2))
                this->removeNode(child);
        }
        if (container->children.isEmpty() || !containerNeeded(container->key))
            this->removeNode(container);
    }

    // ── 3. Insert new processes (creating program / group rows when needed), update the rest
    QHash<Node *, QList<Node *>> pendingLeaves;
    for (int i = 0; i < count; ++i)
    {
        const Process &proc = processes.at(i);
        Node *node = this->m_byPid.value(proc.PID, nullptr);
        if (node && node->process.StartTimeTicks == proc.StartTimeTicks)
        {
            if (!node->process.HasSameData(proc))
            {
                node->process = proc;
                emit dataChanged(createIndex(node->row, 0, node), createIndex(node->row, ColCount - 1, node));
            }
            continue;
        }

        Node *parent = targetParent(i);
        if (!parent)
        {
            parent = new Node();
            parent->kind = Kind::App;
            parent->key = containerOf[i];
            parent->title = titles.value(containerOf[i]);
            parent->icon = icons.value(containerOf[i]);
            this->appendNode(sectionFor(containerOf[i]), parent);
            this->m_containers.insert(containerOf[i], parent);
        }
        const QString group = parent->kind == Kind::App ? targetGroup(i) : QString();
        if (!group.isEmpty())
        {
            Node *groupNode = parent->groups.value(group, nullptr);
            if (!groupNode)
            {
                groupNode = new Node();
                groupNode->kind = Kind::Group;
                groupNode->key = group;
                groupNode->process.Name = group;
                this->appendNode(parent, groupNode);
                parent->groups.insert(group, groupNode);
            }
            parent = groupNode;
        }

        Node *fresh = new Node();
        fresh->process = proc;
        pendingLeaves[parent].append(fresh);
    }
    // New processes are added per parent in one go (same reason as the removals).
    for (auto it = pendingLeaves.cbegin(); it != pendingLeaves.cend(); ++it)
    {
        Node *parent = it.key();
        const QList<Node *> &fresh = it.value();
        const int first = static_cast<int>(parent->children.size());
        beginInsertRows(indexOfNode(parent), first, first + static_cast<int>(fresh.size()) - 1);
        for (Node *node : fresh)
        {
            node->parent = parent;
            node->row = static_cast<int>(parent->children.size());
            parent->children.append(node);
            this->m_byPid.insert(node->process.PID, node);
        }
        endInsertRows();
    }

    // ── 4. Totals: name groups first, then program rows ──────────────────────
    auto refreshTotals = [this](Node *node)
    {
        int members = 0;
        for (const Node *child : std::as_const(node->children))
            members += child->kind == Kind::Process ? 1 : child->memberCount;
        const Process sum = totals(node);
        if (members == node->memberCount && node->process.HasSameData(sum))
            return;
        node->memberCount = members;
        node->process = sum;
        emit dataChanged(createIndex(node->row, 0, node), createIndex(node->row, ColCount - 1, node));
    };
    for (Node *container : std::as_const(this->m_containers))
    {
        // Titles of found programs can change (e.g. Steam's main binary starts after its script).
        container->title = titles.value(container->key);
        if (icons.contains(container->key))
            container->icon = icons.value(container->key);
        for (Node *child : std::as_const(container->children))
        {
            if (child->kind == Kind::Group)
                refreshTotals(child);
        }
        refreshTotals(container);
    }
    for (Node *section : this->m_sections)
        refreshTotals(section);
    return false;
}

ProcessTreeModel::Node *ProcessTreeModel::appendNode(Node *parent, Node *node)
{
    node->parent = parent;
    node->row = static_cast<int>(parent->children.size());
    beginInsertRows(indexOfNode(parent), node->row, node->row);
    parent->children.append(node);
    endInsertRows();
    return node;
}

void ProcessTreeModel::removeLeafRange(Node *parent, int first, int last)
{
    beginRemoveRows(indexOfNode(parent), first, last);
    const QList<Node *> removed = parent->children.mid(first, last - first + 1);
    parent->children.remove(first, last - first + 1);
    for (int i = first; i < parent->children.size(); ++i)
        parent->children[i]->row = i;
    for (Node *node : removed)
        eraseSubtreeFromIndex(node);
    endRemoveRows();
    for (Node *node : removed)
        freeNode(node);   // only after Qt has dropped its references to the rows
}

void ProcessTreeModel::removeNode(Node *node)
{
    Node *parent = node->parent;
    const int row = node->row;
    beginRemoveRows(indexOfNode(parent), row, row);
    parent->children.removeAt(row);
    for (int i = row; i < parent->children.size(); ++i)
        parent->children[i]->row = i;
    eraseSubtreeFromIndex(node);
    if (node->kind == Kind::Group)
    {
        const auto it = parent->groups.constFind(node->key);
        if (it != parent->groups.cend() && it.value() == node)
            parent->groups.erase(it);
    } else if (node->kind == Kind::App)
    {
        const auto it = this->m_containers.constFind(node->key);
        if (it != this->m_containers.cend() && it.value() == node)
            this->m_containers.erase(it);
    }
    endRemoveRows();
    freeNode(node);   // only after Qt has dropped its references to the rows
}

Process ProcessTreeModel::totals(const Node *node)
{
    Process sum;
    sum.PID = 0;
    sum.State = ' ';
    sum.Threads = 0;
    sum.Name = node->key;
    sum.IOPermissionDenied = !node->children.isEmpty();
    bool first = true;
    QString mainName;
    for (const Node *child : std::as_const(node->children))
    {
        const Process &p = child->process;
        // Command and icon come from the member that started first: the app's main
        // process rather than whatever helper it spawned later.
        if (first || p.StartTimeTicks < sum.StartTimeTicks)
        {
            mainName = child->kind == Kind::Group ? child->key : displayName(p);
            sum.StartTimeTicks = p.StartTimeTicks;
            sum.IconName = p.IconName;
            sum.CmdLine = p.CmdLine;
        }
        if (first)
        {
            sum.User = p.User;
            sum.UID = p.UID;
            first = false;
        } else if (sum.User != p.User)
        {
            sum.User.clear();
        }
        sum.CPUPercent += p.CPUPercent;
        sum.VMRssKb += p.VMRssKb;
        sum.vmSizeKb += p.vmSizeKb;
        sum.SharedKb += p.SharedKb;
        sum.TextKb += p.TextKb;
        sum.DataKb += p.DataKb;
        sum.Threads += p.Threads;
        sum.IOReadBytes += p.IOReadBytes;
        sum.IOWriteBytes += p.IOWriteBytes;
        sum.IOReadBps += p.IOReadBps;
        sum.IOWriteBps += p.IOWriteBps;
        sum.IOTotalsAvailable = sum.IOTotalsAvailable || p.IOTotalsAvailable;
        sum.IORatesAvailable = sum.IORatesAvailable || p.IORatesAvailable;
        sum.IOPermissionDenied = sum.IOPermissionDenied && p.IOPermissionDenied;
    }
    if (node->kind == Kind::Section)
    {
        sum.Name.clear();
        sum.CmdLine.clear();
        sum.IconName.clear();
    }
    if (node->kind == Kind::App)
    {
        // Named after the app (or the unit's main process, which started first).
        if (!node->title.isEmpty())
        {
            sum.Name = node->title;
            sum.CmdLine.clear();   // an app row stands for many programs
        } else
        {
            sum.Name = mainName;
        }
        if (!node->icon.isEmpty())
            sum.IconName = node->icon;
    }
    return sum;
}

void ProcessTreeModel::collectPids(const Node *node, QList<pid_t> &out)
{
    if (node->kind == Kind::Process)
    {
        out.append(node->process.PID);
        return;
    }
    if (node->kind == Kind::Group || node->kind == Kind::App)
    {
        for (const Node *child : node->children)
            collectPids(child, out);
    }
}

QList<pid_t> ProcessTreeModel::PidsForIndex(const QModelIndex &index) const
{
    QList<pid_t> pids;
    if (const Node *node = nodeFromIndex(index))
        collectPids(node, pids);
    return pids;
}

bool ProcessTreeModel::IsUnselectableIndex(const QModelIndex &index)
{
    const Node *node = nodeFromIndex(index);
    return node && (node->kind == Kind::Section || (node->kind == Kind::App && node->key == QLatin1String("K")));
}

QModelIndex ProcessTreeModel::SectionIndex(Section section) const
{
    Node *node = this->m_sections[section];
    return createIndex(node->row, 0, node);
}

bool ProcessTreeProxy::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    // Search looks at the name, command line, user and PID only (not at numbers in
    // other, possibly hidden, columns).
    const QRegularExpression pattern = this->filterRegularExpression();
    if (pattern.pattern().isEmpty())
        return true;
    const QAbstractItemModel *model = this->sourceModel();
    for (int column : { ProcessTreeModel::ColName, ProcessTreeModel::ColCmdline, ProcessTreeModel::ColUser, ProcessTreeModel::ColPid })
    {
        if (model->index(sourceRow, column, sourceParent).data(Qt::DisplayRole).toString().contains(pattern))
            return true;
    }
    return false;
}

bool ProcessTreeProxy::lessThan(const QModelIndex &left, const QModelIndex &right) const
{
    // Sections keep their order: compare rows so that the result, after the
    // proxy reverses it for a descending sort, still puts Apps first.
    if (!left.parent().isValid() && !right.parent().isValid())
        return this->sortOrder() == Qt::AscendingOrder ? left.row() < right.row() : left.row() > right.row();
    return QSortFilterProxyModel::lessThan(left, right);
}

QModelIndex ProcessTreeModel::indexOfNode(Node *node) const
{
    if (!node || node == this->m_root)
        return {};
    return createIndex(node->row, 0, node);
}

void ProcessTreeModel::eraseSubtreeFromIndex(Node *node)
{
    const auto it = this->m_byPid.constFind(node->process.PID);
    if (it != this->m_byPid.cend() && it.value() == node)
        this->m_byPid.erase(it);
    for (Node *child : std::as_const(node->children))
        this->eraseSubtreeFromIndex(child);
}

QModelIndex ProcessTreeModel::IndexForPid(pid_t pid) const
{
    Node *node = this->m_byPid.value(pid, nullptr);
    if (!node || !node->parent)
        return {};
    return createIndex(node->row, 0, node);
}

QString ProcessTreeModel::columnHeader(Column col)
{
    switch (col)
    {
        case ColPid:      return tr("PID");
        case ColName:     return tr("Name");
        case ColUser:     return tr("User");
        case ColState:    return tr("Status");
        case ColCpu:      return tr("CPU");
        case ColMemRss:   return tr("Memory");
        case ColMemVirt:  return tr("MEM VIRT");
        case ColMemShared:return tr("MEM SHR");
        case ColMemText:  return tr("MEM TEXT");
        case ColMemData:  return tr("MEM DATA");
        case ColIoReads:  return tr("IO Reads");
        case ColIoWrites: return tr("IO Writes");
        case ColIoReadsPerSec: return tr("IO Read/s");
        case ColIoWritesPerSec:return tr("IO Write/s");
        case ColThreads:  return tr("Threads");
        case ColPriority: return tr("Priority");
        case ColNice:     return tr("Nice");
        case ColCmdline:  return tr("Command");
        case ColDisk:     return tr("Disk");
        default:          return {};
    }
}

void ProcessTreeModel::freeNode(Node *node)
{
    if (!node)
        return;
    for (Node *child : node->children)
        freeNode(child);
    delete node;
}

ProcessTreeModel::Node *ProcessTreeModel::nodeFromIndex(const QModelIndex &index)
{
    if (!index.isValid())
        return nullptr;
    return static_cast<Node *>(index.internalPointer());
}
