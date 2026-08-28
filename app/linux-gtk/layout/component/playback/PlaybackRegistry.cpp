// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "PlaybackRegistry.h"

#include "PlaybackComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include <ao/rt/AppRuntime.h>

namespace ao::gtk::layout
{
  void registerPlaybackComponents(ComponentRegistry& registry,
                                  rt::AppRuntime& runtime,
                                  uimodel::PlaybackActions* playbackActions,
                                  ResourceImageLoader* imageLoader,
                                  i18n::MessageCatalog const& textCatalog,
                                  uimodel::OutputDeviceIntent const& outputDeviceIntent)
  {
    registerOutputDeviceSelectorComponent(registry, runtime.playback(), textCatalog, outputDeviceIntent);
    registerPlaybackImageComponent(registry, runtime, imageLoader, textCatalog);
    registerSoulTransportButtonComponent(registry, runtime.playback(), playbackActions, textCatalog);
    registerSoulButtonComponent(registry, runtime.playback());
    registerTransportButtonComponent(registry, runtime.playback(), playbackActions, textCatalog);
    registerVolumeControlComponent(registry, runtime.playback(), textCatalog);
    registerNowPlayingFieldComponent(registry, runtime, textCatalog);
    registerSeekSliderComponent(registry, runtime.playback());
    registerTimeLabelComponent(registry, runtime.playback());
    registerQualityIndicatorComponent(registry, runtime);
    registerAudioPipelinePanelComponent(registry, runtime.playback(), textCatalog);
  }
} // namespace ao::gtk::layout
