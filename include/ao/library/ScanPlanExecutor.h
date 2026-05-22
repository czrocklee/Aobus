// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "ao/Type.h"
#include "ao/library/FileManifestStore.h"
#include "ao/library/MusicLibrary.h"
#include "ao/library/TrackStore.h"
#include "ao/lmdb/Transaction.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <thread>
#include <vector>

namespace ao::library
{
  struct ScanPlan;
  class DictionaryStore;

  /**
   * ScanPlanExecutor - Applies a ScanPlan to the MusicLibrary database.
   *
   * This class focus exclusively on reconciling the filesystem scan results
   * with the database state.
   */
  class ScanPlanExecutor final
  {
  public:
    using ProgressCallback = std::move_only_function<void(std::filesystem::path const& path, std::int32_t itemIndex)>;
    using FinishedCallback = std::move_only_function<void()>;
    using TrackIdList = std::vector<TrackId>;

    /**
     * ScanApplyResult - Result of applying a scan plan.
     */
    struct ScanApplyResult
    {
      TrackIdList processedIds; // TrackIds that were newly inserted or updated
      std::size_t failureCount = 0;
      std::size_t skippedCount = 0;
      bool cancelled = false;
    };

    ScanPlanExecutor(MusicLibrary& ml,
                     ScanPlan plan,
                     ProgressCallback progressCallback,
                     FinishedCallback finishedCallback);

    ~ScanPlanExecutor();

    ScanPlanExecutor(ScanPlanExecutor const&) = delete;
    ScanPlanExecutor& operator=(ScanPlanExecutor const&) = delete;
    ScanPlanExecutor(ScanPlanExecutor&&) = delete;
    ScanPlanExecutor& operator=(ScanPlanExecutor&&) = delete;

    // Run execution in the current thread - must be called from background thread.
    // Respects cancellation via stop_token.
    void run(std::stop_token stopToken = {});

    // Get the result after run() completes
    ScanApplyResult const& result() const { return _result; }

    // Join the worker thread (call from main thread after finished callback)
    void join();

    std::size_t fileCount() const;

  private:
    void processItem(std::size_t itemIndex,
                     ao::lmdb::WriteTransaction& txn,
                     TrackStore::Writer& trackWriter,
                     FileManifestStore::Writer& manifestWriter,
                     DictionaryStore& dict);

    MusicLibrary& _ml;
    std::unique_ptr<ScanPlan> _plan;
    ProgressCallback _progressCallback;
    FinishedCallback _finishedCallback;

    ScanApplyResult _result;
    std::jthread _workerThread;
  };
} // namespace ao::library
