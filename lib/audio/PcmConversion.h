// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

namespace ao::audio
{
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

  /**
   * @brief Converts interleaved little-endian PCM between explicit encodings.
   *
   * Container width and integer alignment are handled exactly when the target
   * encoding preserves the logical source precision. `sourcePcmFormat` owns
   * the byte layout; `sourceSignalFormat` owns the number of meaningful bits.
   * Precision-losing and quantizing conversions are rejected.
   */
  Result<> convertPcmEncoding(std::span<std::byte const> source,
                              PcmFormat const& sourcePcmFormat,
                              SignalFormat const& sourceSignalFormat,
                              SampleEncoding destinationEncoding,
                              std::vector<std::byte>& destination);
} // namespace ao::audio
