// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/library/AudioCodec.h>
#include <ao/uimodel/track/TrackFieldFormatter.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace ao::uimodel::track
{
  namespace
  {
    constexpr std::uint32_t kKilo = 1000;
    constexpr double kKiloD = 1000.0;

    std::string_view trimAsciiWhitespace(std::string_view value)
    {
      auto const first = value.find_first_not_of(" \t\n\r\f\v");

      if (first == std::string_view::npos)
      {
        return {};
      }

      auto const last = value.find_last_not_of(" \t\n\r\f\v");
      return value.substr(first, last - first + 1);
    }
  } // namespace

  std::string formatDuration(std::chrono::milliseconds duration)
  {
    if (duration <= std::chrono::milliseconds{0})
    {
      return {};
    }

    auto const totalSeconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    auto const hours = totalSeconds / 3600;
    auto const minutes = (totalSeconds % 3600) / 60;
    auto const seconds = totalSeconds % 60;

    if (hours > 0)
    {
      return std::format("{}:{}:{:02}", hours, minutes, seconds);
    }

    return std::format("{}:{:02}", minutes, seconds);
  }

  std::string formatUint16(std::uint16_t value)
  {
    return value == 0 ? std::string{} : std::format("{}", value);
  }

  std::string formatFileSize(std::uint64_t fileSize)
  {
    if (fileSize == 0)
    {
      return {};
    }

    constexpr std::uint64_t kKB = 1024;
    constexpr std::uint64_t kMB = kKB * 1024;
    constexpr std::uint64_t kGB = kMB * 1024;

    if (fileSize >= kGB)
    {
      return std::format("{:.1f} GB", static_cast<double>(fileSize) / kGB);
    }

    if (fileSize >= kMB)
    {
      return std::format("{:.1f} MB", static_cast<double>(fileSize) / kMB);
    }

    return std::format("{:.1f} KB", static_cast<double>(fileSize) / kKB);
  }

  std::string formatTime(std::uint64_t mtime)
  {
    if (mtime == 0)
    {
      return {};
    }

    auto const sysTime = std::chrono::system_clock::from_time_t(static_cast<std::time_t>(mtime));

    return std::format("{:%Y-%m-%d %H:%M}", sysTime);
  }

  std::string formatSampleRate(std::uint32_t sampleRate)
  {
    if (sampleRate == 0)
    {
      return {};
    }

    return std::format("{} Hz", sampleRate);
  }

  std::string formatSampleRateCompact(std::uint32_t sampleRate)
  {
    if (sampleRate == 0)
    {
      return {};
    }

    if (sampleRate % kKilo == 0)
    {
      return std::format("{} kHz", sampleRate / kKilo);
    }

    return std::format("{:.4g} kHz", static_cast<double>(sampleRate) / kKiloD);
  }

  std::string formatBitrate(std::uint32_t bitrate)
  {
    if (bitrate == 0)
    {
      return {};
    }

    return std::format("{} kbps", bitrate / kKilo);
  }

  std::string formatChannels(std::uint8_t channels)
  {
    if (channels == 0)
    {
      return {};
    }

    if (channels == 1)
    {
      return "Mono";
    }

    if (channels == 2)
    {
      return "Stereo";
    }

    return std::format("{} channels", channels);
  }

  std::string formatBitDepth(std::uint8_t bitDepth)
  {
    if (bitDepth == 0)
    {
      return {};
    }

    return std::format("{}-bit", bitDepth);
  }

  std::string formatCodec(library::AudioCodec codec)
  {
    if (codec == library::AudioCodec::Unknown)
    {
      return {};
    }

    return std::string{library::audioCodecName(codec)};
  }

  TrackFieldEditValue makeTextEditValue(std::string_view value)
  {
    return TrackFieldEditValue{std::in_place_type<std::string>, std::string{value}};
  }

  Result<TrackFieldEditValue> parseTextEditValue(std::string_view value)
  {
    return makeTextEditValue(value);
  }

  Result<TrackFieldEditValue> parseUint16EditValue(std::string_view value)
  {
    auto const trimmed = trimAsciiWhitespace(value);

    if (trimmed.empty())
    {
      return TrackFieldEditValue{std::in_place_type<std::uint16_t>, static_cast<std::uint16_t>(0)};
    }

    auto parsed = std::uint32_t{0};
    auto const* const begin = trimmed.data();
    auto const* const end = trimmed.data() + trimmed.size();
    auto const [ptr, ec] = std::from_chars(begin, end, parsed);

    if (ec != std::errc{} || ptr != end || parsed > std::numeric_limits<std::uint16_t>::max())
    {
      return makeError(Error::Code::FormatRejected, "Enter a whole number from 0 to 65535.");
    }

    return TrackFieldEditValue{std::in_place_type<std::uint16_t>, static_cast<std::uint16_t>(parsed)};
  }
} // namespace ao::uimodel::track
