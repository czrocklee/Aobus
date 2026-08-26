// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string_view>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  /**
   * The interchange format version this build writes, and the only one it reads.
   *
   * Version 5 admits scalar-valid UTF-8, stores display text and custom keys in
   * NFC, and preserves URI and saved-filter source bytes exactly.
   */
  constexpr std::uint32_t kYamlFormatVersion = 5;

  /**
   * ExportMode - Controls which data is included in the YAML export.
   */
  enum class ExportMode : std::uint8_t
  {
    Delta,    // User edits + Tags + Lists
    Metadata, // Curated metadata + Tags + Lists
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
    LibraryYamlExporter(LibraryYamlExporter&&) noexcept;
    LibraryYamlExporter& operator=(LibraryYamlExporter&&) noexcept;

    /**
     * Export the library to a YAML file.
     *
     * The document is written whole or not at all: an export that fails or is
     * cancelled leaves whatever the path already held, because the file a user
     * exports over is usually the backup they are replacing.
     *
     * @param path Destination file path.
     * @param mode Export mode. Defaults to Full.
     * @param stopToken Cancellation for the walk. A stop request between records
     *        throws `async::OperationCancelled` and installs no file. The default
     *        token never stops, which is how a synchronous caller opts out.
     * @return Result of the operation.
     */
    Result<> exportToYaml(std::filesystem::path const& path,
                          ExportMode mode = ExportMode::Full,
                          std::stop_token stopToken = {});

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
