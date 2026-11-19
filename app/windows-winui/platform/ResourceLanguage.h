// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <string_view>

namespace ao::winui
{
  // Establishes the one immutable MRT language context used by C++ lookups.
  // The WinUI composition root calls this before InitializeComponent().
  Result<> configureResourceLanguage(std::string_view localeTag);
  void resetResourceLanguage();
} // namespace ao::winui
