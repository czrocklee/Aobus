// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Subscription.h>
#include <ao/async/Task.h>
#include <ao/rt/library/AudioIdentityIndex.h>
#include <ao/rt/library/LibraryImportPlan.h>
#include <ao/rt/library/LibraryTaskEvents.h>
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
  class LibraryMutationService;
  enum class ExportMode : std::uint8_t;

  /**
   * @brief Which delivery contract a resource request is subject to.
   *
   * One walk resolves a handle, consults the cache, and tries carriers; the
   * ceiling is a property of the request rather than of the walk, so every caller
   * states which contract it is under and neither contract is quietly widened.
   */
  enum class ResourceSizeLimit : std::uint8_t
  {
    /// Capped at kMaximumInteractiveResourceBytes, as every frontend is.
    Interactive,
    /// Uncapped, which the cover-art delivery specification grants raw CLI
    /// resource export alone.
    Administrative,
  };

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
      AudioIdentityIndexProgressCallback progressCallback = {},
      AudioIdentityIndexFailureCallback failureCallback = {});
    /**
     * @brief Produces the encoded bytes a resource names.
     *
     * Read-only: no maintenance admission and no task-progress publication. The
     * row holds no bytes, so this resolves the descriptor and the candidate
     * snapshot inside a short read transaction, closes it, and then walks the
     * derived cache and any referencing media file. Nothing is served that does
     * not hash to the descriptor's digest.
     *
     * No bytes means nothing can currently reproduce the content: a frontend
     * turns that into its placeholder, and the reference is never rewritten.
     * Cancellation propagates `async::OperationCancelled`.
     */
    async::Task<Result<std::optional<std::vector<std::byte>>>> loadResourceAsync(
      ResourceId resourceId,
      ResourceSizeLimit limit = ResourceSizeLimit::Interactive,
      std::stop_token stopToken = {});

    /**
     * @brief How many times the carrier index has been built.
     *
     * The index is built lazily, on the first materialization that finds no
     * usable snapshot, and rebuilding is serialized, so this counter is what
     * proves both: that no construction path builds it eagerly, and that a burst
     * of requests arriving on one stale stamp rebuilds once rather than once
     * each.
     */
    std::uint64_t resourceCarrierIndexBuildCount() const noexcept;

    // A progress conversation begins only after successful cancellable
    // callback-executor admission. Once admitted, this status-free
    // presentation pulse occurs exactly once on every ordinary terminal path
    // while the owner remains live. Task values, errors, and cancellation
    // remain exclusively on the awaited task channel.
    async::Subscription onProgressFinished(std::move_only_function<void()> handler) const;
    async::Subscription onProgress(std::move_only_function<void(LibraryTaskProgressUpdated const&)> handler) const;

    LibraryTaskService(LibraryTaskService const&) = delete;
    LibraryTaskService& operator=(LibraryTaskService const&) = delete;
    LibraryTaskService(LibraryTaskService&&) = delete;
    LibraryTaskService& operator=(LibraryTaskService&&) = delete;

  private:
    /// @param cacheDirectory Where the derived cover cache lives, resolved by a
    ///        composition root because the runtime does not discover platform
    ///        application directories. Empty leaves the read walk one tier: every
    ///        miss re-extracts from a carrier, and content whose carriers are all
    ///        gone has nothing left to read.
    LibraryTaskService(async::Runtime& asyncRuntime,
                       library::MusicLibrary& library,
                       LibraryMutationService& mutationService,
                       std::filesystem::path cacheDirectory);

    struct Impl;
    std::unique_ptr<Impl> _implPtr;

    friend class Library;
  };
} // namespace ao::rt
