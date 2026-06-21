// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ao
{
  enum class AudioCodec : std::uint8_t
  {
    Unknown = 0,
    Flac = 1,
    Alac = 2,
    Aac = 128,
    Mp3 = 129,
  };

  namespace detail
  {
    struct AudioCodecNameEntry final
    {
      AudioCodec codec;
      std::string_view name;
    };

    inline constexpr auto kAudioCodecNames = std::to_array<AudioCodecNameEntry>({
      {.codec = AudioCodec::Flac, .name = "FLAC"},
      {.codec = AudioCodec::Alac, .name = "ALAC"},
      {.codec = AudioCodec::Aac, .name = "AAC"},
      {.codec = AudioCodec::Mp3, .name = "MP3"},
    });

    constexpr char toUpperAscii(char ch) noexcept
    {
      return ch >= 'a' && ch <= 'z' ? static_cast<char>(ch - 'a' + 'A') : ch;
    }

    constexpr bool equalsIgnoreAsciiCase(std::string_view lhs, std::string_view rhs) noexcept
    {
      if (lhs.size() != rhs.size())
      {
        return false;
      }

      for (std::size_t index = 0; index < lhs.size(); ++index)
      {
        if (toUpperAscii(lhs[index]) != toUpperAscii(rhs[index]))
        {
          return false;
        }
      }

      return true;
    }
  } // namespace detail

  constexpr AudioCodec audioCodecFromStorage(std::uint8_t value) noexcept
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

  constexpr std::uint8_t audioCodecStorageValue(AudioCodec codec) noexcept
  {
    return static_cast<std::uint8_t>(audioCodecFromStorage(static_cast<std::uint8_t>(codec)));
  }

  constexpr std::string_view audioCodecName(AudioCodec codec) noexcept
  {
    for (auto const& entry : detail::kAudioCodecNames)
    {
      if (entry.codec == codec)
      {
        return entry.name;
      }
    }

    return {};
  }

  constexpr std::optional<AudioCodec> parseAudioCodecName(std::string_view name) noexcept
  {
    if (detail::equalsIgnoreAsciiCase(name, "UNKNOWN"))
    {
      return AudioCodec::Unknown;
    }

    for (auto const& entry : detail::kAudioCodecNames)
    {
      if (detail::equalsIgnoreAsciiCase(name, entry.name))
      {
        return entry.codec;
      }
    }

    return std::nullopt;
  }
} // namespace ao
