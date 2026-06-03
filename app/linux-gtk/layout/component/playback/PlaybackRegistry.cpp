// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "PlaybackRegistry.h"

#include "PlaybackComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"

namespace ao::gtk::layout
{
  void registerPlaybackComponents(ComponentRegistry& registry)
  {
    registerOutputSelectorComponent(registry);
    registerPlaybackImageComponent(registry);
    registerSoulTransportButtonComponent(registry);
    registerSoulButtonComponent(registry);
    registerTransportButtonComponent(registry);
    registerVolumeControlComponent(registry);
    registerNowPlayingFieldComponent(registry);
    registerSeekSliderComponent(registry);
    registerTimeLabelComponent(registry);
    registerQualityIndicatorComponent(registry);
    registerAudioPipelinePanelComponent(registry);
  }
} // namespace ao::gtk::layout
