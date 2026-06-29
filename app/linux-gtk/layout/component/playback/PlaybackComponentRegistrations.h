// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "layout/runtime/LayoutContext.h"
#include "playback/TransportButton.h"
#include <ao/rt/AppRuntime.h>

#include <functional>

namespace ao::gtk::layout
{
  class ComponentRegistry;

  void registerOutputDeviceSelectorComponent(ComponentRegistry& registry);
  void registerPlaybackImageComponent(ComponentRegistry& registry);
  void registerSoulTransportButtonComponent(ComponentRegistry& registry);
  void registerSoulButtonComponent(ComponentRegistry& registry);
  void registerTransportButtonComponent(ComponentRegistry& registry);
  void registerVolumeControlComponent(ComponentRegistry& registry);
  void registerNowPlayingFieldComponent(ComponentRegistry& registry);
  void registerSeekSliderComponent(ComponentRegistry& registry);
  void registerTimeLabelComponent(ComponentRegistry& registry);
  void registerQualityIndicatorComponent(ComponentRegistry& registry);
  void registerAudioPipelinePanelComponent(ComponentRegistry& registry);

  /**
   * @brief Helper to get the transport button callback.
   */
  inline std::function<void()> getTransportCallback(LayoutContext& ctx, TransportButton::Action action)
  {
    if (action == TransportButton::Action::Play || action == TransportButton::Action::PlayPause)
    {
      return [&ctx] { ctx.runtime.playSelectionInFocusedView(); };
    }

    return {};
  }
} // namespace ao::gtk::layout
