// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ScanApplyOperation.h"

#include "MediaTrack.h"
#include "TrackBuilderSnapshot.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/OperationCancelled.h>
#include <ao/library/AudioIdentity.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibraryUri.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/MetadataStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackWrite.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/media/file/File.h>
#include <ao/rt/library/ScanPlan.h>

#include <gsl-lite/gsl-lite.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt
{
  struct ScanApplyOperation::PreparedScanItem final
  {
    explicit PreparedScanItem(library::TrackBuilder const& source,
                              std::optional<library::AudioIdentity> optIdentityValue)
      : builder{source}, optIdentity{std::move(optIdentityValue)}
    {
    }

    TrackBuilderSnapshot builder;
    std::optional<library::AudioIdentity> optIdentity;
  };

  ScanApplyOperation::ScanApplyOperation(library::MusicLibrary& ml,
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

  ScanApplyOperation::~ScanApplyOperation() = default;

  void ScanApplyOperation::reportFailure(std::string_view uri, std::string_view stage, std::string_view message)
  {
    ++_result.failureCount;

    if (_itemFailureCallback)
    {
      _itemFailureCallback(ScanFailure{.uri = uri, .stage = stage, .message = message});
    }
  }

  Result<ScanApplyResult> ScanApplyOperation::run(std::stop_token stopToken)
  {
    if (_state == State::Created)
    {
      if (auto prepareRes = prepare(stopToken); !prepareRes)
      {
        return prepareRes;
      }
    }

    if (_cancelled)
    {
      async::throwOperationCancelled();
    }

    if (_state == State::Prepared)
    {
      if (auto revalidationRes = revalidatePreparedFiles(stopToken); !revalidationRes)
      {
        return revalidationRes;
      }
    }

    if (_cancelled)
    {
      async::throwOperationCancelled();
    }

    if (_state == State::Terminal)
    {
      return _result;
    }

    gsl_Assert(_state == State::Revalidated && "Scan apply operation is not ready for database mutation");

    auto writableRes = library::WritableMusicLibrary::acquire(_ml);

    if (!writableRes)
    {
      return std::unexpected{writableRes.error()};
    }

    auto transaction = writableRes->writeTransaction();
    auto result = transaction.apply([this, stopToken](library::WriteTransaction& activeTransaction)
                                    { return apply(activeTransaction, stopToken); });

    if (!result)
    {
      return result;
    }

    if (_cancelled)
    {
      async::throwOperationCancelled();
    }

    if (!transactionShouldCommit())
    {
      return result;
    }

    result->libraryRevision = _ml.libraryRevision(transaction);

    if (auto commitRes = transaction.commit(); !commitRes)
    {
      result->libraryRevision = 0;
      result->insertedIds.clear();
      result->mutatedIds.clear();
      result->relinkedIds.clear();
      result->missingCount = 0;
      return std::unexpected{commitRes.error()};
    }

    return result;
  }

  Result<ScanApplyResult> ScanApplyOperation::prepare(std::stop_token stopToken)
  {
    gsl_Assert(_state == State::Created && "Scan apply operation is already prepared");

    if (auto const bindingRes = validatePlan(); !bindingRes)
    {
      return std::unexpected{bindingRes.error()};
    }

    _preparedItems.resize(_plan.size());

    for (std::size_t i = 0; i < _plan.size(); ++i)
    {
      if (stopToken.stop_requested())
      {
        _cancelled = true;
        break;
      }

      auto const& item = _plan.items()[i];
      reportProgress(item, i, ScanApplyProgressStage::Updating, 0.0);

      if (item.classification == ScanClassification::Error)
      {
        reportFailure(item.uri, "scan", item.errorMessage);
        continue;
      }

      if (item.classification == ScanClassification::Missing || item.classification == ScanClassification::Unchanged)
      {
        continue;
      }

      auto optMediaTrack = loadTrackBuilder(item);

      if (!optMediaTrack)
      {
        continue;
      }

      auto optIdentity = cachedAudioIdentity(item);

      if (!optIdentity && shouldFingerprintDuringPreparation(item))
      {
        optIdentity = fingerprintAudioPayload(item, optMediaTrack->file(), i, true, stopToken);
      }

      if (_cancelled || stopToken.stop_requested())
      {
        _cancelled = true;
        break;
      }

      if (!optIdentity && isFingerprintRequiredForApply(item))
      {
        continue;
      }

      _preparedItems[i] = std::make_unique<PreparedScanItem>(optMediaTrack->builder(), std::move(optIdentity));
    }

    if (_cancelled)
    {
      _result.failureCount = 0;
      _state = State::Terminal;
    }
    else
    {
      _state = State::Prepared;
    }

    return _result;
  }

  Result<> ScanApplyOperation::validatePlan() const
  {
    gsl_Assert(_plan._executable && "Scan plan has already been consumed");

    auto const transaction = _ml.readTransaction();
    auto const headerRes = _ml.metadata().load(transaction);

    if (!headerRes)
    {
      return std::unexpected{headerRes.error()};
    }

    if (headerRes->libraryId != _plan._libraryId)
    {
      return makeError(Error::Code::InvalidInput, "Scan plan belongs to another library");
    }

    if (_ml.libraryRevision(transaction) != _plan._libraryRevision)
    {
      return makeError(Error::Code::Conflict, "Library changed since the scan plan was built");
    }

    return {};
  }

  Result<std::filesystem::path> ScanApplyOperation::resolveItemPath(ScanItem const& item) const
  {
    auto uriRes = library::LibraryUri::parse(item.uri);

    if (!uriRes)
    {
      return std::unexpected{uriRes.error()};
    }

    return uriRes->resolveUnder(_ml.rootPath());
  }

  Result<ScanApplyResult> ScanApplyOperation::revalidatePreparedFiles(std::stop_token stopToken)
  {
    gsl_Assert(_state == State::Prepared && "Scan apply operation must be prepared before file revalidation");

    for (std::size_t i = 0; i < _plan.size(); ++i)
    {
      auto const& item = _plan.items()[i];

      if (item.classification != ScanClassification::Moved)
      {
        continue;
      }

      if (stopToken.stop_requested())
      {
        _cancelled = true;
        break;
      }

      auto const* const preparedItem = _preparedItems[i].get();

      if (preparedItem == nullptr || !preparedItem->optIdentity)
      {
        _abortTransaction = true;
        break;
      }

      auto fullPathRes = resolveItemPath(item);

      if (!fullPathRes)
      {
        reportFailure(item.uri, "resolve moved destination for", fullPathRes.error().message);
        _abortTransaction = true;
        break;
      }

      auto fileRes = media::file::File::open(*fullPathRes);

      if (!fileRes)
      {
        reportFailure(item.uri, "open moved destination for", fileRes.error().message);
        _abortTransaction = true;
        break;
      }

      auto const optLiveIdentity = fingerprintAudioPayload(item, *fileRes, i, false, stopToken);

      if (_cancelled)
      {
        break;
      }

      if (auto const& preparedIdentity = *preparedItem->optIdentity;
          !optLiveIdentity || optLiveIdentity->payloadLength != preparedIdentity.payloadLength ||
          optLiveIdentity->signature != preparedIdentity.signature ||
          optLiveIdentity->payloadLength != item.audioPayloadLength ||
          optLiveIdentity->signature != item.audioSignature)
      {
        if (optLiveIdentity)
        {
          reportFailure(item.uri, "relink", "audio identity changed after preparation");
        }

        _abortTransaction = true;
        break;
      }
    }

    if (_cancelled)
    {
      _result.insertedIds.clear();
      _result.mutatedIds.clear();
      _result.relinkedIds.clear();
      _result.missingCount = 0;
      _result.failureCount = 0;
      _state = State::Terminal;
    }
    else if (_abortTransaction)
    {
      _state = State::Terminal;
    }
    else
    {
      _state = State::Revalidated;
    }

    return _result;
  }

  Result<ScanApplyResult> ScanApplyOperation::apply(library::WriteTransaction& transaction, std::stop_token stopToken)
  {
    gsl_Assert(_state == State::Revalidated &&
               "Scan apply operation must be revalidated exactly once before database mutation");

    _state = State::Applied;

    if (_cancelled)
    {
      return _result;
    }

    if (_abortTransaction)
    {
      return _result;
    }

    gsl_Assert(_plan._executable && "Scan plan has already been consumed");

    auto const headerRes = _ml.metadata().load(transaction);

    if (!headerRes)
    {
      return std::unexpected{headerRes.error()};
    }

    if (headerRes->libraryId != _plan._libraryId)
    {
      return makeError(Error::Code::InvalidInput, "Scan plan belongs to another library");
    }

    auto const transactionRevision = _ml.libraryRevision(transaction);

    if (transactionRevision == 0 || transactionRevision - 1U != _plan._libraryRevision)
    {
      return makeError(Error::Code::Conflict, "Library changed since the scan plan was built");
    }

    auto trackWriter = _ml.tracks().writer(transaction);
    auto manifestWriter = _ml.manifest().writer(transaction);
    auto const& dictionary = _ml.dictionary();

    if (auto validationRes = validatePersistedTrackEvidence(trackWriter); !validationRes)
    {
      return std::unexpected{validationRes.error()};
    }

    for (std::size_t i = 0; i < _plan.size(); ++i)
    {
      if (stopToken.stop_requested())
      {
        _cancelled = true;
        break;
      }

      if (auto itemRes =
            applyScanItem(i, _preparedItems[i].get(), transaction, trackWriter, manifestWriter, dictionary);
          !itemRes)
      {
        return std::unexpected{std::move(itemRes.error())};
      }

      if (_abortTransaction)
      {
        break;
      }
    }

    if (_cancelled || stopToken.stop_requested())
    {
      _cancelled = true;
      _result.insertedIds.clear();
      _result.mutatedIds.clear();
      _result.relinkedIds.clear();
      _result.missingCount = 0;
      _result.failureCount = 0;
      return _result;
    }

    if (_abortTransaction)
    {
      _result.insertedIds.clear();
      _result.mutatedIds.clear();
      _result.relinkedIds.clear();
      _result.missingCount = 0;
      return _result;
    }

    return _result;
  }

  Result<> ScanApplyOperation::validatePersistedTrackEvidence(library::TrackStore::Writer const& trackWriter) const
  {
    for (auto const& item : _plan.items())
    {
      if (item.trackId == kInvalidTrackId)
      {
        continue;
      }

      auto const optHot = trackWriter.get(item.trackId, library::TrackStore::Reader::LoadMode::Hot);
      auto const optCold = trackWriter.get(item.trackId, library::TrackStore::Reader::LoadMode::Cold);

      if (!optHot || !optCold || !optHot->isHotValid() || !optCold->isColdValid())
      {
        return makeError(
          Error::Code::CorruptData,
          std::format("Scan item '{}' refers to missing or invalid Track {}", item.uri, item.trackId.raw()));
      }
    }

    return {};
  }

  bool ScanApplyOperation::cancelled() const noexcept
  {
    return _cancelled;
  }

  bool ScanApplyOperation::readyForMutation() const noexcept
  {
    return _state == State::Revalidated && !_abortTransaction && !_cancelled;
  }

  bool ScanApplyOperation::transactionShouldCommit() const noexcept
  {
    return _state == State::Applied && !_abortTransaction && !_cancelled &&
           (!_result.insertedIds.empty() || !_result.mutatedIds.empty() || !_result.relinkedIds.empty() ||
            _result.missingCount != 0);
  }

  Result<> ScanApplyOperation::applyScanItem(std::size_t itemIndex,
                                             PreparedScanItem const* preparedItem,
                                             library::WriteTransaction& transaction,
                                             library::TrackStore::Writer& trackWriter,
                                             library::FileManifestStore::Writer& manifestWriter,
                                             library::DictionaryStore const& dictionary)
  {
    auto const& item = _plan.items()[itemIndex];

    if (skipNonActionableItem(item))
    {
      return {};
    }

    if (item.classification == ScanClassification::Missing)
    {
      applyMissingItem(item, transaction, manifestWriter);
      return {};
    }

    if (preparedItem == nullptr)
    {
      return {};
    }

    auto builder = preparedItem->builder.makeBuilder();
    auto const& optIdentity = preparedItem->optIdentity;
    builder.property().uri(item.uri);

    if (item.classification == ScanClassification::Changed && item.trackId != kInvalidTrackId)
    {
      if (!optIdentity)
      {
        return {};
      }

      return applyChangedItem(item, transaction, trackWriter, manifestWriter, dictionary, builder, *optIdentity);
    }

    if (item.classification == ScanClassification::Moved)
    {
      _abortTransaction = true;

      if (optIdentity &&
          applyMovedItem(item, transaction, trackWriter, manifestWriter, dictionary, builder, *optIdentity))
      {
        _abortTransaction = false;
      }

      return {};
    }

    return applyNewItem(item, transaction, trackWriter, manifestWriter, builder, optIdentity);
  }

  bool ScanApplyOperation::skipNonActionableItem(ScanItem const& item)
  {
    if (item.classification == ScanClassification::Unchanged)
    {
      // Benign: the file is already imported and has not changed.
      return true;
    }

    if (item.classification == ScanClassification::Error)
    {
      return true;
    }

    return false;
  }

  void ScanApplyOperation::reportProgress(ScanItem const& item,
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

  void ScanApplyOperation::applyMissingItem(ScanItem const& item,
                                            library::WriteTransaction& transaction,
                                            library::FileManifestStore::Writer& manifestWriter)
  {
    auto optManifest = _ml.manifest().reader(transaction).get(item.uri);

    if (!optManifest)
    {
      return;
    }

    auto builder = library::FileManifestBuilder::fromView(*optManifest);
    builder.status(library::FileStatus::Missing);

    if (writeManifest(manifestWriter, item.uri, builder))
    {
      ++_result.missingCount;
    }
  }

  std::optional<MediaTrack> ScanApplyOperation::loadTrackBuilder(ScanItem const& item)
  {
    auto fullPathRes = resolveItemPath(item);

    if (!fullPathRes)
    {
      reportFailure(item.uri, "resolve media file", fullPathRes.error().message);
      return std::nullopt;
    }

    auto mediaTrackRes = readMediaTrack(*fullPathRes);

    if (!mediaTrackRes)
    {
      // The scanner only admits decodable extensions, so open() should not see
      // an unsupported format here; a failure is a genuine I/O or parse fault.
      reportFailure(item.uri, "read media file", mediaTrackRes.error().message);
      return std::nullopt;
    }

    return std::move(*mediaTrackRes);
  }

  std::optional<library::AudioIdentity> ScanApplyOperation::cachedAudioIdentity(ScanItem const& item) const noexcept
  {
    if (item.classification != ScanClassification::New || !hasAudioIdentity(item))
    {
      return std::nullopt;
    }

    return library::AudioIdentity{.signature = item.audioSignature, .payloadLength = item.audioPayloadLength};
  }

  bool ScanApplyOperation::shouldFingerprintDuringPreparation(ScanItem const& item) const noexcept
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

  bool ScanApplyOperation::isFingerprintRequiredForApply(ScanItem const& item) const noexcept
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

  std::optional<library::AudioIdentity> ScanApplyOperation::fingerprintAudioPayload(ScanItem const& item,
                                                                                    media::file::File const& file,
                                                                                    std::size_t itemIndex,
                                                                                    bool const publishProgress,
                                                                                    std::stop_token stopToken)
  {
    auto payloadRes = file.audioPayload();

    if (!payloadRes)
    {
      reportFailure(item.uri, "read audio payload", payloadRes.error().message);
      return std::nullopt;
    }

    auto progress = library::AudioIdentityProgressCallback{};

    if (publishProgress)
    {
      progress = [this, &item, itemIndex](double fraction)
      { reportProgress(item, itemIndex, ScanApplyProgressStage::Fingerprinting, fraction); };
    }

    auto optIdentity = library::readAudioIdentity(payloadRes->bytes, std::move(progress), stopToken);

    if (!optIdentity)
    {
      _cancelled = true;
      return std::nullopt;
    }

    return optIdentity;
  }

  Result<> ScanApplyOperation::applyChangedItem(ScanItem const& item,
                                                library::WriteTransaction& transaction,
                                                library::TrackStore::Writer& trackWriter,
                                                library::FileManifestStore::Writer& manifestWriter,
                                                library::DictionaryStore const& dictionary,
                                                library::TrackBuilder& builder,
                                                library::AudioIdentity const& identity)
  {
    auto optExisting = trackWriter.get(item.trackId, library::TrackStore::Reader::LoadMode::Both);

    if (!optExisting)
    {
      return makeError(Error::Code::CorruptData,
                       std::format("Changed scan item '{}' refers to missing Track {}", item.uri, item.trackId.raw()));
    }

    auto merged = library::TrackBuilder::fromCompleteView(*optExisting, dictionary);
    merged.property()
      .duration(builder.property().duration())
      .bitrate(builder.property().bitrate())
      .sampleRate(builder.property().sampleRate())
      .channels(builder.property().channels())
      .codec(builder.property().codec())
      .bitDepth(builder.property().bitDepth());

    auto optPrepared = prepareTrack(merged, transaction, item.uri);

    if (!optPrepared)
    {
      return {};
    }

    auto const& [preparedHot, preparedCold] = *optPrepared;

    auto updateRes = library::updatePreparedTrackRecord(trackWriter, item.trackId, preparedHot, preparedCold);

    if (!updateRes)
    {
      return std::unexpected{std::move(updateRes.error())};
    }

    auto manifestBuilder = makeAvailableManifest(item, item.trackId, std::optional<library::AudioIdentity>{identity});

    if (auto manifestRes = writeManifestForStagedTrack(manifestWriter, item.uri, manifestBuilder); !manifestRes)
    {
      return std::unexpected{std::move(manifestRes.error())};
    }

    _result.mutatedIds.push_back(item.trackId);
    return {};
  }

  bool ScanApplyOperation::applyMovedItem(ScanItem const& item,
                                          library::WriteTransaction& transaction,
                                          library::TrackStore::Writer& trackWriter,
                                          library::FileManifestStore::Writer& manifestWriter,
                                          library::DictionaryStore const& dictionary,
                                          library::TrackBuilder& builder,
                                          library::AudioIdentity const& identity)
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

    if (item.audioPayloadLength != identity.payloadLength || item.audioSignature != identity.signature)
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

    auto merged = library::TrackBuilder::fromCompleteView(*optExisting, dictionary);
    merged.property()
      .uri(item.uri)
      .duration(builder.property().duration())
      .bitrate(builder.property().bitrate())
      .sampleRate(builder.property().sampleRate())
      .channels(builder.property().channels())
      .codec(builder.property().codec())
      .bitDepth(builder.property().bitDepth());

    auto optPrepared = prepareTrack(merged, transaction, item.uri);

    if (!optPrepared)
    {
      return false;
    }

    auto const& [preparedHot, preparedCold] = *optPrepared;

    if (!updateTrack(trackWriter, item.trackId, item.uri, preparedHot, preparedCold))
    {
      return false;
    }

    if (!manifestWriter.remove(item.oldUri))
    {
      reportFailure(
        item.uri, "remove old manifest for", std::format("old manifest entry '{}' was not found", item.oldUri));
      return false;
    }

    auto manifestBuilder = makeAvailableManifest(item, item.trackId, std::optional<library::AudioIdentity>{identity});

    if (!writeManifest(manifestWriter, item.uri, manifestBuilder))
    {
      return false;
    }

    _result.relinkedIds.push_back(item.trackId);
    return true;
  }

  Result<> ScanApplyOperation::applyNewItem(ScanItem const& item,
                                            library::WriteTransaction& transaction,
                                            library::TrackStore::Writer& trackWriter,
                                            library::FileManifestStore::Writer& manifestWriter,
                                            library::TrackBuilder& builder,
                                            std::optional<library::AudioIdentity> const& optIdentity)
  {
    auto optPrepared = prepareTrack(builder, transaction, item.uri);

    if (!optPrepared)
    {
      return {};
    }

    auto const& [preparedHot, preparedCold] = *optPrepared;

    auto newTrackIdRes = library::createPreparedTrackRecord(trackWriter, preparedHot, preparedCold);

    if (!newTrackIdRes)
    {
      return std::unexpected{std::move(newTrackIdRes.error())};
    }

    auto manifestBuilder = makeAvailableManifest(item, *newTrackIdRes, optIdentity);

    if (auto manifestRes = writeManifestForStagedTrack(manifestWriter, item.uri, manifestBuilder); !manifestRes)
    {
      return std::unexpected{std::move(manifestRes.error())};
    }

    _result.insertedIds.push_back(*newTrackIdRes);
    return {};
  }

  std::optional<std::pair<library::TrackBuilder::PreparedHot, library::TrackBuilder::PreparedCold>>
  ScanApplyOperation::prepareTrack(library::TrackBuilder const& builder,
                                   library::WriteTransaction& transaction,
                                   std::string const& uri)
  {
    auto preparedRes = builder.prepare(transaction, _ml.resources());

    if (!preparedRes)
    {
      reportFailure(uri, "serialize", preparedRes.error().message);
      return std::nullopt;
    }

    return *preparedRes;
  }

  library::FileManifestBuilder ScanApplyOperation::makeAvailableManifest(
    ScanItem const& item,
    TrackId trackId,
    std::optional<library::AudioIdentity> const& optIdentity)
  {
    auto builder = library::FileManifestBuilder::makeEmpty();
    builder.trackId(trackId).status(library::FileStatus::Available).fileSize(item.fileSize).mtime(item.mtime);

    if (optIdentity)
    {
      builder.audioPayloadLength(optIdentity->payloadLength).audioSignature(optIdentity->signature);
    }

    return builder;
  }

  bool ScanApplyOperation::updateTrack(library::TrackStore::Writer& trackWriter,
                                       TrackId trackId,
                                       std::string const& uri,
                                       library::TrackBuilder::PreparedHot const& hot,
                                       library::TrackBuilder::PreparedCold const& cold)
  {
    auto result = library::updatePreparedTrackRecord(trackWriter, trackId, hot, cold);

    if (!result)
    {
      reportFailure(uri, "update track data for", result.error().message);
      return false;
    }

    return true;
  }

  bool ScanApplyOperation::writeManifest(library::FileManifestStore::Writer& writer,
                                         std::string const& uri,
                                         library::FileManifestBuilder& builder)
  {
    if (auto putRes = writer.put(uri, builder.serialize()); !putRes)
    {
      reportFailure(uri, "update manifest for", putRes.error().message);
      return false;
    }

    return true;
  }

  Result<> ScanApplyOperation::writeManifestForStagedTrack(library::FileManifestStore::Writer& writer,
                                                           std::string const& uri,
                                                           library::FileManifestBuilder& builder)
  {
    if (auto putRes = writer.put(uri, builder.serialize()); !putRes)
    {
      return std::unexpected{std::move(putRes.error())};
    }

    return {};
  }
} // namespace ao::rt
