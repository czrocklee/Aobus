// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/winui/layout/ShellStatePolicy.h>

#include <ryml.hpp>

#include <cstdint>
#include <string>

namespace ao::winui
{
  inline constexpr std::uint32_t kDesktopSettingsVersion = 3;
  inline constexpr std::int32_t kDefaultWindowX = 80;
  inline constexpr std::int32_t kDefaultWindowY = 80;
  inline constexpr std::int32_t kDefaultWindowWidth = 1280;
  inline constexpr std::int32_t kDefaultWindowHeight = 800;
  inline constexpr std::int32_t kMinimumWindowWidth = 640;
  inline constexpr std::int32_t kMinimumWindowHeight = 480;
  inline constexpr double kDefaultNavigationPaneWidth = 240.0;
  inline constexpr double kDefaultInspectorPaneWidth = 320.0;
  inline constexpr double kMinimumNavigationPaneWidth = 120.0;
  inline constexpr double kMinimumInspectorPaneWidth = 160.0;
  inline constexpr double kMaximumNavigationPaneWidth = 360.0;
  inline constexpr double kMaximumInspectorPaneWidth = 480.0;

  struct WindowPlacement final
  {
    std::int32_t x = kDefaultWindowX;
    std::int32_t y = kDefaultWindowY;
    std::int32_t width = kDefaultWindowWidth;
    std::int32_t height = kDefaultWindowHeight;
    bool maximized = false;

    friend bool operator==(WindowPlacement const&, WindowPlacement const&) = default;
  };

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

  struct DesktopSettingsYamlSchema final
  {
    Result<> serialize(ryml::NodeRef node, DesktopSettings const& state) const;
    Result<DesktopSettings> deserialize(ryml::ConstNodeRef node, DesktopSettings const& seed) const;
  };
} // namespace ao::winui
