// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <optional>

namespace ao::winui
{
  enum class ShellMode : std::uint8_t
  {
    Modern,
    Classic,
  };

  enum class ShellWidthClass : std::uint8_t
  {
    Narrow,
    Medium,
    Wide,
  };

  enum class NavigationPaneMode : std::uint8_t
  {
    Expanded,
    Compact,
    Overlay,
  };

  enum class InspectorPaneMode : std::uint8_t
  {
    Inline,
    Overlay,
  };

  struct ShellState final
  {
    ShellMode mode = ShellMode::Modern;
    ShellWidthClass widthClass = ShellWidthClass::Wide;
    bool integratedTitleBar = true;
    NavigationPaneMode navigation = NavigationPaneMode::Expanded;
    InspectorPaneMode inspector = InspectorPaneMode::Inline;

    /**
     * Whether the inspector is showing at all, however it is placed.
     *
     * An inline inspector is part of the workspace and is always showing. An
     * overlay covers the workspace, so it shows only while the user asks for
     * it: the pane mode alone cannot say whether it is on screen.
     */
    bool inspectorRevealed = true;

    friend bool operator==(ShellState const&, ShellState const&) = default;
  };

  inline constexpr double kMediumShellWidth = 720.0;
  inline constexpr double kWideShellWidth = 1120.0;

  ShellWidthClass classifyShellWidth(double width) noexcept;

  /**
   * Resolve the native shell state from its window-owned inputs.
   *
   * @param optInspectorRequest What the user last asked of the inspector, if
   *        anything. Until they ask, each pane mode answers for itself. A
   *        request that has been made outlives the width that prompted it.
   */
  ShellState resolveShellState(ShellMode mode, double width, std::optional<bool> optInspectorRequest) noexcept;
} // namespace ao::winui
