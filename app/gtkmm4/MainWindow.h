#pragma once

#include <rs/core/MusicLibrary.h>
#include <rs/fbs/List_generated.h>
#include <rs/reactive/AbstractItemList.h>
#include <rs/reactive/ItemList.h>

#include <gtkmm.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <vector>

class TrackListAdapter;
class TrackViewPage;
class ListRow;
class ImportWorker;
class CoverArtWidget;
class PlaylistExporter;
class ImportProgressDialog;

class MainWindow : public Gtk::ApplicationWindow
{
public:
  MainWindow();
  ~MainWindow() override;

private:
  // Music library instance
  std::unique_ptr<rs::core::MusicLibrary> _musicLibrary;

  // All tracks list
  rs::reactive::ItemList<rs::core::MusicLibrary::TrackId, rs::fbs::TrackT> _allTracks;

  // Layout: Horizontal paned with left box and right stack
  Gtk::Paned _paned;

  // Left side: vertical box with list + cover art
  Gtk::Box _leftBox;
  Gtk::ListView _listView;
  Gtk::ScrolledWindow _listScrolledWindow;
  std::unique_ptr<CoverArtWidget> _coverArtWidget;
  std::unique_ptr<ImportProgressDialog> _importDialog;

  // Right side: stack for pages
  Gtk::Stack _stack;

  // Menu (placeholder)
  Gtk::PopoverMenuBar _menuBar;

  // List model for sidebar
  Glib::RefPtr<Gio::ListStore<ListRow>> _listStore;
  Glib::RefPtr<Gtk::SingleSelection> _listSelectionModel;

  // Track pages map
  std::map<rs::core::MusicLibrary::ListId, std::unique_ptr<TrackViewPage>> _trackPages;
  std::map<rs::core::MusicLibrary::ListId, std::unique_ptr<PlaylistExporter>> _playlistExporters;

  // List selection callback
  void onListSelectionChanged(std::uint32_t position, std::uint32_t nItems);
  void updateCoverArt(const std::vector<rs::core::MusicLibrary::TrackId>& selectedIds);

  // List context menu
  void showListContextMenu(Gtk::ListView& listView, const Gdk::Rectangle& rect);

  // Track context menu (tagging)
  void showTrackContextMenu(TrackViewPage& page, double x, double y);
  void tagSelectedTracks(TrackViewPage& page);

  void setupMenu();
  void setupLayout();
  void openLibrary();
  void openMusicLibrary(const std::filesystem::path& path);
  void importFiles();
  void importFilesFromPath(const std::filesystem::path& path);
  void scanDirectory(const std::filesystem::path& dir, std::vector<std::filesystem::path>& files);
  void createList(const rs::fbs::ListT& list);
  void onDeleteList();
  void onTagTrack();
  void setupTrackContextMenu();
  void loadAllTracks(rs::lmdb::ReadTransaction& txn);
  void loadLists(rs::lmdb::ReadTransaction& txn);
  void saveSession();
  void loadSession();
};
