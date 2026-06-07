// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

namespace ao::gtk::layout
{
  class ComponentRegistry;

  void registerPlaybackDetailsComponent(ComponentRegistry& registry);
  void registerNowPlayingStatusComponent(ComponentRegistry& registry);
  void registerStatusSlotComponent(ComponentRegistry& registry);
  void registerLibraryTrackCountComponent(ComponentRegistry& registry);
  void registerStatusMessageLabelComponent(ComponentRegistry& registry);
} // namespace ao::gtk::layout
