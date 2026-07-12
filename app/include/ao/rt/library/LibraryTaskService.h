// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/async/Task.h>
#include <ao/rt/library/AudioIdentityIndexer.h>
#include <ao/rt/library/ScanPlan.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <stop_token>

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

  class LibraryTaskService final
  {
  public:
    LibraryTaskService(async::Runtime& asyncRuntime, library::MusicLibrary& library, LibraryChanges& changes);
    ~LibraryTaskService();

    // Returning a Result, including an error Result, resumes the caller on the callback executor.
    // Unexpected exceptions may still propagate from the executor where they occur; UI callers should
    // present them through a boundary that returns to the callback executor first.
    async::Task<Result<>> importLibraryAsync(std::filesystem::path path, std::stop_token stopToken = {});
    async::Task<Result<>> exportLibraryAsync(std::filesystem::path path,
                                             ExportMode mode,
                                             std::stop_token stopToken = {});
    async::Task<Result<ScanPlan>> buildScanPlanAsync(std::stop_token stopToken = {});
    async::Task<Result<ScanApplyResult>> applyScanPlanAsync(ScanPlan plan,
                                                            ScanApplyOptions options = {},
                                                            std::stop_token stopToken = {});
    async::Task<Result<AudioIdentityIndexResult>> backfillAudioIdentityAsync(std::stop_token stopToken = {});

    LibraryTaskService(LibraryTaskService const&) = delete;
    LibraryTaskService& operator=(LibraryTaskService const&) = delete;
    LibraryTaskService(LibraryTaskService&&) = delete;
    LibraryTaskService& operator=(LibraryTaskService&&) = delete;

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
