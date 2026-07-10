// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/GtkUiServices.h"
#include <ao/CoreIds.h>
#include <ao/uimodel/layout/component/LayoutComponentState.h>

#include <giomm/menumodel.h>
#include <glibmm/refptr.h>
#include <gtkmm/window.h>
#include <sigc++/connection.h>
#include <sigc++/functors/slot.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace ao::rt
{
  class AppRuntime;
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
} // namespace ao::gtk

namespace ao::uimodel
{
  class LayoutComponentStateStore;
}

namespace ao::uimodel
{
  class PlaybackCommandSurface;
}

namespace ao::rt
{
  class PlaybackQueueService;
}

namespace ao::uimodel
{
  class TrackPresentationCatalog;
  class ListPresentationPreferenceStore;
}

namespace ao::gtk
{
  class ThemeCoordinator;
}

namespace ao::gtk::layout
{
  class TrackDetailScope;
  class TrackDetailUndoController;
  class ComponentRegistry;
  class ActionRegistry;

  struct TrackUiContext final
  {
    TrackPageHost* pageHost = nullptr;
    uimodel::TrackPresentationCatalog* presentationCatalog = nullptr;
    uimodel::ListPresentationPreferenceStore* presentationPreferences = nullptr;
    TrackRowCache* trackRowCache = nullptr;
    TrackDetailScope* detailScope = nullptr;
    TrackDetailUndoController* detailUndo = nullptr;
  };

  struct ListUiContext final
  {
    ListNavigationController* navigationController = nullptr;
    std::function<void(ListId, std::string)> createSmartListFromExpression;
  };

  struct PlaybackUiContext final
  {
    rt::PlaybackQueueService* queue = nullptr;
    uimodel::PlaybackCommandSurface* commandSurface = nullptr;
  };

  struct DetailUiContext final
  {
    ImageCache* imageCache = nullptr;
  };

  struct TagUiContext final
  {
    TagEditController* editController = nullptr;
  };

  struct ShellUiContext final
  {
    Glib::RefPtr<Gio::MenuModel> menuModelPtr = {};
  };

  struct PortalContext final
  {
    portal::ImportExportActions* coordinator = nullptr;
  };

  struct ThemeUiContext final
  {
    ThemeCoordinator* themeController = nullptr;
  };

  /**
   * @brief Copies UI service pointers into their per-subsystem contexts.
   *
   * Kept free of GTK types so the mapping can be unit-tested without constructing
   * a window or runtime. LayoutContext::bind forwards to this.
   */
  inline void bindServices(TrackUiContext& track,
                           ListUiContext& list,
                           PlaybackUiContext& playback,
                           DetailUiContext& detail,
                           TagUiContext& tag,
                           PortalContext& portal,
                           ThemeUiContext& theme,
                           GtkUiServices const& services)
  {
    track.pageHost = services.trackPageHost;
    track.presentationCatalog = services.trackPresentationCatalog;
    track.presentationPreferences = services.trackPresentationPreferences;
    track.trackRowCache = services.trackRowCache;
    list.navigationController = services.listNavigationController;
    playback.queue = services.playbackQueue;
    playback.commandSurface = services.playbackCommandSurface;
    detail.imageCache = services.imageCache;
    tag.editController = services.tagEditController;
    portal.coordinator = services.importExportCoordinator;
    theme.themeController = services.themeController;
  }

  enum class LayoutSurface : std::uint8_t
  {
    Main,
    Tooltip,
  };

  struct LayoutContext final
  {
    LayoutSurface surface = LayoutSurface::Main;
    ComponentRegistry const& registry;
    ActionRegistry const& actionRegistry;
    rt::AppRuntime& runtime;
    Gtk::Window& parentWindow;
    std::string activePresetId{};
    uimodel::LayoutComponentStateDocument componentState{};
    uimodel::LayoutComponentStateStore* componentStateStore = nullptr;

    /**
     * @brief Monotonically incremented when the active component state document is replaced.
     *
     * Components capture the generation at construction time and refuse to write runtime
     * state if the context's current generation has moved on. This prevents stale
     * components (e.g. destructing during reset/load/save-defaults) from polluting a
     * freshly assigned state document.
     */
    std::uint64_t componentStateGeneration = 1;

    TrackUiContext track{};
    ListUiContext list{};
    PlaybackUiContext playback{};
    DetailUiContext detail{};
    TagUiContext tag{};
    ShellUiContext shell{};
    PortalContext portal{};
    ThemeUiContext theme{};

    std::function<sigc::connection(std::chrono::milliseconds, sigc::slot<bool()>)> timeoutScheduler{};
    std::function<void(std::string const& nodeId, std::int32_t xPosition, std::int32_t yPosition)> onNodeMoved{};
    bool editMode = false;

    void bind(GtkUiServices const& services)
    {
      bindServices(track, list, playback, detail, tag, portal, theme, services);
    }
  };
} // namespace ao::gtk::layout
