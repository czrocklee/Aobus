// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>

namespace ao
{
  enum class AudioCodec : std::uint8_t
  {
    Unknown = 0,
    Flac = 1,
    Alac = 2,
    Wav = 3,
    Aac = 128,
    Mp3 = 129,
    Opus = 130,
  };

  constexpr AudioCodec audioCodecFromStorage(std::uint8_t value) noexcept
  {
    switch (static_cast<AudioCodec>(value))
    {
      case AudioCodec::Unknown:
      case AudioCodec::Flac:
      case AudioCodec::Alac:
      case AudioCodec::Wav:
      case AudioCodec::Aac:
      case AudioCodec::Mp3:
      case AudioCodec::Opus: return static_cast<AudioCodec>(value);
    }

    return AudioCodec::Unknown;
  }

  constexpr std::uint8_t audioCodecStorageValue(AudioCodec codec) noexcept
  {
    return static_cast<std::uint8_t>(audioCodecFromStorage(static_cast<std::uint8_t>(codec)));
  }
} // namespace ao
