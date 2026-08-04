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
   * @brief Apply a fallible XAML update from a noexcept signal handler.
   *
   * A signal reports state that has already changed, so its handler cannot
   * reject publication. Failures are contained at this frontend boundary and
   * leave the affected component at its last rendered state; no exception may
   * cross the Signal contract's noexcept delivery boundary.
   */
  template<typename Handler, typename... Args>
  void applyUiUpdate(std::string_view const component, Handler&& handler, Args&&... args) noexcept
  {
    try
    {
      std::invoke(std::forward<Handler>(handler), std::forward<Args>(args)...);
    }
    catch (winrt::hresult_error const& error)
    {
      APP_LOG_WARN("{}: failed to apply subscribed UI state: {}", component, winrt::to_string(error.message()));
    }
    catch (std::exception const& error)
    {
      APP_LOG_WARN("{}: failed to apply subscribed UI state: {}", component, error.what());
    }
    catch (...)
    {
      APP_LOG_WARN("{}: failed to apply subscribed UI state: unknown exception", component);
    }
  }

  /** Connect @p signal to one component-scoped XAML update. */
  template<typename... Args, typename Handler>
  async::Subscription subscribeUiUpdate(async::Signal<Args...>& signal,
                                        std::string_view const component,
                                        Handler handler)
  {
    return signal.connect([component = std::string{component}, handler = std::move(handler)](
                            Args... args) mutable noexcept { applyUiUpdate(component, handler, std::move(args)...); });
  }
} // namespace ao::winui::layout
