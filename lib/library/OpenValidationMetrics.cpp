// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "OpenValidationMetrics.h"

namespace ao::library::detail
{
  namespace
  {
    OpenValidationMetrics& metrics() noexcept
    {
      thread_local auto state = OpenValidationMetrics{};
      return state;
    }
  }

  void resetOpenValidationMetrics() noexcept
  {
    metrics() = {};
  }

  void recordOpenValidationTrackRow() noexcept
  {
    ++metrics().trackCursorRows;
  }

  void recordOpenValidationManifestPointRead() noexcept
  {
    ++metrics().manifestPointReads;
  }

  OpenValidationMetrics openValidationMetrics() noexcept
  {
    return metrics();
  }
} // namespace ao::library::detail
