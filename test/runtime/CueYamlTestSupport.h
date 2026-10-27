// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/library/LibraryTransfer.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt::test
{
  std::string cueDocument(std::string_view tracks, ExportMode mode = ExportMode::Full, std::string_view lists = "[]");

  std::string cueRecord(std::uint16_t ordinal,
                        std::uint64_t startFrame,
                        std::uint64_t frameCount,
                        std::uint64_t totalFrames = 132300,
                        std::uint32_t sampleRate = 44100,
                        std::string_view extra = "",
                        std::string_view uri = "disc.flac");

  Result<ImportReport> importCue(library::MusicLibrary& library,
                                 std::filesystem::path const& path,
                                 std::string_view document,
                                 ImportMode mode = ImportMode::Restore);

  std::vector<TrackId> carrierIds(library::MusicLibrary const& library);
} // namespace ao::rt::test
