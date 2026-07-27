// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/uimodel/layout/shell/DesktopShellPolicy.h>

#include <ryml.hpp>

#include <cstdint>
#include <string>

namespace ao::uimodel
{
  inline constexpr std::uint32_t kWindowsDesktopSettingsVersion = 2;
  inline constexpr std::int32_t kDefaultWindowsWindowX = 80;
  inline constexpr std::int32_t kDefaultWindowsWindowY = 80;
  inline constexpr std::int32_t kDefaultWindowsWindowWidth = 1280;
  inline constexpr std::int32_t kDefaultWindowsWindowHeight = 800;
  inline constexpr std::int32_t kMinimumWindowsWindowWidth = 640;
  inline constexpr std::int32_t kMinimumWindowsWindowHeight = 480;
  inline constexpr double kDefaultWindowsNavigationPaneWidth = 240.0;
  inline constexpr double kDefaultWindowsInspectorPaneWidth = 320.0;
  inline constexpr double kMinimumWindowsNavigationPaneWidth = 120.0;
  inline constexpr double kMinimumWindowsInspectorPaneWidth = 160.0;
  inline constexpr double kMaximumWindowsNavigationPaneWidth = 360.0;
  inline constexpr double kMaximumWindowsInspectorPaneWidth = 480.0;

  struct WindowsWindowPlacement final
  {
    std::int32_t x = kDefaultWindowsWindowX;
    std::int32_t y = kDefaultWindowsWindowY;
    std::int32_t width = kDefaultWindowsWindowWidth;
    std::int32_t height = kDefaultWindowsWindowHeight;
    bool maximized = false;

    friend bool operator==(WindowsWindowPlacement const&, WindowsWindowPlacement const&) = default;
  };

  struct WindowsDesktopSettings final
  {
    std::uint32_t version = kWindowsDesktopSettingsVersion;
    WindowsWindowPlacement window{};
    DesktopShellMode shellMode = DesktopShellMode::Modern;
    std::string lastLibraryPath{};
    double navigationPaneWidth = kDefaultWindowsNavigationPaneWidth;
    double inspectorPaneWidth = kDefaultWindowsInspectorPaneWidth;

    friend bool operator==(WindowsDesktopSettings const&, WindowsDesktopSettings const&) = default;
  };

  struct WindowsDesktopSettingsYamlSchema final
  {
    Result<> serialize(ryml::NodeRef node, WindowsDesktopSettings const& state) const;
    Result<WindowsDesktopSettings> deserialize(ryml::ConstNodeRef node, WindowsDesktopSettings const& seed) const;
  };
} // namespace ao::uimodel
