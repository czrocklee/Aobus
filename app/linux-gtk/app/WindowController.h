// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/UIState.h"
#include "inspector/CoverArtCache.h"
#include "library_io/ImportExportCoordinator.h"
#include "list/ListSidebarController.h"
#include "playback/PlaybackSequenceController.h"
#include "tag/TagEditController.h"
#include "track/TrackPageManager.h"
#include "track/TrackPresentation.h"
#include "track/TrackPresentationStore.h"
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
    WindowController(MainWindow& window, rt::AppSession& session, std::shared_ptr<rt::ConfigStore> configStore);
    ~WindowController();

    void initializeSession();

    void saveSession();
    void loadSession();

    void rebuildListPages(lmdb::ReadTransaction const& txn);

    TrackRowCache* trackRowCache() { return _trackRowCache.get(); }
    CoverArtCache* coverArtCache() { return _coverArtCache.get(); }
    PlaybackSequenceController* playbackSequenceController() { return _playbackSequenceController.get(); }
    TagEditController* tagEditController() { return _tagEditController.get(); }
    ImportExportCoordinator* importExportCoordinator() { return _importExportCoordinator.get(); }
    TrackPageManager* trackPageManager() { return _trackPageManager.get(); }
    TrackColumnLayoutModel* columnLayoutModel() { return &_trackColumnLayoutModel; }
    ListSidebarController* listSidebarController() { return _listSidebarController.get(); }
    TrackPresentationStore* trackPresentationStore() { return _trackPresentationStore.get(); }

    ImportExportCoordinator& importExport() { return *_importExportCoordinator; }

  private:
    MainWindow& _window;
    rt::AppSession& _session;
    std::shared_ptr<rt::ConfigStore> _configStore;

    std::unique_ptr<TrackRowCache> _trackRowCache;
    std::unique_ptr<CoverArtCache> _coverArtCache;
    std::unique_ptr<TagEditController> _tagEditController;
    std::unique_ptr<ListSidebarController> _listSidebarController;
    std::unique_ptr<TrackPresentationStore> _trackPresentationStore;
    std::unique_ptr<TrackPageManager> _trackPageManager;
    std::unique_ptr<PlaybackSequenceController> _playbackSequenceController;
    std::unique_ptr<ImportExportCoordinator> _importExportCoordinator;

    Gtk::Stack _stack;
    TrackColumnLayoutModel _trackColumnLayoutModel;

    rt::Subscription _tracksMutatedSubscription;
    rt::Subscription _importProgressSubscription;
    rt::Subscription _importCompletedSubscription;
    rt::Subscription _listsMutatedSubscription;
  };
} // namespace ao::gtk
