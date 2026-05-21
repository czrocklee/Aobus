#include "ao/library/ImportWorker.h"

#include "ao/Type.h"
#include "ao/library/FileManifestStore.h"
#include "ao/library/LibraryScanner.h"
#include "ao/library/MusicLibrary.h"
#include "ao/library/TrackBuilder.h"
#include "ao/library/TrackStore.h"
#include "ao/tag/TagFile.h"
#include "ao/utility/Log.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace ao::library
{
  ImportWorker::ImportWorker(MusicLibrary& ml,
                             std::vector<std::filesystem::path> files,
                             ProgressCallback progress,
                             FinishedCallback finished)
    : _ml{ml}, _files{std::move(files)}, _progressCallback{std::move(progress)}, _finishedCallback{std::move(finished)}
  {
  }

  ImportWorker::ImportWorker(MusicLibrary& ml, ScanPlan plan, ProgressCallback progress, FinishedCallback finished)
    : _ml{ml}
    , _plan{std::make_unique<ScanPlan>(std::move(plan))}
    , _progressCallback{std::move(progress)}
    , _finishedCallback{std::move(finished)}
  {
  }

  ImportWorker::~ImportWorker()
  {
    join();
  }

  void ImportWorker::buildPlanFromFiles()
  {
    _plan = std::make_unique<ScanPlan>();

    for (auto const& path : _files)
    {
      auto const uri = std::filesystem::relative(path, _ml.rootPath()).string();
      auto item = ScanItem{.uri = uri, .fullPath = path, .classification = ScanClassification::Error};

      if (!std::filesystem::exists(path))
      {
        item.errorMessage = "File not found";
        _plan->items.push_back(std::move(item));
        continue;
      }

      auto txn = _ml.readTransaction();
      auto const manifestReader = _ml.manifest().reader(txn);
      auto const optEntry = manifestReader.get(uri);

      item.fileSize = std::filesystem::file_size(path);
      item.mtime = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::filesystem::last_write_time(path).time_since_epoch())
          .count());

      if (optEntry)
      {
        item.trackId = optEntry->trackId;

        if (optEntry->fileSize() == item.fileSize && optEntry->mtime() == item.mtime)
        {
          item.classification = ScanClassification::Unchanged;
        }
        else
        {
          item.classification = ScanClassification::Changed;
        }
      }
      else
      {
        item.classification = ScanClassification::New;
      }

      _plan->items.push_back(std::move(item));
    }
  }

  void ImportWorker::run()
  {
    if (!_plan)
    {
      buildPlanFromFiles();
    }

    auto txn = _ml.writeTransaction();
    auto trackWriter = _ml.tracks().writer(txn);
    auto manifestWriter = _ml.manifest().writer(txn);
    auto& dict = _ml.dictionary();

    for (std::size_t i = 0; i < _plan->items.size(); ++i)
    {
      processItem(i, txn, trackWriter, manifestWriter, dict);
    }

    txn.commit();

    if (_finishedCallback)
    {
      _finishedCallback();
    }
  }

  void ImportWorker::processItem(std::size_t itemIndex,
                                 ao::lmdb::WriteTransaction& txn,
                                 TrackStore::Writer& trackWriter,
                                 FileManifestStore::Writer& manifestWriter,
                                 DictionaryStore& dict)
  {
    auto const& item = _plan->items[itemIndex];

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
        // Update manifest status
        if (auto const optEntry = _ml.manifest().reader(txn).get(item.uri))
        {
          auto entry = *optEntry;
          entry.status = FileStatus::Missing;
          manifestWriter.put(item.uri, entry);
        }

        return;
      }

      // Handle NEW or CHANGED
      auto const tagFile = tag::TagFile::open(item.fullPath);

      if (!tagFile)
      {
        APP_LOG_WARN("Skipping unsupported file: {}", item.fullPath.string());
        ++_result.skippedCount;
        return;
      }

      auto builder = tagFile->loadTrack();
      builder.property().uri(item.uri).fileSize(item.fileSize).mtime(item.mtime);

      if (item.classification == ScanClassification::Changed && item.trackId != kInvalidTrackId)
      {
        auto optExisting = trackWriter.get(item.trackId, TrackStore::Reader::LoadMode::Both);

        if (optExisting)
        {
          auto merged = TrackBuilder::fromView(*optExisting, dict);
          merged.property()
            .durationMs(builder.property().durationMs())
            .bitrate(builder.property().bitrate())
            .sampleRate(builder.property().sampleRate())
            .channels(builder.property().channels())
            .codecId(builder.property().codecId())
            .bitDepth(builder.property().bitDepth());

          auto const [preparedHot, preparedCold] = merged.prepare(txn, dict, _ml.resources());
          trackWriter.updateHot(
            item.trackId, preparedHot.size(), [&](std::span<std::byte> hot) { preparedHot.writeTo(hot); });
          trackWriter.updateCold(
            item.trackId, preparedCold.size(), [&](std::span<std::byte> cold) { preparedCold.writeTo(cold); });

          // Update manifest
          auto entry = ManifestEntry{.trackId = item.trackId, .status = FileStatus::Available};
          entry.fileSize(item.fileSize);
          entry.mtime(item.mtime);
          manifestWriter.put(item.uri, entry);
          return;
        }
      }

      // New track
      auto [preparedHot, preparedCold] = builder.prepare(txn, dict, _ml.resources());
      auto [newTrackId, view] = trackWriter.createHotCold(
        preparedHot.size(),
        preparedCold.size(),
        [&preparedHot, &preparedCold](TrackId /*id*/, std::span<std::byte> hot, std::span<std::byte> cold)
        {
          preparedHot.writeTo(hot);
          preparedCold.writeTo(cold);
        });
      std::ignore = view;

      auto entry = ManifestEntry{.trackId = newTrackId, .status = FileStatus::Available};
      entry.fileSize(item.fileSize);
      entry.mtime(item.mtime);
      manifestWriter.put(item.uri, entry);

      _result.insertedIds.push_back(newTrackId);
    }
    catch (std::exception const& e)
    {
      APP_LOG_ERROR("Failed to process {}: {}", item.uri, e.what());
      ++_result.failureCount;
    }
  }

  void ImportWorker::join()
  {
    if (_workerThread.joinable())
    {
      _workerThread.join();
    }
  }

  std::size_t ImportWorker::fileCount() const
  {
    if (_plan)
    {
      return _plan->items.size();
    }

    return _files.size();
  }
} // namespace ao::library
