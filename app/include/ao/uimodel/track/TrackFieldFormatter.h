// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/AudioCodec.h>
#include <ao/Error.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace ao::library
{
  class DictionaryStore;
}

namespace ao::uimodel::track
{
  using TrackFieldEditValue = std::variant<std::monostate, std::string, std::uint16_t>;

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

  TrackFieldEditValue makeTextEditValue(std::string_view value);
  Result<TrackFieldEditValue> parseTextEditValue(std::string_view value);
  Result<TrackFieldEditValue> parseUint16EditValue(std::string_view value);
} // namespace ao::uimodel::track
