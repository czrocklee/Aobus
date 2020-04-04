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
#include <rs/ml/core/List.h>
#include <rs/ml/reactive/ItemList.h>
#include <rs/ml/reactive/ItemFilterList.h>

#include "TrackView.h"
#include "app/rmlGui/ui_MainWindow.h"

class MainWindow : public QMainWindow, public Ui::MainWindow
{
  Q_OBJECT

public:
  MainWindow();

  void openMusicLibrary(const std::string& root);

private:
  using TrackList = rs::ml::reactive::ItemList<rs::ml::core::TrackT>;
  using TrackFilterList = rs::ml::reactive::ItemFilterList<rs::ml::core::TrackT>;
  using MusicLibrary = rs::ml::core::MusicLibrary;
  using ReadTransaction = rs::ml::core::LMDBReadTransaction;

  struct ListItem : public QListWidgetItem
  {
    using QListWidgetItem::QListWidgetItem;

    rs::ml::core::ListT list;
    TrackView* trackView;
    std::unique_ptr<TrackFilterList> tracks;
  };

  void loadTracks(ReadTransaction& txn);
  void loadLists(ReadTransaction& txn);

  void onTrackClicked(const QModelIndex& index);
  void addListItem(const rs::ml::fbs::List* list);

  std::unique_ptr<MusicLibrary> _ml;
  TrackList _allTracks;
};
