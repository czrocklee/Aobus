// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/GtkUiServices.h"

#include <giomm/menumodel.h>
#include <glibmm/refptr.h>
#include <gtkmm/window.h>

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
    class ImportExportCoordinator;
  }

  class TrackPageHost;
  class ListNavigationController;
} // namespace ao::gtk

namespace ao::uimodel::playback
{
  class PlaybackQueueModel;
}

namespace ao::uimodel::track
{
  class TrackPresentationViewModel;
}

namespace ao::gtk
{
  class ThemeCoordinator;
}

namespace ao::gtk::layout
{
  class ITrackDetailScope;
  class ComponentRegistry;
  class ActionRegistry;

  struct TrackUiContext final
  {
    TrackPageHost* pageHost = nullptr;
    uimodel::track::TrackPresentationViewModel* presentationStore = nullptr;
    TrackRowCache* trackRowCache = nullptr;
    ITrackDetailScope* detailScope = nullptr;
  };

  struct ListUiContext final
  {
    ListNavigationController* navigationController = nullptr;
  };

  struct PlaybackUiContext final
  {
    uimodel::playback::PlaybackQueueModel* queueModel = nullptr;
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
    Glib::RefPtr<Gio::MenuModel> menuModelPtr;
  };

  struct PortalContext final
  {
    portal::ImportExportCoordinator* coordinator = nullptr;
  };

  struct ThemeUiContext final
  {
    ThemeCoordinator* themeController = nullptr;
  };

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

    TrackUiContext track{};
    ListUiContext list{};
    PlaybackUiContext playback{};
    DetailUiContext detail{};
    TagUiContext tag{};
    ShellUiContext shell{};
    PortalContext portal{};
    ThemeUiContext theme{};

    std::function<void(std::string const& nodeId, std::int32_t posX, std::int32_t posY)> onNodeMoved{};
    bool editMode = false;

    void bind(GtkUiServices const& services)
    {
      track.pageHost = services.trackPageHost;
      track.presentationStore = services.trackPresentationStore;
      track.trackRowCache = services.trackRowCache;
      list.navigationController = services.listNavigationController;
      playback.queueModel = services.playbackQueueModel;
      detail.imageCache = services.imageCache;
      tag.editController = services.tagEditController;
      portal.coordinator = services.importExportCoordinator;
      theme.themeController = services.themeController;
    }
  };
} // namespace ao::gtk::layout
