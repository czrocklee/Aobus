// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "ao/Error.h"
#include "ao/library/MusicLibrary.h"

#include <cstdint>
#include <filesystem>
#include <memory>

namespace ao::rt
{
  /**
   * ExportMode - Controls the verbosity of the library export.
   */
  enum class ExportMode : std::uint8_t
  {
    Delta,    // Only data not matching physical baseline: user edits, tags, ratings.
    Metadata, // Curated text metadata and cover art (no technical properties).
    Full,     // Everything: Metadata, cover art, and technical properties.
    ListOnly  // Synchronize playlists only, omitting track data entirely.
  };

  /**
   * Exporter - Streaming YAML exporter for library::MusicLibrary.
   */
  class LibraryExporter final
  {
  public:
    explicit LibraryExporter(library::MusicLibrary& ml);
    ~LibraryExporter();

    LibraryExporter(LibraryExporter const&) = delete;
    LibraryExporter& operator=(LibraryExporter const&) = delete;
    LibraryExporter(LibraryExporter&&) noexcept = default;
    LibraryExporter& operator=(LibraryExporter&&) noexcept = default;

    /**
     * Export the library to a YAML file.
     * @param path Target file path.
     * @param mode Export verbosity mode.
     * @return Result of the operation.
     */
    Result<> exportToYaml(std::filesystem::path const& path, ExportMode mode);

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
} // namespace ao::rt
