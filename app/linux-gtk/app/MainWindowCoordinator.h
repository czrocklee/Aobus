// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/GtkUiServices.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/CorePrimitives.h>

#include <gtkmm/stack.h>
#include <sigc++/scoped_connection.h>

#include <memory>

namespace ao::lmdb
{
  class ReadTransaction;
}

namespace ao::uimodel::playback
{
  class PlaybackQueueModel;
}
namespace ao::gtk
{
  class MainWindow;
  class AppConfig;
  class GtkLayoutConfig;
  class TrackRowCache;
  class ImageCache;
  class TagEditController;
  class ListNavigationController;
  class TrackPresentationStore;
  class TrackPageHost;
  namespace portal
  {
    class ImportExportCoordinator;
  }

  class MainWindowCoordinator final
  {
  public:
    MainWindowCoordinator(MainWindow& window, rt::AppRuntime& runtime, std::shared_ptr<AppConfig> configPtr);
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

    TrackRowCache* trackRowCache() { return _trackRowCachePtr.get(); }
    ImageCache* imageCache() { return _imageCachePtr.get(); }
    uimodel::playback::PlaybackQueueModel* playbackQueueModel() { return _playbackQueueModelPtr.get(); }
    TagEditController* tagEditController() { return _tagEditControllerPtr.get(); }
    portal::ImportExportCoordinator* importExportCoordinator() { return _importExportCoordinatorPtr.get(); }
    TrackPageHost* trackPageHost() { return _trackPageHostPtr.get(); }
    ListNavigationController* listSidebarController() { return _listSidebarControllerPtr.get(); }
    TrackPresentationStore* trackPresentationStore() { return _trackPresentationStorePtr.get(); }

    portal::ImportExportCoordinator& importExport() { return *_importExportCoordinatorPtr; }

  private:
    void saveColumnLayout();

    MainWindow& _window;
    rt::AppRuntime& _runtime;
    std::shared_ptr<AppConfig> _configPtr;
    std::unique_ptr<GtkLayoutConfig> _layoutConfigPtr;

    std::unique_ptr<TrackRowCache> _trackRowCachePtr;
    std::unique_ptr<ImageCache> _imageCachePtr;
    std::unique_ptr<TagEditController> _tagEditControllerPtr;
    std::unique_ptr<ListNavigationController> _listSidebarControllerPtr;
    std::unique_ptr<TrackPresentationStore> _trackPresentationStorePtr;
    std::unique_ptr<TrackPageHost> _trackPageHostPtr;
    std::unique_ptr<uimodel::playback::PlaybackQueueModel> _playbackQueueModelPtr;
    std::unique_ptr<portal::ImportExportCoordinator> _importExportCoordinatorPtr;

    Gtk::Stack _stack;

    rt::Subscription _tracksMutatedSubscription;
    rt::Subscription _libraryTaskProgressSubscription;
    rt::Subscription _libraryTaskCompletedSubscription;
    rt::Subscription _listsMutatedSubscription;
    sigc::scoped_connection _trackPresentationChangedConnection;
  };
} // namespace ao::gtk
