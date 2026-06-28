// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/AudioCodec.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackFieldValue.h>
#include <ao/rt/projection/ProjectionTypes.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace ao::library
{
  class DictionaryStore;
}

namespace ao::uimodel
{
  inline constexpr std::string_view kMultipleTrackValuesText = "<Multiple Values>";
  inline constexpr std::string_view kCompositeMixedTrackText = "-";

  std::string formatDuration(std::chrono::milliseconds duration);
  std::string formatUint16(std::uint16_t value);
  std::string formatFileSize(std::uint64_t fileSize);
  std::string formatTime(std::uint64_t mtime);
  std::string formatSampleRate(std::uint32_t sampleRate);
  std::string formatSampleRateCompact(std::uint32_t sampleRate);
  std::string formatBitrate(std::uint32_t bitrate);
  std::string formatChannels(std::uint8_t channels);
  std::string formatBitDepth(std::uint8_t bitDepth);
  std::string formatCodec(AudioCodec codec);
  std::string formatDisplayTrackNumber(std::uint16_t discNumber, std::uint16_t discTotal, std::uint16_t trackNumber);
  std::string formatTechnicalSummary(AudioCodec codec, std::uint32_t sampleRate, std::uint16_t bitDepth);
  std::string formatTrackFieldRawValue(rt::TrackField field, rt::TrackFieldRawValue const& rawValue);
  std::string displayTextForTrackField(rt::TrackField field,
                                       rt::TrackDetailSnapshot const& snap,
                                       std::string_view mixedText,
                                       bool showTechnicalUnknown);
} // namespace ao::uimodel
