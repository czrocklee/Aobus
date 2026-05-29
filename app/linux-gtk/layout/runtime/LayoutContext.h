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
  class TrackPresentationStore;
  class ListNavigationController;
} // namespace ao::gtk

namespace ao::gtk::layout
{
  class ComponentRegistry;
  class ActionRegistry;

  struct TrackUiContext final
  {
    TrackPageHost* pageHost = nullptr;
    TrackPresentationStore* presentationStore = nullptr;
    TrackRowCache* trackRowCache = nullptr;
  };

  struct ListUiContext final
  {
    ListNavigationController* sidebarController = nullptr;
  };

  struct PlaybackUiContext final
  {
    uimodel::playback::PlaybackQueueModel* queueModel = nullptr;
  };

  struct InspectorUiContext final
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

  struct LayoutContext final
  {
    ComponentRegistry const& registry;
    ActionRegistry const& actionRegistry;
    rt::AppRuntime& runtime;
    Gtk::Window& parentWindow;

    TrackUiContext track{};
    ListUiContext list{};
    PlaybackUiContext playback{};
    InspectorUiContext inspector{};
    TagUiContext tag{};
    ShellUiContext shell{};
    PortalContext portal{};

    std::function<void(std::string const& nodeId, std::int32_t posX, std::int32_t posY)> onNodeMoved{};
    bool editMode = false;

    void bind(GtkUiServices const& services)
    {
      track.pageHost = services.trackPageHost;
      track.presentationStore = services.trackPresentationStore;
      track.trackRowCache = services.trackRowCache;
      list.sidebarController = services.listSidebarController;
      playback.queueModel = services.playbackQueueModel;
      inspector.imageCache = services.imageCache;
      tag.editController = services.tagEditController;
      portal.coordinator = services.importExportCoordinator;
    }
  };
} // namespace ao::gtk::layout
