/*
 * Copyright (C) <year> <name of author>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of  MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <TrackSortFilterProxyModel.H>

void TrackSortFilterProxyModel::onQuickFilterChanged(const QString& filter)
{
  beginResetModel();

  if (filter.isEmpty())
  {
    _filter.reset();
  }
  else
  {
    _filter = rs::ml::query::TrackFilter{filter.toStdString()};
  }

  endResetModel();
}

bool TrackSortFilterProxyModel::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
  if (!_filter)
  {
    return true;
  }
  else
  {
    auto index = sourceModel()->index(sourceRow, 0);
    return std::invoke(_filter.value(), static_cast<rs::ml::core::Track*>(index.internalPointer()));
  }
}
