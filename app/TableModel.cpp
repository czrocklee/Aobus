// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "TableModel.h"
#include <QtCore/QDebug>
#include <boost/algorithm/string/join.hpp>
#include <iostream>
#include <ranges>

// using IdTrackPair = TableModel::IdTrackPair;
using AbstractTrackList = TableModel::AbstractTrackList;

class TableModelPrivate : public AbstractTrackList::Observer
{
public:
  Q_DECLARE_PUBLIC(TableModel)

  TableModelPrivate(TableModel* q, AbstractTrackList& tracks) : q_ptr{q}, tracks{tracks} { tracks.attach(*this); }

  ~TableModelPrivate()
  {
    // tracks.detach(*this);
  }

  void onBeginInsert(TableModel::TrackId id, AbstractTrackList::Index index) override
  {
    Q_Q(TableModel);
    q->beginInsertRows({}, index, index);
  }

  void onEndInsert(TableModel::TrackId, rs::fbs::TrackT const&, AbstractTrackList::Index index)
  {
    Q_Q(TableModel);
    q->endInsertRows();
  }

  void onEndUpdate(TableModel::TrackId, rs::fbs::TrackT const&, AbstractTrackList::Index index) override
  {
    Q_Q(TableModel);
    emit(q->dataChanged(q->index(index, 0, {}), q->index(index, 2, {})));
  }

  void onBeginRemove(TableModel::TrackId, rs::fbs::TrackT const&, AbstractTrackList::Index index) override
  {
    Q_Q(TableModel);
    q->beginRemoveRows({}, index, index);
  }

  void onEndRemove(TableModel::TrackId, AbstractTrackList::Index) override
  {
    Q_Q(TableModel);
    q->endRemoveRows();
  }

  void onBeginClear() override
  {
    Q_Q(TableModel);
    q->beginResetModel();
  }

  void onEndClear() override
  {
    Q_Q(TableModel);
    q->endResetModel();
  }

  TableModel* q_ptr;
  AbstractTrackList& tracks;
};

TableModel::TableModel(QObject* parent) : QAbstractTableModel{parent} {}

TableModel::TableModel(AbstractTrackList& tracks, QObject* parent)
  : QAbstractTableModel{parent}
  , d_ptr{std::make_unique<TableModelPrivate>(this, tracks)}
{
}

TableModel::~TableModel() {}

int TableModel::rowCount(QModelIndex const& parent) const
{
  Q_UNUSED(parent);
  return d_ptr->tracks.size();
}

int TableModel::columnCount(QModelIndex const& parent) const
{
  Q_UNUSED(parent);
  return 4;
}

QVariant TableModel::data(QModelIndex const& index, int role) const
{
  if (!index.isValid()) return QVariant();

  if (index.row() >= static_cast<int>(d_ptr->tracks.size()) || index.row() < 0) return QVariant();

  auto const& track = d_ptr->tracks.at(AbstractTrackList::Index{static_cast<std::size_t>(index.row())}).second;

  if (role == Qt::DisplayRole)
  {
    if (index.column() == 0)
      return QString::fromUtf8(track.meta->artist.c_str());
    else if (index.column() == 1)
      return QString::fromUtf8(track.meta->album.c_str());
    else if (index.column() == 2)
      return QString::fromUtf8(track.meta->title.c_str());
    else if (index.column() == 3)
    {
      auto tagViews = track.tags | std::views::transform([](auto const& tag) { return QString::fromUtf8(tag.c_str()); });
      return QStringList{tagViews.begin(), tagViews.end()};
    }
  }

  if (role == Qt::UserRole && !track.rsrc.empty())
  {
    return static_cast<qulonglong>(track.rsrc.front()->id);
  }

  return {};
}

QVariant TableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
  if (role != Qt::DisplayRole) return QVariant();

  if (orientation == Qt::Horizontal)
  {
    switch (section)
    {
      case 0:
        return tr("Artist");
      case 1:
        return tr("Album");
      case 2:
        return tr("Title");
      case 3:
        return tr("Tags");

      default:
        return QVariant();
    }
  }
  return QVariant();
}
//! [3]

//! [4]
bool TableModel::insertRows(int position, int rows, QModelIndex const& index)
{
  Q_UNUSED(index);
  beginInsertRows(QModelIndex(), position, position + rows - 1);

  for (int row = 0; row < rows; ++row)
    //       _list.insert(position, { QString(), QString() });

    endInsertRows();
  return true;
}
//! [4]

//! [5]
bool TableModel::removeRows(int position, int rows, QModelIndex const& index)
{
  Q_UNUSED(index);
  beginRemoveRows(QModelIndex(), position, position + rows - 1);

  for (int row = 0; row < rows; ++row)
    //       _list.removeAt(position);

    endRemoveRows();
  return true;
}
//! [5]

//! [6]
bool TableModel::setData(QModelIndex const& index, QVariant const& value, int role)
{
  /*   if (index.isValid() && role == Qt::EditRole)
    {
      std::cout << "setData " << index.row() << ' ' << index.column() << std::endl;
      int row = index.row();

      auto track = rs::core::clone(_list[row].second);
      if (index.column() == 0)
      {
        track.artist = value.toString().toStdString();
      } else if (index.column() == 1)
      {
        track.album = value.toString().toStdString();
      } else if (index.column() == 2)
      {
        track.title = value.toString().toStdString();
      } else
      {
        return false;
      }

      _list.mutate(row, track);

      return true;
    } */

  return false;
}
//! [6]

//! [7]
Qt::ItemFlags TableModel::flags(QModelIndex const& index) const
{
  if (!index.isValid()) return Qt::ItemIsEnabled;

  return QAbstractTableModel::flags(index) | Qt::ItemIsEditable;
}

QModelIndex TableModel::index(int row, int column, QModelIndex const& parent) const
{
  return createIndex(
    row,
    column,
    &const_cast<AbstractTrackList::Value&>(d_ptr->tracks.at(AbstractTrackList::Index{static_cast<std::size_t>(row)})));
}
