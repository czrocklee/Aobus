// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <winrt/base.h>

#include <concepts>
#include <functional>
#include <source_location>
#include <string_view>
#include <utility>

namespace ao::winui
{
  void reportOptionalWinRtFailure(std::string_view context,
                                  winrt::hresult_error const& error,
                                  std::source_location location) noexcept;

  void logWinUiCritical(std::string_view context,
                        std::string_view detail,
                        std::source_location location = std::source_location::current()) noexcept;

  /**
   * @brief Degrade an optional WinRT operation to its already-safe fallback.
   *
   * This boundary is only for optional visuals or external OS integrations
   * whose failure leaves no callback or lifetime obligation behind. Ordinary
   * UI cleanup and internal cancellation are invariant-bearing operations and
   * must not be routed through it. Non-WinRT exceptions, including
   * std::bad_alloc, deliberately propagate.
   *
   * @see doc/spec/shell/windows-desktop.md#failure-and-cancellation
   */
  template<typename Operation>
    requires std::invocable<Operation&&>
  void runOptionalWinRt(std::string_view const context,
                        Operation&& operation,
                        std::source_location const location = std::source_location::current())
  {
    try
    {
      std::invoke(std::forward<Operation>(operation));
    }
    catch (winrt::hresult_error const& error)
    {
      reportOptionalWinRtFailure(context, error, location);
    }
  }
} // namespace ao::winui
