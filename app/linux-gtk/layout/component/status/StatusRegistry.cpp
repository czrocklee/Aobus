// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "StatusRegistry.h"

#include "StatusComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"

namespace ao::gtk::layout
{
  void registerStatusComponents(ComponentRegistry& registry)
  {
    registerPlaybackDetailsComponent(registry);
    registerNowPlayingStatusComponent(registry);
    registerStatusSlotComponent(registry);
    registerLibraryTrackCountComponent(registry);
    registerStatusMessageLabelComponent(registry);
  }
} // namespace ao::gtk::layout
