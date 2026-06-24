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

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::library
{
  ScanPlanExecutor::ScanPlanExecutor(MusicLibrary& ml,
                                     ScanPlan plan,
                                     ProgressCallback progress,
                                     FailureCallback failure)
    : _ml{ml}
    , _planPtr{std::make_unique<ScanPlan>(std::move(plan))}
    , _progressCallback{std::move(progress)}
    , _failureCallback{std::move(failure)}
  {
  }

  void ScanPlanExecutor::reportFailure(std::string_view uri, std::string_view stage, std::string_view message)
  {
    if (_failureCallback)
    {
      _failureCallback(ScanFailure{.uri = uri, .stage = stage, .message = message});
    }
  }

  void ScanPlanExecutor::run(std::stop_token stopToken)
  {
    if (!_planPtr)
    {
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

    if (auto result = txn.commit(); !result)
    {
      // The transaction did not persist, so nothing was actually processed.
      _result.processedIds.clear();
      reportFailure({}, "commit scan results", result.error().message);
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

      if (processSkips(item))
      {
        return;
      }

      if (item.classification == ScanClassification::Missing)
      {
        processMissing(item, txn, manifestWriter);
        return;
      }

      auto optLoad = loadTrackBuilder(item);

      if (!optLoad)
      {
        return;
      }

      auto& [tagFilePtr, builder] = *optLoad;
      std::ignore = tagFilePtr;

      builder.property().uri(item.uri);

      if (item.classification == ScanClassification::Changed && item.trackId != kInvalidTrackId)
      {
        if (processChanged(item, txn, trackWriter, manifestWriter, dict, builder))
        {
          return;
        }
      }

      processNew(item, txn, trackWriter, manifestWriter, dict, builder);
    }
    catch (std::exception const& e)
    {
      reportFailure(item.uri, "process", e.what());
    }
  }

  bool ScanPlanExecutor::processSkips(ScanItem const& item)
  {
    if (item.classification == ScanClassification::Unchanged)
    {
      // Benign: the file is already imported and has not changed.
      return true;
    }

    if (item.classification == ScanClassification::Error)
    {
      reportFailure(item.uri, "scan", item.errorMessage);
      return true;
    }

    return false;
  }

  void ScanPlanExecutor::processMissing(ScanItem const& item,
                                        ao::lmdb::WriteTransaction& txn,
                                        FileManifestStore::Writer& manifestWriter)
  {
    auto manifestResult = _ml.manifest().reader(txn).get(item.uri);

    if (!manifestResult)
    {
      if (manifestResult.error().code == Error::Code::NotFound)
      {
        return;
      }

      reportFailure(item.uri, "read manifest for", manifestResult.error().message);
      return;
    }

    auto builder = FileManifestBuilder::fromView(*manifestResult);
    builder.status(FileStatus::Missing);

    writeManifest(manifestWriter, item.uri, builder);
  }

  std::optional<std::pair<std::unique_ptr<tag::TagFile>, TrackBuilder>> ScanPlanExecutor::loadTrackBuilder(
    ScanItem const& item)
  {
    auto tagFileResult = tag::TagFile::open(item.fullPath);

    if (!tagFileResult)
    {
      // The scanner only admits decodable extensions, so open() should not see
      // an unsupported format here; a failure is a genuine I/O or parse fault.
      reportFailure(item.uri, "open", tagFileResult.error().message);
      return std::nullopt;
    }

    auto tagFilePtr = std::move(*tagFileResult);
    auto builderResult = tagFilePtr->loadTrack();

    if (!builderResult)
    {
      reportFailure(item.uri, "read tags from", builderResult.error().message);
      return std::nullopt;
    }

    return std::make_pair(std::move(tagFilePtr), *builderResult);
  }

  bool ScanPlanExecutor::processChanged(ScanItem const& item,
                                        ao::lmdb::WriteTransaction& txn,
                                        TrackStore::Writer& trackWriter,
                                        FileManifestStore::Writer& manifestWriter,
                                        DictionaryStore& dict,
                                        TrackBuilder& builder)
  {
    auto optExisting = trackWriter.get(item.trackId, TrackStore::Reader::LoadMode::Both);

    if (!optExisting)
    {
      return false;
    }

    auto merged = TrackBuilder::fromView(*optExisting, dict);
    merged.property()
      .duration(builder.property().duration())
      .bitrate(builder.property().bitrate())
      .sampleRate(builder.property().sampleRate())
      .channels(builder.property().channels())
      .codec(builder.property().codec())
      .bitDepth(builder.property().bitDepth());

    auto optPrepared = prepareTrack(merged, txn, dict, item.uri);

    if (!optPrepared)
    {
      return true;
    }

    auto const& [preparedHot, preparedCold] = *optPrepared;

    if (!updateTrack(trackWriter, item.trackId, item.uri, preparedHot, preparedCold))
    {
      return true;
    }

    auto manifestBuilder = FileManifestBuilder::createNew();
    manifestBuilder.trackId(item.trackId).status(FileStatus::Available).fileSize(item.fileSize).mtime(item.mtime);

    if (!writeManifest(manifestWriter, item.uri, manifestBuilder))
    {
      return true;
    }

    _result.processedIds.push_back(item.trackId);
    return true;
  }

  void ScanPlanExecutor::processNew(ScanItem const& item,
                                    ao::lmdb::WriteTransaction& txn,
                                    TrackStore::Writer& trackWriter,
                                    FileManifestStore::Writer& manifestWriter,
                                    DictionaryStore& dict,
                                    TrackBuilder& builder)
  {
    auto optPrepared = prepareTrack(builder, txn, dict, item.uri);

    if (!optPrepared)
    {
      return;
    }

    auto const& [preparedHot, preparedCold] = *optPrepared;

    auto optNewTrackId = createTrack(trackWriter, item.uri, preparedHot, preparedCold);

    if (!optNewTrackId)
    {
      return;
    }

    auto manifestBuilder = FileManifestBuilder::createNew();
    manifestBuilder.trackId(*optNewTrackId).status(FileStatus::Available).fileSize(item.fileSize).mtime(item.mtime);

    if (!writeManifest(manifestWriter, item.uri, manifestBuilder))
    {
      return;
    }

    _result.processedIds.push_back(*optNewTrackId);
  }

  std::optional<std::pair<TrackBuilder::PreparedHot, TrackBuilder::PreparedCold>> ScanPlanExecutor::prepareTrack(
    TrackBuilder const& builder,
    ao::lmdb::WriteTransaction& txn,
    DictionaryStore& dict,
    std::string const& uri)
  {
    auto preparedResult = builder.prepare(txn, dict, _ml.resources());

    if (!preparedResult)
    {
      reportFailure(uri, "serialize", preparedResult.error().message);
      return std::nullopt;
    }

    return *preparedResult;
  }

  bool ScanPlanExecutor::updateTrack(TrackStore::Writer& trackWriter,
                                     TrackId trackId,
                                     std::string const& uri,
                                     TrackBuilder::PreparedHot const& hot,
                                     TrackBuilder::PreparedCold const& cold)
  {
    auto hotResult =
      trackWriter.updateHot(trackId, hot.size(), [&](std::span<std::byte> hotBuffer) { hot.writeTo(hotBuffer); });

    if (!hotResult)
    {
      reportFailure(uri, "update hot track data for", hotResult.error().message);
      return false;
    }

    auto coldResult =
      trackWriter.updateCold(trackId, cold.size(), [&](std::span<std::byte> coldBuffer) { cold.writeTo(coldBuffer); });

    if (!coldResult)
    {
      reportFailure(uri, "update cold track data for", coldResult.error().message);
      return false;
    }

    return true;
  }

  std::optional<TrackId> ScanPlanExecutor::createTrack(TrackStore::Writer& trackWriter,
                                                       std::string const& uri,
                                                       TrackBuilder::PreparedHot const& hot,
                                                       TrackBuilder::PreparedCold const& cold)
  {
    auto createResult = trackWriter.createHotCold(
      hot.size(),
      cold.size(),
      [&hot, &cold](TrackId /*id*/, std::span<std::byte> hotBuffer, std::span<std::byte> coldBuffer)
      {
        hot.writeTo(hotBuffer);
        cold.writeTo(coldBuffer);
      });

    if (!createResult)
    {
      reportFailure(uri, "create track data for", createResult.error().message);
      return std::nullopt;
    }

    auto const [newTrackId, trackView] = *createResult;
    std::ignore = trackView;

    return newTrackId;
  }

  bool ScanPlanExecutor::writeManifest(FileManifestStore::Writer& writer,
                                       std::string const& uri,
                                       FileManifestBuilder& builder)
  {
    if (auto putResult = writer.put(uri, builder.serialize()); !putResult)
    {
      reportFailure(uri, "update manifest for", putResult.error().message);
      return false;
    }

    return true;
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
