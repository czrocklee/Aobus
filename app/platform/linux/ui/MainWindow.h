// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "platform/linux/ui/GtkMainThreadDispatcher.h"
#include "platform/linux/ui/ImportExportCoordinator.h"
#include "platform/linux/ui/LibrarySession.h"
#include "platform/linux/ui/ListSidebarController.h"
#include "platform/linux/ui/PlaybackCoordinator.h"
#include "platform/linux/ui/SessionPersistence.h"
#include "platform/linux/ui/StatusBar.h"
#include "platform/linux/ui/TagEditController.h"
#include "platform/linux/ui/TrackPageGraph.h"
#include "platform/linux/ui/TrackViewPage.h"
#include <cstdint>
#include <filesystem>
#include <gtkmm.h>
#include <memory>
#include <optional>
#include <rs/audio/PlaybackTypes.h>
#include <vector>

namespace app::services
{
  class PlaylistExporter;
}

namespace rs::audio
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
    TrackPageContext* findTrackPageContext(rs::ListId listId) override;
    void showListPage(rs::ListId listId) override;
    void updatePlaybackStatus(rs::audio::PlaybackSnapshot const& snapshot) override;
    void showPlaybackMessage(std::string const& message,
                             std::optional<std::chrono::seconds> timeout = std::nullopt) override;

  private:
    void updateCoverArt(std::vector<rs::TrackId> const& selectedIds);

    void showStatusMessage(std::string const& message);

    void setupMenu();
    void setupLayout();
    void installLibrarySession(std::unique_ptr<LibrarySession> session);

    // Page management helpers
    void rebuildListPages(rs::lmdb::ReadTransaction& txn);

    void onTrackSelectionChanged();
    void updateImportProgress(double fraction, std::string const& info);

    void saveSession();
    void loadSession();

    // Playback support
    void jumpToPlayingList();
    void onOutputChanged(rs::audio::BackendKind kind, std::string const& deviceId);
    std::optional<rs::audio::TrackPlaybackDescriptor> currentSelectionPlaybackDescriptor() const;

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

} // namespace app::ui
