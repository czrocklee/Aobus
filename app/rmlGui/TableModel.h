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

#pragma once

#include <rs/ml/core/MusicLibrary.h>
#include <rs/ml/reactive/AbstractItemList.h>
#include <rs/ml/fbs/Track_generated.h>

#include <QtCore/QAbstractTableModel>
#include <memory>

class TableModelPrivate;

class TableModel : public QAbstractTableModel
{
  Q_OBJECT
  std::unique_ptr<TableModelPrivate> d_ptr;
  friend class TableModelPrivate;

public:
  using MusicLibrary = rs::ml::core::MusicLibrary;
  using TrackId = MusicLibrary::TrackId;
  using AbstractTrackList = rs::ml::reactive::AbstractItemList<TrackId, rs::ml::fbs::TrackT>;

  TableModel(QObject* parent = nullptr);
  TableModel(AbstractTrackList& tracks, QObject* parent = nullptr);
  ~TableModel() override;

  int rowCount(const QModelIndex& parent) const override;
  int columnCount(const QModelIndex& parent) const override;
  QVariant data(const QModelIndex& index, int role) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
  Qt::ItemFlags flags(const QModelIndex& index) const override;
  bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;
  bool insertRows(int position, int rows, const QModelIndex& index = QModelIndex()) override;
  bool removeRows(int position, int rows, const QModelIndex& index = QModelIndex()) override;

  QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
};
