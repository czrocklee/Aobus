// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <string_view>

namespace ao
{
  // Names the calling thread for debuggers and profilers. Truncated to the
  // platform limit (15 characters on Linux); best-effort, never fails.
  void setCurrentThreadName(std::string_view name) noexcept;
} // namespace ao
