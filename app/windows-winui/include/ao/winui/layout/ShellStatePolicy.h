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
     * @brief Whether the inspector is showing at all, however it is placed.
     *
     * An inline inspector is part of the workspace and is always showing. An
     * overlay covers the workspace, so it shows only while the user asks for
     * it: the pane mode alone cannot say whether it is on screen.
     */
    bool inspectorRevealed = true;

    friend bool operator==(ShellState const&, ShellState const&) = default;
  };

  /** Resolves the state of the native Windows shell from window-owned inputs. */
  class ShellStatePolicy final
  {
  public:
    static constexpr double kMediumWidth = 720.0;
    static constexpr double kWideWidth = 1120.0;

    static ShellWidthClass classify(double width) noexcept;

    /**
     * @brief The shell state @p width implies for @p mode.
     *
     * @param optInspectorRequest What the user last asked of the inspector, if
     *        anything. Until they ask, each pane mode answers for itself: an
     *        inline inspector shows because it costs the workspace nothing,
     *        while an overlay covers the workspace and so waits to be wanted.
     *        A request that has been made outlives the width that prompted it,
     *        so a pane revealed narrow is still wanted once the window widens.
     */
    static ShellState resolve(ShellMode mode, double width, std::optional<bool> optInspectorRequest) noexcept;
  };
} // namespace ao::winui
