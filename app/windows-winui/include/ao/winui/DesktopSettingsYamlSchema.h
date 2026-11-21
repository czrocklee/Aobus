// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/winui/WindowPlacement.h>
#include <ao/winui/layout/ShellState.h>

#include <ryml.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace ao::winui
{
  inline constexpr auto kDesktopSettingsConfigGroup = std::string_view{"desktop"};
  inline constexpr std::uint32_t kDesktopSettingsVersion = 1;
  inline constexpr std::int32_t kMinimumWindowWidth = 640;
  inline constexpr std::int32_t kMinimumWindowHeight = 480;
  inline constexpr double kDefaultNavigationPaneWidth = 240.0;
  inline constexpr double kDefaultInspectorPaneWidth = 320.0;
  inline constexpr double kMinimumNavigationPaneWidth = 120.0;
  inline constexpr double kMinimumInspectorPaneWidth = 160.0;
  inline constexpr double kMaximumNavigationPaneWidth = 360.0;
  inline constexpr double kMaximumInspectorPaneWidth = 480.0;

  struct DesktopSettings final
  {
    std::uint32_t version = kDesktopSettingsVersion;
    WindowPlacement window{};
    ShellMode shellMode = ShellMode::Modern;
    std::string lastLibraryPath{};
    audio::OutputDeviceSelection preferredOutputSelection{};
    double navigationPaneWidth = kDefaultNavigationPaneWidth;
    double inspectorPaneWidth = kDefaultInspectorPaneWidth;

    friend bool operator==(DesktopSettings const&, DesktopSettings const&) = default;
  };

  /**
   * @brief Remember captured normal/restore bounds without invalidating desktop settings.
   *
   * Floors width and height independently to the persisted minimum. Position,
   * maximized state and unrelated preferences are preserved. This changes only
   * the live settings; it neither resizes the native window nor writes a file.
   */
  void rememberDesktopWindowPlacement(DesktopSettings& settings, WindowPlacement placement) noexcept;

  /// Protected configuration (decision 0020): an older build never overwrites a newer group.
  struct DesktopSettingsYamlSchema final
  {
    static constexpr std::uint32_t kVersion = kDesktopSettingsVersion;

    Result<> serialize(ryml::NodeRef node, DesktopSettings const& state) const;
    Result<DesktopSettings> deserialize(ryml::ConstNodeRef node, DesktopSettings const& seed) const;
  };
} // namespace ao::winui
