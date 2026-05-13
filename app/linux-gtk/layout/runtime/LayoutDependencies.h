// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <giomm/menumodel.h>
#include <gtkmm/window.h>

#include <functional>
#include <string>

namespace ao::rt
{
  class AppSession;
}

namespace ao::gtk
{
  class TrackRowCache;
  class CoverArtCache;
  class PlaybackSequenceController;
  class TagEditController;
  class ImportExportCoordinator;
  class TrackPageManager;
  class TrackColumnLayoutModel;
  class ListSidebarController;
} // namespace ao::gtk

namespace ao::gtk::layout
{
  class ComponentRegistry;

  struct TrackUiContext final
  {
    TrackPageManager* pageManager = nullptr;
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

  struct LibraryIoContext final
  {
    ImportExportCoordinator* coordinator = nullptr;
  };

  struct LayoutDependencies final
  {
    ComponentRegistry const& registry;
    ao::rt::AppSession& session;
    Gtk::Window& parentWindow;

    TrackUiContext track{};
    ListUiContext list{};
    PlaybackUiContext playback{};
    InspectorUiContext inspector{};
    TagUiContext tag{};
    ShellUiContext shell{};
    LibraryIoContext libraryIo{};

    std::function<void(std::string const& nodeId, int posX, int posY)> onNodeMoved{};
    bool editMode = false;
  };
} // namespace ao::gtk::layout
