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

#ifndef OS_PROCESSTREEMODEL_H
#define OS_PROCESSTREEMODEL_H

#include "process.h"

#include <QAbstractItemModel>
#include <QFont>
#include <QHash>
#include <QSet>
#include <QSortFilterProxyModel>

namespace OS
{
    class AppRegistry;

    class ProcessTreeModel : public QAbstractItemModel
    {
        Q_OBJECT

        public:
            enum Column
            {
                ColPid = 0,
                ColName,
                ColUser,
                ColState,
                ColCpu,
                ColMemRss,
                ColMemVirt,
                ColMemShared,
                ColMemText,
                ColMemData,
                ColIoReads,
                ColIoWrites,
                ColIoReadsPerSec,
                ColIoWritesPerSec,
                ColThreads,
                ColPriority,
                ColNice,
                ColCmdline,
                ColDisk,           ///< Combined disk read+write rate
                ColCount
            };

            explicit ProcessTreeModel(QObject *parent = nullptr);
            ~ProcessTreeModel() override;

            QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
            QModelIndex parent(const QModelIndex &child) const override;
            int rowCount(const QModelIndex &parent = QModelIndex()) const override;
            int columnCount(const QModelIndex &parent = QModelIndex()) const override;
            QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
            QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
            Qt::ItemFlags flags(const QModelIndex &index) const override;

            //! Top-level sections, in display order.
            enum Section
            {
                SectionApps = 0,     ///< Programs the user started (app-*.scope / app-*@.service cgroups)
                SectionSystem,       ///< Desktop services, autostart helpers, other users, kernel threads
                SectionCount
            };

            //! Updates the tree in place: exited processes are removed, new ones inserted,
            //! changed rows signalled. Always returns false (kept for API compatibility).
            //!
            //! Layout: two sections, Apps and Background & system. In them, one row per program, i.e. per
            //! systemd unit — apps you started are named after their desktop entry, kernel
            //! threads share one "Linux kernel" row, a single-process service is just that
            //! process. Inside a program row, same-name processes fold into "name (N)".
            //! Program and group rows show the totals of their members.
            bool SetProcesses(const QList<Process> &processes);
            //! PIDs a row stands for: one for a process, all members for a program / group row.
            QList<pid_t> PidsForIndex(const QModelIndex &index) const;
            //! True for rows that must not be selected / ended (section headers, "Linux kernel").
            static bool IsUnselectableIndex(const QModelIndex &index);
            //! Index (column 0) of a section row.
            QModelIndex SectionIndex(Section section) const;
            /// Registry used to provide icons for the name column; nullptr disables icons.
            void SetAppRegistry(AppRegistry *registry) { this->m_appRegistry = registry; }
            QModelIndex IndexForPid(pid_t pid) const;
            //! PIDs owning a taskbar window (see WindowPids); known=false when the window
            //! manager gives no client list. Used to decide which programs are Apps.
            void SetWindowPids(const QSet<pid_t> &pids, bool known) { this->m_windowPids = pids; this->m_windowsKnown = known; }
            //! Font of the view, used as the base for bold section headers.
            void SetBaseFont(const QFont &font) { this->m_baseFont = font; }

        private:
            enum class Kind { Process, Group, App, Section };
            struct Node
            {
                Process process;     //!< the process, or the totals of a group / app / section
                Kind kind { Kind::Process };
                QString key;         //!< group: process name it folds; program row: unit key ("A:Claude", "K", "U:<cgroup>")
                QString title;       //!< program row: fixed title (app name, "Linux kernel"); empty = main process name
                QString icon;        //!< program row: icon from the app's desktop entry
                QHash<QString, Node *> groups; //!< program row: name -> group child
                int memberCount { 0 }; //!< group/section: number of processes inside
                Node *parent { nullptr };
                QList<Node *> children;
                int row { 0 };   //!< index in parent->children (avoids O(n) indexOf lookups)
            };

            Node *m_root { nullptr };
            QFont m_baseFont;
            QSet<pid_t> m_windowPids;
            bool m_windowsKnown { false };
            Node *m_sections[SectionCount] {};
            QHash<QString, Node *> m_containers;             //!< unit key -> program row
            QHash<QString, QPair<QString, QString>> m_appInfoCache; //!< cgroup -> (app name, icon)
            AppRegistry *m_appRegistry { nullptr };
            QHash<pid_t, Node *> m_byPid;

            static QString columnHeader(Column col);
            static void freeNode(Node *node);
            QModelIndex indexOfNode(Node *node) const;
            void eraseSubtreeFromIndex(Node *node);
            static Node *nodeFromIndex(const QModelIndex &index);
            void removeNode(Node *node);
            void removeLeafRange(Node *parent, int first, int last);
            Node *appendNode(Node *parent, Node *node);
            static void collectPids(const Node *node, QList<pid_t> &out);
            static Process totals(const Node *node);
    };
}

namespace OS
{
    //! Sorting proxy for the process tree that keeps the sections in their fixed
    //! order (Apps first) whatever column / direction is sorted.
    class ProcessTreeProxy : public QSortFilterProxyModel
    {
        Q_OBJECT

        public:
            using QSortFilterProxyModel::QSortFilterProxyModel;

        protected:
            bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;
            bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;
    };
}

#endif // OS_PROCESSTREEMODEL_H
