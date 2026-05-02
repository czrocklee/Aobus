// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "GtkMainThreadDispatcher.h"
#include "ImportExportCoordinator.h"
#include "LibrarySession.h"
#include "ListSidebarController.h"
#include "PlaybackCoordinator.h"
#include "SessionPersistence.h"
#include "StatusBar.h"
#include "TagEditController.h"
#include "TrackPageGraph.h"
#include "TrackViewPage.h"
#include <ao/audio/Types.h>
#include <cstdint>
#include <filesystem>
#include <gtkmm.h>
#include <memory>
#include <optional>
#include <vector>

namespace ao::gtk::services
{
  class PlaylistExporter;
}

namespace ao::audio
{
  class Player;
}

namespace ao::gtk
{
  class TrackListAdapter;
  class TrackViewPage;
  class ListRow;
  class ListTreeNode;
  class CoverArtWidget;
  class ImportProgressDialog;
  class PlaybackBar;
  class TrackRowDataProvider;
  class TrackPageGraph;

  class MainWindow final
    : public Gtk::ApplicationWindow
    , public IPlaybackHost
  {
  public:
    MainWindow();
    ~MainWindow() override;

    // IPlaybackHost implementation
    TrackPageContext const* currentVisibleTrackPageContext() const override;
    TrackPageContext* findTrackPageContext(ao::ListId listId) override;
    void showListPage(ao::ListId listId) override;
    void updatePlaybackStatus(ao::audio::Snapshot const& snapshot) override;
    void showPlaybackMessage(std::string const& message,
                             std::optional<std::chrono::seconds> timeout = std::nullopt) override;

  private:
    void updateCoverArt(std::vector<ao::TrackId> const& selectedIds);

    void showStatusMessage(std::string const& message);

    void setupMenu();
    void setupLayout();
    void installLibrarySession(std::unique_ptr<LibrarySession> session);

    // Page management helpers
    void rebuildListPages(ao::lmdb::ReadTransaction& txn);

    void onTrackSelectionChanged();
    void updateImportProgress(double fraction, std::string const& info);

    void saveSession();
    void loadSession();

    // Playback support
    void jumpToPlayingList();
    void onOutputChanged(ao::audio::BackendKind kind, std::string const& deviceId);
    std::optional<ao::audio::TrackPlaybackDescriptor> currentSelectionPlaybackDescriptor() const;

    // Active library session
    std::unique_ptr<LibrarySession> _librarySession;

    SessionPersistence _sessionPersistence;

    // Layout: Horizontal paned with left box and right stack
    Gtk::Paned _paned;

    // Left side: vertical box with list + cover art
    Gtk::Box _leftBox;
    std::unique_ptr<ListSidebarController> _listSidebarController;
    std::unique_ptr<CoverArtWidget> _coverArtWidget;

    // Import/Export orchestration
    std::unique_ptr<ImportExportCoordinator> _importExportCoordinator;

    // Right side: stack for pages
    Gtk::Stack _stack;

    // Menu (placeholder)
    Gtk::PopoverMenuBar _menuBar;

    // Track pages graph
    std::unique_ptr<TrackPageGraph> _trackPageGraph;
    TrackColumnLayoutModel _trackColumnLayoutModel;

    // Tag editing
    std::unique_ptr<TagEditController> _tagEditController;

    // Playback support
    std::shared_ptr<GtkMainThreadDispatcher> _dispatcher;
    std::unique_ptr<PlaybackCoordinator> _playbackCoordinator;

    // Status bar
    std::unique_ptr<StatusBar> _statusBar;

    // Layout constants
    static constexpr int kCoverArtSize = 50;
  };
} // namespace ao::gtk
