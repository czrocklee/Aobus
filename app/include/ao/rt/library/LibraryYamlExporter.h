// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  /**
   * ExportMode - Controls which data is included in the YAML export.
   */
  enum class ExportMode : std::uint8_t
  {
    Delta,    // User edits + Tags + Lists
    Metadata, // Curated text + cover art
    Full,     // Everything
    ListOnly  // Playlists only
  };

  constexpr std::string_view exportModeName(ExportMode const mode) noexcept
  {
    switch (mode)
    {
      case ExportMode::Delta: return "delta";
      case ExportMode::Metadata: return "metadata";
      case ExportMode::Full: return "full";
      case ExportMode::ListOnly: return "listOnly";
    }

    return {};
  }

  /**
   * LibraryYamlExporter - logical YAML exporter for library::MusicLibrary.
   */
  class LibraryYamlExporter final
  {
  public:
    explicit LibraryYamlExporter(library::MusicLibrary const& ml);
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
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
