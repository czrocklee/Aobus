// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/library/MusicLibrary.h>

#include <filesystem>
#include <map>
#include <string>

namespace YAML
{
  class Emitter;
}

namespace rs::library
{
  /**
   * ExportMode - Controls the verbosity of the library export.
   */
  enum class ExportMode
  {
    Minimum,  // Only data not in file tags: user tags, ratings, lists.
    Metadata, // Minimum + Metadata (title, artist, etc.) + Custom metadata.
    Full      // Metadata + Audio properties + Resources (Base64 cover art).
  };

  /**
   * LibraryExporter - Streaming YAML exporter for MusicLibrary.
   */
  class LibraryExporter final
  {
  public:
    explicit LibraryExporter(MusicLibrary& ml);

    /**
     * Export the library to a YAML file.
     * @param path Target file path.
     * @param mode Export verbosity mode.
     * @throws std::exception on failure.
     */
    void exportToYaml(std::filesystem::path const& path, ExportMode mode);

  private:
    void exportTracks(YAML::Emitter& out, rs::lmdb::ReadTransaction& txn, ExportMode mode);
    void exportTrack(YAML::Emitter& out, TrackId id, TrackView const& view, ExportMode mode);
    void exportLists(YAML::Emitter& out, rs::lmdb::ReadTransaction& txn);

    MusicLibrary& _ml;
  };
} // namespace rs::library
