// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "layout/runtime/LayoutContext.h"

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
} // namespace ao::gtk::layout
