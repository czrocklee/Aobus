// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/rt/library/LibraryTransfer.h>

#include <filesystem>
#include <memory>
#include <stop_token>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  /** Source-private serializer behind LibraryJobs and the offline CLI path. */
  class LibraryYamlExporter final
  {
  public:
    explicit LibraryYamlExporter(library::MusicLibrary const& library);
    ~LibraryYamlExporter();

    LibraryYamlExporter(LibraryYamlExporter const&) = delete;
    LibraryYamlExporter& operator=(LibraryYamlExporter const&) = delete;
    LibraryYamlExporter(LibraryYamlExporter&&) noexcept;
    LibraryYamlExporter& operator=(LibraryYamlExporter&&) noexcept;

    Result<> exportToYaml(std::filesystem::path const& path,
                          ExportMode mode = ExportMode::Full,
                          std::stop_token stopToken = {});

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
