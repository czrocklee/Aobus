// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/library/TrackView.h>

#include <cstddef>
#include <span>

namespace ao::library::detail
{
  /**
   * Diagnostic access to the exact bytes supplied to TrackView.
   *
   * Raw access deliberately bypasses the decoded-accessor validity contract:
   * absent and structurally invalid sides remain inspectable by diagnostic
   * commands and validators.
   */
  struct TrackViewRawAccess final
  {
    static std::span<std::byte const> hotData(TrackView const& view) noexcept { return view.hotData(); }

    static std::span<std::byte const> coldData(TrackView const& view) noexcept { return view.coldData(); }
  };
} // namespace ao::library::detail
