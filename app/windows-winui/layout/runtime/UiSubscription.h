// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/async/Signal.h>
#include <ao/async/Subscription.h>
#include <ao/rt/Log.h>

#include <winrt/base.h>

#include <exception>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace ao::winui::layout
{
  /**
   * @brief Apply a fallible XAML update from a Signal observer.
   *
   * A signal reports state that has already changed, so its handler cannot
   * reject publication. Expected WinRT failures leave the affected component
   * at its last rendered state. Any other exception escapes to Signal's owning
   * diagnostic-and-abort boundary.
   */
  template<typename Handler, typename... Args>
  void applyUiUpdate(std::string_view const component, Handler&& handler, Args&&... args)
  {
    try
    {
      std::invoke(std::forward<Handler>(handler), std::forward<Args>(args)...);
    }
    catch (winrt::hresult_error const& error)
    {
      APP_LOG_WARN("{}: failed to apply subscribed UI state: {}", component, winrt::to_string(error.message()));
    }
  }

  /** Connect @p signal to one component-scoped XAML update. */
  template<typename... Args, typename Handler>
  async::Subscription subscribeUiUpdate(async::Signal<Args...>& signal,
                                        std::string_view const component,
                                        Handler handler)
  {
    return signal.connect([component = std::string{component}, handler = std::move(handler)](Args... args) mutable
                          { applyUiUpdate(component, handler, std::move(args)...); });
  }
} // namespace ao::winui::layout
