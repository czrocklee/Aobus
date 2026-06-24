// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/async/Task.h>

#include <cstdint>
#include <filesystem>
#include <memory>

namespace ao::library
{
  class MusicLibrary;
  struct ScanPlan;
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

    async::Task<void> importLibraryAsync(std::filesystem::path path);
    async::Task<void> exportLibraryAsync(std::filesystem::path path, ExportMode mode);
    async::Task<library::ScanPlan> buildScanPlanAsync();
    async::Task<void> applyScanPlanAsync(library::ScanPlan plan);

    LibraryTasks(LibraryTasks const&) = delete;
    LibraryTasks& operator=(LibraryTasks const&) = delete;
    LibraryTasks(LibraryTasks&&) = delete;
    LibraryTasks& operator=(LibraryTasks&&) = delete;

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
