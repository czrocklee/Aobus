// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstddef>

namespace ao::library::detail
{
  /** Source-private counters used to prove the open validator's growth law. */
  struct OpenValidationMetrics final
  {
    std::size_t trackCursorRows = 0;
    std::size_t manifestPointReads = 0;
  };

  void resetOpenValidationMetrics() noexcept;
  void recordOpenValidationTrackRow() noexcept;
  void recordOpenValidationManifestPointRead() noexcept;
  OpenValidationMetrics openValidationMetrics() noexcept;
} // namespace ao::library::detail
