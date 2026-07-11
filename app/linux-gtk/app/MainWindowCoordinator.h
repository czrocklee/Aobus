// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/GtkUiServices.h"
#include "app/ThemeCoordinator.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/Subscription.h>

#include <memory>
#include <optional>

namespace ao::lmdb
{
  class ReadTransaction;
}

namespace ao::rt
{
  class PlaybackSequenceService;
}

namespace ao::uimodel
{
  class PlaybackCommandSurface;
}
namespace ao::uimodel
{
  class TrackPresentationCatalog;
  class ListPresentationPreferenceStore;
}
namespace ao::gtk
{
  class MainWindow;
  class AppConfigStore;
  class GtkLayoutConfig;
  class TrackRowCache;
  class ImageCache;
  class TagEditController;
  class ListNavigationController;
  class TrackPageHost;
  class ThemeCoordinator;

  namespace portal
  {
    class ImportExportCoordinator;
  }

  class MainWindowCoordinator final
  {
  public:
    MainWindowCoordinator(MainWindow& window, rt::AppRuntime& runtime, std::shared_ptr<AppConfigStore> configStorePtr);
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

    void rebuildListPages(lmdb::ReadTransaction const& transaction);

    TrackRowCache* trackRowCache();
    ImageCache* imageCache();
    rt::PlaybackSequenceService* playbackSequence();
    uimodel::PlaybackCommandSurface* playbackCommandSurface();
    TagEditController* tagEditController();
    portal::ImportExportCoordinator* importExportCoordinator();
    TrackPageHost* trackPageHost();
    ListNavigationController* listNavigationController();
    uimodel::TrackPresentationCatalog* trackPresentationCatalog();
    uimodel::ListPresentationPreferenceStore* trackPresentationPreferences();
    ThemeCoordinator* themeController();

    portal::ImportExportCoordinator& importExport();

  private:
    void saveColumnLayout();

    MainWindow& _window;
    rt::AppRuntime& _runtime;
    std::shared_ptr<AppConfigStore> _configStorePtr;
    struct Impl;
    std::unique_ptr<Impl> _implPtr;

    std::optional<ThemeRegistrationToken> _optThemeToken;

    rt::Subscription _tracksMutatedSubscription;
    rt::Subscription _libraryTaskCompletedSubscription;
    rt::Subscription _listsMutatedSubscription;
    rt::Subscription _trackPresentationChangedSubscription;
    rt::Subscription _trackColumnLayoutChangedSubscription;
  };
} // namespace ao::gtk
