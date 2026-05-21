// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

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
#include <thread>
#include <vector>

namespace ao::library
{
  struct ScanPlan;
  class DictionaryStore;

  class ImportWorker final
  {
  public:
    using ProgressCallback = std::move_only_function<void(std::filesystem::path const& path, std::int32_t itemIndex)>;
    using FinishedCallback = std::move_only_function<void()>;
    using TrackIdList = std::vector<TrackId>;

    /**
     * ImportResult - Result of an import operation.
     */
    struct ImportResult
    {
      TrackIdList insertedIds;
      std::size_t failureCount = 0;
      std::size_t skippedCount = 0;
    };

    ImportWorker(MusicLibrary& ml,
                 std::vector<std::filesystem::path> files,
                 ProgressCallback progressCallback,
                 FinishedCallback finishedCallback);

    ImportWorker(MusicLibrary& ml, ScanPlan plan, ProgressCallback progressCallback, FinishedCallback finishedCallback);

    ~ImportWorker();

    ImportWorker(ImportWorker const&) = delete;
    ImportWorker& operator=(ImportWorker const&) = delete;
    ImportWorker(ImportWorker&&) = delete;
    ImportWorker& operator=(ImportWorker&&) = delete;

    // Run import in the current thread - must be called from background thread
    void run();

    // Get the result after run() completes
    ImportResult const& result() const { return _result; }

    // Join the worker thread (call from main thread after finished callback)
    void join();

    std::size_t fileCount() const;

  private:
    void buildPlanFromFiles();
    void processItem(std::size_t itemIndex,
                     ao::lmdb::WriteTransaction& txn,
                     TrackStore::Writer& trackWriter,
                     FileManifestStore::Writer& manifestWriter,
                     DictionaryStore& dict);

    MusicLibrary& _ml;
    std::vector<std::filesystem::path> _files;
    std::unique_ptr<ScanPlan> _plan;
    ProgressCallback _progressCallback;
    FinishedCallback _finishedCallback;

    ImportResult _result;
    std::jthread _workerThread;
  };
} // namespace ao::library
