// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "TrackSortFilterProxyModel.h"
#include <rs/expr/Evaluator.h>
#include <rs/expr/Parser.h>
// #include <QtConcurrent>

/* TrackSortFilterProxyModel::TrackSortFilterProxyModel(rs::core::MusicLibrary& ml, QObject *parent)
  : QSortFilterProxyModel{parent}, _ml{ml}
{
} */

void TrackSortFilterProxyModel::onQuickFilterChanged(QString const& filter)
{
  _quick = filter.toStdString();
  /*
  if (filter.isEmpty())
  {
    _filter.reset();
  }
  else

    _filter = rs::query::TrackFilter{filter.toStdString()};
  }*/

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
  beginFilterChange();
  endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
  invalidateFilter();
#endif
}
#include <QtCore/QDebug>
bool TrackSortFilterProxyModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{
  if (_quick.empty())
  {
    return true;
  }
  else
  {
    auto index = sourceModel()->index(sourceRow, 0);
    using IdTrackPair = std::pair<rs::core::MusicLibrary::TrackId, rs::fbs::TrackT>;
    auto const& track = static_cast<IdTrackPair*>(index.internalPointer())->second;

    return track.meta->title.find(_quick) != std::string::npos || track.meta->album.find(_quick) != std::string::npos ||
           track.meta->artist.find(_quick) != std::string::npos;
    /*
    auto index = sourceModel()->index(sourceRow, 0);
    auto reader = _ml.reader();
    auto track = reader[static_cast<rs::core::TrackId>(index.internalId())];
    return std::invoke(_filter.value(), track);*/
  }
}
