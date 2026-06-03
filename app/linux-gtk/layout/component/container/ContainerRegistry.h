// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "layout/runtime/ComponentRegistry.h"

namespace ao::gtk::layout
{
  /**
   * @brief Register the built-in container components.
   */
  void registerContainerComponents(ComponentRegistry& registry);
} // namespace ao::gtk::layout
