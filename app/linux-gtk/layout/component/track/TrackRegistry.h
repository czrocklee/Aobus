// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

namespace ao::gtk::layout
{
  class ComponentRegistry;

  /**
   * @brief Register core track components (quickFilter, presentationButton).
   */
  void registerTrackComponents(ComponentRegistry& registry);

  /**
   * @brief Register track detail components.
   */
  void registerTrackDetailComponents(ComponentRegistry& registry);

  /**
   * @brief Register track editor components.
   */
  void registerTrackEditorComponents(ComponentRegistry& registry);
} // namespace ao::gtk::layout
