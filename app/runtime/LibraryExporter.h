// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/library/MusicLibrary.h>

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
    Minimum,  // Only data not in file tags: user tags, ratings, lists.
    Metadata, // Minimum + Metadata (title, artist, etc.) + Custom metadata.
    Full      // Metadata + Audio properties + Resources (Base64 cover art).
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
     * @throws std::exception on failure.
     */
    void exportToYaml(std::filesystem::path const& path, ExportMode mode);

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
} // namespace ao::rt
