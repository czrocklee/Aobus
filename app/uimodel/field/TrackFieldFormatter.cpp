// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/AudioCodec.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackFieldValue.h>
#include <ao/rt/projection/TrackDetailProjection.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <variant>

namespace ao::uimodel
{
  namespace
  {
    std::string readRawText(rt::TrackFieldRawValue const& rawValue)
    {
      if (auto const* text = std::get_if<std::string>(&rawValue); text != nullptr)
      {
        return *text;
      }

      return {};
    }

    std::string readRawUint16(rt::TrackFieldRawValue const& rawValue)
    {
      if (auto const* value = std::get_if<std::uint16_t>(&rawValue); value != nullptr)
      {
        return formatUint16(*value);
      }

      return {};
    }

    std::string readRawUint32(rt::TrackFieldRawValue const& rawValue, std::string (*formatter)(std::uint32_t))
    {
      if (auto const* value = std::get_if<std::uint32_t>(&rawValue); value != nullptr)
      {
        return formatter(*value);
      }

      return {};
    }

    std::string readRawUint64(rt::TrackFieldRawValue const& rawValue, std::string (*formatter)(std::uint64_t))
    {
      if (auto const* value = std::get_if<std::uint64_t>(&rawValue); value != nullptr)
      {
        return formatter(*value);
      }

      return {};
    }

    std::string readRawUint32AsUint8(rt::TrackFieldRawValue const& rawValue, std::string (*formatter)(std::uint8_t))
    {
      if (auto const* value = std::get_if<std::uint32_t>(&rawValue); value != nullptr)
      {
        return formatter(static_cast<std::uint8_t>(*value));
      }

      return {};
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
      return std::format("{}:{:02}:{:02}", hours, minutes, seconds);
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

  std::string formatBitrate(std::uint32_t bitrate)
  {
    if (bitrate == 0)
    {
      return {};
    }

    return std::format("{} kbps", bitrate / 1000);
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

  std::string formatCodec(AudioCodec codec)
  {
    if (codec == AudioCodec::Unknown)
    {
      return {};
    }

    return std::string{audioCodecName(codec)};
  }

  std::string formatDisplayTrackNumber(std::uint16_t discNumber, std::uint16_t discTotal, std::uint16_t trackNumber)
  {
    if (trackNumber == 0)
    {
      return {};
    }

    if (discTotal > 1 && discNumber != 0)
    {
      return std::format("{}-{}", discNumber, trackNumber);
    }

    return std::format("{}", trackNumber);
  }

  std::string formatTechnicalSummary(AudioCodec codec,
                                     std::uint32_t sampleRate,
                                     std::uint16_t bitDepth,
                                     std::uint32_t bitrate)
  {
    auto result = std::string{};
    result.reserve(64);

    bool first = true;

    auto append = [&]<typename... Args>(bool condition, std::format_string<Args...> fmtStr, Args&&... args)
    {
      if (condition)
      {
        if (!first)
        {
          result.append(" \u00b7 ");
        }

        std::format_to(std::back_inserter(result), fmtStr, std::forward<Args>(args)...);
        first = false;
      }
    };

    append(codec != AudioCodec::Unknown, "{}", audioCodecName(codec));
    append(sampleRate > 0, "{:.4g} kHz", static_cast<double>(sampleRate) / 1000);
    append(bitDepth > 0, "{}-bit", bitDepth);
    append(bitrate > 0, "{} kbps", bitrate / 1000);

    return result;
  }

  std::string formatTrackFieldRawValue(rt::TrackField field, rt::TrackFieldRawValue const& rawValue)
  {
    using F = rt::TrackField;

    switch (field)
    {
      case F::Title:
      case F::Artist:
      case F::Album:
      case F::AlbumArtist:
      case F::Genre:
      case F::Composer:
      case F::Conductor:
      case F::Ensemble:
      case F::Work:
      case F::Movement:
      case F::Soloist:
      case F::Tags:
      case F::FilePath:
      case F::Codec:
      case F::DisplayTrackNumber:
      case F::TechnicalSummary: return readRawText(rawValue);

      case F::Year:
      case F::DiscNumber:
      case F::DiscTotal:
      case F::TrackNumber:
      case F::TrackTotal:
      case F::MovementNumber:
      case F::MovementTotal: return readRawUint16(rawValue);

      case F::Duration:
        if (auto const* duration = std::get_if<rt::TrackFieldDuration>(&rawValue); duration != nullptr)
        {
          return formatDuration(*duration);
        }

        return {};

      case F::SampleRate: return readRawUint32(rawValue, formatSampleRate);
      case F::Channels: return readRawUint32AsUint8(rawValue, formatChannels);
      case F::BitDepth: return readRawUint32AsUint8(rawValue, formatBitDepth);
      case F::Bitrate: return readRawUint32(rawValue, formatBitrate);
      case F::FileSize: return readRawUint64(rawValue, formatFileSize);
      case F::ModifiedTime: return readRawUint64(rawValue, formatTime);

      case F::Quality: return {};
    }

    return {};
  }

  std::string formatTrackFieldDisplayText(rt::TrackField field,
                                          rt::TrackDetailSnapshot const& snap,
                                          std::string_view mixedText,
                                          bool showTechnicalUnknown)
  {
    auto const& agg = rt::trackFieldArrayAt(snap.fields, field);
    auto const* def = rt::trackFieldDefinition(field);

    if (agg.mixed)
    {
      return std::string{mixedText};
    }

    if (!agg.optValue)
    {
      if (showTechnicalUnknown && def != nullptr && def->category == rt::TrackFieldCategory::Technical)
      {
        return "Unknown";
      }

      return {};
    }

    return formatTrackFieldRawValue(field, *agg.optValue);
  }
} // namespace ao::uimodel
