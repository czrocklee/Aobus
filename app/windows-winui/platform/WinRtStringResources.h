// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#pragma push_macro("GetCurrentTime")
#include <unknwn.h>
#pragma pop_macro("GetCurrentTime")

#include <winrt/base.h>

#include <string_view>

namespace ao::winui
{
  winrt::hstring resourceHstring(std::wstring_view resourceId);
} // namespace ao::winui
