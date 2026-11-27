// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/AudioCodecText.h>

#include <ao/AudioCodec.h>
#include <ao/utility/String.h>

#include <array>
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
      {.codec = AudioCodec::Opus, .name = "Opus"},
    });
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
    if (utility::isEqualIgnoringAsciiCase(name, "UNKNOWN"))
    {
      return AudioCodec::Unknown;
    }

    for (auto const& entry : kAudioCodecNames)
    {
      if (utility::isEqualIgnoringAsciiCase(name, entry.name))
      {
        return entry.codec;
      }
    }

    return std::nullopt;
  }
} // namespace ao
