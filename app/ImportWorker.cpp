// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "ImportWorker.h"

#include <rs/core/TrackBuilder.h>

#include <chrono>

ImportWorker::ImportWorker(rs::core::MusicLibrary& ml,
                           std::vector<std::filesystem::path> const& files,
                           ProgressCallback progressCallback,
                           FinishedCallback finishedCallback)
  : _ml{ml}, _files{files}, _progressCallback{progressCallback}, _finishedCallback{finishedCallback}
{
}

ImportWorker::~ImportWorker() = default;

void ImportWorker::run()
{
  auto txn = _ml.writeTransaction();
  auto trackWriter = _ml.tracks().writer(txn);
  auto& dict = _ml.dictionary();

  for (auto i = 0u; i < _files.size(); ++i)
  {
    try
    {
      auto const& path = _files[i];

      // Report progress
      if (_progressCallback)
      {
        _progressCallback(path, static_cast<std::int32_t>(i));
      }

      // Open tag file
      auto tagFile = rs::tag::File::open(path);
      if (!tagFile)
      {
        ++_result.skippedCount;
        continue;
      }

      auto builder = tagFile->loadTrack();

      // Fill in library context
      builder.property()
        .uri(std::filesystem::relative(path, _ml.rootPath()).string())
        .mtime(std::chrono::duration_cast<std::chrono::nanoseconds>(
                 std::filesystem::last_write_time(path).time_since_epoch())
                 .count());
      if (std::filesystem::exists(path))
      {
        builder.property().fileSize(std::filesystem::file_size(path));
      }

      auto [preparedHot, preparedCold] = builder.prepare(txn, dict, _ml.resources());

      auto [trackId, view] = trackWriter.createHotCold(
        preparedHot.size(),
        preparedCold.size(),
        [&preparedHot, &preparedCold](rs::core::TrackId id, std::span<std::byte> hot, std::span<std::byte> cold)
        {
          preparedHot.writeTo(hot);
          preparedCold.writeTo(cold);
        });
      _result.insertedIds.push_back(trackId);
    }
    catch ([[maybe_unused]] std::exception const& e)
    {
      ++_result.failureCount;
      continue;
    }
  }

  // Commit the transaction
  txn.commit();

  // Call finished callback
  if (_finishedCallback)
  {
    _finishedCallback();
  }
}

void ImportWorker::join()
{
  if (_workerThread.joinable())
  {
    _workerThread.join();
  }
}
