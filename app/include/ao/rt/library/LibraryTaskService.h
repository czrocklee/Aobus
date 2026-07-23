// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Task.h>
#include <ao/rt/library/AudioIdentityIndexer.h>
#include <ao/rt/library/LibraryImportPlan.h>
#include <ao/rt/library/LibraryYamlImporter.h>
#include <ao/rt/library/ScanPlan.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

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
  class LibraryMutationService;
  enum class ExportMode : std::uint8_t;

  class LibraryTaskService final
  {
  public:
    static constexpr std::size_t kMaximumInteractiveResourceBytes = std::size_t{32U} * 1024U * 1024U;

    ~LibraryTaskService();

    // Returning a Result, including an error Result, resumes the caller on the callback executor.
    // Unexpected exceptions may still propagate from the executor where they occur; UI callers should
    // present them through a boundary that returns to the callback executor first.
    using ScanProgressCallback = std::move_only_function<void(ScanApplyProgress const& progress)>;
    using ScanFailureCallback = std::move_only_function<void(ScanFailure const& failure)>;

    async::Task<Result<LibraryImportPlan>> prepareLibraryImportAsync(std::filesystem::path path,
                                                                     ImportMode mode,
                                                                     std::stop_token stopToken = {});
    async::Task<Result<ImportReport>> applyLibraryImportPlanAsync(LibraryImportPlan plan,
                                                                  std::stop_token stopToken = {});
    async::Task<Result<>> exportLibraryAsync(std::filesystem::path path,
                                             ExportMode mode,
                                             std::stop_token stopToken = {});
    async::Task<Result<ScanPlan>> buildScanPlanAsync(std::stop_token stopToken = {});
    // Cooperative cancellation propagates OperationCancelled; a returned
    // ScanApplyResult always represents a completed, non-cancelled scan.
    async::Task<Result<ScanApplyResult>> applyScanPlanAsync(ScanPlan plan,
                                                            ScanApplyOptions options = {},
                                                            std::stop_token stopToken = {},
                                                            ScanProgressCallback progressCallback = {},
                                                            ScanFailureCallback failureCallback = {});
    async::Task<Result<AudioIdentityIndexResult>> backfillAudioIdentityAsync(
      std::stop_token stopToken = {},
      AudioIdentityIndexer::ProgressCallback progressCallback = {},
      AudioIdentityIndexer::ItemFailureCallback failureCallback = {});
    // Read-only interactive delivery: no maintenance admission or task-progress publication.
    async::Task<Result<std::optional<std::vector<std::byte>>>> loadResourceAsync(ResourceId resourceId,
                                                                                 std::stop_token stopToken = {});

    LibraryTaskService(LibraryTaskService const&) = delete;
    LibraryTaskService& operator=(LibraryTaskService const&) = delete;
    LibraryTaskService(LibraryTaskService&&) = delete;
    LibraryTaskService& operator=(LibraryTaskService&&) = delete;

  private:
    LibraryTaskService(async::Runtime& asyncRuntime,
                       library::MusicLibrary& library,
                       LibraryChanges& changes,
                       LibraryMutationService& mutationService);

    struct Impl;
    std::unique_ptr<Impl> _implPtr;

    friend class Library;
  };
} // namespace ao::rt
