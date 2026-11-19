// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <winrt/base.h>

#include <string_view>

namespace ao::winui
{
  winrt::hstring resourceHstring(std::wstring_view resourceId);
} // namespace ao::winui
