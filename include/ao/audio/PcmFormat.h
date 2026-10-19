// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <cstddef>
#include <cstdint>

namespace ao::audio
{
  /** @brief Concrete byte-level PCM stream format exchanged with a backend. */
  struct PcmFormat final
  {
    std::uint32_t sampleRate = 0;
    std::uint8_t channels = 0;
    SampleEncoding encoding = SampleEncoding::Unknown;
    // Effective speaker identity, independent of container width and stride.
    // Zero does not select a default layout from the channel count.
    std::uint32_t speakerMask = 0;

    bool operator==(PcmFormat const&) const = default;
  };

  constexpr SignalFormat signalFormat(PcmFormat const& format) noexcept
  {
    return SignalFormat{
      .sampleRate = format.sampleRate,
      .channels = format.channels,
      .precisionBits = encodingNominalBits(format.encoding),
      .sampleKind = isFloatEncoding(format.encoding) ? SampleKind::FloatingPoint : SampleKind::Integer,
      .speakerMask = format.speakerMask};
  }

  constexpr PcmFormat pcmFormat(SignalFormat const& signal, SampleEncoding const encoding) noexcept
  {
    return PcmFormat{.sampleRate = signal.sampleRate,
                     .channels = signal.channels,
                     .encoding = encoding,
                     .speakerMask = signal.speakerMask};
  }

  constexpr std::size_t frameBytes(PcmFormat const& format) noexcept
  {
    if (format.channels == 0U || format.encoding == SampleEncoding::Unknown)
    {
      return 0U;
    }

    return static_cast<std::size_t>(format.channels) * bytesPerSample(format.encoding);
  }

  constexpr std::uint64_t bytesPerSecond(PcmFormat const& format) noexcept
  {
    if (format.sampleRate == 0U)
    {
      return 0U;
    }

    return static_cast<std::uint64_t>(format.sampleRate) * frameBytes(format);
  }

  constexpr bool isSamePcmMode(PcmFormat const& lhs, PcmFormat const& rhs) noexcept
  {
    return lhs == rhs;
  }
} // namespace ao::audio
