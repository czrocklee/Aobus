// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/utility/StrongType.h>

#include <cstdint>

namespace ao
{
  // Physical audio scalars. Strong types so they cannot be silently swapped with
  // each other or with other numeric fields (year, frame counts, dictionary IDs).
  using SampleRate = utility::StrongType<std::uint32_t, struct SampleRateTag>; // Hz
  using Bitrate = utility::StrongType<std::uint32_t, struct BitrateTag>;       // bits per second
  using Channels = utility::StrongType<std::uint8_t, struct ChannelsTag>;      // audio channel count
  using BitDepth = utility::StrongType<std::uint8_t, struct BitDepthTag>;      // bits per sample
} // namespace ao
