// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/MusicLibrary.h>
#include <rs/fbs/List_generated.h>
#include <rs/reactive/ItemFilterList.h>
#include <rs/reactive/ItemList.h>

#include "TrackView.h"
#include "app/ui_MainWindow.h"

class MainWindow : public QMainWindow
{
  Q_OBJECT

public:
  MainWindow();

  void openMusicLibrary(std::string const& root);

  void importMusicLibrary(std::string const& root);

private:
  using MusicLibrary = rs::core::MusicLibrary;
  using TrackList = rs::reactive::ItemList<MusicLibrary::TrackId, rs::fbs::TrackT>;
  using ReadTransaction = rs::lmdb::ReadTransaction;

  TrackView* createTrackView(std::string_view name, TableModel::AbstractTrackList& list);
  void loadTracks(ReadTransaction& txn);
  void loadLists(ReadTransaction& txn);

  void onTrackClicked(QModelIndex const& index);
  void addListItem(rs::core::MusicLibrary::ListId id, rs::fbs::List const* list);

  Ui::MainWindow _ui;
  std::unique_ptr<MusicLibrary> _ml;
  TrackList _allTracks;
};
