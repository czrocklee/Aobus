// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>

namespace ao::uimodel
{
  enum class DesktopShellMode : std::uint8_t
  {
    Modern,
    Classic,
  };

  enum class DesktopShellWidthClass : std::uint8_t
  {
    Narrow,
    Medium,
    Wide,
  };

  enum class DesktopNavigationPresentation : std::uint8_t
  {
    Expanded,
    Compact,
    Overlay,
  };

  enum class DesktopInspectorPresentation : std::uint8_t
  {
    Inline,
    Overlay,
  };

  struct DesktopShellViewState final
  {
    DesktopShellMode mode = DesktopShellMode::Modern;
    DesktopShellWidthClass widthClass = DesktopShellWidthClass::Wide;
    bool integratedTitleBar = true;
    DesktopNavigationPresentation navigation = DesktopNavigationPresentation::Expanded;
    DesktopInspectorPresentation inspector = DesktopInspectorPresentation::Inline;

    friend bool operator==(DesktopShellViewState const&, DesktopShellViewState const&) = default;
  };

  class DesktopShellPolicy final
  {
  public:
    static constexpr double kMediumWidth = 720.0;
    static constexpr double kWideWidth = 1120.0;

    static DesktopShellWidthClass classify(double width) noexcept;
    static DesktopShellViewState resolve(DesktopShellMode mode, double width) noexcept;
  };
} // namespace ao::uimodel
