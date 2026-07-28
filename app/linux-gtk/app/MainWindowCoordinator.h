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
  class ListPresentationPreferenceStore;
}
namespace ao::rt
{
  class ResourceByteLoader;
}
namespace Gtk
{
  class Window;
}
namespace ao::gtk
{
  class AppConfigStore;
  class TrackRowCache;
  class ListNavigationController;

  namespace portal
  {
    class ImportExportCoordinator;
  }

  class MainWindowCoordinator final
  {
  public:
    MainWindowCoordinator(Gtk::Window& window, rt::AppRuntime& runtime, std::shared_ptr<AppConfigStore> configStorePtr);
    ~MainWindowCoordinator();

    // Not copyable or movable
    MainWindowCoordinator(MainWindowCoordinator const&) = delete;
    MainWindowCoordinator& operator=(MainWindowCoordinator const&) = delete;
    MainWindowCoordinator(MainWindowCoordinator&&) = delete;
    MainWindowCoordinator& operator=(MainWindowCoordinator&&) = delete;

    void prepareSession();
    void restorePlaybackSession();

    void saveSession();
    void loadSession();

    GtkUiDependencies uiDependencies();

    void rebuildListPages();

    TrackRowCache* trackRowCache();
    uimodel::PlaybackCommandSurface* playbackCommandSurface();
    ListNavigationController* listNavigationController();
    uimodel::ListPresentationPreferenceStore* trackPresentationPreferences();
    ThemeCoordinator* themeCoordinator();
    rt::ResourceByteLoader* resourceByteLoader();

    portal::ImportExportCoordinator& importExport();

  private:
    void saveColumnLayout();
    void saveColumnLayoutIfNotRestoring();

    Gtk::Window& _window;
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
