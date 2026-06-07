// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

namespace ao::gtk::layout
{
  class ComponentRegistry;

  void registerLabelComponent(ComponentRegistry& registry);
  void registerActionButtonComponent(ComponentRegistry& registry);
  void registerListTreeComponent(ComponentRegistry& registry);
  void registerTracksTableComponent(ComponentRegistry& registry);
  void registerOpenLibraryButtonComponent(ComponentRegistry& registry);
  void registerMenuBarComponent(ComponentRegistry& registry);
  void registerMenuButtonComponent(ComponentRegistry& registry);
  void registerWorkspaceWithDetailPaneComponent(ComponentRegistry& registry);
} // namespace ao::gtk::layout
