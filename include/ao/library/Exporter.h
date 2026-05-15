// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Type.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackView.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace YAML
{
  class Emitter;
}

namespace ao::library
{
  /**
   * ExportMode - Controls the verbosity of the library export.
   */
  enum class ExportMode : std::uint8_t
  {
    Minimum,  // Only data not in file tags: user tags, ratings, lists.
    Metadata, // Minimum + Metadata (title, artist, etc.) + Custom metadata.
    Full      // Metadata + Audio properties + Resources (Base64 cover art).
  };

  /**
   * Exporter - Streaming YAML exporter for MusicLibrary.
   */
  class Exporter final
  {
  public:
    explicit Exporter(MusicLibrary& ml);

    /**
     * Export the library to a YAML file.
     * @param path Target file path.
     * @param mode Export verbosity mode.
     * @throws std::exception on failure.
     */
    void exportToYaml(std::filesystem::path const& path, ExportMode mode);

  private:
    void exportTracks(YAML::Emitter& out, lmdb::ReadTransaction const& txn, ExportMode mode);
    void exportTrack(YAML::Emitter& out, TrackId id, TrackView const& view, ExportMode mode);
    void exportLists(YAML::Emitter& out, lmdb::ReadTransaction const& txn);

    MusicLibrary& _ml;
  };
} // namespace ao::library
