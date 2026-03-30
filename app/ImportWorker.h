// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/MusicLibrary.h>
#include <rs/core/TrackRecord.h>
#include <rs/tag/Metadata.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class ImportWorker final
{
public:
  using ProgressCallback = std::function<void(std::filesystem::path const& path, std::int32_t itemIndex)>;
  using FinishedCallback = std::function<void()>;
  using TrackIdList = std::vector<rs::core::TrackId>;

  /**
   * ImportResult - Result of an import operation.
   */
  struct ImportResult
  {
    TrackIdList insertedIds;
    std::size_t failureCount = 0;
    std::size_t skippedCount = 0;
  };

  ImportWorker(rs::core::MusicLibrary& ml,
               std::vector<std::filesystem::path> const& files,
               ProgressCallback progressCallback,
               FinishedCallback finishedCallback);

  ~ImportWorker();

  // Run import in the current thread - must be called from background thread
  void run();

  // Get the result after run() completes
  ImportResult const& result() const { return _result; }

  // Join the worker thread (call from main thread after finished callback)
  void join();

private:
  // Process a single file and return the TrackId if successful
  std::optional<rs::core::TrackId> processFile(std::filesystem::path const& path, std::size_t index);

  // Populate TrackRecord from tag metadata
  rs::core::TrackRecord populateRecord(rs::tag::Metadata const& metadata,
                                       std::filesystem::path const& path,
                                       rs::core::DictionaryStore& dict,
                                       rs::core::ResourceStore::Writer& resourceWriter);

  rs::core::MusicLibrary& _ml;
  std::vector<std::filesystem::path> const& _files;
  ProgressCallback _progressCallback;
  FinishedCallback _finishedCallback;

  ImportResult _result;
  std::thread _workerThread;
  std::filesystem::path _rootPathStr;
};