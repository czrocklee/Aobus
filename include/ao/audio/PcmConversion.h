// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace ao::audio
{
  template<typename TSource, typename TDestination>
  void padPcmSamples(std::span<TSource const> source, std::span<TDestination> destination, std::uint8_t shift) noexcept
  {
    auto const count = std::min(source.size(), destination.size());

    for (std::size_t i = 0; i < count; ++i)
    {
      using UnsignedDestination = std::make_unsigned_t<TDestination>;
      auto bits = static_cast<UnsignedDestination>(static_cast<TDestination>(source[i]));
      bits <<= shift;
      destination[i] = std::bit_cast<TDestination>(bits);
    }
  }

  template<typename TSource, typename TDestination>
  void interleaveAndPadPcmSamples(std::span<std::span<TSource const> const> channels,
                                  std::span<TDestination> destination,
                                  std::uint8_t shift) noexcept
  {
    if (channels.empty())
    {
      return;
    }

    auto const channelCount = channels.size();
    auto const frameCount = std::min(channels[0].size(), destination.size() / channelCount);

    for (std::size_t i = 0; i < frameCount; ++i)
    {
      for (std::size_t ch = 0; ch < channelCount; ++ch)
      {
        using UnsignedDestination = std::make_unsigned_t<TDestination>;
        auto bits = static_cast<UnsignedDestination>(static_cast<TDestination>(channels[ch][i]));
        bits <<= shift;
        destination[(i * channelCount) + ch] = std::bit_cast<TDestination>(bits);
      }
    }
  }

  void unpackS24PcmSamples(std::span<std::byte const> source,
                           std::span<std::int32_t> destination,
                           std::uint8_t shift = 0) noexcept;
} // namespace ao::audio
