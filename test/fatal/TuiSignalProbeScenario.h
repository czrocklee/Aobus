// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstdint>

namespace ao::tui::test
{
  /// Child exit when the host cannot deliver the requested console control event.
  inline constexpr std::int32_t kTuiSignalProbeSkipped = 77;

  std::int32_t runTuiSignalProbeScenario(char const* scenario);
} // namespace ao::tui::test
