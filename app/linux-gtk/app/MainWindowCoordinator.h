// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/GtkUiDependencies.h"
#include "app/ThemeCoordinator.h"
#include <ao/async/Subscription.h>
#include <ao/rt/AppRuntime.h>

#include <memory>
#include <optional>

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
  class GtkLayoutStateStore;
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

    GtkUiDependencies uiDependencies();

    void rebuildListPages();

    TrackRowCache* trackRowCache();
    ImageCache* imageCache();
    uimodel::PlaybackCommandSurface* playbackCommandSurface();
    TagEditController* tagEditController();
    portal::ImportExportCoordinator* importExportCoordinator();
    TrackPageHost* trackPageHost();
    ListNavigationController* listNavigationController();
    uimodel::TrackPresentationCatalog* trackPresentationCatalog();
    uimodel::ListPresentationPreferenceStore* trackPresentationPreferences();
    ThemeCoordinator* themeCoordinator();

    portal::ImportExportCoordinator& importExport();

  private:
    void saveColumnLayout();
    void saveColumnLayoutIfNotRestoring();

    MainWindow& _window;
    rt::AppRuntime& _runtime;
    std::shared_ptr<AppConfigStore> _configStorePtr;
    struct Impl;
    std::unique_ptr<Impl> _implPtr;

    std::optional<ThemeRegistrationToken> _optThemeToken;
    bool _restoringLayoutState = false;

    async::Subscription _tracksMutatedSubscription;
    async::Subscription _libraryTaskCompletedSubscription;
    async::Subscription _listsMutatedSubscription;
    async::Subscription _trackPresentationChangedSubscription;
    async::Subscription _trackColumnLayoutChangedSubscription;
  };
} // namespace ao::gtk
