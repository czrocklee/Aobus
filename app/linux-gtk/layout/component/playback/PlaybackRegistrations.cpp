// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/component/ComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include <ao/CoreIds.h>
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
    registerPlaybackImageComponent(
      registry,
      runtime.playback(),
      runtime.library(),
      [&runtime](TrackId const trackId) { return runtime.jumpToAlbum(trackId); },
      imageLoader,
      textCatalog);
    registerSoulTransportButtonComponent(registry, runtime.playback(), playbackActions, textCatalog);
    registerSoulButtonComponent(registry, runtime.playback());
    registerTransportButtonComponent(registry, runtime.playback(), playbackActions, textCatalog);
    registerVolumeControlComponent(registry, runtime.playback(), textCatalog);
    registerNowPlayingFieldComponent(registry, runtime.playback(), runtime.workspace(), textCatalog);
    registerSeekSliderComponent(registry, runtime.playback());
    registerTimeLabelComponent(registry, runtime.playback());
    registerQualityIndicatorComponent(registry, runtime.playback());
    registerAudioPipelinePanelComponent(registry, runtime.playback(), textCatalog);
  }
} // namespace ao::gtk::layout
