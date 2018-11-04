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

#include <TableModel.H>
#include <iostream>
#include <boost/algorithm/string/join.hpp>

/*
QVariant getString(const flatbuffers::String* str)
{
  if (str == nullptr)
  {
    return {};
  }

  return QString::fromUtf8(str->c_str(), str->size());
}*/

TableModel::TableModel(rs::ml::core::TrackList& list, QObject *parent)
    : QAbstractTableModel(parent), _list{list}
{
  _list.attach(*this);
}

int TableModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return _list.size();
}

int TableModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return 4;
}

QVariant TableModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return QVariant();

    if (index.row() >= _list.size() || index.row() < 0)
        return QVariant();

    if (role == Qt::DisplayRole) {
        const auto& track = _list[index.row()].second;
        if (index.column() == 0)
            return QString::fromUtf8(track.artist.c_str());
        else if (index.column() == 1)
            return QString::fromUtf8(track.album.c_str());
        else if (index.column() == 2)
            return QString::fromUtf8(track.title.c_str());
        else if (index.column() == 3)
        {
          auto val = boost::algorithm::join(track.tags, "|");
          return QString::fromUtf8(val.c_str());
        }
    }
    return {};
}

QVariant TableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole)
        return QVariant();

    if (orientation == Qt::Horizontal) {
        switch (section) {
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
bool TableModel::insertRows(int position, int rows, const QModelIndex &index)
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
bool TableModel::removeRows(int position, int rows, const QModelIndex &index)
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
bool TableModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (index.isValid() && role == Qt::EditRole) {
      std::cout << "setData " << index.row() << ' ' << index.column() << std::endl;
        int row = index.row();

        auto track = rs::ml::core::clone(_list[row].second);
        if (index.column() == 0)
        {
          track.artist = value.toString().toStdString();
        }
        else if (index.column() == 1)
        {
          track.album = value.toString().toStdString();
        }
        else if (index.column() == 2)
        {
          track.title = value.toString().toStdString();
        }
       else
        {
          return false;
        }

        _list.mutate(row, track);

        return true;
    }

    return false;
}
//! [6]

//! [7]
Qt::ItemFlags TableModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::ItemIsEnabled;

    return QAbstractTableModel::flags(index) | Qt::ItemIsEditable;
}

QModelIndex TableModel::index(int row, int column, const QModelIndex &parent) const
{
  //return createIndex(row, column, _list[row].first);
  return createIndex(row, column, const_cast<rs::ml::core::TrackT*>(&_list[row].second));
}

void TableModel::onQuickFilterChanged(const QString& filter)
{
  /*
  beginResetModel();
  rs::ml::query::TrackFilter f{filter.toStdString()};
//  auto future = QtConcurrent::filtered(_allTracks, f);
//  future.waitForFinished();
//  _list = future.results();
  

  _list.clear();
  for (auto pair : _ml.reader())
  {
    if (f(pair.second))
    {
      _list.append(pair.first);
    }
  }


  endResetModel();*/
}

void TableModel::onCreate(rs::ml::core::TrackId, const rs::ml::core::TrackT&, std::size_t index)
{
  std::cout << "created " << index << std::endl;
  beginInsertRows({}, index, index);
  endInsertRows();
}

void TableModel::onModify(rs::ml::core::TrackId, const rs::ml::core::TrackT&, std::size_t index)
{

  std::cout << "modify " << index << std::endl;
  emit(dataChanged(this->index(index, 0, {}), this->index(index, 2, {})));
}

void TableModel::onRemove(rs::ml::core::TrackId, const rs::ml::core::TrackT&, std::size_t index)
{
}

