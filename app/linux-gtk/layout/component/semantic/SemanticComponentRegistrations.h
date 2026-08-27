// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <giomm/menumodel.h>
#include <glibmm/refptr.h>

namespace ao::gtk
{
  class TrackRowCache;
  class ListNavigationController;
  class TrackPageHost;
  namespace portal
  {
    class ImportExportActions;
  }
} // namespace ao::gtk
namespace ao::i18n
{
  class MessageCatalog;
}

namespace ao::gtk::layout
{
  class ComponentRegistry;

  void registerLabelComponent(ComponentRegistry& registry);
  void registerActionButtonComponent(ComponentRegistry& registry);
  void registerListTreeComponent(ComponentRegistry& registry,
                                 TrackRowCache* trackRowCache,
                                 ListNavigationController* listNavigationController);
  void registerTracksTableComponent(ComponentRegistry& registry, TrackPageHost* trackPageHost);
  void registerOpenLibraryButtonComponent(ComponentRegistry& registry,
                                          portal::ImportExportActions* importExportActions,
                                          i18n::MessageCatalog const& textCatalog);
  void registerMenuBarComponent(ComponentRegistry& registry, Glib::RefPtr<Gio::MenuModel> const& menuModelPtr);
  void registerMenuButtonComponent(ComponentRegistry& registry,
                                   Glib::RefPtr<Gio::MenuModel> const& menuModelPtr,
                                   i18n::MessageCatalog const& textCatalog);
  void registerWorkspaceWithDetailPaneComponent(ComponentRegistry& registry,
                                                TrackPageHost* trackPageHost,
                                                i18n::MessageCatalog const& textCatalog);
} // namespace ao::gtk::layout
