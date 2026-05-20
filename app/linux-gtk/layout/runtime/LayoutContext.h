// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/GtkUiServices.h"

#include <giomm/menumodel.h>
#include <glibmm/refptr.h>
#include <gtkmm/window.h>

#include <functional>
#include <string>

namespace ao::rt
{
  class AppRuntime;
}

namespace ao::gtk
{
  class TrackRowCache;
  class CoverArtCache;
  class PlaybackSequenceController;
  class TagEditController;
  namespace portal
  {
    class ImportExportCoordinator;
  }
  class TrackPageHost;
  class TrackColumnLayoutModel;
  class ListSidebarController;
} // namespace ao::gtk

namespace ao::gtk::layout
{
  class ComponentRegistry;

  struct TrackUiContext final
  {
    TrackPageHost* pageHost = nullptr;
    TrackColumnLayoutModel* columnLayoutModel = nullptr;
    TrackRowCache* trackRowCache = nullptr;
  };

  struct ListUiContext final
  {
    ListSidebarController* sidebarController = nullptr;
  };

  struct PlaybackUiContext final
  {
    PlaybackSequenceController* sequenceController = nullptr;
  };

  struct InspectorUiContext final
  {
    CoverArtCache* coverArtCache = nullptr;
  };

  struct TagUiContext final
  {
    TagEditController* editController = nullptr;
  };

  struct ShellUiContext final
  {
    Glib::RefPtr<Gio::MenuModel> menuModel;
  };

  struct PortalContext final
  {
    portal::ImportExportCoordinator* coordinator = nullptr;
  };

  struct LayoutContext final
  {
    ComponentRegistry const& registry;
    rt::AppRuntime& runtime;
    Gtk::Window& parentWindow;

    TrackUiContext track{};
    ListUiContext list{};
    PlaybackUiContext playback{};
    InspectorUiContext inspector{};
    TagUiContext tag{};
    ShellUiContext shell{};
    PortalContext portal{};

    std::function<void(std::string const& nodeId, int posX, int posY)> onNodeMoved{};
    bool editMode = false;

    void bind(GtkUiServices const& services)
    {
      track.pageHost = services.trackPageHost;
      track.columnLayoutModel = services.columnLayoutModel;
      track.trackRowCache = services.trackRowCache;
      list.sidebarController = services.listSidebarController;
      playback.sequenceController = services.playbackSequenceController;
      inspector.coverArtCache = services.coverArtCache;
      tag.editController = services.tagEditController;
      portal.coordinator = services.importExportCoordinator;
    }
  };
} // namespace ao::gtk::layout
