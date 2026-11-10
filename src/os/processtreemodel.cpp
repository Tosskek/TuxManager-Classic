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

#include <QSet>
#include <functional>

#include "appregistry.h"
#include "../misc.h"
using namespace OS;

ProcessTreeModel::ProcessTreeModel(QObject *parent) : QAbstractItemModel(parent), m_root(new Node())
{
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
            case ColName:
                return node->children.isEmpty() ? proc.Name
                                                : QString("%1 (%2)").arg(proc.Name).arg(node->children.size());
            case ColUser:     return proc.User;
            case ColState:    return Process::GetStateString(proc.State);
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
        static const double cpuCapacity = 100.0 * qMax(1L, sysconf(_SC_NPROCESSORS_ONLN));
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
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

bool ProcessTreeModel::SetProcesses(const QList<Process> &processes)
{
    // Classic grouping instead of the full parent chain:
    //  - kernel threads (children of kthreadd, PID 2) fold under kthreadd;
    //  - helper processes of the same program (parent has the same name)
    //    fold under that program, like "app (3)";
    //  - everything else is a top-level row, so apps are never buried
    //    under systemd / the session manager.
    QHash<pid_t, const Process *> incomingByPid;
    incomingByPid.reserve(processes.size());
    for (const Process &proc : processes)
        incomingByPid.insert(proc.PID, &proc);

    auto groupParentPid = [&incomingByPid](const Process &proc) -> pid_t
    {
        const pid_t ppid = proc.PPID;
        if (ppid <= 0 || ppid == proc.PID)
            return 0;
        const Process *pp = incomingByPid.value(ppid, nullptr);
        if (pp && (ppid == 2 || pp->Name == proc.Name))
            return ppid;
        return 0;
    };

    // Incremental path: keep existing nodes, remove exited processes, insert new
    // ones and update changed rows in place. Only touched rows are signalled, so the
    // view repaints just those and keeps its expansion, selection and scroll
    // position. A full reset is needed only when a surviving process moves to a
    // different group (e.g. it was re-parented) or loses its parent but keeps children.
    if (this->m_root)
    {
        auto isAlive = [&incomingByPid](const Node *node)
        {
            const Process *in = incomingByPid.value(node->process.PID, nullptr);
            return in && in->StartTimeTicks == node->process.StartTimeTicks;
        };

        bool canPatch = true;
        QList<Node *> removedRoots;   // top-most removed nodes (their subtree goes with them)
        for (auto it = this->m_byPid.cbegin(); canPatch && it != this->m_byPid.cend(); ++it)
        {
            Node *node = it.value();
            if (isAlive(node))
            {
                const Process &in = *incomingByPid.value(node->process.PID);
                const pid_t currentParent = (node->parent && node->parent != this->m_root) ? node->parent->process.PID : 0;
                if (currentParent != groupParentPid(in))
                    canPatch = false;
                continue;
            }
            // Removed: its children must be removed too, otherwise they'd need regrouping.
            for (const Node *child : std::as_const(node->children))
            {
                if (isAlive(child))
                {
                    canPatch = false;
                    break;
                }
            }
            if (node->parent == this->m_root || isAlive(node->parent))
                removedRoots.append(node);
        }

        if (canPatch)
        {
            QSet<pid_t> countChanged;   // parents whose "name (N)" label changes (by PID: nodes may be freed)

            // 1) Remove exited processes (whole subtrees).
            for (Node *node : std::as_const(removedRoots))
            {
                Node *parent = node->parent;
                const int row = node->row;
                beginRemoveRows(indexOfNode(parent), row, row);
                parent->children.removeAt(row);
                for (int i = row; i < parent->children.size(); ++i)
                    parent->children[i]->row = i;
                eraseSubtreeFromIndex(node);
                endRemoveRows();
                freeNode(node);   // only after Qt has dropped its references to the rows
                if (parent != this->m_root)
                    countChanged.insert(parent->process.PID);
            }

            // 2) Insert new processes; parents are inserted before their children.
            std::function<Node *(const Process &)> ensureNode = [&](const Process &proc) -> Node *
            {
                Node *existing = this->m_byPid.value(proc.PID, nullptr);
                if (existing && existing->process.StartTimeTicks == proc.StartTimeTicks)
                    return existing;

                const pid_t groupPid = groupParentPid(proc);
                Node *parent = this->m_root;
                if (groupPid > 0)
                    parent = ensureNode(*incomingByPid.value(groupPid));

                Node *node = new Node();
                node->process = proc;
                node->parent = parent;
                node->row = static_cast<int>(parent->children.size());
                beginInsertRows(indexOfNode(parent), node->row, node->row);
                parent->children.append(node);
                this->m_byPid.insert(proc.PID, node);
                endInsertRows();
                if (parent != this->m_root)
                    countChanged.insert(parent->process.PID);
                return node;
            };

            for (const Process &proc : processes)
            {
                Node *node = this->m_byPid.value(proc.PID, nullptr);
                if (!node || node->process.StartTimeTicks != proc.StartTimeTicks)
                {
                    ensureNode(proc);
                    continue;
                }
                // 3) Update surviving rows whose data changed.
                if (node->process.HasSameData(proc))
                    continue;
                node->process = proc;
                emit dataChanged(createIndex(node->row, 0, node), createIndex(node->row, ColCount - 1, node));
            }

            for (pid_t parentPid : std::as_const(countChanged))
            {
                Node *parent = this->m_byPid.value(parentPid, nullptr);
                if (!parent)
                    continue;
                const QModelIndex idx = createIndex(parent->row, ColName, parent);
                emit dataChanged(idx, idx, { Qt::DisplayRole });
            }
            return false;
        }
    }

    beginResetModel();
    freeNode(this->m_root);
    this->m_root = new Node();
    this->m_byPid.clear();

    QList<Node *> nodes;
    nodes.reserve(processes.size());
    for (const Process &proc : processes)
    {
        Node *node = new Node();
        node->process = proc;
        this->m_byPid.insert(proc.PID, node);
        nodes.append(node);
    }

    for (Node *node : nodes)
    {
        const pid_t groupPid = groupParentPid(node->process);
        Node *parent = groupPid > 0 ? this->m_byPid.value(groupPid, this->m_root) : this->m_root;

        node->parent = parent;
        node->row = static_cast<int>(parent->children.size());
        parent->children.append(node);
    }

    endResetModel();
    return true;
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
