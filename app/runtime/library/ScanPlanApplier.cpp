// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ScanPlanApplier.h"

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/AudioIdentity.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackWrite.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/tag/TagFile.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt
{
  ScanPlanApplier::ScanPlanApplier(library::MusicLibrary& ml,
                                   ScanPlan plan,
                                   std::move_only_function<void(ScanApplyProgress const& progress)> progress,
                                   std::move_only_function<void(ScanFailure const& failure)> itemFailure,
                                   ScanApplyOptions options)
    : _ml{ml}
    , _plan{std::move(plan)}
    , _options{options}
    , _progressCallback{std::move(progress)}
    , _itemFailureCallback{std::move(itemFailure)}
  {
  }

  void ScanPlanApplier::reportFailure(std::string_view uri, std::string_view stage, std::string_view message)
  {
    ++_result.failureCount;

    if (_itemFailureCallback)
    {
      _itemFailureCallback(ScanFailure{.uri = uri, .stage = stage, .message = message});
    }
  }

  Result<ScanApplyResult> ScanPlanApplier::run(std::stop_token stopToken)
  {
    auto transaction = _ml.writeTransaction();
    auto trackWriter = _ml.tracks().writer(transaction);
    auto manifestWriter = _ml.manifest().writer(transaction);
    auto& dictionary = _ml.dictionary();

    for (std::size_t i = 0; i < _plan.items.size(); ++i)
    {
      if (stopToken.stop_requested())
      {
        _result.cancelled = true;
        break;
      }

      applyScanItem(i, transaction, trackWriter, manifestWriter, dictionary, stopToken);

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

    if (auto result = transaction.commit(); !result)
    {
      // The transaction did not persist, so nothing was actually processed.
      _result.processedIds.clear();
      _result.relinkedCount = 0;
      _result.missingCount = 0;
      return std::unexpected{result.error()};
    }

    return _result;
  }

  void ScanPlanApplier::applyScanItem(std::size_t itemIndex,
                                      ao::lmdb::WriteTransaction& transaction,
                                      library::TrackStore::Writer& trackWriter,
                                      library::FileManifestStore::Writer& manifestWriter,
                                      library::DictionaryStore& dictionary,
                                      std::stop_token stopToken)
  {
    auto const& item = _plan.items[itemIndex];

    reportProgress(item, itemIndex, ScanApplyProgressStage::Updating, 0.0);

    if (skipNonActionableItem(item))
    {
      return;
    }

    if (item.classification == ScanClassification::Missing)
    {
      applyMissingItem(item, transaction, manifestWriter);
      return;
    }

    auto optLoad = loadTrackBuilder(item);

    if (!optLoad)
    {
      return;
    }

    auto& [tagFilePtr, builder] = *optLoad;
    auto optFingerprint = cachedAudioFingerprint(item);

    if (!optFingerprint && shouldFingerprintDuringApply(item))
    {
      optFingerprint = fingerprintAudioPayload(item, *tagFilePtr, itemIndex, stopToken);
    }

    if (!optFingerprint && requiresFingerprintForApply(item))
    {
      return;
    }

    builder.property().uri(item.uri);

    if (item.classification == ScanClassification::Changed && item.trackId != kInvalidTrackId)
    {
      if (!optFingerprint)
      {
        return;
      }

      if (tryApplyChangedItem(item, transaction, trackWriter, manifestWriter, dictionary, builder, *optFingerprint))
      {
        return;
      }
    }

    if (item.classification == ScanClassification::Moved)
    {
      _abortTransaction = true;

      if (optFingerprint &&
          applyMovedItem(item, transaction, trackWriter, manifestWriter, dictionary, builder, *optFingerprint))
      {
        _abortTransaction = false;
      }

      return;
    }

    applyNewItem(item, transaction, trackWriter, manifestWriter, dictionary, builder, optFingerprint);
  }

  bool ScanPlanApplier::skipNonActionableItem(ScanItem const& item)
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

  void ScanPlanApplier::reportProgress(ScanItem const& item,
                                       std::size_t itemIndex,
                                       ScanApplyProgressStage stage,
                                       double itemFraction)
  {
    if (_progressCallback)
    {
      _progressCallback(ScanApplyProgress{.path = item.fullPath,
                                          .itemIndex = static_cast<std::int32_t>(itemIndex),
                                          .stage = stage,
                                          .itemFraction = itemFraction});
    }
  }

  void ScanPlanApplier::applyMissingItem(ScanItem const& item,
                                         ao::lmdb::WriteTransaction& transaction,
                                         library::FileManifestStore::Writer& manifestWriter)
  {
    auto manifestResult = _ml.manifest().reader(transaction).get(item.uri);

    if (!manifestResult)
    {
      if (manifestResult.error().code == Error::Code::NotFound)
      {
        return;
      }

      reportFailure(item.uri, "read manifest for", manifestResult.error().message);
      return;
    }

    auto builder = library::FileManifestBuilder::fromView(*manifestResult);
    builder.status(library::FileStatus::Missing);

    if (writeManifest(manifestWriter, item.uri, builder))
    {
      ++_result.missingCount;
    }
  }

  std::optional<std::pair<std::unique_ptr<tag::TagFile>, library::TrackBuilder>> ScanPlanApplier::loadTrackBuilder(
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

  std::optional<ScanPlanApplier::AudioFingerprint> ScanPlanApplier::cachedAudioFingerprint(
    ScanItem const& item) const noexcept
  {
    if (item.classification != ScanClassification::New || !hasAudioIdentity(item))
    {
      return std::nullopt;
    }

    return AudioFingerprint{.signature = item.audioSignature, .payloadLength = item.audioPayloadLength};
  }

  bool ScanPlanApplier::shouldFingerprintDuringApply(ScanItem const& item) const noexcept
  {
    switch (item.classification)
    {
      case ScanClassification::Moved:
      case ScanClassification::Changed: return true;
      case ScanClassification::New: return _options.audioIdentityPolicy == AudioIdentityPolicy::Eager;
      case ScanClassification::Missing:
      case ScanClassification::Unchanged:
      case ScanClassification::Error: return false;
    }

    return false;
  }

  bool ScanPlanApplier::requiresFingerprintForApply(ScanItem const& item) const noexcept
  {
    switch (item.classification)
    {
      case ScanClassification::Moved:
      case ScanClassification::Changed: return true;
      case ScanClassification::New: return _options.audioIdentityPolicy == AudioIdentityPolicy::Eager;
      case ScanClassification::Missing:
      case ScanClassification::Unchanged:
      case ScanClassification::Error: return false;
    }

    return false;
  }

  std::optional<ScanPlanApplier::AudioFingerprint> ScanPlanApplier::fingerprintAudioPayload(ScanItem const& item,
                                                                                            tag::TagFile const& tagFile,
                                                                                            std::size_t itemIndex,
                                                                                            std::stop_token stopToken)
  {
    auto identityResult = library::readAudioIdentity(
      tagFile,
      [this, &item, itemIndex](double fraction)
      { reportProgress(item, itemIndex, ScanApplyProgressStage::Fingerprinting, fraction); },
      stopToken);

    if (!identityResult)
    {
      reportFailure(item.uri, "fingerprint", identityResult.error().message);
      return std::nullopt;
    }

    if (!*identityResult)
    {
      _result.cancelled = true;
      return std::nullopt;
    }

    auto const& identity = **identityResult;
    return AudioFingerprint{.signature = identity.signature, .payloadLength = identity.payloadLength};
  }

  bool ScanPlanApplier::tryApplyChangedItem(ScanItem const& item,
                                            ao::lmdb::WriteTransaction& transaction,
                                            library::TrackStore::Writer& trackWriter,
                                            library::FileManifestStore::Writer& manifestWriter,
                                            library::DictionaryStore& dictionary,
                                            library::TrackBuilder& builder,
                                            AudioFingerprint const& fingerprint)
  {
    auto optExisting = trackWriter.get(item.trackId, library::TrackStore::Reader::LoadMode::Both);

    if (!optExisting)
    {
      return false;
    }

    auto merged = library::TrackBuilder::fromView(*optExisting, dictionary);
    merged.property()
      .duration(builder.property().duration())
      .bitrate(builder.property().bitrate())
      .sampleRate(builder.property().sampleRate())
      .channels(builder.property().channels())
      .codec(builder.property().codec())
      .bitDepth(builder.property().bitDepth());

    auto optPrepared = prepareTrack(merged, transaction, dictionary, item.uri);

    if (!optPrepared)
    {
      return true;
    }

    auto const& [preparedHot, preparedCold] = *optPrepared;

    if (!updateTrack(trackWriter, item.trackId, item.uri, preparedHot, preparedCold))
    {
      return true;
    }

    auto manifestBuilder = makeAvailableManifest(item, item.trackId, std::optional<AudioFingerprint>{fingerprint});

    if (!writeManifest(manifestWriter, item.uri, manifestBuilder))
    {
      return true;
    }

    _result.processedIds.push_back(item.trackId);
    return true;
  }

  bool ScanPlanApplier::applyMovedItem(ScanItem const& item,
                                       ao::lmdb::WriteTransaction& transaction,
                                       library::TrackStore::Writer& trackWriter,
                                       library::FileManifestStore::Writer& manifestWriter,
                                       library::DictionaryStore& dictionary,
                                       library::TrackBuilder& builder,
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

    auto optExisting = trackWriter.get(item.trackId, library::TrackStore::Reader::LoadMode::Both);

    if (!optExisting)
    {
      reportFailure(item.uri, "read existing track for", "track record was not found");
      return false;
    }

    auto merged = library::TrackBuilder::fromView(*optExisting, dictionary);
    merged.property()
      .uri(item.uri)
      .duration(builder.property().duration())
      .bitrate(builder.property().bitrate())
      .sampleRate(builder.property().sampleRate())
      .channels(builder.property().channels())
      .codec(builder.property().codec())
      .bitDepth(builder.property().bitDepth());

    auto optPrepared = prepareTrack(merged, transaction, dictionary, item.uri);

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

    auto manifestBuilder = makeAvailableManifest(item, item.trackId, std::optional<AudioFingerprint>{fingerprint});

    if (!writeManifest(manifestWriter, item.uri, manifestBuilder))
    {
      return false;
    }

    _result.processedIds.push_back(item.trackId);
    ++_result.relinkedCount;
    return true;
  }

  void ScanPlanApplier::applyNewItem(ScanItem const& item,
                                     ao::lmdb::WriteTransaction& transaction,
                                     library::TrackStore::Writer& trackWriter,
                                     library::FileManifestStore::Writer& manifestWriter,
                                     library::DictionaryStore& dictionary,
                                     library::TrackBuilder& builder,
                                     std::optional<AudioFingerprint> const& optFingerprint)
  {
    auto optPrepared = prepareTrack(builder, transaction, dictionary, item.uri);

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

    auto manifestBuilder = makeAvailableManifest(item, *optNewTrackId, optFingerprint);

    if (!writeManifest(manifestWriter, item.uri, manifestBuilder))
    {
      return;
    }

    _result.processedIds.push_back(*optNewTrackId);
  }

  std::optional<std::pair<library::TrackBuilder::PreparedHot, library::TrackBuilder::PreparedCold>>
  ScanPlanApplier::prepareTrack(library::TrackBuilder const& builder,
                                ao::lmdb::WriteTransaction& transaction,
                                library::DictionaryStore& dictionary,
                                std::string const& uri)
  {
    auto preparedResult = builder.prepare(transaction, dictionary, _ml.resources());

    if (!preparedResult)
    {
      reportFailure(uri, "serialize", preparedResult.error().message);
      return std::nullopt;
    }

    return *preparedResult;
  }

  library::FileManifestBuilder ScanPlanApplier::makeAvailableManifest(
    ScanItem const& item,
    TrackId trackId,
    std::optional<AudioFingerprint> const& optFingerprint)
  {
    auto builder = library::FileManifestBuilder::makeEmpty();
    builder.trackId(trackId).status(library::FileStatus::Available).fileSize(item.fileSize).mtime(item.mtime);

    if (optFingerprint)
    {
      builder.audioPayloadLength(optFingerprint->payloadLength).audioSignature(optFingerprint->signature);
    }

    return builder;
  }

  bool ScanPlanApplier::updateTrack(library::TrackStore::Writer& trackWriter,
                                    TrackId trackId,
                                    std::string const& uri,
                                    library::TrackBuilder::PreparedHot const& hot,
                                    library::TrackBuilder::PreparedCold const& cold)
  {
    auto hotResult = library::updatePreparedHotTrackData(trackWriter, trackId, hot);

    if (!hotResult)
    {
      reportFailure(uri, "update hot track data for", hotResult.error().message);
      return false;
    }

    auto coldResult = library::updatePreparedColdTrackData(trackWriter, trackId, cold);

    if (!coldResult)
    {
      reportFailure(uri, "update cold track data for", coldResult.error().message);
      return false;
    }

    return true;
  }

  std::optional<TrackId> ScanPlanApplier::createTrack(library::TrackStore::Writer& trackWriter,
                                                      std::string const& uri,
                                                      library::TrackBuilder::PreparedHot const& hot,
                                                      library::TrackBuilder::PreparedCold const& cold)
  {
    auto createResult = library::createPreparedTrackData(trackWriter, hot, cold);

    if (!createResult)
    {
      reportFailure(uri, "create track data for", createResult.error().message);
      return std::nullopt;
    }

    auto const [newTrackId, trackView] = *createResult;
    std::ignore = trackView;

    return newTrackId;
  }

  bool ScanPlanApplier::writeManifest(library::FileManifestStore::Writer& writer,
                                      std::string const& uri,
                                      library::FileManifestBuilder& builder)
  {
    if (auto putResult = writer.put(uri, builder.serialize()); !putResult)
    {
      reportFailure(uri, "update manifest for", putResult.error().message);
      return false;
    }

    return true;
  }

  std::size_t ScanPlanApplier::fileCount() const
  {
    return _plan.items.size();
  }
} // namespace ao::rt
