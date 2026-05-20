// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/GtkUiServices.h"
#include "runtime/AppRuntime.h"
#include "runtime/CorePrimitives.h"
#include "track/TrackPresentation.h"

#include <gtkmm/stack.h>

#include <memory>

namespace ao::rt
{
  class ConfigStore;
}

namespace ao::lmdb
{
  class ReadTransaction;
}

namespace ao::gtk
{
  class MainWindow;
  class WindowStatePersistence;
  class TrackRowCache;
  class CoverArtCache;
  class TagEditController;
  class ListSidebarController;
  class TrackPresentationStore;
  class TrackPageHost;
  class PlaybackSequenceController;
  namespace portal
  {
    class ImportExportCoordinator;
  }

  class MainWindowCoordinator final
  {
  public:
    MainWindowCoordinator(MainWindow& window, rt::AppRuntime& runtime, std::shared_ptr<rt::ConfigStore> configStore);
    ~MainWindowCoordinator();

    // Not copyable or movable
    MainWindowCoordinator(MainWindowCoordinator const&) = delete;
    MainWindowCoordinator& operator=(MainWindowCoordinator const&) = delete;
    MainWindowCoordinator(MainWindowCoordinator&&) = delete;
    MainWindowCoordinator& operator=(MainWindowCoordinator&&) = delete;

    void initializeSession();

    void saveSession();
    void loadSession();

    GtkUiServices uiServices();

    void rebuildListPages(lmdb::ReadTransaction const& txn);

    TrackRowCache* trackRowCache() { return _trackRowCache.get(); }
    CoverArtCache* coverArtCache() { return _coverArtCache.get(); }
    PlaybackSequenceController* playbackSequenceController() { return _playbackSequenceController.get(); }
    TagEditController* tagEditController() { return _tagEditController.get(); }
    portal::ImportExportCoordinator* importExportCoordinator() { return _importExportCoordinator.get(); }
    TrackPageHost* trackPageHost() { return _trackPageHost.get(); }
    TrackColumnLayoutModel* columnLayoutModel() { return &_trackColumnLayoutModel; }
    ListSidebarController* listSidebarController() { return _listSidebarController.get(); }
    TrackPresentationStore* trackPresentationStore() { return _trackPresentationStore.get(); }

    portal::ImportExportCoordinator& importExport() { return *_importExportCoordinator; }

  private:
    MainWindow& _window;
    rt::AppRuntime& _runtime;
    std::shared_ptr<rt::ConfigStore> _configStore;
    std::unique_ptr<WindowStatePersistence> _persistence;

    std::unique_ptr<TrackRowCache> _trackRowCache;
    std::unique_ptr<CoverArtCache> _coverArtCache;
    std::unique_ptr<TagEditController> _tagEditController;
    std::unique_ptr<ListSidebarController> _listSidebarController;
    std::unique_ptr<TrackPresentationStore> _trackPresentationStore;
    std::unique_ptr<TrackPageHost> _trackPageHost;
    std::unique_ptr<PlaybackSequenceController> _playbackSequenceController;
    std::unique_ptr<portal::ImportExportCoordinator> _importExportCoordinator;

    Gtk::Stack _stack;
    TrackColumnLayoutModel _trackColumnLayoutModel;

    rt::Subscription _tracksMutatedSubscription;
    rt::Subscription _importProgressSubscription;
    rt::Subscription _importCompletedSubscription;
    rt::Subscription _listsMutatedSubscription;
  };
} // namespace ao::gtk
