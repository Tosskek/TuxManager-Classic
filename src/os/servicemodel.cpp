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

#include "servicemodel.h"

#include <algorithm>

using namespace OS;

namespace
{
    QString displayServiceUnit(const QString &unit)
    {
        if (unit.endsWith(".service"))
            return unit.left(unit.size() - 8);
        return unit;
    }
}

ServiceModel::ServiceModel(QObject *parent) : QAbstractTableModel(parent)
{
}

int ServiceModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return this->m_services.size();
}

int ServiceModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return ColCount;
}

QVariant ServiceModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid()
        || index.row() < 0
        || index.row() >= this->m_services.size()
        || index.column() < 0
        || index.column() >= ColCount)
    {
        return {};
    }

    const Service &s = this->m_services.at(index.row());

    if (role == Qt::DisplayRole)
    {
        switch (static_cast<Column>(index.column()))
        {
            case ColService:     return displayServiceUnit(s.Unit);
            case ColLoad:        return s.LoadState;
            case ColActive:      return s.ActiveState;
            case ColSubState:    return s.SubState;
            case ColDescription: return s.Description;
            default:             return {};
        }
    }

    if (role == Qt::UserRole)
    {
        switch (static_cast<Column>(index.column()))
        {
            case ColService:     return s.Unit;
            case ColLoad:        return s.LoadState;
            case ColActive:      return s.ActiveState;
            case ColSubState:    return s.SubState;
            case ColDescription: return s.Description;
            default:             return {};
        }
    }

    return {};
}

QVariant ServiceModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    if (section < 0 || section >= ColCount)
        return {};

    switch (static_cast<Column>(section))
    {
        case ColService:     return tr("Service");
        case ColLoad:        return tr("Load");
        case ColActive:      return tr("Active");
        case ColSubState:    return tr("SubState");
        case ColDescription: return tr("Description");
        default:             return {};
    }
}

Qt::ItemFlags ServiceModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

void ServiceModel::SetServices(const QList<Service> &unsortedServices)
{
    // systemd returns units in hash order, which can differ between calls; sort by
    // unit name so unchanged lists compare equal (the view sorts via its proxy).
    QList<Service> services = unsortedServices;
    std::sort(services.begin(), services.end(), [](const Service &a, const Service &b) { return a.Unit < b.Unit; });

    // Usual case: the same units in the same order -> update changed rows only, so
    // the view keeps its scroll position and repaints just what changed.
    bool sameUnits = services.size() == this->m_services.size();
    for (int i = 0; sameUnits && i < services.size(); ++i)
        sameUnits = services.at(i).Unit == this->m_services.at(i).Unit;

    if (sameUnits)
    {
        for (int i = 0; i < services.size(); ++i)
        {
            const Service &cur = this->m_services.at(i);
            const Service &in = services.at(i);
            if (cur.LoadState == in.LoadState && cur.ActiveState == in.ActiveState
                && cur.SubState == in.SubState && cur.Description == in.Description)
                continue;
            this->m_services[i] = in;
            emit dataChanged(this->index(i, 0), this->index(i, ColCount - 1));
        }
        return;
    }

    beginResetModel();
    this->m_services = std::move(services);
    endResetModel();
}
