// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/library/TrackView.h>

#include <cstddef>
#include <span>

namespace ao::library::detail
{
  struct TrackViewRawAccess final
  {
    static std::span<std::byte const> hotData(TrackView const& view) noexcept { return view.hotData(); }

    static std::span<std::byte const> coldData(TrackView const& view) noexcept { return view.coldData(); }
  };
} // namespace ao::library::detail
