// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "layout/runtime/ComponentRegistry.h"

namespace ao::gtk::layout
{
  /**
   * Registers all status-related layout components:
   * - status.playbackDetails
   * - status.nowPlaying
   * - status.importProgress
   * - status.notification (includes selection info)
   * - status.trackCount
   * - status.messageLabel
   * - status.defaultBar (composite)
   */
  void registerStatusComponents(ComponentRegistry& registry);
} // namespace ao::gtk::layout
