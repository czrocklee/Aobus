// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ScanApplyOperation.h"

#include "MediaTrack.h"
#include "TrackBuilderSnapshot.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/OperationCancelled.h>
#include <ao/library/AudioIdentity.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibraryUri.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackWriter.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/media/file/File.h>
#include <ao/rt/library/ScanPlan.h>

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

    AO_INVARIANT(_state == State::Revalidated, "Scan apply operation is not ready for database mutation");

    auto writableRes = library::WritableMusicLibrary::acquire(_ml);

    if (!writableRes)
    {
      return std::unexpected{writableRes.error()};
    }

    auto transaction = writableRes->writeTransaction();
    auto result =
      transaction.apply([this, stopToken](library::LibraryWrite& write) { return apply(write, stopToken); });

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
    AO_INVARIANT(_state == State::Created, "Scan apply operation is already prepared");

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
    AO_INVARIANT(_plan._executable, "Scan plan has already been consumed");

    auto const transaction = _ml.readTransaction();

    if (auto const header = _ml.metadataHeader(transaction); header.libraryId != _plan._libraryId)
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
    AO_INVARIANT(_state == State::Prepared, "Scan apply operation must be prepared before file revalidation");

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

  Result<ScanApplyResult> ScanApplyOperation::apply(library::LibraryWrite& write, std::stop_token stopToken)
  {
    AO_INVARIANT(
      _state == State::Revalidated, "Scan apply operation must be revalidated exactly once before database mutation");

    _state = State::Applied;

    if (_cancelled)
    {
      return _result;
    }

    if (_abortTransaction)
    {
      return _result;
    }

    AO_INVARIANT(_plan._executable, "Scan plan has already been consumed");

    if (auto const header = _ml.metadataHeader(write); header.libraryId != _plan._libraryId)
    {
      return makeError(Error::Code::InvalidInput, "Scan plan belongs to another library");
    }

    if (auto const transactionRevision = _ml.libraryRevision(write);
        transactionRevision == 0 || transactionRevision - 1U != _plan._libraryRevision)
    {
      return makeError(Error::Code::Conflict, "Library changed since the scan plan was built");
    }

    auto trackWriter = write.tracks();
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

      if (auto itemRes = applyScanItem(i, _preparedItems[i].get(), trackWriter, dictionary); !itemRes)
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

  Result<> ScanApplyOperation::validatePersistedTrackEvidence(library::TrackWriter const& trackWriter) const
  {
    for (auto const& item : _plan.items())
    {
      if (item.trackId == kInvalidTrackId)
      {
        continue;
      }

      auto const optTrack = trackWriter.get(item.trackId, library::TrackStore::Reader::LoadMode::Both);
      AO_INVARIANT(
        optTrack, "Revision-matched scan item '{}' refers to missing Track {}", item.uri, item.trackId.raw());
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
                                             library::TrackWriter& trackWriter,
                                             library::DictionaryStore const& dictionary)
  {
    auto const& item = _plan.items()[itemIndex];

    if (skipNonActionableItem(item))
    {
      return {};
    }

    if (item.classification == ScanClassification::Missing)
    {
      applyMissingItem(item, trackWriter);
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

      return applyChangedItem(item, trackWriter, dictionary, builder, *optIdentity);
    }

    if (item.classification == ScanClassification::Moved)
    {
      _abortTransaction = true;

      if (optIdentity && applyMovedItem(item, trackWriter, dictionary, builder, *optIdentity))
      {
        _abortTransaction = false;
      }

      return {};
    }

    return applyNewItem(item, trackWriter, builder, optIdentity);
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

  void ScanApplyOperation::applyMissingItem(ScanItem const& item, library::TrackWriter& trackWriter)
  {
    auto optManifest = trackWriter.manifest(item.uri);

    if (!optManifest)
    {
      return;
    }

    auto builder = library::FileManifestBuilder::fromView(*optManifest);
    builder.status(library::FileStatus::Missing);

    if (writeManifest(trackWriter, optManifest->trackId(), item.uri, builder))
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
                                                library::TrackWriter& trackWriter,
                                                library::DictionaryStore const& dictionary,
                                                library::TrackBuilder& builder,
                                                library::AudioIdentity const& identity)
  {
    auto optExisting = trackWriter.get(item.trackId, library::TrackStore::Reader::LoadMode::Both);
    AO_INVARIANT(optExisting,
                 "Prevalidated changed scan item '{}' lost Track {} inside one write transaction",
                 item.uri,
                 item.trackId.raw());

    auto merged = library::TrackBuilder::fromCompleteView(*optExisting, dictionary);
    merged.property()
      .duration(builder.property().duration())
      .bitrate(builder.property().bitrate())
      .sampleRate(builder.property().sampleRate())
      .channels(builder.property().channels())
      .codec(builder.property().codec())
      .bitDepth(builder.property().bitDepth());

    if (!validateTrack(merged, trackWriter, item.uri))
    {
      return {};
    }

    auto manifestBuilder = makeAvailableManifest(item, std::optional<library::AudioIdentity>{identity});

    if (auto replaceRes = trackWriter.replace(item.trackId, merged, manifestBuilder); !replaceRes)
    {
      return std::unexpected{std::move(replaceRes.error())};
    }

    _result.mutatedIds.push_back(item.trackId);
    return {};
  }

  bool ScanApplyOperation::applyMovedItem(ScanItem const& item,
                                          library::TrackWriter& trackWriter,
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

    if (optExisting->property().uri() != item.oldUri)
    {
      reportFailure(item.uri, "relink", "stored Track URI no longer matches the planned source");
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

    if (!validateTrack(merged, trackWriter, item.uri))
    {
      return false;
    }

    auto manifestBuilder = makeAvailableManifest(item, std::optional<library::AudioIdentity>{identity});

    if (auto relinkRes = trackWriter.relink(item.trackId, merged, manifestBuilder); !relinkRes)
    {
      reportFailure(item.uri, "relink", relinkRes.error().message);
      return false;
    }

    _result.relinkedIds.push_back(item.trackId);
    return true;
  }

  Result<> ScanApplyOperation::applyNewItem(ScanItem const& item,
                                            library::TrackWriter& trackWriter,
                                            library::TrackBuilder& builder,
                                            std::optional<library::AudioIdentity> const& optIdentity)
  {
    if (!validateTrack(builder, trackWriter, item.uri))
    {
      return {};
    }

    auto manifestBuilder = makeAvailableManifest(item, optIdentity);
    auto newTrackIdRes = trackWriter.create(builder, manifestBuilder);

    if (!newTrackIdRes)
    {
      return std::unexpected{std::move(newTrackIdRes.error())};
    }

    _result.insertedIds.push_back(*newTrackIdRes);
    return {};
  }

  bool ScanApplyOperation::validateTrack(library::TrackBuilder const& builder,
                                         library::TrackWriter const& trackWriter,
                                         std::string const& uri)
  {
    if (auto validationRes = trackWriter.validate(builder); !validationRes)
    {
      reportFailure(uri, "serialize", validationRes.error().message);
      return false;
    }

    return true;
  }

  library::FileManifestBuilder ScanApplyOperation::makeAvailableManifest(
    ScanItem const& item,
    std::optional<library::AudioIdentity> const& optIdentity)
  {
    auto builder = library::FileManifestBuilder::makeEmpty();
    builder.status(library::FileStatus::Available).fileSize(item.fileSize).mtime(item.mtime);

    if (optIdentity)
    {
      builder.audioPayloadLength(optIdentity->payloadLength).audioSignature(optIdentity->signature);
    }

    return builder;
  }

  bool ScanApplyOperation::writeManifest(library::TrackWriter& writer,
                                         TrackId const trackId,
                                         std::string const& uri,
                                         library::FileManifestBuilder& builder)
  {
    if (auto putRes = writer.updateManifest(trackId, builder); !putRes)
    {
      reportFailure(uri, "update manifest for", putRes.error().message);
      return false;
    }

    return true;
  }
} // namespace ao::rt
