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
    QString displayName(const Process &proc)
    {
        if (proc.IsKernelThread || proc.CmdLine.isEmpty())
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

        static const QStringList interpreters = { "python", "python3", "bash", "sh", "dash", "zsh", "perl", "ruby", "node", "env" };
        QString base = name;
        base.remove(QRegularExpression(QStringLiteral("[0-9.]+$")));   // python3.12 -> python
        if (interpreters.contains(name) || interpreters.contains(base))
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
                const QString script = arg.section('/', -1);
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
    // Like Windows Task Manager: one top-level row per program (systemd unit), the
    // program's processes inside it. Apps you started are named after their desktop
    // entry; all kernel threads share one "Linux kernel" row; a service with a single
    // process is shown as that process. Inside a program row, same-name processes
    // fold into one "name (N)" row.
    QVector<QString> containerOf(count);   // "A:<app>", "K", "U:<cgroup>", "P:<pid>"
    QVector<QString> nameOf(count);
    QHash<QString, QString> titles;        // container -> fixed title (apps, kernel)
    QHash<QString, QString> icons;
    QHash<QString, int> membersOf;         // container -> process count
    QHash<QString, int> membersByGroup;    // "<container>\n<name>" -> count
    QHash<pid_t, int> incomingByPid;
    incomingByPid.reserve(count);
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
            containerOf[i] = QStringLiteral("A:") + info->first;
            titles.insert(containerOf[i], info->first);
            if (!info->second.isEmpty() && !icons.contains(containerOf[i]))
                icons.insert(containerOf[i], info->second);
        } else if (!proc.CGroup.isEmpty() && proc.CGroup != QLatin1String("/"))
        {
            containerOf[i] = QStringLiteral("U:") + proc.CGroup;
            // The login session (display manager, X server, session start-up).
            static const QRegularExpression sessionScope(QStringLiteral("/session-[^/]+\\.scope$"));
            if (sessionScope.match(proc.CGroup).hasMatch())
                titles.insert(containerOf[i], tr("Desktop session"));
        } else
        {
            containerOf[i] = QStringLiteral("P:") + QString::number(proc.PID);
        }
        ++membersOf[containerOf[i]];
        ++membersByGroup[containerOf[i] + '\n' + nameOf[i]];
        incomingByPid.insert(proc.PID, i);
    }
    if (this->m_appInfoCache.size() > 4096)
        this->m_appInfoCache.clear();

    // App and kernel rows always exist; other units only when they hold 2+ processes.
    auto containerNeeded = [&](const QString &key)
    {
        return titles.contains(key) || membersOf.value(key) >= 2;
    };
    auto targetGroup = [&](int i) -> QString
    {
        return membersByGroup.value(containerOf[i] + '\n' + nameOf[i]) >= 2 ? nameOf[i] : QString();
    };
    // Apps go to the Apps section, everything else (incl. single-process services) to Background & system.
    auto sectionFor = [this](const QString &key) { return this->m_sections[key.startsWith(QLatin1String("A:")) ? SectionApps : SectionSystem]; };
    auto targetParent = [&](int i) -> Node *   // program row or section; nullptr = program row not created yet
    {
        return containerNeeded(containerOf[i]) ? this->m_containers.value(containerOf[i], nullptr) : sectionFor(containerOf[i]);
    };

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
    for (Node *node : std::as_const(stale))
        this->removeNode(node);

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
        this->appendNode(parent, fresh);
        this->m_byPid.insert(proc.PID, fresh);
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
