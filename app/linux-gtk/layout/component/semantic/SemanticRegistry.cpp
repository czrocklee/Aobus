// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "SemanticRegistry.h"

#include "SemanticComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"

namespace ao::gtk::layout
{
  void registerSemanticComponents(ComponentRegistry& registry)
  {
    registerLabelComponent(registry);
    registerActionButtonComponent(registry);
    registerListTreeComponent(registry);
    registerTracksTableComponent(registry);
    registerOpenLibraryButtonComponent(registry);
    registerMenuBarComponent(registry);
    registerMenuButtonComponent(registry);
    // registerWorkspaceWithDetailPaneComponent(registry); // Unregistered in original SemanticComponents.cpp
  }
} // namespace ao::gtk::layout
