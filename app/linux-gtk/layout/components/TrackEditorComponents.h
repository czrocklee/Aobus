// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "layout/runtime/ComponentRegistry.h"

namespace ao::gtk::layout
{
  /**
   * @brief Register track editor layout components (e.g. tag editor).
   */
  void registerTrackEditorComponents(ComponentRegistry& registry);
} // namespace ao::gtk::layout
