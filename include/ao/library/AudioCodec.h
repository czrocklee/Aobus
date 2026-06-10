// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace ao::library
{
  enum class AudioCodec : std::uint8_t
  {
    Unknown = 0,
    Flac = 1,
    Alac = 2,
    Aac = 128,
    Mp3 = 129,
  };

  AudioCodec audioCodecFromStorage(std::uint8_t value) noexcept;
  std::uint8_t audioCodecStorageValue(AudioCodec codec) noexcept;
  std::string_view audioCodecName(AudioCodec codec) noexcept;
  std::optional<AudioCodec> parseAudioCodecName(std::string_view name) noexcept;
} // namespace ao::library
