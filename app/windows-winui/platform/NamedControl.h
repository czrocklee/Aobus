// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Contract.h>

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/base.h>

#include <concepts>

namespace ao::winui
{
  /**
   * @brief Construct one @p T carrying @p name as its UI Automation name.
   *
   * The automation name is the control's contract with assistive technology,
   * so an empty name is a construction bug rather than a presentation choice.
   * The factory owns nothing else: content, tooltip, style, and enablement
   * stay with the caller.
   */
  template<typename T>
    requires std::convertible_to<T, winrt::Microsoft::UI::Xaml::UIElement>
  T makeNamed(winrt::hstring name)
  {
    AO_EXPECTS(!name.empty(), "a named control requires a nonempty automation name");
    auto element = T{};
    winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(element, name);
    return element;
  }
} // namespace ao::winui
