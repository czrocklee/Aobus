// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/AudioCodec.h>
#include <ao/CoreIds.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/rt/TrackFieldValue.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace ao::rt
{
  // Plain read-model DTO for a single track row. Carries resolved values only
  // (dictionary strings already resolved, codec/status as library value enums);
  // no storage handles or transactions cross this boundary.
  struct TrackRow final
  {
    TrackId id{};
    ResourceId coverArtId{kInvalidResourceId};
    std::optional<std::filesystem::path> optUriPath{};

    std::string title{};
    std::string artist{};
    std::string album{};
    std::string albumArtist{};
    std::string genre{};
    std::string composer{};
    std::string work{};
    std::string movement{};
    std::string tags{};

    TrackFieldDuration duration{0};
    std::uint16_t year = 0;
    std::uint16_t discNumber = 0;
    std::uint16_t discTotal = 0;
    std::uint16_t trackNumber = 0;
    std::uint16_t trackTotal = 0;
    std::uint16_t movementNumber = 0;
    std::uint16_t movementTotal = 0;

    std::uint32_t sampleRate = 0;
    std::uint8_t channels = 0;
    std::uint8_t bitDepth = 0;
    AudioCodec codec = AudioCodec::Unknown;
    std::uint32_t bitrate = 0;
    std::uint64_t fileSize = 0;
    std::uint64_t modifiedTime = 0;
    library::FileStatus status = library::FileStatus::Available;
  };
} // namespace ao::rt
