// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/MusicLibrary.h>
#include <rs/fbs/Track_generated.h>
#include <rs/reactive/AbstractItemList.h>

#include <QtCore/QAbstractTableModel>
#include <memory>

class TableModelPrivate;

class TableModel : public QAbstractTableModel
{
  Q_OBJECT
  std::unique_ptr<TableModelPrivate> d_ptr;
  friend class TableModelPrivate;

public:
  using MusicLibrary = rs::core::MusicLibrary;
  using TrackId = MusicLibrary::TrackId;
  using AbstractTrackList = rs::reactive::AbstractItemList<TrackId, rs::fbs::TrackT>;

  TableModel(QObject* parent = nullptr);
  TableModel(AbstractTrackList& tracks, QObject* parent = nullptr);
  ~TableModel() override;

  int rowCount(QModelIndex const& parent) const override;
  int columnCount(QModelIndex const& parent) const override;
  QVariant data(QModelIndex const& index, int role) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
  Qt::ItemFlags flags(QModelIndex const& index) const override;
  bool setData(QModelIndex const& index, QVariant const& value, int role = Qt::EditRole) override;
  bool insertRows(int position, int rows, QModelIndex const& index = QModelIndex()) override;
  bool removeRows(int position, int rows, QModelIndex const& index = QModelIndex()) override;

  QModelIndex index(int row, int column, QModelIndex const& parent = QModelIndex()) const override;
};
