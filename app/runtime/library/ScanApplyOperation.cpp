// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ScanApplyOperation.h"

#include "CueBinding.h"
#include "MediaTrack.h"
#include "ScanSpelling.h"
#include "TrackBuilderSnapshot.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/FileTimestamp.h>
#include <ao/async/OperationCancelled.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/library/AudioIdentity.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/FileStatus.h>
#include <ao/library/FileTimeConversion.h>
#include <ao/library/LibraryUri.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackWriter.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/media/file/File.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/utility/FileAccessPath.h>
#include <ao/utility/Path.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
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
      std::optional<FileTimestamp> optMtime;
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

      auto const timestampRes = library::lastWriteTimestamp(path);

      if (!timestampRes)
      {
        auto error = timestampRes.error();
        error.message = "Failed to read modification time during scan revalidation: " + error.message;
        return std::unexpected{std::move(error)};
      }

      return FileFacts{
        .size = size,
        .optMtime = *timestampRes,
      };
    }

    bool isRelink(ScanItem const& item) noexcept
    {
      return item.classification == ScanClassification::Moved ||
             item.classification == ScanClassification::SpellingChanged;
    }

    ScanSpellingObservation const* findPlannedSpelling(ScanSpellingEvidence const* evidence, std::string const& uri)
    {
      if (evidence == nullptr)
      {
        return nullptr;
      }

      auto const found = evidence->byDestination.find(uri);
      return found == evidence->byDestination.end() ? nullptr : &found->second;
    }

    bool isDirectorySpelling(ScanSpellingObservation const& observation) noexcept
    {
      return std::holds_alternative<ScanDirectorySpellingIdentity>(observation) ||
             std::holds_alternative<ScanDirectorySpellingNeedsFingerprint>(observation);
    }

    bool matchesPlannedSpelling(ScanSpellingObservation const& planned,
                                ScanSpellingObservation const& live,
                                ScanClassification const classification) noexcept
    {
      if (classification != ScanClassification::Moved)
      {
        return (std::holds_alternative<ScanFileIdentity>(planned) ||
                std::holds_alternative<ScanDirectorySpellingIdentity>(planned)) &&
               live == planned;
      }

      // Only planning can grant an audio fallback. A live native ID can
      // support the pair but does not waive any audio-move checks.
      if (auto const* const fallback = std::get_if<ScanDirectorySpellingNeedsFingerprint>(&planned);
          fallback != nullptr)
      {
        return matchesScanDirectorySpellingFallback(*fallback, live);
      }

      return std::holds_alternative<ScanSpellingNeedsFingerprint>(planned) &&
             (std::holds_alternative<ScanFileIdentity>(live) ||
              std::holds_alternative<ScanSpellingNeedsFingerprint>(live));
    }

    bool matchesPersistedEvidence(library::TrackWriter const& trackWriter,
                                  ScanItem const& item,
                                  std::string_view const manifestUri,
                                  bool const requireAudioIdentity)
    {
      if (!item.optManifestEvidence)
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
          optManifest->mtime() != expected.optMtime || optManifest->status() != expected.status)
      {
        return false;
      }

      if (requireAudioIdentity && (optManifest->audioPayloadLength() != expected.audioPayloadLength ||
                                   optManifest->audioSignature() != expected.audioSignature))
      {
        return false;
      }

      if (optManifest->totalFrames() != expected.totalFrames ||
          optManifest->segmentTrackIds().size() != expected.members.size())
      {
        return false;
      }

      for (std::size_t index = 0; index < expected.members.size(); ++index)
      {
        auto const& member = expected.members[index];

        if (optManifest->segmentTrackIds()[index] != member.trackId)
        {
          return false;
        }

        auto const optTrack = trackWriter.get(member.trackId, library::TrackStore::Reader::LoadMode::Both);

        if (!optTrack || optTrack->property().uri() != manifestUri ||
            optTrack->property().segment() != member.segment ||
            optTrack->property().sampleRate().raw() != expected.sampleRate)
        {
          return false;
        }
      }

      if (!expected.members.empty())
      {
        return true;
      }

      auto const optTrack = trackWriter.get(item.trackId, library::TrackStore::Reader::LoadMode::Both);
      return optTrack && !optTrack->property().segment() && optTrack->property().uri() == manifestUri;
    }
  } // namespace

  // Filesystem evidence may be shared within one phase, never across the
  // boundary between preparation and the command's final pre-transaction turn.
  struct ScanApplyOperation::InspectionPhase final
  {
    std::filesystem::path const& root;
    std::stop_token stopToken;
    ScanSpellingInspection spelling{root, stopToken};
    std::unordered_map<std::filesystem::path, Result<std::vector<CueSheetSnapshot>>> directories{};

    Result<std::vector<CueSheetSnapshot>> const& sheetsFor(std::filesystem::path const& directory)
    {
      auto found = directories.find(directory);

      if (found == directories.end())
      {
        found = directories.emplace(directory, readCueDirectory(root, directory)).first;
      }

      return found->second;
    }
  };

  struct ScanApplyOperation::PreparedScanItem final
  {
    // A spelling-only relink has no replacement media facts.
    std::optional<TrackBuilderSnapshot> optBuilder;
    std::optional<library::AudioIdentity> optIdentity;
    std::vector<TrackBuilderSnapshot> segments;
    // Views into the plan's sorted URI snapshot, certified only in final filesystem revalidation.
    std::vector<std::string_view> independentDirectoryPeers;
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

  void ScanApplyOperation::finishCancelled() noexcept
  {
    _cancelled = true;
    _result = ScanApplyResult{};
    _manifestMutated = false;
    _state = State::Terminal;
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
    auto res = transaction.apply([this, stopToken](library::LibraryWrite& write) { return apply(write, stopToken); });

    if (!res)
    {
      return res;
    }

    if (_cancelled)
    {
      async::throwOperationCancelled();
    }

    if (!shouldCommitTransaction())
    {
      return res;
    }

    res->libraryRevision = _ml.libraryRevision(transaction);

    if (auto commitRes = transaction.commit(); !commitRes)
    {
      res->libraryRevision = 0;
      res->insertedIds.clear();
      res->mutatedIds.clear();
      res->relinkedIds.clear();
      res->relinkedFileCount = 0;
      res->missingCount = 0;
      return std::unexpected{commitRes.error()};
    }

    return res;
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
    auto phase = InspectionPhase{.root = _ml.rootPath(), .stopToken = stopToken};

    for (std::size_t i = 0; i < _plan.size(); ++i)
    {
      if (stopToken.stop_requested())
      {
        _cancelled = true;
        break;
      }

      if (auto itemRes = prepareItem(i, phase); !itemRes)
      {
        return std::unexpected{itemRes.error()};
      }

      // A relink consumes a pair. Never apply the rest of the batch if either
      // its filesystem proof or its required replacement media could not be prepared.
      if (isRelink(_plan.items()[i]) && !_preparedItems[i])
      {
        _abortTransaction = true;
      }

      if (_cancelled || _abortTransaction)
      {
        break;
      }
    }

    if (_cancelled)
    {
      finishCancelled();
    }
    else
    {
      _state = State::Prepared;
    }

    return _result;
  }

  Result<> ScanApplyOperation::prepareItem(std::size_t itemIndex, InspectionPhase& phase)
  {
    auto const& item = _plan.items()[itemIndex];
    reportProgress(item, itemIndex, ScanApplyProgressStage::Updating, 0.0);

    // Only a failed item fails the apply. A notice was already reported with
    // the plan and accompanies an item that may still apply.
    if (isScanFailure(item))
    {
      if (!item.errorMessage.empty())
      {
        auto const* const stage = item.cueDiagnostic == ScanCueDiagnostic::None ? "scan" : "bind CUE sheet for";
        reportFailure(item.uri, stage, item.errorMessage);
      }

      return {};
    }

    if (item.classification == ScanClassification::Missing || item.classification == ScanClassification::Unchanged ||
        item.classification == ScanClassification::Notice)
    {
      return {};
    }

    auto const fullPathRes = resolveItemPath(item);

    if (!fullPathRes)
    {
      reportFailure(item.uri, "resolve media file", fullPathRes.error().message);
      return {};
    }

    if (!tryValidatePathProvenance(itemIndex, *fullPathRes, phase))
    {
      return {};
    }

    if (item.optManifestEvidence && item.optManifestEvidence->members.empty() && isPlainSpelling(item))
    {
      revalidatePreparedCueSheets(itemIndex, *fullPathRes, phase);

      if (!_abortTransaction)
      {
        _preparedItems[itemIndex] = std::make_unique<PreparedScanItem>();
      }

      return {};
    }

    auto optMediaTrack = loadTrackBuilder(item, *fullPathRes);

    if (!optMediaTrack)
    {
      return {};
    }

    auto const& sheetsRes = phase.sheetsFor(utility::ordinaryFilePath(*fullPathRes).parent_path());

    if (!sheetsRes)
    {
      reportFailure(item.uri, "read CUE directory", sheetsRes.error().message);
      return {};
    }

    auto binding =
      bindCueSheet(utility::pathToGenericUtf8(utility::pathFromUtf8(item.uri).filename()), *optMediaTrack, *sheetsRes);

    if (!tryValidatePreparedBinding(itemIndex, *optMediaTrack, binding))
    {
      return {};
    }

    if (isPlainSpelling(item))
    {
      // Carrier extent and rate were proven above. Relink its live members
      // without taking replacement properties or covers from the file.
      _preparedItems[itemIndex] = std::make_unique<PreparedScanItem>();
      return {};
    }

    auto optIdentity = cachedAudioIdentity(item);

    if (!optIdentity && shouldFingerprintDuringPreparation(item))
    {
      optIdentity = fingerprintAudioPayload(item, optMediaTrack->file(), itemIndex, true, phase.stopToken);
    }

    if (_cancelled || phase.stopToken.stop_requested())
    {
      _cancelled = true;
      return {};
    }

    if (!optIdentity && isFingerprintRequiredForApply(item))
    {
      return {};
    }

    auto snapshotRes = TrackBuilderSnapshot::make(optMediaTrack->builder());

    if (!snapshotRes)
    {
      return std::unexpected{snapshotRes.error()};
    }

    auto preparedPtr = std::make_unique<PreparedScanItem>();
    preparedPtr->optBuilder = std::move(*snapshotRes);
    preparedPtr->optIdentity = std::move(optIdentity);

    if (auto membersRes = prepareCarrierMembers(itemIndex, *optMediaTrack, binding, *preparedPtr); !membersRes)
    {
      return std::unexpected{membersRes.error()};
    }

    if (!_skippedItems[itemIndex])
    {
      _preparedItems[itemIndex] = std::move(preparedPtr);
    }

    return {};
  }

  bool ScanApplyOperation::tryValidatePreparedBinding(std::size_t itemIndex,
                                                      MediaTrack const& media,
                                                      CueBinding const& binding)
  {
    auto const& item = _plan.items()[itemIndex];

    if (binding.evidence != item.cueEvidence)
    {
      if (isRelink(item))
      {
        reportFailure(item.uri, "relink", "CUE sheets changed after planning");
        _abortTransaction = true;
        return false;
      }

      skipStaleItem(itemIndex);
      return false;
    }

    if (item.optManifestEvidence && !item.optManifestEvidence->members.empty() &&
        (media.optTotalFrames() != item.optManifestEvidence->totalFrames ||
         media.builder().property().sampleRate().raw() != item.optManifestEvidence->sampleRate))
    {
      reportFailure(item.uri, "read carrier", "Carrier extent or sample rate changed");

      if (isRelink(item))
      {
        _abortTransaction = true;
      }

      return false;
    }

    return true;
  }

  Result<> ScanApplyOperation::prepareCarrierMembers(std::size_t itemIndex,
                                                     MediaTrack const& media,
                                                     CueBinding const& binding,
                                                     PreparedScanItem& prepared)
  {
    auto const& item = _plan.items()[itemIndex];

    if ((item.classification != ScanClassification::New || item.cueLayout.empty()) &&
        item.classification != ScanClassification::MissingSegments)
    {
      return {};
    }

    if (binding.state != CueBindingState::Bound || binding.layout != item.cueLayout ||
        media.optTotalFrames() != item.totalFrames || media.builder().property().sampleRate().raw() != item.sampleRate)
    {
      skipStaleItem(itemIndex);
      return {};
    }

    for (auto const& segmentBuilder : makeCueTracks(media, binding))
    {
      auto const optSegment = segmentBuilder.property().segment();
      AO_INVARIANT(optSegment, "Prepared CUE member lost its range");

      if (auto const segment = *optSegment;
          item.optManifestEvidence &&
          std::ranges::any_of(item.optManifestEvidence->members,
                              [&segment](auto const& member) { return member.segment.ordinal == segment.ordinal; }))
      {
        continue;
      }

      auto segmentRes = TrackBuilderSnapshot::make(segmentBuilder);

      if (!segmentRes)
      {
        return std::unexpected{segmentRes.error()};
      }

      prepared.segments.push_back(std::move(*segmentRes));
    }

    return {};
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

    return uriRes->resolveFileUnder(_ml.rootPath());
  }

  bool ScanApplyOperation::tryValidatePathProvenance(std::size_t const itemIndex,
                                                     std::filesystem::path const& fullPath,
                                                     InspectionPhase& phase)
  {
    auto const& item = _plan.items()[itemIndex];

    if (utility::ordinaryFilePath(fullPath) == item.fullPath)
    {
      return tryValidateSpelling(item, phase);
    }

    if (isRelink(item))
    {
      reportFailure(item.uri, "relink", "Resolved media path changed after planning");
      _abortTransaction = true;
    }
    else
    {
      skipStaleItem(itemIndex);
    }

    return false;
  }

  bool ScanApplyOperation::needsSpellingProof(ScanItem const& item) const
  {
    return item.classification == ScanClassification::SpellingChanged ||
           (item.classification == ScanClassification::Moved &&
            findPlannedSpelling(_plan._spellingEvidencePtr.get(), item.uri) != nullptr);
  }

  bool ScanApplyOperation::tryValidateSpelling(ScanItem const& item, InspectionPhase& phase)
  {
    if (!needsSpellingProof(item))
    {
      return true;
    }

    auto const* const planned = findPlannedSpelling(_plan._spellingEvidencePtr.get(), item.uri);

    if (planned == nullptr)
    {
      reportFailure(item.uri, "validate spelling", "The relink has no planned filesystem spelling evidence");
      _abortTransaction = true;
      return false;
    }

    auto observationRes = Result<ScanSpellingObservation>{};

    try
    {
      observationRes = isDirectorySpelling(*planned) ? phase.spelling.inspectDirectory(item.oldUri, item.uri)
                                                     : phase.spelling.inspect(item.oldUri, item.uri);
    }
    catch (async::OperationCancelled const&)
    {
      _cancelled = true;
      return false;
    }

    if (phase.stopToken.stop_requested())
    {
      _cancelled = true;
      return false;
    }

    if (observationRes && matchesPlannedSpelling(*planned, *observationRes, item.classification))
    {
      return true;
    }

    reportFailure(
      item.uri,
      "validate spelling",
      observationRes ? "Filesystem spelling or file identity changed after planning" : observationRes.error().message);
    _abortTransaction = true;
    return false;
  }

  Result<ScanApplyResult> ScanApplyOperation::revalidatePreparedFiles(std::stop_token stopToken)
  {
    AO_INVARIANT(_state == State::Prepared, "Scan apply operation must be prepared before file revalidation");

    if (_abortTransaction)
    {
      _state = State::Terminal;
      return _result;
    }

    auto phase = InspectionPhase{.root = _ml.rootPath(), .stopToken = stopToken};

    for (std::size_t i = 0; i < _plan.size(); ++i)
    {
      if (stopToken.stop_requested())
      {
        _cancelled = true;
        break;
      }

      if (_skippedItems[i])
      {
        continue;
      }

      revalidatePreparedItem(i, phase);

      if (_cancelled || _abortTransaction)
      {
        break;
      }
    }

    if (!_cancelled && !_abortTransaction)
    {
      revalidateKnownDirectoryPeers(phase);
    }

    if (_cancelled)
    {
      finishCancelled();
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

  void ScanApplyOperation::revalidatePreparedItem(std::size_t itemIndex, InspectionPhase& phase)
  {
    auto const& item = _plan.items()[itemIndex];

    if (item.classification == ScanClassification::Missing)
    {
      revalidateMissingPath(itemIndex, phase.stopToken);
      return;
    }

    if (item.classification == ScanClassification::Unchanged || item.classification == ScanClassification::Error ||
        item.classification == ScanClassification::Notice)
    {
      return;
    }

    auto const isMoved = item.classification == ScanClassification::Moved;

    if (!_preparedItems[itemIndex])
    {
      return;
    }

    auto const fullPathRes = resolveItemPath(item);

    if (!fullPathRes)
    {
      auto const* const stage = isRelink(item) ? "resolve relink destination for" : "revalidate file";
      reportFailure(item.uri, stage, fullPathRes.error().message);

      if (isRelink(item))
      {
        _abortTransaction = true;
      }
      else
      {
        _skippedItems[itemIndex] = true;
      }

      return;
    }

    if (!tryValidatePathProvenance(itemIndex, *fullPathRes, phase))
    {
      return;
    }

    // Keep physical and sheet proof on the same admitted path. Sheets use a
    // fresh phase-local cache only after the file facts and moved identity.
    revalidatePreparedRegularFile(itemIndex, *fullPathRes);

    if (isMoved && !_abortTransaction)
    {
      revalidateMovedFile(itemIndex, *fullPathRes, phase.stopToken);
    }

    if (_cancelled || _abortTransaction || _skippedItems[itemIndex])
    {
      return;
    }

    revalidatePreparedCueSheets(itemIndex, *fullPathRes, phase);
  }

  void ScanApplyOperation::revalidateKnownDirectoryPeers(InspectionPhase& phase)
  {
    if (_plan._knownSpellingPeers.empty())
    {
      return;
    }

    try
    {
      auto peers = std::unordered_map<std::string, std::vector<std::string_view>>{};

      for (auto const& uri : _plan._knownSpellingPeers)
      {
        async::throwIfStopRequested(phase.stopToken);

        if (uri.contains('/'))
        {
          peers[scanDirectorySpellingKey(uri)].push_back(uri);
        }
      }

      for (std::size_t index = 0; index < _plan.size(); ++index)
      {
        async::throwIfStopRequested(phase.stopToken);
        auto const& item = _plan.items()[index];

        if (_skippedItems[index] || !_preparedItems[index] || !item.uri.contains('/') ||
            (item.classification != ScanClassification::New && !isRelink(item)))
        {
          continue;
        }

        auto const found = peers.find(scanDirectorySpellingKey(item.uri));

        if (found == peers.end())
        {
          continue;
        }

        for (auto const peerUri : found->second)
        {
          auto const endpoint = directoryPeerEndpoint(item, peerUri);

          if (endpoint.empty())
          {
            continue;
          }

          if (auto const proofRes =
                phase.spelling.inspectDirectory(peerUri, endpoint, ScanDirectoryProbe::IndependentPeer);
              proofRes && std::holds_alternative<DistinctScanSpellings>(*proofRes))
          {
            _preparedItems[index]->independentDirectoryPeers.push_back(peerUri);
          }
          else
          {
            // Decide against live rows inside preflight; an uncertified peer may have been removed.
            _hasUnconfirmedDirectoryPeers = true;
          }
        }
      }
    }
    catch (async::OperationCancelled const&)
    {
      _cancelled = true;
    }
  }

  void ScanApplyOperation::revalidatePreparedCueSheets(std::size_t itemIndex,
                                                       std::filesystem::path const& fullPath,
                                                       InspectionPhase& phase)
  {
    auto const& item = _plan.items()[itemIndex];
    auto const& sheetsRes = phase.sheetsFor(utility::ordinaryFilePath(fullPath).parent_path());

    if (!sheetsRes)
    {
      reportFailure(item.uri, "read CUE directory", sheetsRes.error().message);

      if (isRelink(item))
      {
        _abortTransaction = true;
      }
      else
      {
        _skippedItems[itemIndex] = true;
      }

      return;
    }

    if (cueEvidenceFor(utility::pathToGenericUtf8(utility::pathFromUtf8(item.uri).filename()), *sheetsRes) !=
        item.cueEvidence)
    {
      if (isRelink(item))
      {
        reportFailure(item.uri, "relink", "CUE sheets no longer match the plan");
        _abortTransaction = true;
        return;
      }

      skipStaleItem(itemIndex);
    }
  }

  void ScanApplyOperation::revalidatePreparedRegularFile(std::size_t const itemIndex,
                                                         std::filesystem::path const& fullPath)
  {
    auto const& item = _plan.items()[itemIndex];
    auto factsRes = inspectRegularFile(fullPath);

    if (!factsRes)
    {
      if (isRelink(item))
      {
        reportFailure(item.uri, "revalidate relink destination", factsRes.error().message);
        _abortTransaction = true;
      }
      else if (factsRes.error().code == Error::Code::NotFound)
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

    if (factsRes->size != item.fileSize || factsRes->optMtime != item.optMtime)
    {
      if (isRelink(item))
      {
        reportFailure(
          item.uri, "revalidate relink destination", "file size or modification time changed after preparation");
        _abortTransaction = true;
      }
      else
      {
        skipStaleItem(itemIndex);
      }
    }
  }

  void ScanApplyOperation::revalidateMissingPath(std::size_t const itemIndex, std::stop_token stopToken)
  {
    try
    {
      auto const& item = _plan.items()[itemIndex];
      auto const fullPathRes = resolveItemPath(item);
      auto existsEc = std::error_code{};
      auto const exists = fullPathRes && std::filesystem::exists(*fullPathRes, existsEc);
      async::throwIfStopRequested(stopToken);

      if (exists)
      {
        skipStaleItem(itemIndex);
      }
      else if (!fullPathRes || existsEc)
      {
        auto const blocked = hasExactRegularScanAncestor(_ml.rootPath(), item.uri, stopToken);
        async::throwIfStopRequested(stopToken);

        if (!blocked)
        {
          reportFailure(
            item.uri, "revalidate missing path", fullPathRes ? existsEc.message() : fullPathRes.error().message);
          _skippedItems[itemIndex] = true;
        }
      }
    }
    catch (async::OperationCancelled const&)
    {
      _cancelled = true;
    }
  }

  void ScanApplyOperation::revalidateMovedFile(std::size_t const itemIndex,
                                               std::filesystem::path const& fullPath,
                                               std::stop_token const stopToken)
  {
    auto const& item = _plan.items()[itemIndex];
    auto const& preparedItem = *_preparedItems[itemIndex];

    if (!preparedItem.optIdentity)
    {
      _abortTransaction = true;
      return;
    }

    auto fileRes = media::file::File::open(fullPath);

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

    if (auto const& preparedIdentity = *preparedItem.optIdentity;
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

    // A matching revision rules out new spelling peers, not a change to an existing peer's directory.
    // The writer exposes its candidate commit revision, one past the committed snapshot.
    if (!_cancelled && !_abortTransaction &&
        (_hasUnconfirmedDirectoryPeers || _ml.libraryRevision(write) != _plan._libraryRevision + 1U))
    {
      admitSpellingGroups(write, stopToken);
    }

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
      finishCancelled();
      return _result;
    }

    if (_abortTransaction)
    {
      _result.insertedIds.clear();
      _result.mutatedIds.clear();
      _result.relinkedIds.clear();
      _result.relinkedFileCount = 0;
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
        case ScanClassification::MissingSegments:
        case ScanClassification::Missing:
        {
          if (!matchesPersistedEvidence(trackWriter, item, item.uri, false))
          {
            skipStaleItem(i);
          }

          break;
        }

        case ScanClassification::Moved:
        case ScanClassification::SpellingChanged:
        {
          // Only audio-identity moves rely on the source fingerprint. A spelling-only
          // update preserves a live backfill, just as a changed file retains live curation.
          if (!matchesPersistedEvidence(
                trackWriter, item, item.oldUri, item.classification == ScanClassification::Moved))
          {
            reportFailure(item.uri,
                          "validate library evidence",
                          "the stored source manifest or Track no longer matches the planned relink");
            _skippedItems[i] = true;
            _abortTransaction = true;
            return;
          }

          if (trackWriter.manifest(item.uri))
          {
            reportFailure(item.uri, "validate library evidence", "the relink destination already has a manifest entry");
            _skippedItems[i] = true;
            _abortTransaction = true;
            return;
          }

          break;
        }

        case ScanClassification::Unchanged:
        case ScanClassification::Error:
        case ScanClassification::Notice: break;
      }
    }
  }

  struct ScanApplyOperation::SpellingAdmissions final
  {
    std::unordered_map<std::string, std::size_t> spellingSources{};
    std::unordered_map<std::string, std::vector<std::size_t>> destinations{};
    std::unordered_map<std::string, std::vector<std::size_t>> directoryPeers{};

    bool empty() const noexcept { return spellingSources.empty() && destinations.empty() && directoryPeers.empty(); }
  };

  ScanApplyOperation::SpellingAdmissions ScanApplyOperation::collectSpellingAdmissions(std::stop_token stopToken)
  {
    auto admissions = SpellingAdmissions{};
    auto& [spellingSources, destinations, directoryPeers] = admissions;

    for (std::size_t i = 0; i < _plan.size(); ++i)
    {
      if (stopToken.stop_requested())
      {
        _cancelled = true;
        break;
      }

      if (_skippedItems[i] || !_preparedItems[i])
      {
        continue;
      }

      if (auto const& item = _plan.items()[i]; needsSpellingProof(item))
      {
        auto const* const planned = findPlannedSpelling(_plan._spellingEvidencePtr.get(), item.uri);
        AO_INVARIANT(planned, "Prepared spelling relink lost its plan evidence");

        if (isDirectorySpelling(*planned))
        {
          // A late filename variant at either end can contest a directory relink.
          destinations[scanSpellingKey(item.oldUri)].push_back(i);
          destinations[scanSpellingKey(item.uri)].push_back(i);
          // The old and new URIs share this shortlist key by construction.
          directoryPeers[scanDirectorySpellingKey(item.uri)].push_back(i);
        }
        else
        {
          // Retain the existing same-parent filename checks, including fallback.
          spellingSources.emplace(scanSpellingKey(item.oldUri), i);
        }
      }
      else if (item.classification == ScanClassification::New || item.classification == ScanClassification::Moved)
      {
        destinations[scanSpellingKey(item.uri)].push_back(i);

        if (item.uri.contains('/'))
        {
          directoryPeers[scanDirectorySpellingKey(item.uri)].push_back(i);
        }
      }
    }

    return admissions;
  }

  void ScanApplyOperation::admitSpellingGroups(library::LibraryWrite const& write, std::stop_token stopToken)
  {
    auto admissions = collectSpellingAdmissions(stopToken);

    if (_cancelled || admissions.empty())
    {
      return;
    }

    auto& [spellingSources, destinations, directoryPeers] = admissions;

    // Exact source/destination checks already passed. Check every affected
    // group in one walk, before mutating any rows.
    for (auto const& row : _ml.manifest().reader(write))
    {
      if (stopToken.stop_requested())
      {
        _cancelled = true;
        return;
      }

      auto const key = scanSpellingKey(row.first);

      if (auto const found = spellingSources.find(key); found != spellingSources.end())
      {
        if (auto const& item = _plan.items()[found->second]; row.first != item.oldUri)
        {
          reportFailure(
            item.uri, "validate library evidence", "another stored spelling now matches the planned source");
          _abortTransaction = true;
          return;
        }
      }

      auto const destination = destinations.find(key);
      auto const directory =
        directoryPeers.empty() ? directoryPeers.end() : directoryPeers.find(scanDirectorySpellingKey(row.first));

      if (destination != destinations.end() && !std::ranges::binary_search(_plan._knownSpellingPeers, row.first))
      {
        rejectLateSpellingPeer(destination->second, stopToken);

        if (_cancelled || _abortTransaction)
        {
          return;
        }

        destinations.erase(destination);
      }

      if (directory != directoryPeers.end())
      {
        rejectLateDirectoryPeer(row.first, directory->second, stopToken);

        if (_cancelled || _abortTransaction)
        {
          return;
        }
      }
    }
  }

  std::string_view ScanApplyOperation::directoryPeerEndpoint(ScanItem const& item, std::string_view peerUri) const
  {
    auto const* const planned = findPlannedSpelling(_plan._spellingEvidencePtr.get(), item.uri);

    if (planned != nullptr && !isDirectorySpelling(*planned))
    {
      return {};
    }

    auto const isDirectoryRelink = !item.oldUri.empty() && planned != nullptr;

    // A native or fallback spelling relink deliberately consumes this exact source.
    if (isDirectoryRelink && peerUri == item.oldUri)
    {
      return {};
    }

    if (isScanDirectorySpellingPair(peerUri, item.uri))
    {
      return item.uri;
    }

    // Ordinary (including explicit) moves only guard the destination's directory group.
    return isDirectoryRelink && isScanDirectorySpellingPair(peerUri, item.oldUri) ? std::string_view{item.oldUri}
                                                                                  : std::string_view{};
  }

  void ScanApplyOperation::rejectLateDirectoryPeer(std::string_view const uri,
                                                   std::span<std::size_t const> candidates,
                                                   std::stop_token stopToken)
  {
    auto matches = std::vector<std::size_t>{};

    for (auto const index : candidates)
    {
      if (stopToken.stop_requested())
      {
        _cancelled = true;
        return;
      }

      // The key is only an index. Unlike a filename group, not every
      // folded peer is a candidate for every destination in this group.
      if (!_skippedItems[index] && !directoryPeerEndpoint(_plan.items()[index], uri).empty() &&
          !std::ranges::binary_search(_preparedItems[index]->independentDirectoryPeers, uri))
      {
        matches.push_back(index);
      }
    }

    rejectLateSpellingPeer(matches, stopToken);
  }

  void ScanApplyOperation::rejectLateSpellingPeer(std::span<std::size_t const> destinations, std::stop_token stopToken)
  {
    for (auto const index : destinations)
    {
      if (stopToken.stop_requested())
      {
        _cancelled = true;
        return;
      }

      if (_skippedItems[index])
      {
        continue;
      }

      if (auto const& item = _plan.items()[index]; isRelink(item))
      {
        auto const* const message = item.classification == ScanClassification::Moved
                                      ? "another stored spelling now matches the moved destination"
                                      : "another stored spelling now conflicts with the planned relink";
        reportFailure(item.uri, "validate library evidence", message);
        _abortTransaction = true;
        return;
      }

      skipStaleItem(index);
    }
  }

  bool ScanApplyOperation::isCancelled() const noexcept
  {
    return _cancelled;
  }

  bool ScanApplyOperation::isReadyForMutation() const noexcept
  {
    return _state == State::Revalidated && !_abortTransaction && !_cancelled;
  }

  bool ScanApplyOperation::shouldCommitTransaction() const noexcept
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

    if (trySkipNonActionableItem(item))
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

    if (isPlainSpelling(item))
    {
      applyPlainSpelling(item, trackWriter, dictionary);
      return {};
    }

    if ((item.optManifestEvidence && !item.optManifestEvidence->members.empty()) || !preparedItem->segments.empty())
    {
      return applyCarrierItem(item, *preparedItem, trackWriter, dictionary);
    }

    AO_INVARIANT(preparedItem->optBuilder, "Prepared media refresh has no builder");
    auto builder = preparedItem->optBuilder->makeBuilder();
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

    if (isRelink(item))
    {
      _abortTransaction = true;

      if (optIdentity && tryApplyRefreshedRelink(item, trackWriter, dictionary, builder, *optIdentity))
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
   * An embedded cover is a scan fact, not curated metadata. A
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

  bool ScanApplyOperation::trySkipNonActionableItem(ScanItem const& item)
  {
    if (item.classification == ScanClassification::Unchanged)
    {
      // Benign: the file is already imported and has not changed.
      return true;
    }

    // Preparation reported failures; standalone notices have no write to apply.
    return item.classification == ScanClassification::Error || item.classification == ScanClassification::Notice;
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

    if (tryWriteManifest(trackWriter, item.uri, builder))
    {
      _manifestMutated = true;
    }
  }

  std::optional<MediaTrack> ScanApplyOperation::loadTrackBuilder(ScanItem const& item,
                                                                 std::filesystem::path const& fullPath)
  {
    auto mediaTrackRes = readMediaTrack(fullPath);

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
    if ((item.classification != ScanClassification::New &&
         item.classification != ScanClassification::MissingSegments) ||
        !hasAudioIdentity(item))
    {
      return std::nullopt;
    }

    return library::AudioIdentity{.signature = item.audioSignature, .payloadLength = item.audioPayloadLength};
  }

  bool ScanApplyOperation::shouldFingerprintDuringPreparation(ScanItem const& item) const noexcept
  {
    switch (item.classification)
    {
      case ScanClassification::SpellingChanged: return !isPlainSpelling(item);
      case ScanClassification::Moved:
      case ScanClassification::Changed: return true;
      case ScanClassification::New: return _options.audioIdentityPolicy == AudioIdentityPolicy::Eager;
      case ScanClassification::MissingSegments:
      case ScanClassification::Missing:
      case ScanClassification::Unchanged:
      case ScanClassification::Error:
      case ScanClassification::Notice: return false;
    }

    return false;
  }

  bool ScanApplyOperation::isFingerprintRequiredForApply(ScanItem const& item) const noexcept
  {
    return shouldFingerprintDuringPreparation(item);
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

    if (!tryValidateTrack(merged, trackWriter, item.uri))
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

  void ScanApplyOperation::applyPlainSpelling(ScanItem const& item,
                                              library::TrackWriter& writer,
                                              library::DictionaryStore const& dictionary)
  {
    auto const optManifest = writer.manifest(item.oldUri);
    AO_INVARIANT(optManifest, "Admitted spelling source lost its manifest inside the write transaction");
    auto manifest = library::FileManifestBuilder::fromView(*optManifest);
    AO_INVARIANT(item.optManifestEvidence, "A spelling-only relink has no source manifest evidence");

    if (auto const& members = item.optManifestEvidence->members; !members.empty())
    {
      if (auto res = writer.relinkCarrier(item.oldUri, item.uri, std::move(manifest)); !res)
      {
        reportFailure(item.uri, "relink carrier", res.error().message);
        _abortTransaction = true;
        return;
      }

      for (auto const& member : members)
      {
        _result.relinkedIds.push_back(member.trackId);
      }
    }
    else
    {
      auto const optTrack = writer.get(item.trackId, library::TrackStore::Reader::LoadMode::Both);
      AO_INVARIANT(optTrack, "Admitted spelling source lost its Track inside the write transaction");
      auto merged = library::TrackBuilder::fromCompleteView(*optTrack, dictionary);
      merged.property().uri(item.uri);

      if (auto res = writer.relink(item.trackId, merged, std::move(manifest)); !res)
      {
        reportFailure(item.uri, "relink", res.error().message);
        _abortTransaction = true;
        return;
      }

      _result.relinkedIds.push_back(item.trackId);
    }

    ++_result.relinkedFileCount;
  }

  bool ScanApplyOperation::tryApplyRefreshedRelink(ScanItem const& item,
                                                   library::TrackWriter& trackWriter,
                                                   library::DictionaryStore const& dictionary,
                                                   library::TrackBuilder& builder,
                                                   library::AudioIdentity const& identity)
  {
    if (item.trackId == kInvalidTrackId || item.oldUri.empty())
    {
      reportFailure(item.uri, "relink", "relink is missing its previous track identity");
      return false;
    }

    if (item.classification == ScanClassification::Moved)
    {
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

    if (!tryValidateTrack(merged, trackWriter, item.uri))
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
    ++_result.relinkedFileCount;
    return true;
  }

  Result<> ScanApplyOperation::insertCarrierSegments(ScanItem const& item,
                                                     PreparedScanItem const& prepared,
                                                     library::TrackWriter& writer)
  {
    auto builders = std::vector<library::TrackBuilder>{};

    for (auto const& snapshot : prepared.segments)
    {
      auto builder = snapshot.makeBuilder();
      builder.property().uri(item.uri);

      if (!tryValidateTrack(builder, writer, item.uri))
      {
        return {};
      }

      builders.push_back(std::move(builder));
    }

    if (builders.empty())
    {
      return {};
    }

    auto idsRes =
      item.classification == ScanClassification::New
        ? writer.createCarrier(builders, makeAvailableManifest(item, prepared.optIdentity), item.totalFrames)
        : writer.addCarrierMembers(item.uri, builders);

    if (!idsRes)
    {
      return std::unexpected{idsRes.error()};
    }

    _result.insertedIds.append_range(*idsRes);
    return {};
  }

  Result<> ScanApplyOperation::applyCarrierItem(ScanItem const& item,
                                                PreparedScanItem const& prepared,
                                                library::TrackWriter& writer,
                                                library::DictionaryStore const& dictionary)
  {
    AO_INVARIANT(prepared.optBuilder, "Prepared carrier has no media builder");

    if (item.classification == ScanClassification::New || item.classification == ScanClassification::MissingSegments)
    {
      return insertCarrierSegments(item, prepared, writer);
    }

    AO_INVARIANT(item.optManifestEvidence, "An existing carrier has no manifest evidence");
    auto const& expected = *item.optManifestEvidence;
    auto parsed = prepared.optBuilder->makeBuilder();
    auto manifest = makeAvailableManifest(item, prepared.optIdentity);

    // Rate, range and derived duration belong to the immutable carrier structure.
    auto const mergeMember = [&](TrackId const trackId)
    {
      auto const optTrack = writer.get(trackId, library::TrackStore::Reader::LoadMode::Both);
      AO_INVARIANT(optTrack, "Admitted carrier lost its member inside the write transaction");
      auto merged = library::TrackBuilder::fromCompleteView(*optTrack, dictionary);
      merged.property()
        .uri(item.uri)
        .bitrate(parsed.property().bitrate())
        .channels(parsed.property().channels())
        .codec(parsed.property().codec())
        .bitDepth(parsed.property().bitDepth());
      applyFileCoverArt(merged, parsed);
      return merged;
    };

    if (item.classification == ScanClassification::Moved &&
        (!prepared.optIdentity || prepared.optIdentity->payloadLength != item.audioPayloadLength ||
         prepared.optIdentity->signature != item.audioSignature))
    {
      reportFailure(item.uri, "relink carrier", "Audio identity changed before apply");
      _abortTransaction = true;
      return {};
    }

    // Validate every member before the first write, as a whole-file item is
    // validated: a member the writer would reject skips this item, or aborts a
    // relink, instead of failing the scan. Merged builders borrow the write
    // transaction, so none is kept across the writes below.
    for (auto const& member : expected.members)
    {
      if (!tryValidateTrack(mergeMember(member.trackId), writer, item.uri))
      {
        _abortTransaction = isRelink(item);
        return {};
      }
    }

    if (isRelink(item))
    {
      if (auto res = writer.relinkCarrier(item.oldUri, item.uri, manifest); !res)
      {
        reportFailure(item.uri, "relink carrier", res.error().message);
        _abortTransaction = true;
        return {};
      }

      ++_result.relinkedFileCount;
    }

    for (auto const& member : expected.members)
    {
      if (auto res = writer.update(member.trackId, mergeMember(member.trackId)); !res)
      {
        return std::unexpected{res.error()};
      }

      if (isRelink(item))
      {
        _result.relinkedIds.push_back(member.trackId);
      }
      else
      {
        _result.mutatedIds.push_back(member.trackId);
      }
    }

    if (!isRelink(item))
    {
      if (auto res = writer.updateManifest(item.uri, manifest); !res)
      {
        return std::unexpected{res.error()};
      }
    }

    return {};
  }

  Result<> ScanApplyOperation::applyNewItem(ScanItem const& item,
                                            library::TrackWriter& trackWriter,
                                            library::TrackBuilder& builder,
                                            std::optional<library::AudioIdentity> const& optIdentity)
  {
    if (!tryValidateTrack(builder, trackWriter, item.uri))
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

  bool ScanApplyOperation::tryValidateTrack(library::TrackBuilder const& builder,
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
    builder.status(library::FileStatus::Available).fileSize(item.fileSize).mtime(item.optMtime);

    if (optIdentity)
    {
      builder.audioPayloadLength(optIdentity->payloadLength).audioSignature(optIdentity->signature);
    }

    return builder;
  }

  bool ScanApplyOperation::tryWriteManifest(library::TrackWriter& writer,
                                            std::string const& uri,
                                            library::FileManifestBuilder& builder)
  {
    if (auto putRes = writer.updateManifest(uri, builder); !putRes)
    {
      reportFailure(uri, "update manifest for", putRes.error().message);
      return false;
    }

    return true;
  }
} // namespace ao::rt
