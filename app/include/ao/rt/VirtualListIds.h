// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <cstdint>
#include <limits>

namespace ao::rt
{
  inline constexpr auto kAllTracksListId = ListId{std::numeric_limits<std::uint32_t>::max()};
} // namespace ao::rt
