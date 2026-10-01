// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/AudioCodec.h>
#include <ao/FileTimestamp.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackFieldValue.h>
#include <ao/rt/projection/TrackDetailSnapshot.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ao::library
{
  class DictionaryStore;
}

namespace ao::i18n
{
  class MessageCatalog;
}

namespace ao::uimodel
{
  inline constexpr std::string_view kCompositeMixedTrackText = "-";

  std::string formatDuration(std::chrono::milliseconds duration);
  std::string formatUint16(std::uint16_t value);
  std::string formatFileSize(std::uint64_t fileSize);
  // mtime is a Unix/POSIX modification instant; absence is distinct from the
  // epoch-zero instant. Use the local zone when available, otherwise UTC. An
  // absent or calendar-unrepresentable instant renders empty. The nanosecond
  // fraction stays in the payload for exact equality and is not displayed.
  std::string formatTime(std::optional<FileTimestamp> optMtime);
  std::string formatSampleRate(std::uint32_t sampleRate);
  std::string formatBitrate(std::uint32_t bitrate);
  std::string formatChannels(i18n::MessageCatalog const& textCatalog, std::uint8_t channels);
  std::string formatBitDepth(std::uint8_t bitDepth);
  std::string formatCodec(AudioCodec codec);
  std::string formatDisplayTrackNumber(std::uint16_t discNumber, std::uint16_t discTotal, std::uint16_t trackNumber);
  std::string formatTechnicalSummary(AudioCodec codec,
                                     std::uint32_t sampleRate,
                                     std::uint16_t bitDepth,
                                     std::uint32_t bitrate);
  std::string formatTrackFieldRawValue(i18n::MessageCatalog const& textCatalog,
                                       rt::TrackField field,
                                       rt::TrackFieldRawValue const& rawValue);
  std::string formatTrackFieldDisplayText(i18n::MessageCatalog const& textCatalog,
                                          rt::TrackField field,
                                          rt::TrackDetailSnapshot const& snap,
                                          std::string_view mixedText,
                                          bool showTechnicalUnknown);
} // namespace ao::uimodel
