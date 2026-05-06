// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "CoverArtCache.h"
#include "GtkMainThreadDispatcher.h"
#include "ImportExportCoordinator.h"
#include "InspectorSidebar.h"
#include "ListSidebarController.h"
#include "PlaybackController.h"
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
#include <runtime/AppSession.h>
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

  class MainWindow final : public Gtk::ApplicationWindow
  {
  public:
    explicit MainWindow(ao::app::AppSession& session);
    ~MainWindow() override;

    TrackPageContext const* currentVisibleTrackPageContext() const;
    TrackPageContext* findTrackPageContext(ao::ListId listId);
    void showListPage(ao::ListId listId);
    ImportExportCoordinator& importExportCoordinator() { return *_importExportCoordinator; }

    void initializeSession();

  private:
    void updateCoverArt(std::vector<ao::TrackId> const& selectedIds);

    void showStatusMessage(std::string const& message);

    void setupMenu();
    void setupLayout();

    // Page management helpers
    void rebuildListPages(ao::lmdb::ReadTransaction& txn);

    void onTrackSelectionChanged();
    void updateImportProgress(double fraction, std::string const& info);

    void saveSession();
    void loadSession();

    std::optional<ao::audio::TrackPlaybackDescriptor> currentSelectionPlaybackDescriptor() const;

    // GTK-side row data cache
    std::unique_ptr<TrackRowDataProvider> _rowDataProvider;

    SessionPersistence _sessionPersistence;

    ao::app::AppSession& _session;

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

    // Menu
    Gtk::PopoverMenuBar _menuBar;

    // Track pages graph
    std::unique_ptr<TrackPageGraph> _trackPageGraph;
    TrackColumnLayoutModel _trackColumnLayoutModel;

    // Tag editing
    std::unique_ptr<TagEditController> _tagEditController;

    // Playback support
    std::shared_ptr<GtkMainThreadDispatcher> _dispatcher;
    std::unique_ptr<PlaybackBar> _playbackBar;
    std::unique_ptr<PlaybackController> _playbackController;

    // Pending output selection from session restore (applied when runtime is ready)
    ao::audio::BackendId _pendingOutputBackend{};
    ao::audio::DeviceId _pendingOutputDevice{};
    ao::audio::ProfileId _pendingOutputProfile{};

    ao::app::Subscription _tracksMutatedSubscription;
    ao::app::Subscription _importProgressSubscription;
    ao::app::Subscription _importCompletedSubscription;

    // Status bar
    std::unique_ptr<StatusBar> _statusBar;

    // Inspector
    std::unique_ptr<InspectorSidebar> _inspectorSidebar;
    Gtk::Revealer _inspectorRevealer;
    Gtk::ToggleButton _inspectorHandle;

    std::unique_ptr<CoverArtCache> _coverArtCache;

    // Layout constants
    static constexpr int kCoverArtSize = 50;
  };
} // namespace ao::gtk
