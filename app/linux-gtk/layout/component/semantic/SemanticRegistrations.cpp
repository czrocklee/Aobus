// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/component/ComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"

namespace ao::gtk::layout
{
  void registerSemanticComponents(ComponentRegistry& registry,
                                  TrackPageHost* trackPageHost,
                                  ListNavigationController* listNavigationController,
                                  portal::ImportExportActions* importExportActions,
                                  i18n::MessageCatalog const& textCatalog,
                                  Glib::RefPtr<Gio::MenuModel> const& menuModelPtr)
  {
    registerLabelComponent(registry);
    registerActionButtonComponent(registry);
    registerListTreeComponent(registry, listNavigationController);
    registerTracksTableComponent(registry, trackPageHost);
    registerOpenLibraryButtonComponent(registry, importExportActions, textCatalog);
    registerMenuBarComponent(registry, menuModelPtr);
    registerMenuButtonComponent(registry, menuModelPtr, textCatalog);
    registerWorkspaceWithDetailPaneComponent(registry, trackPageHost, textCatalog);
  }
} // namespace ao::gtk::layout
