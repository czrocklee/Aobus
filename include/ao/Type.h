// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/utility/StrongType.h>

#include <cstdint>

namespace ao
{
  using TrackId = utility::StrongType<std::uint32_t, struct TrackIdTag>;
  using ListId = utility::StrongType<std::uint32_t, struct ListIdTag>;
  using ResourceId = utility::StrongType<std::uint32_t, struct ResourceIdTag>;
  using DictionaryId = utility::StrongType<std::uint32_t, struct DictionaryIdTag>;

  // Physical audio scalars. Strong types so they cannot be silently swapped with
  // each other or with other numeric fields (year, frame counts, dictionary IDs).
  using SampleRate = utility::StrongType<std::uint32_t, struct SampleRateTag>; // Hz
  using Bitrate = utility::StrongType<std::uint32_t, struct BitrateTag>;       // bits per second
  using Channels = utility::StrongType<std::uint8_t, struct ChannelsTag>;      // audio channel count
  using BitDepth = utility::StrongType<std::uint8_t, struct BitDepthTag>;      // bits per sample

  inline constexpr auto kInvalidTrackId = TrackId{0};
  inline constexpr auto kInvalidListId = ListId{0};
  inline constexpr auto kInvalidResourceId = ResourceId{0};
  inline constexpr auto kInvalidDictionaryId = DictionaryId{0};
} // namespace ao
