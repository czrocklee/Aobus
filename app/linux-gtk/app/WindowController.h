// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "inspector/CoverArtCache.h"
#include "library_io/ImportExportCoordinator.h"
#include "list/ListSidebarController.h"
#include "playback/PlaybackSequenceController.h"
#include "shell/UIState.h"
#include "tag/TagEditController.h"
#include "track/TrackPageManager.h"
#include "track/TrackPresentation.h"
#include "track/TrackRowCache.h"
#include <memory>
#include <runtime/AppSession.h>
#include <runtime/ConfigStore.h>

namespace ao::gtk
{
  class MainWindow;

  class WindowController final
  {
  public:
    WindowController(MainWindow& window, ao::rt::AppSession& session, std::shared_ptr<ao::rt::ConfigStore> configStore);
    ~WindowController();

    void initializeSession();

    void saveSession();
    void loadSession();

    void rebuildListPages(ao::lmdb::ReadTransaction const& txn);

    TrackRowCache* trackRowCache() { return _trackRowCache.get(); }
    CoverArtCache* coverArtCache() { return _coverArtCache.get(); }
    PlaybackSequenceController* playbackSequenceController() { return _playbackSequenceController.get(); }
    TagEditController* tagEditController() { return _tagEditController.get(); }
    ImportExportCoordinator* importExportCoordinator() { return _importExportCoordinator.get(); }
    TrackPageManager* trackPageManager() { return _trackPageManager.get(); }
    TrackColumnLayoutModel* columnLayoutModel() { return &_trackColumnLayoutModel; }
    ListSidebarController* listSidebarController() { return _listSidebarController.get(); }

    ImportExportCoordinator& importExport() { return *_importExportCoordinator; }

  private:
    MainWindow& _window;
    ao::rt::AppSession& _session;
    std::shared_ptr<ao::rt::ConfigStore> _configStore;

    std::unique_ptr<TrackRowCache> _trackRowCache;
    std::unique_ptr<CoverArtCache> _coverArtCache;
    std::unique_ptr<TagEditController> _tagEditController;
    std::unique_ptr<ListSidebarController> _listSidebarController;
    std::unique_ptr<TrackPageManager> _trackPageManager;
    std::unique_ptr<PlaybackSequenceController> _playbackSequenceController;
    std::unique_ptr<ImportExportCoordinator> _importExportCoordinator;

    Gtk::Stack _stack;
    TrackColumnLayoutModel _trackColumnLayoutModel;

    ao::rt::Subscription _tracksMutatedSubscription;
    ao::rt::Subscription _importProgressSubscription;
    ao::rt::Subscription _importCompletedSubscription;
    ao::rt::Subscription _listsMutatedSubscription;
  };
} // namespace ao::gtk
