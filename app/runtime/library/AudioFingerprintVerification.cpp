// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "AudioFingerprintVerification.h"

#include "LibraryWriteLane.h"
#include "MediaTrack.h"
#include <ao/AudioScalars.h>
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/FileTimestamp.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Task.h>
#include <ao/library/AudioIdentity.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/FileStatus.h>
#include <ao/library/FileTimeConversion.h>
#include <ao/library/LibraryUri.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/media/file/File.h>
#include <ao/rt/library/AudioFingerprintReport.h>
#include <ao/utility/Hash128.h>
#include <ao/utility/Uuid.h>
#include <ao/utility/Xxh3.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace ao::rt
{
  namespace
  {
    constexpr auto kStaleFingerprintConfirmation = "Fingerprint confirmation is stale; verify this URI again";

    struct CarrierExtent final
    {
      std::uint64_t totalFrames = 0;
      SampleRate sampleRate{};
    };

    struct VerificationSnapshot final
    {
      utility::UuidBytes libraryId{};
      std::uint64_t revision = 0;
      utility::Hash128 manifestDigest{};
      std::uint64_t fileSize = 0;
      FileTimestamp mtime{};
      library::AudioIdentity stored{};
      std::optional<CarrierExtent> optCarrier{};
    };

    Result<VerificationSnapshot> readSnapshot(library::MusicLibrary const& source, std::string_view uri)
    {
      auto const transaction = source.readTransaction();
      auto const optRow = source.manifest().reader(transaction).get(uri);

      if (!optRow)
      {
        return makeError(Error::Code::NotFound, "No manifest entry for this URI; scan first");
      }

      auto const optMtime = optRow->mtime();

      if (optRow->status() != library::FileStatus::Available || !optMtime)
      {
        return makeError(Error::Code::Conflict,
                         "Fingerprint verification requires an available file with a recorded mtime; scan first");
      }

      auto snapshot = VerificationSnapshot{
        .libraryId = source.metadataHeader(transaction).libraryId,
        .revision = source.libraryRevision(transaction),
        .manifestDigest = utility::xxh3Hash128(optRow->rawData()),
        .fileSize = optRow->fileSize(),
        .mtime = *optMtime,
        .stored = {.signature = optRow->audioSignature(), .payloadLength = optRow->audioPayloadLength()},
      };

      if (auto const members = optRow->segmentTrackIds(); !members.empty())
      {
        // Carrier creation enforces one sample rate for all members. The
        // manifest owns the full carrier extent, even for a sparse membership.
        auto const optMember = source.tracks().reader(transaction).get(members.front());

        if (!optMember || optMember->property().uri() != uri || !optMember->property().segment() ||
            optMember->property().sampleRate().raw() == 0 || optRow->totalFrames() == 0)
        {
          return makeError(
            Error::Code::Conflict, "Cannot establish the stored carrier extent and sample rate; scan first");
        }

        snapshot.optCarrier =
          CarrierExtent{.totalFrames = optRow->totalFrames(), .sampleRate = optMember->property().sampleRate()};
      }

      return snapshot;
    }

    Result<std::filesystem::path> resolveVerifiedFile(library::MusicLibrary const& source,
                                                      library::LibraryUri const& uri,
                                                      VerificationSnapshot const& snapshot)
    {
      auto pathRes = uri.resolveFileUnder(source.rootPath());

      if (!pathRes)
      {
        return std::unexpected{pathRes.error()};
      }

      auto ec = std::error_code{};

      if (!std::filesystem::is_regular_file(*pathRes, ec))
      {
        return makeError(Error::Code::IoError, ec ? ec.message() : "Expected a regular audio file; scan first");
      }

      if (!media::file::File::isSupported(*pathRes))
      {
        return makeError(Error::Code::NotSupported, "Unsupported audio file");
      }

      auto const fileSize = std::filesystem::file_size(*pathRes, ec);

      if (ec)
      {
        return makeError(Error::Code::IoError, ec.message());
      }

      auto const mtimeRes = library::lastWriteTimestamp(*pathRes);

      if (!mtimeRes)
      {
        return std::unexpected{mtimeRes.error()};
      }

      if (fileSize != snapshot.fileSize || *mtimeRes != snapshot.mtime)
      {
        return makeError(Error::Code::Conflict, "File size or mtime differs from the manifest; scan first");
      }

      return pathRes;
    }

    Result<library::AudioIdentity> hashPayload(media::file::File const& file,
                                               std::stop_token stopToken,
                                               library::AudioIdentityProgressCallback progress)
    {
      auto const payloadRes = file.audioPayload();

      if (!payloadRes)
      {
        return std::unexpected{payloadRes.error()};
      }

      if (payloadRes->bytes.empty())
      {
        return makeError(Error::Code::FormatRejected, "Audio payload is empty");
      }

      auto const optIdentity = library::readAudioIdentity(payloadRes->bytes, std::move(progress), stopToken);

      if (!optIdentity)
      {
        async::throwOperationCancelled();
      }

      if (!library::hasAudioIdentity(optIdentity->payloadLength, optIdentity->signature))
      {
        return makeError(Error::Code::FormatRejected, "Audio payload has a reserved fingerprint");
      }

      return *optIdentity;
    }

    Result<library::AudioIdentity> observeIdentity(std::filesystem::path const& path,
                                                   std::optional<CarrierExtent> const& optCarrier,
                                                   std::stop_token stopToken,
                                                   library::AudioIdentityProgressCallback progress)
    {
      async::throwIfStopRequested(stopToken);

      if (optCarrier)
      {
        auto const mediaRes = readMediaTrack(path);

        if (!mediaRes)
        {
          return std::unexpected{mediaRes.error()};
        }

        if (mediaRes->optTotalFrames() != optCarrier->totalFrames ||
            mediaRes->builder().property().sampleRate() != optCarrier->sampleRate)
        {
          return makeError(Error::Code::Conflict, "Carrier extent or sample rate differs from the library; scan first");
        }

        return hashPayload(mediaRes->file(), stopToken, std::move(progress));
      }

      auto const fileRes = media::file::File::open(path);

      if (!fileRes)
      {
        return std::unexpected{fileRes.error()};
      }

      return hashPayload(*fileRes, stopToken, std::move(progress));
    }

    std::string confirmationFor(utility::UuidBytes const& libraryId,
                                std::uint64_t revision,
                                utility::Hash128 const& manifestDigest,
                                std::string_view uri,
                                library::AudioIdentity const& observed)
    {
      // All prefix fields are colon-free. The URI occupies the entire suffix,
      // so arbitrary URI text cannot introduce an ambiguous field boundary.
      return utility::xxh3Hash128Hex(std::format("{}:{}:{}:{}:{}:{}",
                                                 utility::formatUuid(libraryId),
                                                 revision,
                                                 utility::hash128Hex(manifestDigest),
                                                 observed.payloadLength,
                                                 utility::hash128Hex(observed.signature),
                                                 uri));
    }

    Result<> adoptAudioFingerprint(library::MusicLibrary const& source,
                                   library::LibraryWrite& write,
                                   AudioFingerprintReport const& verified)
    {
      auto writer = write.tracks();
      auto const optCurrent = writer.manifest(verified.uri);

      // The whole-row digest binds the available status, old identity, facts,
      // and track binding even if two copies advanced to the same revision.
      if (!optCurrent || verified.optConfirmation != confirmationFor(source.metadataHeader(write).libraryId,
                                                                     write.baseRevision(),
                                                                     utility::xxh3Hash128(optCurrent->rawData()),
                                                                     verified.uri,
                                                                     verified.observed))
      {
        return makeError(Error::Code::Conflict, kStaleFingerprintConfirmation);
      }

      auto builder = library::FileManifestBuilder::fromView(*optCurrent);
      builder.audioPayloadLength(verified.observed.payloadLength).audioSignature(verified.observed.signature);
      return writer.updateManifest(verified.uri, std::move(builder));
    }
  } // namespace

  Result<AudioFingerprintReport> inspectAudioFingerprint(library::MusicLibrary const& source,
                                                         std::string_view uri,
                                                         std::stop_token stopToken,
                                                         library::AudioIdentityProgressCallback progress)
  {
    async::throwIfStopRequested(stopToken);
    auto const uriRes = library::LibraryUri::parse(uri);

    if (!uriRes)
    {
      return std::unexpected{uriRes.error()};
    }

    auto const snapshotRes = readSnapshot(source, uriRes->value());

    if (!snapshotRes)
    {
      return std::unexpected{snapshotRes.error()};
    }

    auto const pathRes = resolveVerifiedFile(source, *uriRes, *snapshotRes);

    if (!pathRes)
    {
      return std::unexpected{pathRes.error()};
    }

    auto const observedRes = observeIdentity(*pathRes, snapshotRes->optCarrier, stopToken, std::move(progress));

    if (!observedRes)
    {
      return std::unexpected{observedRes.error()};
    }

    auto const afterRes = resolveVerifiedFile(source, *uriRes, *snapshotRes);

    if (!afterRes)
    {
      return std::unexpected{afterRes.error()};
    }

    if (*afterRes != *pathRes)
    {
      return makeError(Error::Code::Conflict, "File resolved to a different path during verification; scan first");
    }

    async::throwIfStopRequested(stopToken);
    auto report = AudioFingerprintReport{.uri = std::string{uriRes->value()}, .observed = *observedRes};

    if (auto const& stored = snapshotRes->stored; library::hasAudioIdentity(stored.payloadLength, stored.signature))
    {
      report.optStored = stored;
      report.status =
        stored.payloadLength == report.observed.payloadLength && stored.signature == report.observed.signature
          ? AudioFingerprintStatus::Match
          : AudioFingerprintStatus::Mismatch;
    }

    if (report.status != AudioFingerprintStatus::Match)
    {
      report.optConfirmation = confirmationFor(
        snapshotRes->libraryId, snapshotRes->revision, snapshotRes->manifestDigest, report.uri, report.observed);
    }

    return report;
  }

  async::Task<Result<AudioFingerprintReport>> confirmAudioFingerprintAsync(
    LibraryWriteLane::Submission submission,
    LibraryWriteLane::BackgroundTaskLease const* backgroundTask,
    library::MusicLibrary const* source,
    std::string confirmation,
    std::stop_token const stopToken,
    AudioFingerprintInspection inspect)
  {
    AO_EXPECTS(backgroundTask != nullptr && source != nullptr);
    auto report = AudioFingerprintReport{};
    auto mutationRes = co_await LibraryWriteLane::beginBackgroundMutationAsync(
      std::move(submission),
      *backgroundTask,
      [&confirmation, &report, &inspect, stopToken](std::stop_token const closingToken) -> Result<>
      {
        auto combined = std::stop_source{};
        auto stop = [&combined] { std::ignore = combined.request_stop(); };
        auto callerStop = std::stop_callback{stopToken, stop};
        auto closingStop = std::stop_callback{closingToken, stop};
        auto inspectionRes = inspect(combined.get_token());

        if (!inspectionRes)
        {
          return std::unexpected{std::move(inspectionRes.error())};
        }

        if (inspectionRes->status == AudioFingerprintStatus::Match)
        {
          return makeError(Error::Code::Conflict, "The fingerprint already matches; no update is needed");
        }

        if (inspectionRes->optConfirmation != confirmation)
        {
          return makeError(Error::Code::Conflict, kStaleFingerprintConfirmation);
        }

        report = std::move(*inspectionRes);
        return {};
      });

    if (!mutationRes)
    {
      co_return std::unexpected{std::move(mutationRes.error())};
    }

    auto const closingToken = mutationRes->closingStopToken();
    auto executionRes = co_await mutationRes->executeAsync(
      [source, &report, closingToken, stopToken](
        library::LibraryWrite& transaction) -> Result<OperationOutcome<AudioFingerprintReport>>
      {
        async::throwIfStopRequested(stopToken);
        async::throwIfStopRequested(closingToken);

        if (auto adoptRes = adoptAudioFingerprint(*source, transaction, report); !adoptRes)
        {
          return std::unexpected{std::move(adoptRes.error())};
        }

        report.optConfirmation.reset();
        report.updated = true;
        return Changed<AudioFingerprintReport>{.value = std::move(report), .changeSet = {}};
      },
      "Adopt verified audio fingerprint");

    if (!executionRes)
    {
      co_return std::unexpected{std::move(executionRes.error())};
    }

    co_return std::move(executionRes->value);
  }
} // namespace ao::rt
