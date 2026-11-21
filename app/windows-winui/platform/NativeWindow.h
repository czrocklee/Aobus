// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/winui/WindowPlacement.h>

// Use the SDK definition of HWND without exporting the macro that collides with XAML.
#pragma push_macro("GetCurrentTime")
#include <windows.h>
#pragma pop_macro("GetCurrentTime")

#include <optional>

namespace winrt::Microsoft::UI::Xaml
{
  struct Window;
}

namespace ao::winui
{
  HWND nativeWindowHandle(winrt::Microsoft::UI::Xaml::Window const& window);
  void showNativeSystemMenu(winrt::Microsoft::UI::Xaml::Window const& window);
  void restoreWindowPlacement(winrt::Microsoft::UI::Xaml::Window const& window, WindowPlacement const& placement);
  std::optional<WindowPlacement> readWindowPlacement(winrt::Microsoft::UI::Xaml::Window const& window);
} // namespace ao::winui
