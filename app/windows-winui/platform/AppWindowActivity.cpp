// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "platform/AppWindowActivity.h"

#include <ao/utility/ScopedRegistration.h>

#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Windows.Foundation.h>

#include <functional>
#include <utility>

namespace ao::winui
{
  using winrt::Microsoft::UI::Windowing::AppWindow;
  using winrt::Microsoft::UI::Windowing::AppWindowChangedEventArgs;
  using winrt::Microsoft::UI::Windowing::OverlappedPresenter;
  using winrt::Microsoft::UI::Windowing::OverlappedPresenterState;

  bool isWindowMinimized(AppWindow const& window)
  {
    auto const presenter = window.Presenter().try_as<OverlappedPresenter>();
    return presenter && presenter.State() == OverlappedPresenterState::Minimized;
  }

  utility::ScopedRegistration observeWindowActivityChanges(AppWindow const& window, std::function<void()> onChange)
  {
    auto const handler = winrt::Windows::Foundation::TypedEventHandler<AppWindow, AppWindowChangedEventArgs>{
      [onChange = std::move(onChange)](AppWindow const&, AppWindowChangedEventArgs const& args)
      {
        if (args.DidVisibilityChange() || args.DidPresenterChange())
        {
          onChange();
        }
      }};
    auto const token = window.Changed(handler);

    // AppWindow has no IWeakReferenceSource for auto_revoke. Retain the source
    // until its owner removes the handler with the noexcept token overload.
    try
    {
      return utility::ScopedRegistration{[window, token] noexcept { window.Changed(token); }};
    }
    catch (...)
    {
      // The handler is already installed if allocating its cleanup closure fails.
      window.Changed(token);
      throw;
    }
  }
} // namespace ao::winui
