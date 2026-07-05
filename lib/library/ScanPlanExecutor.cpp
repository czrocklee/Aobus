// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibraryScanner.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ScanPlanExecutor.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackWrite.h>
#include <ao/tag/TagFile.h>
#include <ao/utility/Fnv1a.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
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
  namespace
  {
    constexpr std::size_t kFingerprintChunkSize = 4ULL * 1024ULL * 1024ULL;
  }

  ScanPlanExecutor::ScanPlanExecutor(MusicLibrary& ml,
                                     ScanPlan plan,
                                     ProgressCallback progress,
                                     ItemFailureCallback itemFailure)
    : _ml{ml}
    , _planPtr{std::make_unique<ScanPlan>(std::move(plan))}
    , _progressCallback{std::move(progress)}
    , _itemFailureCallback{std::move(itemFailure)}
  {
  }

  void ScanPlanExecutor::reportFailure(std::string_view uri, std::string_view stage, std::string_view message)
  {
    ++_result.failureCount;

    if (_itemFailureCallback)
    {
      _itemFailureCallback(ScanFailure{.uri = uri, .stage = stage, .message = message});
    }
  }

  Result<ScanApplyResult> ScanPlanExecutor::run(std::stop_token stopToken)
  {
    if (!_planPtr)
    {
      return _result;
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

      processItem(i, txn, trackWriter, manifestWriter, dict, stopToken);

      if (_abortTransaction)
      {
        break;
      }
    }

    if (_result.cancelled || stopToken.stop_requested())
    {
      _result.cancelled = true;
      _result.processedIds.clear();
      _result.relinkedCount = 0;
      _result.missingCount = 0;
      _result.failureCount = 0;
      return _result;
    }

    if (_abortTransaction)
    {
      _result.processedIds.clear();
      _result.relinkedCount = 0;
      _result.missingCount = 0;
      return _result;
    }

    if (auto result = txn.commit(); !result)
    {
      // The transaction did not persist, so nothing was actually processed.
      _result.processedIds.clear();
      _result.relinkedCount = 0;
      _result.missingCount = 0;
      return std::unexpected{result.error()};
    }

    return _result;
  }

  void ScanPlanExecutor::processItem(std::size_t itemIndex,
                                     ao::lmdb::WriteTransaction& txn,
                                     TrackStore::Writer& trackWriter,
                                     FileManifestStore::Writer& manifestWriter,
                                     DictionaryStore& dict,
                                     std::stop_token stopToken)
  {
    auto const& item = _planPtr->items[itemIndex];

    reportProgress(item, itemIndex, ProgressStage::Updating, 0.0);

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
    auto optFingerprint = cachedAudioFingerprint(item);

    if (!optFingerprint)
    {
      optFingerprint = fingerprintAudioPayload(item, *tagFilePtr, itemIndex, stopToken);
    }

    if (!optFingerprint)
    {
      return;
    }

    builder.property().uri(item.uri);

    if (item.classification == ScanClassification::Changed && item.trackId != kInvalidTrackId)
    {
      if (processChanged(item, txn, trackWriter, manifestWriter, dict, builder, *optFingerprint))
      {
        return;
      }
    }

    if (item.classification == ScanClassification::Moved)
    {
      if (!processMoved(item, txn, trackWriter, manifestWriter, dict, builder, *optFingerprint))
      {
        _abortTransaction = true;
      }

      return;
    }

    processNew(item, txn, trackWriter, manifestWriter, dict, builder, *optFingerprint);
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

  void ScanPlanExecutor::reportProgress(ScanItem const& item,
                                        std::size_t itemIndex,
                                        ProgressStage stage,
                                        double itemFraction)
  {
    if (_progressCallback)
    {
      _progressCallback(Progress{.path = item.fullPath,
                                 .itemIndex = static_cast<std::int32_t>(itemIndex),
                                 .stage = stage,
                                 .itemFraction = itemFraction});
    }
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

    if (writeManifest(manifestWriter, item.uri, builder))
    {
      ++_result.missingCount;
    }
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

  std::optional<ScanPlanExecutor::AudioFingerprint> ScanPlanExecutor::cachedAudioFingerprint(
    ScanItem const& item) const noexcept
  {
    if (item.classification != ScanClassification::New || !hasAudioIdentity(item))
    {
      return std::nullopt;
    }

    return AudioFingerprint{.signature = item.audioSignature, .payloadLength = item.audioPayloadLength};
  }

  std::optional<ScanPlanExecutor::AudioFingerprint> ScanPlanExecutor::fingerprintAudioPayload(
    ScanItem const& item,
    tag::TagFile const& tagFile,
    std::size_t itemIndex,
    std::stop_token stopToken)
  {
    auto payloadResult = tagFile.audioPayload();

    if (!payloadResult)
    {
      reportFailure(item.uri, "fingerprint", payloadResult.error().message);
      return std::nullopt;
    }

    auto const bytes = payloadResult->bytes;
    auto accumulator = utility::Fnv1a128Accumulator{};
    std::size_t processed = 0;

    reportProgress(item, itemIndex, ProgressStage::Fingerprinting, 0.0);

    while (processed < bytes.size())
    {
      if (stopToken.stop_requested())
      {
        _result.cancelled = true;
        return std::nullopt;
      }

      auto const remaining = bytes.size() - processed;
      auto const chunkSize = std::min(kFingerprintChunkSize, remaining);
      accumulator.mix(bytes.subspan(processed, chunkSize));
      processed += chunkSize;

      auto const fraction = bytes.empty() ? 1.0 : static_cast<double>(processed) / static_cast<double>(bytes.size());
      reportProgress(item, itemIndex, ProgressStage::Fingerprinting, fraction);

      if (stopToken.stop_requested())
      {
        _result.cancelled = true;
        return std::nullopt;
      }
    }

    if (bytes.empty())
    {
      reportProgress(item, itemIndex, ProgressStage::Fingerprinting, 1.0);
    }

    return AudioFingerprint{.signature = accumulator.value(), .payloadLength = bytes.size()};
  }

  bool ScanPlanExecutor::processChanged(ScanItem const& item,
                                        ao::lmdb::WriteTransaction& txn,
                                        TrackStore::Writer& trackWriter,
                                        FileManifestStore::Writer& manifestWriter,
                                        DictionaryStore& dict,
                                        TrackBuilder& builder,
                                        AudioFingerprint const& fingerprint)
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

    auto manifestBuilder = makeAvailableManifest(item, item.trackId, fingerprint);

    if (!writeManifest(manifestWriter, item.uri, manifestBuilder))
    {
      return true;
    }

    _result.processedIds.push_back(item.trackId);
    return true;
  }

  bool ScanPlanExecutor::processMoved(ScanItem const& item,
                                      ao::lmdb::WriteTransaction& txn,
                                      TrackStore::Writer& trackWriter,
                                      FileManifestStore::Writer& manifestWriter,
                                      DictionaryStore& dict,
                                      TrackBuilder& builder,
                                      AudioFingerprint const& fingerprint)
  {
    if (item.trackId == kInvalidTrackId || item.oldUri.empty())
    {
      reportFailure(item.uri, "relink", "moved scan item is missing its previous track identity");
      return false;
    }

    if (!hasAudioIdentity(item))
    {
      reportFailure(item.uri, "relink", "moved scan item is missing its planned audio identity");
      return false;
    }

    if (item.audioPayloadLength != fingerprint.payloadLength || item.audioSignature != fingerprint.signature)
    {
      reportFailure(item.uri, "relink", "audio identity changed before apply");
      return false;
    }

    auto optExisting = trackWriter.get(item.trackId, TrackStore::Reader::LoadMode::Both);

    if (!optExisting)
    {
      reportFailure(item.uri, "read existing track for", "track record was not found");
      return false;
    }

    auto merged = TrackBuilder::fromView(*optExisting, dict);
    merged.property()
      .uri(item.uri)
      .duration(builder.property().duration())
      .bitrate(builder.property().bitrate())
      .sampleRate(builder.property().sampleRate())
      .channels(builder.property().channels())
      .codec(builder.property().codec())
      .bitDepth(builder.property().bitDepth());

    auto optPrepared = prepareTrack(merged, txn, dict, item.uri);

    if (!optPrepared)
    {
      return false;
    }

    auto const& [preparedHot, preparedCold] = *optPrepared;

    if (!updateTrack(trackWriter, item.trackId, item.uri, preparedHot, preparedCold))
    {
      return false;
    }

    if (auto removeResult = manifestWriter.remove(item.oldUri); !removeResult)
    {
      reportFailure(item.uri, "remove old manifest for", removeResult.error().message);
      return false;
    }

    auto manifestBuilder = makeAvailableManifest(item, item.trackId, fingerprint);

    if (!writeManifest(manifestWriter, item.uri, manifestBuilder))
    {
      return false;
    }

    _result.processedIds.push_back(item.trackId);
    ++_result.relinkedCount;
    return true;
  }

  void ScanPlanExecutor::processNew(ScanItem const& item,
                                    ao::lmdb::WriteTransaction& txn,
                                    TrackStore::Writer& trackWriter,
                                    FileManifestStore::Writer& manifestWriter,
                                    DictionaryStore& dict,
                                    TrackBuilder& builder,
                                    AudioFingerprint const& fingerprint)
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

    auto manifestBuilder = makeAvailableManifest(item, *optNewTrackId, fingerprint);

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

  FileManifestBuilder ScanPlanExecutor::makeAvailableManifest(ScanItem const& item,
                                                              TrackId trackId,
                                                              AudioFingerprint const& fingerprint)
  {
    auto builder = FileManifestBuilder::createNew();
    builder.trackId(trackId)
      .status(FileStatus::Available)
      .fileSize(item.fileSize)
      .mtime(item.mtime)
      .audioPayloadLength(fingerprint.payloadLength)
      .audioSignature(fingerprint.signature);
    return builder;
  }

  bool ScanPlanExecutor::updateTrack(TrackStore::Writer& trackWriter,
                                     TrackId trackId,
                                     std::string const& uri,
                                     TrackBuilder::PreparedHot const& hot,
                                     TrackBuilder::PreparedCold const& cold)
  {
    auto hotResult = updatePreparedHotTrackData(trackWriter, trackId, hot);

    if (!hotResult)
    {
      reportFailure(uri, "update hot track data for", hotResult.error().message);
      return false;
    }

    auto coldResult = updatePreparedColdTrackData(trackWriter, trackId, cold);

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
    auto createResult = createPreparedTrackData(trackWriter, hot, cold);

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
