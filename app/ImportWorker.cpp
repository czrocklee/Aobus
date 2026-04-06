// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "ImportWorker.h"

#include <rs/core/TrackBuilder.h>

#include <chrono>

ImportWorker::ImportWorker(rs::core::MusicLibrary& ml,
                           std::vector<std::filesystem::path> const& files,
                           ProgressCallback progressCallback,
                           FinishedCallback finishedCallback)
  : _ml{ml}
  , _files{files}
  , _progressCallback{progressCallback}
  , _finishedCallback{finishedCallback}
{
}

ImportWorker::~ImportWorker() = default;

void ImportWorker::run()
{
  auto txn = _ml.writeTransaction();
  auto trackWriter = _ml.tracks().writer(txn);
  auto resourceWriter = _ml.resources().writer(txn);
  auto& dict = _ml.dictionary();

  for (auto i = 0u; i < _files.size(); ++i)
  {
    try
    {
      auto const& path = _files[i];

      // Report progress
      if (_progressCallback) { _progressCallback(path, static_cast<std::int32_t>(i)); }

      // Open tag file
      auto tagFile = rs::tag::File::open(path);
      if (!tagFile)
      {
        ++_result.skippedCount;
        continue;
      }

      auto parsed = tagFile->loadTrack();

      // Fill in library context
      parsed.record.property.uri = std::filesystem::relative(path, _ml.rootPath()).string();
      if (std::filesystem::exists(path)) { parsed.record.property.fileSize = std::filesystem::file_size(path); }
      auto ftime = std::filesystem::last_write_time(path);
      parsed.record.property.mtime =
        std::chrono::duration_cast<std::chrono::nanoseconds>(ftime.time_since_epoch()).count();

      // Store cover art
      if (!parsed.embeddedCoverArt.empty())
      {
        parsed.record.metadata.coverArtId = resourceWriter.create(parsed.embeddedCoverArt).value();
      }

      auto builder = rs::core::TrackBuilder::fromRecord(std::move(parsed.record));
      auto [hotData, coldData] = builder.serialize(txn, dict, _ml.resources());

      auto [trackId, view] = trackWriter.createHotCold(hotData, coldData);
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
  if (_finishedCallback) { _finishedCallback(); }
}

void ImportWorker::join()
{
  if (_workerThread.joinable()) { _workerThread.join(); }
}
