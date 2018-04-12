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
#include <rs/ml/query/TrackFilter.H>
#include <rs/ml/query/Parser.H>
#include <QtConcurrent>
#include <boost/iterator/transform_iterator.hpp>

#include <iostream>

TableModel::TableModel(rs::ml::core::MediaLibrary& ml, QObject *parent)
    : QAbstractTableModel(parent), _ml{ml}
{
  for (auto pair : ml)
  {
    _allTracks.append(pair.second);
  }

  _tracks = _allTracks;
}
//! [0]

//! [1]
int TableModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return _tracks.size();
}

int TableModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return 3;
}
//! [1]

//! [2]
QVariant TableModel::data(const QModelIndex &index, int role) const
{
    std::cout << index.row() << ' ' << index.column() << std::endl;
    if (!index.isValid())
        return QVariant();

    if (index.row() >= _tracks.size() || index.row() < 0)
        return QVariant();

    if (role == Qt::DisplayRole) {
        auto track = _tracks.at(index.row());

        if (index.column() == 0)
            return QString::fromUtf8(track->artist()->c_str(), track->artist()->size());
        else if (index.column() == 1)
            return QString::fromUtf8(track->album()->c_str(), track->album()->size());
        else if (index.column() == 2)
            return QString::fromUtf8(track->title()->c_str(), track->title()->size());
    }
    return QVariant();
}
//! [2]

//! [3]
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
 //       _tracks.insert(position, { QString(), QString() });

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
 //       _tracks.removeAt(position);

    endRemoveRows();
    return true;
}
//! [5]

//! [6]
bool TableModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (index.isValid() && role == Qt::EditRole) {
        int row = index.row();
        /*
        auto contact = _tracks.value(row);

        if (index.column() == 0)
            contact.name = value.toString();
        else if (index.column() == 1)
            contact.address = value.toString();
        else
            return false;

        _tracks.replace(row, contact);
        emit(dataChanged(index, index));
*/
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
  return createIndex(row, column, const_cast<void*>(static_cast<const void*>(_tracks[row])));
}

void TableModel::onQuickFilterChanged(const QString& filter)
{
  beginResetModel();
  rs::ml::query::TrackFilter f{filter.toStdString()};
  auto future = QtConcurrent::filtered(_allTracks, f);
  future.waitForFinished();
  _tracks = future.results();
  endResetModel();
}

