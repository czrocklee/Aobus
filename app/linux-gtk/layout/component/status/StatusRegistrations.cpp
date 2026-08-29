// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/component/ComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/library/Library.h>

namespace ao::gtk::layout
{
  void registerStatusComponents(ComponentRegistry& registry,
                                rt::AppRuntime& runtime,
                                i18n::MessageCatalog const& textCatalog)
  {
    registerPlaybackDetailsComponent(registry, runtime.playback(), textCatalog);
    registerNowPlayingStatusComponent(registry, runtime.playback(), textCatalog);
    registerActivityStatusComponent(registry, runtime.notifications(), runtime.library().jobs(), textCatalog);
    registerSelectionInfoComponent(registry, runtime.views(), textCatalog);
    registerLibraryTrackCountComponent(registry, runtime, textCatalog);
    registerStatusMessageLabelComponent(registry, textCatalog);
  }
} // namespace ao::gtk::layout
