// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/rt/library/LibraryTransfer.h>

#include <filesystem>
#include <memory>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  class LibraryYamlImportOperation;

  /** Source-private YAML importer shared by LibraryJobs and the synchronous offline operations. */
  class LibraryYamlImporter final
  {
  public:
    explicit LibraryYamlImporter(library::MusicLibrary& library);
    ~LibraryYamlImporter();

    LibraryYamlImporter(LibraryYamlImporter const&) = delete;
    LibraryYamlImporter& operator=(LibraryYamlImporter const&) = delete;
    LibraryYamlImporter(LibraryYamlImporter&&) noexcept;
    LibraryYamlImporter& operator=(LibraryYamlImporter&&) noexcept;

    Result<ImportReport> importFromYamlOffline(std::filesystem::path const& path,
                                               ImportMode mode = ImportMode::Restore);
    Result<ImportReport> previewImportFromYamlOffline(std::filesystem::path const& path,
                                                      ImportMode mode = ImportMode::Restore);

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;

    friend class LibraryYamlImportOperation;
  };
} // namespace ao::rt
