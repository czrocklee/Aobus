// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CueYamlTestSupport.h"

#include "runtime/library/LibraryYamlImporter.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/FileManifestView.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/library/LibraryTransfer.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt::test
{
  std::string cueDocument(std::string_view tracks, ExportMode mode, std::string_view lists)
  {
    return std::format("version: {}\nexport-mode: {}\nlibrary:\n{}  tracks:\n{}  lists: {}\n",
                       kYamlFormatVersion,
                       exportModeName(mode),
                       mode == ExportMode::Full ? "  resources: []\n" : "",
                       tracks,
                       lists);
  }

  std::string cueRecord(std::uint16_t ordinal,
                        std::uint64_t startFrame,
                        std::uint64_t frameCount,
                        std::uint64_t totalFrames,
                        std::uint32_t sampleRate,
                        std::string_view extra,
                        std::string_view uri)
  {
    return std::format(
      "    - uri: {}\n"
      "      segment: {{ordinal: {}, start-frame: {}, frame-count: {}, total-frames: {}, sample-rate: {}}}\n{}",
      uri,
      ordinal,
      startFrame,
      frameCount,
      totalFrames,
      sampleRate,
      extra);
  }

  Result<ImportReport> importCue(library::MusicLibrary& library,
                                 std::filesystem::path const& path,
                                 std::string_view document,
                                 ImportMode mode)
  {
    {
      auto output = std::ofstream{path, std::ios::binary};
      output << document;
    }

    return LibraryYamlImporter{library}.importFromYamlOffline(path, mode);
  }

  std::vector<TrackId> carrierIds(library::MusicLibrary const& library)
  {
    auto transaction = library.readTransaction();
    auto const optManifest = library.manifest().reader(transaction).get("disc.flac");
    REQUIRE(optManifest);
    CHECK(optManifest->trackId() == kInvalidTrackId);
    return {optManifest->segmentTrackIds().begin(), optManifest->segmentTrackIds().end()};
  }
} // namespace ao::rt::test
