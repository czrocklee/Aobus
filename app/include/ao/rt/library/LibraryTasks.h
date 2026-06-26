// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/async/Task.h>
#include <ao/library/LibraryScanner.h>

#include <cstdint>
#include <filesystem>
#include <memory>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::async
{
  class Runtime;
}

namespace ao::rt
{
  class LibraryChanges;
  enum class ExportMode : std::uint8_t;

  class [[nodiscard]] LibraryTasks final
  {
  public:
    LibraryTasks(async::Runtime& asyncRuntime, library::MusicLibrary& library, LibraryChanges& changes);
    ~LibraryTasks();

    // Returning a Result, including an error Result, resumes the caller on the callback executor.
    // Unexpected exceptions may still propagate from the executor where they occur; UI callers should
    // present them through a boundary that returns to the callback executor first.
    async::Task<Result<>> importLibraryAsync(std::filesystem::path path);
    async::Task<Result<>> exportLibraryAsync(std::filesystem::path path, ExportMode mode);
    async::Task<Result<library::ScanPlan>> buildScanPlanAsync();
    async::Task<Result<library::ScanApplyResult>> applyScanPlanAsync(library::ScanPlan plan);

    LibraryTasks(LibraryTasks const&) = delete;
    LibraryTasks& operator=(LibraryTasks const&) = delete;
    LibraryTasks(LibraryTasks&&) = delete;
    LibraryTasks& operator=(LibraryTasks&&) = delete;

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
