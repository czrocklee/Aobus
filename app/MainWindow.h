// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/MusicLibrary.h>

#include <gtkmm.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace app::model
{
  class ListDraft;
  class TrackRowDataProvider;
  class AllTrackIdsList;
  class FilteredTrackIdList;
  class ManualTrackIdList;
  class TrackIdList;
}

class TrackListAdapter;
class TrackViewPage;
class ListRow;
class ImportWorker;
class CoverArtWidget;
class PlaylistExporter;
class ImportProgressDialog;

// Page context structure
struct TrackPageContext final
{
  std::unique_ptr<app::model::TrackIdList> membershipList;
  std::shared_ptr<TrackListAdapter> adapter;
  std::unique_ptr<TrackViewPage> page;
  std::unique_ptr<PlaylistExporter> exporter;
};

class MainWindow : public Gtk::ApplicationWindow
{
public:
  MainWindow();
  ~MainWindow() override;

private:
  // List selection callback
  void onListSelectionChanged(std::uint32_t position, std::uint32_t nItems);
  void updateCoverArt(std::vector<rs::core::TrackId> const& selectedIds);

  // List context menu
  void showListContextMenu(Gtk::ListView& listView, Gdk::Rectangle const& rect);

  // Track context menu (tagging)
  void showTrackContextMenu(TrackViewPage& page, double x, double y);
  void tagSelectedTracks(TrackViewPage& page);

  void setupMenu();
  void setupLayout();
  void openLibrary();
  void openMusicLibrary(std::filesystem::path const& path);
  void importFiles();
  void importFilesFromPath(std::filesystem::path const& path);
  void scanDirectory(std::filesystem::path const& dir, std::vector<std::filesystem::path>& files);

  // List management - using ListDraft
  void createList(app::model::ListDraft const& draft);
  void onDeleteList();
  void onTagTrack();

  void setupTrackContextMenu();

  // Page management helpers
  void clearTrackPages();
  void rebuildListPages(rs::lmdb::ReadTransaction& txn);
  void buildPageForAllTracks();
  void buildPageForStoredList(rs::core::ListId listId, rs::core::ListView const& view, rs::lmdb::ReadTransaction& txn);

  // Notification handlers from AllTrackIdsList
  void notifyTracksInserted(std::vector<rs::core::TrackId> const& ids);
  void notifyTracksUpdated(std::vector<rs::core::TrackId> const& ids);
  void notifyTracksRemoved(std::vector<rs::core::TrackId> const& ids);

  void saveSession();
  void loadSession();

  // Music library instance
  std::unique_ptr<rs::core::MusicLibrary> _musicLibrary;

  // Shared row data provider (owned)
  std::shared_ptr<app::model::TrackRowDataProvider> _rowDataProvider;

  // All tracks TrackId list (owned)
  std::unique_ptr<app::model::AllTrackIdsList> _allTrackIds;

  // Layout: Horizontal paned with left box and right stack
  Gtk::Paned _paned;

  // Left side: vertical box with list + cover art
  Gtk::Box _leftBox;
  Gtk::ListView _listView;
  Gtk::ScrolledWindow _listScrolledWindow;
  std::unique_ptr<CoverArtWidget> _coverArtWidget;
  std::unique_ptr<ImportProgressDialog> _importDialog;

  // Import worker - owned and joined on window destruction
  std::unique_ptr<ImportWorker> _importWorker;
  std::jthread _importThread;

  // Right side: stack for pages
  Gtk::Stack _stack;

  // Menu (placeholder)
  Gtk::PopoverMenuBar _menuBar;

  // List model for sidebar
  Glib::RefPtr<Gio::ListStore<ListRow>> _listStore;
  Glib::RefPtr<Gtk::SingleSelection> _listSelectionModel;

  // Track pages map
  std::map<rs::core::ListId, TrackPageContext> _trackPages;
};