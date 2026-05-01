// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/library/ImportWorker.h>
#include <rs/utility/Log.h>

#include <rs/library/TrackBuilder.h>

#include <chrono>

namespace rs::library
{
  ImportWorker::ImportWorker(MusicLibrary& ml,
                             std::vector<std::filesystem::path> const& files,
                             ProgressCallback progressCallback,
                             FinishedCallback finishedCallback)
    : _ml{ml}
    , _files{files}
    , _progressCallback{std::move(progressCallback)}
    , _finishedCallback{std::move(finishedCallback)}
  {
  }

  ImportWorker::~ImportWorker() = default;

  void ImportWorker::run()
  {
    APP_LOG_INFO("Starting import of {} files", _files.size());
    auto txn = _ml.writeTransaction();
    auto trackWriter = _ml.tracks().writer(txn);
    auto& dict = _ml.dictionary();

    for (auto i = 0U; i < _files.size(); ++i)
    {
      try
      {
        auto const& path = _files[i];
        APP_LOG_DEBUG("Importing file: {}", path.string());

        // Report progress

        if (_progressCallback)
        {
          _progressCallback(path, static_cast<std::int32_t>(i));
        }

        // Open tag file
        auto tagFile = rs::tag::File::open(path);

        if (!tagFile)
        {
          APP_LOG_WARN("Skipping unsupported file: {}", path.string());
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
          [&preparedHot, &preparedCold](
            [[maybe_unused]] rs::TrackId id, std::span<std::byte> hot, std::span<std::byte> cold)
          {
            preparedHot.writeTo(hot);
            preparedCold.writeTo(cold);
          });
        _result.insertedIds.push_back(trackId);
      }
      catch (std::exception const& e)
      {
        APP_LOG_ERROR("Failed to import {}: {}", _files[i].string(), e.what());
        ++_result.failureCount;
        continue;
      }
    }

    // Commit the transaction
    txn.commit();
    APP_LOG_INFO("Import finished: {} inserted, {} failures, {} skipped",
                 _result.insertedIds.size(),
                 _result.failureCount,
                 _result.skippedCount);

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
} // namespace rs::library
