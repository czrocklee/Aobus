// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/rt/library/LibraryYamlImporter.h>

#include <filesystem>
#include <memory>

namespace ao::library
{
  class WriteTransaction;
}

namespace ao::rt
{
  struct LibraryChangeSet;

  /** Runtime-private prepare/apply split for coordinated YAML imports. */
  class LibraryYamlImportOperation final
  {
  public:
    class PreparedImport final
    {
    public:
      struct Impl;

      ~PreparedImport();
      PreparedImport(PreparedImport&&) noexcept;
      PreparedImport& operator=(PreparedImport&&) noexcept;

      PreparedImport(PreparedImport const&) = delete;
      PreparedImport& operator=(PreparedImport const&) = delete;

    private:
      explicit PreparedImport(std::unique_ptr<Impl> implPtr);

      std::unique_ptr<Impl> _implPtr;

      friend class LibraryYamlImportOperation;
    };

    explicit LibraryYamlImportOperation(LibraryYamlImporter& importer) noexcept;

    Result<PreparedImport> prepare(std::filesystem::path const& path, ImportMode mode, bool buildChangeSet);
    Result<ImportReport> apply(PreparedImport const& prepared,
                               library::WriteTransaction& transaction,
                               LibraryChangeSet& changeSet);
    Result<ImportReport> preview(PreparedImport const& prepared, library::WriteTransaction& transaction);
    Result<ImportReport> applyOffline(PreparedImport const& prepared);
    Result<ImportReport> previewOffline(PreparedImport const& prepared);

  private:
    LibraryYamlImporter& _importer;
  };
} // namespace ao::rt
