// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "layout/runtime/ComponentRegistry.h"

namespace ao::gtk::layout
{
  /**
   * @brief Register track-related layout components (QuickFilter, PresentationButton).
   */
  void registerTrackComponents(ComponentRegistry& registry);
} // namespace ao::gtk::layout
