// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/layout/ShellStatePolicy.h>

#include <optional>

namespace ao::winui
{
  ShellWidthClass ShellStatePolicy::classify(double const width) noexcept
  {
    if (width < kMediumWidth)
    {
      return ShellWidthClass::Narrow;
    }

    if (width < kWideWidth)
    {
      return ShellWidthClass::Medium;
    }

    return ShellWidthClass::Wide;
  }

  ShellState ShellStatePolicy::resolve(ShellMode const mode,
                                       double const width,
                                       std::optional<bool> const optInspectorRequest) noexcept
  {
    auto const widthClass = classify(width);
    auto navigation = NavigationPaneMode::Overlay;
    auto inspector = InspectorPaneMode::Overlay;

    if (widthClass == ShellWidthClass::Wide)
    {
      navigation = NavigationPaneMode::Expanded;
      inspector = InspectorPaneMode::Inline;
    }
    else if (widthClass == ShellWidthClass::Medium)
    {
      navigation = NavigationPaneMode::Compact;
    }

    return ShellState{
      .mode = mode,
      .widthClass = widthClass,
      .integratedTitleBar = mode == ShellMode::Modern,
      .navigation = navigation,
      .inspector = inspector,
      .inspectorRevealed = optInspectorRequest.value_or(inspector == InspectorPaneMode::Inline),
    };
  }
} // namespace ao::winui
