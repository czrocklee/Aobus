// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/library/MusicLibrary.h>

#include <cstdint>
#include <filesystem>
#include <memory>

namespace ao::rt
{
  /**
   * ExportMode - Controls which data is included in the YAML export.
   */
  enum class ExportMode : std::uint8_t
  {
    Delta,    // User edits + Tags + Ratings + Lists
    Metadata, // Curated text + cover art
    Full,     // Everything
    ListOnly  // Playlists only
  };

  /**
   * LibraryYamlExporter - logical YAML exporter for library::MusicLibrary.
   */
  class LibraryYamlExporter final
  {
  public:
    explicit LibraryYamlExporter(library::MusicLibrary& ml);
    ~LibraryYamlExporter();

    LibraryYamlExporter(LibraryYamlExporter const&) = delete;
    LibraryYamlExporter& operator=(LibraryYamlExporter const&) = delete;
    LibraryYamlExporter(LibraryYamlExporter&&) noexcept = default;
    LibraryYamlExporter& operator=(LibraryYamlExporter&&) noexcept = default;

    /**
     * Export the library to a YAML file.
     * @param path Destination file path.
     * @param mode Export mode. Defaults to Full.
     * @return Result of the operation.
     */
    Result<> exportToYaml(std::filesystem::path const& path, ExportMode mode = ExportMode::Full);

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
} // namespace ao::rt
