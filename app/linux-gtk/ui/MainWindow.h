// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "CoverArtCache.h"
#include "GtkMainThreadDispatcher.h"
#include "ImportExportCoordinator.h"
#include "InspectorSidebar.h"
#include "ListSidebarController.h"
#include "PlaybackController.h"
#include "StatusBar.h"
#include "TagEditController.h"
#include "TrackPageGraph.h"
#include "TrackViewPage.h"
#include "UIState.h"
#include <ao/audio/Types.h>
#include <runtime/AppSession.h>
#include <runtime/ConfigStore.h>

#include "layout/ComponentContext.h"
#include "layout/ComponentRegistry.h"
#include "layout/LayoutHost.h"

#include <gtkmm.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace ao::gtk::service
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
    explicit MainWindow(ao::rt::AppSession& session, std::shared_ptr<ao::rt::ConfigStore> configStore);
    ~MainWindow() override;

    void on_hide() override;

    ImportExportCoordinator& importExportCoordinator() { return *_importExportCoordinator; }

    void initializeSession();

  private:
    void showStatusMessage(std::string_view message);

    void setupMenu();
    void setupLayout();

    // Page management helpers
    void rebuildListPages(ao::lmdb::ReadTransaction const& txn);

    void updateImportProgress(double fraction, std::string_view info);

    void saveSession();
    void loadSession();

    void setupLayoutHost();
    void openLayoutEditor();

    // Layout constants
    static constexpr int kCoverArtSize = 50;

    // GTK-side row data cache
    std::unique_ptr<TrackRowDataProvider> _rowDataProvider;

    std::shared_ptr<ao::rt::ConfigStore> _configStore;

    ao::rt::AppSession& _session;

    // Layout system
    layout::ComponentRegistry _componentRegistry;
    layout::ComponentContext _componentContext;
    layout::LayoutHost _layoutHost;
    layout::LayoutDocument _activeLayout;

    // Left side: controllers
    std::unique_ptr<ListSidebarController> _listSidebarController;

    // Import/Export orchestration
    std::unique_ptr<ImportExportCoordinator> _importExportCoordinator;

    // Right side: stack for pages
    Gtk::Stack _stack;

    // Track pages graph
    std::unique_ptr<TrackPageGraph> _trackPageGraph;
    TrackColumnLayoutModel _trackColumnLayoutModel;

    // Tag editing
    std::unique_ptr<TagEditController> _tagEditController;

    // Playback support
    std::shared_ptr<GtkMainThreadDispatcher> _dispatcher;
    std::unique_ptr<PlaybackController> _playbackController;

    ao::rt::Subscription _tracksMutatedSubscription;
    ao::rt::Subscription _importProgressSubscription;
    ao::rt::Subscription _importCompletedSubscription;
    ao::rt::Subscription _listsMutatedSubscription;

    // Status bar
    std::unique_ptr<StatusBar> _statusBar;

    // Inspector support
    Gtk::Revealer _inspectorRevealer;
    Gtk::ToggleButton _inspectorHandle;

    std::unique_ptr<CoverArtCache> _coverArtCache;
  };
} // namespace ao::gtk
