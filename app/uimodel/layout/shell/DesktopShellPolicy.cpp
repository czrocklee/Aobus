// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/shell/DesktopShellPolicy.h>

namespace ao::uimodel
{
  DesktopShellWidthClass DesktopShellPolicy::classify(double const width) noexcept
  {
    if (width < kMediumWidth)
    {
      return DesktopShellWidthClass::Narrow;
    }

    if (width < kWideWidth)
    {
      return DesktopShellWidthClass::Medium;
    }

    return DesktopShellWidthClass::Wide;
  }

  DesktopShellViewState DesktopShellPolicy::resolve(DesktopShellMode const mode, double const width) noexcept
  {
    auto const widthClass = classify(width);
    auto navigation = DesktopNavigationPresentation::Overlay;
    auto inspector = DesktopInspectorPresentation::Overlay;

    if (widthClass == DesktopShellWidthClass::Wide)
    {
      navigation = DesktopNavigationPresentation::Expanded;
      inspector = DesktopInspectorPresentation::Inline;
    }
    else if (widthClass == DesktopShellWidthClass::Medium)
    {
      navigation = DesktopNavigationPresentation::Compact;
    }

    return DesktopShellViewState{
      .mode = mode,
      .widthClass = widthClass,
      .integratedTitleBar = mode == DesktopShellMode::Modern,
      .navigation = navigation,
      .inspector = inspector,
    };
  }
} // namespace ao::uimodel
