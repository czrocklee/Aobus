// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <giomm/menumodel.h>
#include <gtkmm/window.h>

namespace ao::rt
{
  class AppSession;
}

namespace ao::gtk
{
  class TrackRowDataProvider;
  class CoverArtCache;
  class PlaybackController;
  class TagEditController;
  class ImportExportCoordinator;
  class TrackPageGraph;
  class TrackColumnLayoutModel;
  class StatusBar;
  class ListSidebarController;
} // namespace ao::gtk

namespace ao::gtk::layout
{
  class ComponentRegistry;
  class PlaybackUiProjection;

  /**
   * @brief Shared context for creating layout components.
   *
   * This is an explicit dependency injection structure, not a service locator.
   */
  struct ComponentContext final
  {
    ComponentRegistry const& registry;
    ao::rt::AppSession& session;
    Gtk::Window& parentWindow;

    ao::gtk::TrackRowDataProvider* rowDataProvider = nullptr;
    ao::gtk::CoverArtCache* coverArtCache = nullptr;
    PlaybackUiProjection* playbackUi = nullptr;

    ao::gtk::PlaybackController* playbackController = nullptr;
    ao::gtk::TagEditController* tagEditController = nullptr;
    ao::gtk::ImportExportCoordinator* importExportCoordinator = nullptr;
    ao::gtk::TrackPageGraph* trackPageGraph = nullptr;
    ao::gtk::TrackColumnLayoutModel* columnLayoutModel = nullptr;
    ao::gtk::ListSidebarController* listSidebarController = nullptr;
    ao::gtk::StatusBar* statusBar = nullptr;
    Glib::RefPtr<Gio::MenuModel> menuModel = {};

    std::function<void(std::string const& nodeId, int posX, int posY)> onNodeMoved = {};

    // Edit mode support
    bool editMode = false;
  };
} // namespace ao::gtk::layout
