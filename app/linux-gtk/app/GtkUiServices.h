// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

namespace ao::uimodel
{
  class PlaybackCommandSurface;
  class PlaybackQueueSession;
}
namespace ao::gtk
{
  class TrackRowCache;
  class ImageCache;
  class TagEditController;
  namespace portal
  {
    class ImportExportActions;
  }
  class TrackPageHost;
  class ListNavigationController;
  class ThemeCoordinator;
} // namespace ao::gtk
namespace ao::uimodel
{
  class TrackPresentationCatalog;
  class ListPresentationPreferenceStore;
}
namespace ao::gtk
{
  /**
   * A bag of services provided by the GTK application layer to be consumed by
   * the UI layout components.
   */
  struct GtkUiServices final
  {
    TrackRowCache* trackRowCache = nullptr;
    ImageCache* imageCache = nullptr;
    uimodel::PlaybackQueueSession* playbackQueueSession = nullptr;
    uimodel::PlaybackCommandSurface* playbackCommandSurface = nullptr;
    TagEditController* tagEditController = nullptr;
    portal::ImportExportActions* importExportCoordinator = nullptr;
    TrackPageHost* trackPageHost = nullptr;
    uimodel::TrackPresentationCatalog* trackPresentationCatalog = nullptr;
    uimodel::ListPresentationPreferenceStore* trackPresentationPreferences = nullptr;
    ListNavigationController* listNavigationController = nullptr;
    ThemeCoordinator* themeController = nullptr;
  };
} // namespace ao::gtk
