// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/utility/ScopedRegistration.h>

#pragma push_macro("GetCurrentTime")
#include <unknwn.h>
#pragma pop_macro("GetCurrentTime")

#include <winrt/Microsoft.UI.Windowing.h>

#include <functional>

namespace ao::winui
{
  bool isWindowMinimized(winrt::Microsoft::UI::Windowing::AppWindow const& window);
  utility::ScopedRegistration observeWindowActivityChanges(winrt::Microsoft::UI::Windowing::AppWindow const& window,
                                                           std::function<void()> onChange);
} // namespace ao::winui
