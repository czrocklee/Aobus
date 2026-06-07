// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

namespace ao::gtk::layout
{
  class ComponentRegistry;

  /**
   * @brief Register semantic components for library and tracks.
   */
  void registerSemanticComponents(ComponentRegistry& registry);
} // namespace ao::gtk::layout
