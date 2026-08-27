// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "SemanticRegistry.h"

#include "SemanticComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"

namespace ao::gtk::layout
{
  void registerSemanticComponents(ComponentRegistry& registry,
                                  TrackRowCache* trackRowCache,
                                  TrackPageHost* trackPageHost,
                                  ListNavigationController* listNavigationController,
                                  portal::ImportExportActions* importExportActions,
                                  i18n::MessageCatalog const& textCatalog,
                                  Glib::RefPtr<Gio::MenuModel> const& menuModelPtr)
  {
    registerLabelComponent(registry);
    registerActionButtonComponent(registry);
    registerListTreeComponent(registry, trackRowCache, listNavigationController);
    registerTracksTableComponent(registry, trackPageHost);
    registerOpenLibraryButtonComponent(registry, importExportActions, textCatalog);
    registerMenuBarComponent(registry, menuModelPtr);
    registerMenuButtonComponent(registry, menuModelPtr, textCatalog);
    // Deliberately not registered: workspace.withDetailPane reparents the same TrackPageHost::stack()
    // that track.table takes, and the explicit single-parent handoff for that shared stack is RFC 0002
    // Phase 4 work. Registering it here would offer it in the layout editor palette before then.
    // registerWorkspaceWithDetailPaneComponent(registry, trackPageHost, textCatalog);
  }
} // namespace ao::gtk::layout
