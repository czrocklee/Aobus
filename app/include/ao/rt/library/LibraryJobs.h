// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/async/Subscription.h>
#include <ao/async/Task.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/rt/library/AudioIdentityIndex.h>
#include <ao/rt/library/LibraryImportPlan.h>
#include <ao/rt/library/LibraryTaskEvents.h>
#include <ao/rt/library/LibraryTransfer.h>
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
  class LibraryWriteLane;
  enum class ExportMode : std::uint8_t;

  class LibraryJobs final
  {
  public:
    ~LibraryJobs();

    // Returning a Result, including an error Result, resumes the caller on the callback executor.
    // Unexpected exceptions may still propagate from the executor where they occur; UI callers should
    // present them through a boundary that returns to the callback executor first.
    using ScanProgressCallback = compat::MoveOnlyFunction<void(ScanApplyProgress const& progress)>;
    using ScanFailureCallback = compat::MoveOnlyFunction<void(ScanFailure const& failure)>;

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
      AudioIdentityIndexProgressCallback progressCallback = {},
      AudioIdentityIndexFailureCallback failureCallback = {});
    // A progress conversation begins only after successful cancellable
    // callback-executor admission. Progress and its status-free terminal pulse
    // carry the same owner-local id so overlapping read-only tasks cannot clear
    // one another. Task values, errors, and cancellation remain exclusively on
    // the awaited task channel.
    async::Subscription onProgressFinished(
      compat::MoveOnlyFunction<void(LibraryTaskProgressFinished const&)> handler) const;
    async::Subscription onProgress(compat::MoveOnlyFunction<void(LibraryTaskProgressUpdated const&)> handler) const;

    LibraryJobs(LibraryJobs const&) = delete;
    LibraryJobs& operator=(LibraryJobs const&) = delete;
    LibraryJobs(LibraryJobs&&) = delete;
    LibraryJobs& operator=(LibraryJobs&&) = delete;

  private:
    LibraryJobs(async::Runtime& asyncRuntime, library::MusicLibrary& library, LibraryWriteLane& writeLane);

    struct Impl;
    std::unique_ptr<Impl> _implPtr;

    friend class Library;
  };
} // namespace ao::rt
