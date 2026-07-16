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

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
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
    explicit PreparedScanItem(library::TrackBuilder const& source, std::optional<AudioFingerprint> optFingerprintValue)
      : builder{source}, optFingerprint{std::move(optFingerprintValue)}
    {
    }

    TrackBuilderSnapshot builder;
    std::optional<AudioFingerprint> optFingerprint;
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
    auto writableResult = library::WritableMusicLibrary::acquire(_ml);

    if (!writableResult)
    {
      return std::unexpected{writableResult.error()};
    }

    return runOffline(*writableResult, stopToken);
  }

  Result<ScanApplyResult> ScanApplyOperation::runOffline(library::WritableMusicLibrary& writableLibrary,
                                                         std::stop_token stopToken)
  {
    if (&writableLibrary.library() != &_ml)
    {
      return makeError(Error::Code::InvalidInput, "Writable library does not match the scan apply operation");
    }

    if (_state == State::Created)
    {
      if (auto prepareResult = prepare(stopToken); !prepareResult)
      {
        return prepareResult;
      }
    }

    if (_cancelled)
    {
      async::throwOperationCancelled();
    }

    if (_state == State::Prepared)
    {
      if (auto revalidationResult = revalidatePreparedFiles(stopToken); !revalidationResult)
      {
        return revalidationResult;
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

    if (_state != State::Revalidated)
    {
      return makeError(Error::Code::InvalidState, "Scan apply operation is not ready for database mutation");
    }

    auto transaction = writableLibrary.writeTransaction();
    auto result = apply(transaction, stopToken);

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

    if (auto commitResult = transaction.commit(); !commitResult)
    {
      result->libraryRevision = 0;
      result->insertedIds.clear();
      result->mutatedIds.clear();
      result->relinkedIds.clear();
      result->missingCount = 0;
      return std::unexpected{commitResult.error()};
    }

    return result;
  }

  Result<ScanApplyResult> ScanApplyOperation::prepare(std::stop_token stopToken)
  {
    if (_state != State::Created)
    {
      return makeError(Error::Code::InvalidState, "Scan apply operation is already prepared");
    }

    if (auto const bindingResult = validatePlan(); !bindingResult)
    {
      return std::unexpected{bindingResult.error()};
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

      auto optFingerprint = cachedAudioFingerprint(item);

      if (!optFingerprint && shouldFingerprintDuringPreparation(item))
      {
        optFingerprint = fingerprintAudioPayload(item, optMediaTrack->file(), i, true, stopToken);
      }

      if (_cancelled || stopToken.stop_requested())
      {
        _cancelled = true;
        break;
      }

      if (!optFingerprint && isFingerprintRequiredForApply(item))
      {
        continue;
      }

      _preparedItems[i] = std::make_unique<PreparedScanItem>(optMediaTrack->builder(), std::move(optFingerprint));
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
    if (!_plan._executable)
    {
      return makeError(Error::Code::InvalidState, "Scan plan has already been consumed");
    }

    auto const transaction = _ml.readTransaction();
    auto const headerResult = _ml.metadata().load(transaction);

    if (!headerResult)
    {
      return std::unexpected{headerResult.error()};
    }

    if (headerResult->libraryId != _plan._libraryId)
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
    auto uri = library::LibraryUri::parse(item.uri);

    if (!uri)
    {
      return std::unexpected{uri.error()};
    }

    return uri->resolveUnder(_ml.rootPath());
  }

  Result<ScanApplyResult> ScanApplyOperation::revalidatePreparedFiles(std::stop_token stopToken)
  {
    if (_state != State::Prepared)
    {
      return makeError(Error::Code::InvalidState, "Scan apply operation must be prepared before file revalidation");
    }

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

      if (preparedItem == nullptr || !preparedItem->optFingerprint)
      {
        _abortTransaction = true;
        break;
      }

      auto fullPath = resolveItemPath(item);

      if (!fullPath)
      {
        reportFailure(item.uri, "resolve moved destination for", fullPath.error().message);
        _abortTransaction = true;
        break;
      }

      auto fileResult = media::file::File::open(*fullPath);

      if (!fileResult)
      {
        reportFailure(item.uri, "open moved destination for", fileResult.error().message);
        _abortTransaction = true;
        break;
      }

      auto const optLiveFingerprint = fingerprintAudioPayload(item, *fileResult, i, false, stopToken);

      if (_cancelled)
      {
        break;
      }

      auto const& preparedFingerprint = *preparedItem->optFingerprint;

      if (!optLiveFingerprint || optLiveFingerprint->payloadLength != preparedFingerprint.payloadLength ||
          optLiveFingerprint->signature != preparedFingerprint.signature ||
          optLiveFingerprint->payloadLength != item.audioPayloadLength ||
          optLiveFingerprint->signature != item.audioSignature)
      {
        if (optLiveFingerprint)
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
    if (_state != State::Revalidated)
    {
      return makeError(
        Error::Code::InvalidState, "Scan apply operation must be revalidated exactly once before database mutation");
    }

    _state = State::Applied;

    if (_cancelled)
    {
      return _result;
    }

    if (_abortTransaction)
    {
      return _result;
    }

    if (!_plan._executable)
    {
      return makeError(Error::Code::InvalidState, "Scan plan has already been consumed");
    }

    auto const headerResult = _ml.metadata().load(transaction);

    if (!headerResult)
    {
      return std::unexpected{headerResult.error()};
    }

    if (headerResult->libraryId != _plan._libraryId)
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

    for (std::size_t i = 0; i < _plan.size(); ++i)
    {
      if (stopToken.stop_requested())
      {
        _cancelled = true;
        break;
      }

      applyScanItem(i, _preparedItems[i].get(), transaction, trackWriter, manifestWriter, dictionary);

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

  void ScanApplyOperation::applyScanItem(std::size_t itemIndex,
                                         PreparedScanItem const* preparedItem,
                                         library::WriteTransaction& transaction,
                                         library::TrackStore::Writer& trackWriter,
                                         library::FileManifestStore::Writer& manifestWriter,
                                         library::DictionaryStore const& dictionary)
  {
    auto const& item = _plan.items()[itemIndex];

    if (skipNonActionableItem(item))
    {
      return;
    }

    if (item.classification == ScanClassification::Missing)
    {
      applyMissingItem(item, transaction, manifestWriter);
      return;
    }

    if (preparedItem == nullptr)
    {
      return;
    }

    auto builder = preparedItem->builder.makeBuilder();
    auto const& optFingerprint = preparedItem->optFingerprint;
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

    applyNewItem(item, transaction, trackWriter, manifestWriter, builder, optFingerprint);
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

  std::optional<MediaTrack> ScanApplyOperation::loadTrackBuilder(ScanItem const& item)
  {
    auto fullPath = resolveItemPath(item);

    if (!fullPath)
    {
      reportFailure(item.uri, "resolve media file", fullPath.error().message);
      return std::nullopt;
    }

    auto mediaTrackResult = readMediaTrack(*fullPath);

    if (!mediaTrackResult)
    {
      // The scanner only admits decodable extensions, so open() should not see
      // an unsupported format here; a failure is a genuine I/O or parse fault.
      reportFailure(item.uri, "read media file", mediaTrackResult.error().message);
      return std::nullopt;
    }

    return std::move(*mediaTrackResult);
  }

  std::optional<ScanApplyOperation::AudioFingerprint> ScanApplyOperation::cachedAudioFingerprint(
    ScanItem const& item) const noexcept
  {
    if (item.classification != ScanClassification::New || !hasAudioIdentity(item))
    {
      return std::nullopt;
    }

    return AudioFingerprint{.signature = item.audioSignature, .payloadLength = item.audioPayloadLength};
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

  std::optional<ScanApplyOperation::AudioFingerprint> ScanApplyOperation::fingerprintAudioPayload(
    ScanItem const& item,
    media::file::File const& file,
    std::size_t itemIndex,
    bool const publishProgress,
    std::stop_token stopToken)
  {
    auto payloadResult = file.audioPayload();

    if (!payloadResult)
    {
      reportFailure(item.uri, "read audio payload", payloadResult.error().message);
      return std::nullopt;
    }

    auto progress = library::AudioIdentityProgressCallback{};

    if (publishProgress)
    {
      progress = [this, &item, itemIndex](double fraction)
      { reportProgress(item, itemIndex, ScanApplyProgressStage::Fingerprinting, fraction); };
    }

    auto optIdentity = library::readAudioIdentity(payloadResult->bytes, std::move(progress), stopToken);

    if (!optIdentity)
    {
      _cancelled = true;
      return std::nullopt;
    }

    auto const& identity = *optIdentity;
    return AudioFingerprint{.signature = identity.signature, .payloadLength = identity.payloadLength};
  }

  bool ScanApplyOperation::tryApplyChangedItem(ScanItem const& item,
                                               library::WriteTransaction& transaction,
                                               library::TrackStore::Writer& trackWriter,
                                               library::FileManifestStore::Writer& manifestWriter,
                                               library::DictionaryStore const& dictionary,
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

    auto optPrepared = prepareTrack(merged, transaction, item.uri);

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

    _result.mutatedIds.push_back(item.trackId);
    return true;
  }

  bool ScanApplyOperation::applyMovedItem(ScanItem const& item,
                                          library::WriteTransaction& transaction,
                                          library::TrackStore::Writer& trackWriter,
                                          library::FileManifestStore::Writer& manifestWriter,
                                          library::DictionaryStore const& dictionary,
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

    _result.relinkedIds.push_back(item.trackId);
    return true;
  }

  void ScanApplyOperation::applyNewItem(ScanItem const& item,
                                        library::WriteTransaction& transaction,
                                        library::TrackStore::Writer& trackWriter,
                                        library::FileManifestStore::Writer& manifestWriter,
                                        library::TrackBuilder& builder,
                                        std::optional<AudioFingerprint> const& optFingerprint)
  {
    auto optPrepared = prepareTrack(builder, transaction, item.uri);

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

    _result.insertedIds.push_back(*optNewTrackId);
  }

  std::optional<std::pair<library::TrackBuilder::PreparedHot, library::TrackBuilder::PreparedCold>>
  ScanApplyOperation::prepareTrack(library::TrackBuilder const& builder,
                                   library::WriteTransaction& transaction,
                                   std::string const& uri)
  {
    auto preparedResult = builder.prepare(transaction, _ml.resources());

    if (!preparedResult)
    {
      reportFailure(uri, "serialize", preparedResult.error().message);
      return std::nullopt;
    }

    return *preparedResult;
  }

  library::FileManifestBuilder ScanApplyOperation::makeAvailableManifest(
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

  bool ScanApplyOperation::updateTrack(library::TrackStore::Writer& trackWriter,
                                       TrackId trackId,
                                       std::string const& uri,
                                       library::TrackBuilder::PreparedHot const& hot,
                                       library::TrackBuilder::PreparedCold const& cold)
  {
    auto hotResult = library::updatePreparedHotTrackRecord(trackWriter, trackId, hot);

    if (!hotResult)
    {
      reportFailure(uri, "update hot track data for", hotResult.error().message);
      return false;
    }

    auto coldResult = library::updatePreparedColdTrackRecord(trackWriter, trackId, cold);

    if (!coldResult)
    {
      reportFailure(uri, "update cold track data for", coldResult.error().message);
      return false;
    }

    return true;
  }

  std::optional<TrackId> ScanApplyOperation::createTrack(library::TrackStore::Writer& trackWriter,
                                                         std::string const& uri,
                                                         library::TrackBuilder::PreparedHot const& hot,
                                                         library::TrackBuilder::PreparedCold const& cold)
  {
    auto createResult = library::createPreparedTrackRecord(trackWriter, hot, cold);

    if (!createResult)
    {
      reportFailure(uri, "create track data for", createResult.error().message);
      return std::nullopt;
    }

    auto const [newTrackId, trackView] = *createResult;
    std::ignore = trackView;

    return newTrackId;
  }

  bool ScanApplyOperation::writeManifest(library::FileManifestStore::Writer& writer,
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
} // namespace ao::rt
