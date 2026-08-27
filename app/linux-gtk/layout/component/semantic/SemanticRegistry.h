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

  /**
   * @brief Register semantic components for library and tracks.
   */
  void registerSemanticComponents(ComponentRegistry& registry,
                                  TrackRowCache* trackRowCache,
                                  TrackPageHost* trackPageHost,
                                  ListNavigationController* listNavigationController,
                                  portal::ImportExportActions* importExportActions,
                                  i18n::MessageCatalog const& textCatalog,
                                  Glib::RefPtr<Gio::MenuModel> const& menuModelPtr);
} // namespace ao::gtk::layout
