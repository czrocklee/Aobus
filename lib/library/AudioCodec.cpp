// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/library/AudioCodec.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ao::library
{
  namespace
  {
    struct CodecName final
    {
      AudioCodec codec;
      std::string_view name;
    };

    constexpr auto kCodecNames = std::to_array<CodecName>({
      {.codec = AudioCodec::Flac, .name = "FLAC"},
      {.codec = AudioCodec::Alac, .name = "ALAC"},
      {.codec = AudioCodec::Aac, .name = "AAC"},
      {.codec = AudioCodec::Mp3, .name = "MP3"},
    });

    char toUpperAscii(char ch) noexcept
    {
      return static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }

    bool equalsIgnoreAsciiCase(std::string_view lhs, std::string_view rhs) noexcept
    {
      if (lhs.size() != rhs.size())
      {
        return false;
      }

      return std::ranges::equal(
        lhs, rhs, [](char left, char right) noexcept { return toUpperAscii(left) == toUpperAscii(right); });
    }
  } // namespace

  AudioCodec audioCodecFromStorage(std::uint8_t value) noexcept
  {
    switch (static_cast<AudioCodec>(value))
    {
      case AudioCodec::Unknown:
      case AudioCodec::Flac:
      case AudioCodec::Alac:
      case AudioCodec::Aac:
      case AudioCodec::Mp3: return static_cast<AudioCodec>(value);
    }

    return AudioCodec::Unknown;
  }

  std::uint8_t audioCodecStorageValue(AudioCodec codec) noexcept
  {
    return static_cast<std::uint8_t>(audioCodecFromStorage(static_cast<std::uint8_t>(codec)));
  }

  std::string_view audioCodecName(AudioCodec codec) noexcept
  {
    for (auto const& entry : kCodecNames)
    {
      if (entry.codec == codec)
      {
        return entry.name;
      }
    }

    return {};
  }

  std::optional<AudioCodec> parseAudioCodecName(std::string_view name) noexcept
  {
    if (equalsIgnoreAsciiCase(name, "UNKNOWN"))
    {
      return AudioCodec::Unknown;
    }

    for (auto const& entry : kCodecNames)
    {
      if (equalsIgnoreAsciiCase(name, entry.name))
      {
        return entry.codec;
      }
    }

    return std::nullopt;
  }
} // namespace ao::library
