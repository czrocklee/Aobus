// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "platform/NativeWindow.h"

#include <ao/rt/Log.h>
#include <ao/winui/WindowPlacement.h>

// IWindowNative and the projected Window must agree on native IUnknown.
#include <microsoft.ui.xaml.window.h>
#include <unknwn.h>
#undef GetCurrentTime

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/base.h>

#include <cstdint>
#include <limits>
#include <optional>

namespace ao::winui
{
  using winrt::Microsoft::UI::Xaml::Window;

  HWND nativeWindowHandle(Window const& window)
  {
    auto windowNative = window.as<::IWindowNative>();
    HWND handle = nullptr;
    winrt::check_hresult(windowNative->get_WindowHandle(&handle));
    return handle;
  }

  void showNativeSystemMenu(Window const& window)
  {
    auto* const handle = nativeWindowHandle(window);
    ::SetForegroundWindow(handle);
    ::PostMessageW(handle, WM_SYSCOMMAND, SC_KEYMENU, static_cast<LPARAM>(L' '));
  }

  void restoreWindowPlacement(Window const& window, WindowPlacement const& placement)
  {
    auto const right = static_cast<std::int64_t>(placement.x) + placement.width;
    auto const bottom = static_cast<std::int64_t>(placement.y) + placement.height;
    constexpr auto kMinimumLong = static_cast<std::int64_t>(std::numeric_limits<LONG>::min());
    constexpr auto kMaximumLong = static_cast<std::int64_t>(std::numeric_limits<LONG>::max());

    if (right < kMinimumLong || right > kMaximumLong || bottom < kMinimumLong || bottom > kMaximumLong)
    {
      APP_LOG_WARN("MainWindow: rejected native window placement with overflowing bounds");
      return;
    }

    auto nativePlacement = WINDOWPLACEMENT{};
    nativePlacement.length = sizeof(nativePlacement);
    nativePlacement.showCmd = placement.maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    nativePlacement.rcNormalPosition = {
      .left = placement.x,
      .top = placement.y,
      .right = static_cast<LONG>(right),
      .bottom = static_cast<LONG>(bottom),
    };

    if (::SetWindowPlacement(nativeWindowHandle(window), &nativePlacement) == FALSE)
    {
      APP_LOG_WARN("MainWindow: failed to restore native window placement: {}", ::GetLastError());
    }
  }

  std::optional<WindowPlacement> readWindowPlacement(Window const& window)
  {
    auto nativePlacement = WINDOWPLACEMENT{};
    nativePlacement.length = sizeof(nativePlacement);

    if (::GetWindowPlacement(nativeWindowHandle(window), &nativePlacement) == FALSE)
    {
      APP_LOG_WARN("MainWindow: failed to read native window placement: {}", ::GetLastError());
      return std::nullopt;
    }

    auto const& normal = nativePlacement.rcNormalPosition;
    return WindowPlacement{
      .x = normal.left,
      .y = normal.top,
      .width = normal.right - normal.left,
      .height = normal.bottom - normal.top,
      .maximized = nativePlacement.showCmd == SW_SHOWMAXIMIZED || (nativePlacement.flags & WPF_RESTORETOMAXIMIZED) != 0,
    };
  }
} // namespace ao::winui
