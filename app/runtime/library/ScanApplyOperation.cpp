// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ScanApplyOperation.h"

#include "MediaTrack.h"
#include "TrackBuilderSnapshot.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/OperationCancelled.h>
#include <ao/compat/MoveOnlyFunction.h>
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

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt
{
  namespace
  {
    struct FileFacts final
    {
      std::uint64_t size = 0;
      std::uint64_t mtime = 0;
    };

    Result<FileFacts> inspectRegularFile(std::filesystem::path const& path)
    {
      auto ec = std::error_code{};

      if (!std::filesystem::is_regular_file(path, ec))
      {
        if (ec)
        {
          return makeError(Error::Code::IoError, "Failed to inspect file during scan revalidation: " + ec.message());
        }

        return makeError(Error::Code::NotFound, "File is no longer a regular file during scan revalidation");
      }

      auto const size = std::filesystem::file_size(path, ec);

      if (ec)
      {
        return makeError(Error::Code::IoError, "Failed to read file size during scan revalidation: " + ec.message());
      }

      auto const lastWriteTime = std::filesystem::last_write_time(path, ec);

      if (ec)
      {
        return makeError(
          Error::Code::IoError, "Failed to read modification time during scan revalidation: " + ec.message());
      }

      return FileFacts{
        .size = size,
        .mtime = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(lastWriteTime.time_since_epoch()).count()),
      };
    }

    bool matchesPersistedEvidence(library::TrackWriter const& trackWriter,
                                  ScanItem const& item,
                                  std::string_view const manifestUri,
                                  bool const requireAudioIdentity)
    {
      if (item.trackId == kInvalidTrackId || !item.optManifestEvidence)
      {
        return false;
      }

      auto const optManifest = trackWriter.manifest(manifestUri);

      if (!optManifest)
      {
        return false;
      }

      auto const& expected = *item.optManifestEvidence;

      if (optManifest->trackId() != item.trackId || optManifest->fileSize() != expected.fileSize ||
          optManifest->mtime() != expected.mtime || optManifest->status() != expected.status)
      {
        return false;
      }

      if (requireAudioIdentity && (optManifest->audioPayloadLength() != expected.audioPayloadLength ||
                                   optManifest->audioSignature() != expected.audioSignature))
      {
        return false;
      }

      auto const optTrack = trackWriter.get(item.trackId, library::TrackStore::Reader::LoadMode::Both);
      return optTrack && optTrack->property().uri() == manifestUri;
    }
  } // namespace

  struct ScanApplyOperation::PreparedScanItem final
  {
    explicit PreparedScanItem(TrackBuilderSnapshot source, std::optional<library::AudioIdentity> optIdentityValue)
      : builder{std::move(source)}, optIdentity{std::move(optIdentityValue)}
    {
    }

    TrackBuilderSnapshot builder;
    std::optional<library::AudioIdentity> optIdentity;
  };

  ScanApplyOperation::ScanApplyOperation(library::MusicLibrary& ml,
                                         ScanPlan plan,
                                         compat::MoveOnlyFunction<void(ScanApplyProgress const& progress)> progress,
                                         compat::MoveOnlyFunction<void(ScanFailure const& failure)> itemFailure,
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

  void ScanApplyOperation::skipStaleItem(std::size_t const itemIndex) noexcept
  {
    AO_EXPECTS(itemIndex < _skippedItems.size());
    AO_INVARIANT(!_skippedItems[itemIndex], "Scan item was already skipped");
    _skippedItems[itemIndex] = true;
    ++_result.staleCount;
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
    _skippedItems.assign(_plan.size(), false);

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

      auto snapshotRes = TrackBuilderSnapshot::make(optMediaTrack->builder());

      if (!snapshotRes)
      {
        return std::unexpected{snapshotRes.error()};
      }

      _preparedItems[i] = std::make_unique<PreparedScanItem>(std::move(*snapshotRes), std::move(optIdentity));
    }

    if (_cancelled)
    {
      _result.staleCount = 0;
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
      if (stopToken.stop_requested())
      {
        _cancelled = true;
        break;
      }

      switch (_plan.items()[i].classification)
      {
        case ScanClassification::New:
        case ScanClassification::Changed: revalidatePreparedRegularFile(i); break;
        case ScanClassification::Missing: revalidateMissingPath(i); break;
        case ScanClassification::Moved: revalidateMovedFile(i, stopToken); break;
        case ScanClassification::Unchanged:
        case ScanClassification::Error: break;
      }

      if (_cancelled || _abortTransaction)
      {
        break;
      }
    }

    if (_cancelled)
    {
      _result.insertedIds.clear();
      _result.mutatedIds.clear();
      _result.relinkedIds.clear();
      _result.missingCount = 0;
      _result.staleCount = 0;
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

  void ScanApplyOperation::revalidatePreparedRegularFile(std::size_t const itemIndex)
  {
    if (_preparedItems[itemIndex] == nullptr)
    {
      return;
    }

    auto const& item = _plan.items()[itemIndex];
    auto fullPathRes = resolveItemPath(item);

    if (!fullPathRes)
    {
      reportFailure(item.uri, "revalidate file", fullPathRes.error().message);
      _skippedItems[itemIndex] = true;
      return;
    }

    auto factsRes = inspectRegularFile(*fullPathRes);

    if (!factsRes)
    {
      if (factsRes.error().code == Error::Code::NotFound)
      {
        skipStaleItem(itemIndex);
      }
      else
      {
        reportFailure(item.uri, "revalidate file", factsRes.error().message);
        _skippedItems[itemIndex] = true;
      }

      return;
    }

    if (factsRes->size != item.fileSize || factsRes->mtime != item.mtime)
    {
      skipStaleItem(itemIndex);
    }
  }

  void ScanApplyOperation::revalidateMissingPath(std::size_t const itemIndex)
  {
    auto const& item = _plan.items()[itemIndex];
    auto fullPathRes = resolveItemPath(item);

    if (!fullPathRes)
    {
      reportFailure(item.uri, "revalidate missing path", fullPathRes.error().message);
      _skippedItems[itemIndex] = true;
      return;
    }

    auto existsEc = std::error_code{};
    auto const exists = std::filesystem::exists(*fullPathRes, existsEc);

    if (existsEc)
    {
      reportFailure(item.uri, "revalidate missing path", existsEc.message());
      _skippedItems[itemIndex] = true;
    }
    else if (exists)
    {
      skipStaleItem(itemIndex);
    }
  }

  void ScanApplyOperation::revalidateMovedFile(std::size_t const itemIndex, std::stop_token const stopToken)
  {
    auto const& item = _plan.items()[itemIndex];
    auto const* const preparedItem = _preparedItems[itemIndex].get();

    if (preparedItem == nullptr || !preparedItem->optIdentity)
    {
      _abortTransaction = true;
      return;
    }

    auto fullPathRes = resolveItemPath(item);

    if (!fullPathRes)
    {
      reportFailure(item.uri, "resolve moved destination for", fullPathRes.error().message);
      _abortTransaction = true;
      return;
    }

    auto factsRes = inspectRegularFile(*fullPathRes);

    if (!factsRes)
    {
      reportFailure(item.uri, "revalidate moved destination", factsRes.error().message);
      _abortTransaction = true;
      return;
    }

    if (factsRes->size != item.fileSize || factsRes->mtime != item.mtime)
    {
      reportFailure(
        item.uri, "revalidate moved destination", "file size or modification time changed after preparation");
      _abortTransaction = true;
      return;
    }

    auto fileRes = media::file::File::open(*fullPathRes);

    if (!fileRes)
    {
      reportFailure(item.uri, "open moved destination for", fileRes.error().message);
      _abortTransaction = true;
      return;
    }

    auto const optLiveIdentity = fingerprintAudioPayload(item, *fileRes, itemIndex, false, stopToken);

    if (_cancelled)
    {
      return;
    }

    if (auto const& preparedIdentity = *preparedItem->optIdentity;
        !optLiveIdentity || optLiveIdentity->payloadLength != preparedIdentity.payloadLength ||
        optLiveIdentity->signature != preparedIdentity.signature ||
        optLiveIdentity->payloadLength != item.audioPayloadLength || optLiveIdentity->signature != item.audioSignature)
    {
      if (optLiveIdentity)
      {
        reportFailure(item.uri, "relink", "audio identity changed after preparation");
      }

      _abortTransaction = true;
    }
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

    auto trackWriter = write.tracks();
    auto const& dictionary = _ml.dictionary();

    admitItemsAgainstDatabase(trackWriter, stopToken);

    if (!_cancelled && !_abortTransaction)
    {
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
    }

    if (_cancelled || stopToken.stop_requested())
    {
      _cancelled = true;
      _result.insertedIds.clear();
      _result.mutatedIds.clear();
      _result.relinkedIds.clear();
      _result.missingCount = 0;
      _result.staleCount = 0;
      _result.failureCount = 0;
      _manifestMutated = false;
      return _result;
    }

    if (_abortTransaction)
    {
      _result.insertedIds.clear();
      _result.mutatedIds.clear();
      _result.relinkedIds.clear();
      _result.missingCount = 0;
      _manifestMutated = false;
      return _result;
    }

    return _result;
  }

  void ScanApplyOperation::admitItemsAgainstDatabase(library::TrackWriter const& trackWriter,
                                                     std::stop_token const stopToken)
  {
    for (std::size_t i = 0; i < _plan.size(); ++i)
    {
      if (stopToken.stop_requested())
      {
        _cancelled = true;
        return;
      }

      if (_skippedItems[i])
      {
        continue;
      }

      switch (auto const& item = _plan.items()[i]; item.classification)
      {
        case ScanClassification::New:
        {
          if (trackWriter.manifest(item.uri))
          {
            skipStaleItem(i);
          }

          break;
        }

        case ScanClassification::Changed:
        case ScanClassification::Missing:
        {
          if (!matchesPersistedEvidence(trackWriter, item, item.uri, false))
          {
            skipStaleItem(i);
          }

          break;
        }

        case ScanClassification::Moved:
        {
          if (!matchesPersistedEvidence(trackWriter, item, item.oldUri, true))
          {
            reportFailure(item.uri,
                          "validate library evidence",
                          "the stored source manifest or Track no longer matches the planned move");
            _skippedItems[i] = true;
            _abortTransaction = true;
            return;
          }

          if (trackWriter.manifest(item.uri))
          {
            reportFailure(item.uri, "validate library evidence", "the moved destination manifest now exists");
            _skippedItems[i] = true;
            _abortTransaction = true;
            return;
          }

          break;
        }

        case ScanClassification::Unchanged:
        case ScanClassification::Error: break;
      }
    }
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
            _manifestMutated);
  }

  Result<> ScanApplyOperation::applyScanItem(std::size_t itemIndex,
                                             PreparedScanItem const* preparedItem,
                                             library::TrackWriter& trackWriter,
                                             library::DictionaryStore const& dictionary)
  {
    auto const& item = _plan.items()[itemIndex];

    if (_skippedItems[itemIndex])
    {
      return {};
    }

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

  /**
   * @brief Replaces @p merged's cover references with the ones @p parsed found.
   *
   * An embedded cover is a scan fact in version 6, not curated metadata. A
   * reference names content by digest, so leaving the previous set in place after
   * a file's art changed would leave the library naming content that file no
   * longer holds, and every read for it would fall through to another carrier or
   * to no image. Curated metadata around it is still preserved: this replaces the
   * cover set and nothing else.
   *
   * `Moved` needs this as much as `Changed` and is easier to overlook, because a
   * move is matched by audio payload while tags and pictures are free to differ.
   * It is also self-concealing: once the move commits, the manifest matches the
   * file, so the next scan classifies it `Unchanged` and never looks again.
   */
  void ScanApplyOperation::applyFileCoverArt(library::TrackBuilder& merged, library::TrackBuilder const& parsed)
  {
    merged.coverArt().clear();

    for (auto const& pending : parsed.coverArt().entries())
    {
      std::visit([&merged, &pending](auto source) { merged.coverArt().add(pending.type, source); }, pending.source);
    }
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

    ++_result.missingCount;

    if (optManifest->status() != library::FileStatus::Available)
    {
      return;
    }

    auto builder = library::FileManifestBuilder::fromView(*optManifest);
    builder.status(library::FileStatus::Missing);

    if (writeManifest(trackWriter, optManifest->trackId(), item.uri, builder))
    {
      _manifestMutated = true;
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
    applyFileCoverArt(merged, builder);

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
    applyFileCoverArt(merged, builder);

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
