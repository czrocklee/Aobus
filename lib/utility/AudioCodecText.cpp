// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/AudioCodecText.h>

#include <ao/AudioCodec.h>

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace ao
{
  namespace
  {
    struct AudioCodecNameEntry final
    {
      AudioCodec codec;
      std::string_view name;
    };

    constexpr auto kAudioCodecNames = std::to_array<AudioCodecNameEntry>({
      {.codec = AudioCodec::Flac, .name = "FLAC"},
      {.codec = AudioCodec::Alac, .name = "ALAC"},
      {.codec = AudioCodec::Wav, .name = "WAV"},
      {.codec = AudioCodec::Aac, .name = "AAC"},
      {.codec = AudioCodec::Mp3, .name = "MP3"},
    });

    constexpr char toUpperAscii(char ch) noexcept
    {
      return ch >= 'a' && ch <= 'z' ? static_cast<char>(ch - 'a' + 'A') : ch;
    }

    bool equalsIgnoreAsciiCase(std::string_view lhs, std::string_view rhs) noexcept
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
  } // namespace

  std::string_view audioCodecName(AudioCodec codec) noexcept
  {
    for (auto const& entry : kAudioCodecNames)
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

    for (auto const& entry : kAudioCodecNames)
    {
      if (equalsIgnoreAsciiCase(name, entry.name))
      {
        return entry.codec;
      }
    }

    return std::nullopt;
  }
} // namespace ao
