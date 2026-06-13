// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Type.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibraryScanner.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ScanPlanExecutor.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/tag/TagFile.h>
#include <ao/utility/Log.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <span>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace ao::library
{
  ScanPlanExecutor::ScanPlanExecutor(MusicLibrary& ml,
                                     ScanPlan plan,
                                     ProgressCallback progress,
                                     FinishedCallback finished)
    : _ml{ml}
    , _planPtr{std::make_unique<ScanPlan>(std::move(plan))}
    , _progressCallback{std::move(progress)}
    , _finishedCallback{std::move(finished)}
  {
  }

  ScanPlanExecutor::~ScanPlanExecutor()
  {
    join();
  }

  void ScanPlanExecutor::run(std::stop_token stopToken)
  {
    if (!_planPtr)
    {
      APP_LOG_ERROR("ScanPlanExecutor: No plan provided");
      return;
    }

    auto txn = _ml.writeTransaction();
    auto trackWriter = _ml.tracks().writer(txn);
    auto manifestWriter = _ml.manifest().writer(txn);
    auto& dict = _ml.dictionary();

    for (std::size_t i = 0; i < _planPtr->items.size(); ++i)
    {
      if (stopToken.stop_requested())
      {
        _result.cancelled = true;
        break;
      }

      processItem(i, txn, trackWriter, manifestWriter, dict);
    }

    txn.commit();

    if (_finishedCallback)
    {
      _finishedCallback();
    }
  }

  void ScanPlanExecutor::processItem(std::size_t itemIndex,
                                     ao::lmdb::WriteTransaction& txn,
                                     TrackStore::Writer& trackWriter,
                                     FileManifestStore::Writer& manifestWriter,
                                     DictionaryStore& dict)
  {
    auto const& item = _planPtr->items[itemIndex];

    try
    {
      if (_progressCallback)
      {
        _progressCallback(item.fullPath, static_cast<std::int32_t>(itemIndex));
      }

      if (item.classification == ScanClassification::Unchanged)
      {
        ++_result.skippedCount;
        return;
      }

      if (item.classification == ScanClassification::Unsupported || item.classification == ScanClassification::Error)
      {
        ++_result.failureCount;
        return;
      }

      if (item.classification == ScanClassification::Missing)
      {
        // Update manifest status to Missing
        if (auto const optView = _ml.manifest().reader(txn).get(item.uri); optView)
        {
          auto builder = FileManifestBuilder::fromView(*optView);
          builder.status(FileStatus::Missing);
          manifestWriter.put(item.uri, builder.serialize());
        }

        return;
      }

      // Handle NEW or CHANGED
      auto const tagFilePtr = tag::TagFile::open(item.fullPath);

      if (!tagFilePtr)
      {
        APP_LOG_WARN("Skipping unsupported file: {}", item.fullPath.string());
        ++_result.skippedCount;
        return;
      }

      auto builder = tagFilePtr->loadTrack();
      builder.property().uri(item.uri);

      if (item.classification == ScanClassification::Changed && item.trackId != kInvalidTrackId)
      {
        auto optExisting = trackWriter.get(item.trackId, TrackStore::Reader::LoadMode::Both);

        if (optExisting)
        {
          auto merged = TrackBuilder::fromView(*optExisting, dict);
          merged.property()
            .duration(builder.property().duration())
            .bitrate(builder.property().bitrate())
            .sampleRate(builder.property().sampleRate())
            .channels(builder.property().channels())
            .codec(builder.property().codec())
            .bitDepth(builder.property().bitDepth());

          auto const [preparedHot, preparedCold] = merged.prepare(txn, dict, _ml.resources());
          trackWriter.updateHot(
            item.trackId, preparedHot.size(), [&](std::span<std::byte> hot) { preparedHot.writeTo(hot); });
          trackWriter.updateCold(
            item.trackId, preparedCold.size(), [&](std::span<std::byte> cold) { preparedCold.writeTo(cold); });

          // Update manifest
          auto manifestBuilder = FileManifestBuilder::createNew();
          manifestBuilder.trackId(item.trackId).status(FileStatus::Available).fileSize(item.fileSize).mtime(item.mtime);
          manifestWriter.put(item.uri, manifestBuilder.serialize());

          _result.processedIds.push_back(item.trackId);
          return;
        }
      }

      // New track
      auto [preparedHot, preparedCold] = builder.prepare(txn, dict, _ml.resources());
      [[maybe_unused]] auto [newTrackId, view] = trackWriter.createHotCold(
        preparedHot.size(),
        preparedCold.size(),
        [&preparedHot, &preparedCold](TrackId /*id*/, std::span<std::byte> hot, std::span<std::byte> cold)
        {
          preparedHot.writeTo(hot);
          preparedCold.writeTo(cold);
        });

      auto manifestBuilder = FileManifestBuilder::createNew();
      manifestBuilder.trackId(newTrackId).status(FileStatus::Available).fileSize(item.fileSize).mtime(item.mtime);
      manifestWriter.put(item.uri, manifestBuilder.serialize());

      _result.processedIds.push_back(newTrackId);
    }
    catch (std::exception const& e)
    {
      APP_LOG_ERROR("Failed to process {}: {}", item.uri, e.what());
      ++_result.failureCount;
    }
  }

  void ScanPlanExecutor::join()
  {
    if (_workerThread.joinable())
    {
      _workerThread.join();
    }
  }

  std::size_t ScanPlanExecutor::fileCount() const
  {
    if (_planPtr)
    {
      return _planPtr->items.size();
    }

    return 0;
  }
} // namespace ao::library
