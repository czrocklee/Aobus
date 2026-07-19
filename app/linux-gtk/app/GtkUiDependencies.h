// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <giomm/menumodel.h>
#include <glibmm/refptr.h>

#include <functional>
#include <string>

namespace ao::uimodel
{
  class PlaybackCommandSurface;
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
   * Construction-scoped bundle of borrowed collaborators supplied by the GTK
   * application layer. Consumers unpack the collaborators they retain; the
   * bundle itself is not retained as subsystem wiring.
   */
  struct GtkUiDependencies final
  {
    TrackRowCache* trackRowCache = nullptr;
    ImageCache* imageCache = nullptr;
    uimodel::PlaybackCommandSurface* playbackCommandSurface = nullptr;
    TagEditController* tagEditController = nullptr;
    portal::ImportExportActions* importExportActions = nullptr;
    TrackPageHost* trackPageHost = nullptr;
    uimodel::TrackPresentationCatalog* trackPresentationCatalog = nullptr;
    uimodel::ListPresentationPreferenceStore* trackPresentationPreferences = nullptr;
    ListNavigationController* listNavigationController = nullptr;
    ThemeCoordinator* themeCoordinator = nullptr;

    /// Creates a smart list from a filter expression under the given parent (defaults to the
    /// navigation controller's dialog flow; injectable so components stay decoupled from it).
    std::function<void(ao::ListId parentListId, std::string expression)> createSmartListFromExpression{};

    /// Shell menu model, supplied post-construction by MainWindow (not part of coordinator wiring).
    Glib::RefPtr<Gio::MenuModel> menuModelPtr{};
  };
} // namespace ao::gtk
