// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

namespace ao::winui::layout
{
  class ComponentRegistry;

  /// Each family registers the Windows components the two built-in presets name.
  void registerContainerComponents(ComponentRegistry& registry);
  void registerGenericComponents(ComponentRegistry& registry);
  void registerPlaybackComponents(ComponentRegistry& registry);
  void registerShellComponents(ComponentRegistry& registry);
  void registerStatusComponents(ComponentRegistry& registry);
  void registerTrackComponents(ComponentRegistry& registry);
} // namespace ao::winui::layout
