// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/playback/output/OutputDeviceIntent.h>

#include <giomm/menumodel.h>
#include <glibmm/refptr.h>

#include <functional>
#include <string>

namespace ao::uimodel
{
  class PlaybackCommandSurface;
  class TrackPresentationCatalog;
  class ListPresentationPreferenceStore;
} // namespace ao::uimodel

namespace ao::gtk
{
  class ListNavigationController;
  class ResourceImageLoader;
  class TagEditController;
  class ThemeCoordinator;
  class TrackPageHost;
  class TrackRowCache;

  namespace portal
  {
    class ImportExportActions;
  }

  /**
   * @brief The exact shell-owned collaborators the GTK layout session needs.
   *
   * Assembled once by the window that owns every member and consumed once:
   * component factories capture the collaborators they name at registration
   * time, while the shell copies its action dependencies into exact fields.
   * It is an argument object, not an ambient catalog: no owner stores or
   * re-exports the aggregate and no build traversal reaches it.
   *
   * A null pointer means the owner deliberately supplies no such collaborator,
   * which the components and actions that name it must tolerate.
   */
  struct ShellLayoutCollaborators final
  {
    i18n::MessageCatalog textCatalog;
    uimodel::PlaybackCommandSurface* playbackCommandSurface = nullptr;
    ThemeCoordinator* themeCoordinator = nullptr;
    TrackRowCache* trackRowCache = nullptr;
    ResourceImageLoader* imageLoader = nullptr;
    TagEditController* tagEditController = nullptr;
    portal::ImportExportActions* importExportActions = nullptr;
    TrackPageHost* trackPageHost = nullptr;
    uimodel::TrackPresentationCatalog* trackPresentationCatalog = nullptr;
    uimodel::ListPresentationPreferenceStore* trackPresentationPreferences = nullptr;
    ListNavigationController* listNavigationController = nullptr;
    uimodel::OutputDeviceIntent outputDeviceIntent = uimodel::OutputDeviceIntent::discarded();
    std::function<void(ao::ListId, std::string)> createSmartListFromExpression{};
    Glib::RefPtr<Gio::MenuModel> menuModelPtr{};
  };
} // namespace ao::gtk
