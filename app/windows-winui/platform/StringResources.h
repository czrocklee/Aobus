// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <winrt/Windows.Foundation.h>

#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace ao::winui
{
  // Establishes the one immutable MRT language context used by C++ lookups.
  // The WinUI composition root calls this before InitializeComponent().
  Result<> configureResourceLanguage(std::string_view localeTag);
  void resetResourceLanguage();

  winrt::hstring resourceHstring(std::wstring_view resourceId);
  std::string resourceString(std::string_view resourceId);
  std::string resourceStringOr(std::string_view resourceId, std::string_view fallback);
  std::string stableResourceString(std::string_view prefix, std::string_view stableId, std::string_view fallback);

  // Formats canonical MRT projections whose named ICU argument is generated as {0}.
  template<typename... Args>
  std::string formatResource(std::string_view const resourceId, Args const&... args)
  {
    auto const pattern = resourceString(resourceId);
    return std::vformat(pattern, std::make_format_args(args...));
  }
} // namespace ao::winui
