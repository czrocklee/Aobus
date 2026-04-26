// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/AppConfig.h"
#include "core/model/SmartListEngine.h"
#include "core/playback/PlaybackTypes.h"
#include "platform/linux/ui/StatusBar.h"
#include "platform/linux/ui/TrackViewPage.h"

#include <rs/core/MusicLibrary.h>

#include <gtkmm.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace app::core::model
{
  class ListDraft;
  class AllTrackIdsList;
  class FilteredTrackIdList;
  class ManualTrackIdList;
  class TrackIdList;
}

namespace app::core
{
  class ImportWorker;
}

namespace app::services
{
  class PlaylistExporter;
}

namespace app::core::playback
{
  class PlaybackController;
}

namespace app::ui
{
  class TrackListAdapter;
  class TrackViewPage;
  class ListRow;
  class ListTreeNode;
  class CoverArtWidget;
  class ImportProgressDialog;
  class PlaybackBar;
  class TrackRowDataProvider;

  // Page context structure
  struct TrackPageContext final
  {
    std::unique_ptr<::app::core::model::TrackIdList> membershipList;
    std::unique_ptr<TrackListAdapter> adapter;
    std::unique_ptr<TrackViewPage> page;
    std::unique_ptr<::app::services::PlaylistExporter> exporter;
  };

  struct ActivePlaybackSequence final
  {
    std::vector<rs::core::TrackId> trackIds;
    std::size_t currentIndex = 0;
    std::optional<rs::core::ListId> sourceListId;
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
    void showTagEditor(TrackViewPage& page, std::vector<rs::core::TrackId> selectedIds, double x, double y);
    void addTagToCurrentSelection(std::string const& tag);
    void removeTagFromCurrentSelection(std::string const& tag);
    void applyTagChangeToCurrentSelection(std::vector<std::string> const& tagsToAdd,
                                          std::vector<std::string> const& tagsToRemove);
    void showStatusMessage(std::string const& message);

    void setupMenu();
    void setupLayout();
    void openLibrary();
    void openMusicLibrary(std::filesystem::path const& path);
    void importFiles();
    void importFilesFromPath(std::filesystem::path const& path);
    void exportLibrary();
    void importLibrary();
    void scanDirectory(std::filesystem::path const& dir, std::vector<std::filesystem::path>& files);
    void openNewListDialog(rs::core::ListId parentListId);
    void openNewSmartListDialog();
    void openEditListDialog(rs::core::ListId listId);
    bool listHasChildren(rs::core::ListId listId) const;

    // List management - using ListDraft
    void createList(::app::core::model::ListDraft const& draft);
    void updateList(::app::core::model::ListDraft const& draft);
    void onDeleteList();
    void onEditList();

    void setupTrackContextMenu();

    // Page management helpers
    void clearTrackPages();
    void buildListTree(rs::lmdb::ReadTransaction& txn);
    void rebuildListPages(rs::lmdb::ReadTransaction& txn);
    void buildPageForAllTracks();
    void buildPageForStoredList(rs::core::ListId listId, rs::core::ListView const& view);

    // Notification handlers from AllTrackIdsList
    void notifyTracksInserted(std::vector<rs::core::TrackId> const& ids);
    void notifyTracksUpdated(std::vector<rs::core::TrackId> const& ids);
    void notifyTracksRemoved(std::vector<rs::core::TrackId> const& ids);

    void onTrackSelectionChanged();
    void updateImportProgress(double fraction, std::string const& info);

    void saveSession();
    void loadSession();

    // Playback support
    void setupPlayback();
    void refreshPlaybackBar();
    void onPlayRequested();
    void onPauseRequested();
    void onStopRequested();
    void onSeekRequested(std::uint32_t positionMs);
    void playCurrentSelection();
    void pausePlayback();
    void stopPlayback();
    void seekPlayback(std::uint32_t positionMs);
    bool startPlaybackFromVisiblePage(TrackViewPage const& page, rs::core::TrackId trackId);
    bool startPlaybackSequence(std::vector<rs::core::TrackId> trackIds,
                               rs::core::TrackId startTrackId,
                               std::optional<rs::core::ListId> sourceListId = std::nullopt);
    bool playTrackAtSequenceIndex(std::size_t index);
    void jumpToPlayingList();
    void onOutputChanged(app::core::playback::BackendKind kind, std::string const& deviceId);
    void clearActivePlaybackSequence();
    void handlePlaybackFinished();
    void bindTrackPagePlayback(TrackViewPage& page);
    TrackPageContext* currentVisibleTrackPageContext();
    TrackPageContext const* currentVisibleTrackPageContext() const;
    std::optional<::app::core::playback::TrackPlaybackDescriptor> currentSelectionPlaybackDescriptor() const;

    // Music library instance
    std::unique_ptr<rs::core::MusicLibrary> _musicLibrary;

    // Shared row data provider (owned)
    std::unique_ptr<TrackRowDataProvider> _rowDataProvider;

    // All tracks TrackId list (owned)
    std::unique_ptr<::app::core::model::AllTrackIdsList> _allTrackIds;

    // Smart list engine for shared evaluation
    std::unique_ptr<::app::core::model::SmartListEngine> _smartListEngine;
    ::app::core::AppConfig _appConfig;

    // Layout: Horizontal paned with left box and right stack
    Gtk::Paned _paned;

    // Left side: vertical box with list + cover art
    Gtk::Box _leftBox;
    Gtk::ListView _listView;
    Gtk::ScrolledWindow _listScrolledWindow;
    std::unique_ptr<CoverArtWidget> _coverArtWidget;
    std::unique_ptr<ImportProgressDialog> _importDialog;

    // Import worker - owned and joined on window destruction
    std::unique_ptr<::app::core::ImportWorker> _importWorker;
    std::jthread _importThread;

    // Right side: stack for pages
    Gtk::Stack _stack;

    // Menu (placeholder)
    Gtk::PopoverMenuBar _menuBar;
    Gtk::PopoverMenu _listContextMenu;

    // List model for sidebar - tree model
    Glib::RefPtr<Gio::ListStore<ListTreeNode>> _listTreeStore;
    Glib::RefPtr<Gtk::TreeListModel> _treeListModel;
    Glib::RefPtr<Gtk::SingleSelection> _listSelectionModel;
    std::map<rs::core::ListId, Glib::RefPtr<ListTreeNode>> _nodesById;
    Glib::RefPtr<Gio::SimpleAction> _newListAction;
    Glib::RefPtr<Gio::SimpleAction> _deleteListAction;
    Glib::RefPtr<Gio::SimpleAction> _editListAction;
    Glib::RefPtr<Gio::SimpleAction> _trackTagAddAction;
    Glib::RefPtr<Gio::SimpleAction> _trackTagRemoveAction;
    Glib::RefPtr<Gio::SimpleAction> _trackTagToggleAction;

    // Track pages map
    std::map<rs::core::ListId, TrackPageContext> _trackPages;
    TrackColumnLayoutModel _trackColumnLayoutModel;

    // Playback support
    std::unique_ptr<PlaybackBar> _playbackBar;
    std::unique_ptr<::app::core::playback::PlaybackController> _playbackController;
    std::uint32_t _playbackTimer = 0;
    std::optional<ActivePlaybackSequence> _activePlaybackSequence;
    ::app::core::playback::TransportState _lastPlaybackState = ::app::core::playback::TransportState::Idle;

    // Status bar
    std::unique_ptr<StatusBar> _statusBar;
  };

} // namespace app::ui
