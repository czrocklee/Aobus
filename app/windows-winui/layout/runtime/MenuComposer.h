// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include <functional>
#include <string_view>

namespace ao::winui::layout
{
  struct LayoutBuildContext;

  /**
   * @brief The shell's menu composition, borrowed by the components that present it.
   *
   * Menus belong to the shell, which owns the commands their items invoke; a
   * component only presents what it is handed. A missing composer means the
   * shell offers no menus and therefore rejects a document that asks for one.
   */
  struct MenuComposer final
  {
    /// The flyout a `menuButton` presents, keyed by its authored `menuId`.
    std::function<winrt::Microsoft::UI::Xaml::Controls::MenuFlyout(LayoutBuildContext const&, std::string_view)> flyout;

    /// Fills the application menu bar the Classic shell shows.
    std::function<void(LayoutBuildContext const&, winrt::Microsoft::UI::Xaml::Controls::MenuBar const&)> composeMenuBar;
  };
} // namespace ao::winui::layout
