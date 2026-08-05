// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/runtime/PlaybackSuccessionTransportTestSupport.h"

#include "runtime/playback/PlaybackSuccession.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/audio/EngineTestSupport.h"
#include "test/unit/audio/ScriptedDecoderSession.h"
#include "test/unit/runtime/PlaybackSuccessionBaseTestSupport.h"
#include "test/unit/runtime/PlaybackTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/Engine.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ao::rt::test::playback_succession
{
  struct DecoderActivationProbe::Impl final
  {
    mutable std::mutex mutex;
    std::map<std::filesystem::path, std::size_t> counts;
  };

  DecoderActivationProbe::DecoderActivationProbe()
    : _implPtr{std::make_unique<Impl>()}
  {
  }

  DecoderActivationProbe::~DecoderActivationProbe() = default;

  void DecoderActivationProbe::registerPath(std::filesystem::path const& path)
  {
    auto const lock = std::scoped_lock{_implPtr->mutex};
    _implPtr->counts.try_emplace(path, 0);
  }

  void DecoderActivationProbe::notify(std::filesystem::path const& path)
  {
    auto const lock = std::scoped_lock{_implPtr->mutex};

    if (auto const it = _implPtr->counts.find(path); it != _implPtr->counts.end())
    {
      ++it->second;
    }
  }

  std::size_t DecoderActivationProbe::count(std::filesystem::path const& path) const
  {
    auto const lock = std::scoped_lock{_implPtr->mutex};
    auto const it = _implPtr->counts.find(path);
    return it == _implPtr->counts.end() ? 0 : it->second;
  }

  std::map<std::filesystem::path, std::size_t> DecoderActivationProbe::snapshot() const
  {
    auto const lock = std::scoped_lock{_implPtr->mutex};
    return _implPtr->counts;
  }

  PreparationReleaseGuard::PreparationReleaseGuard(std::shared_ptr<audio::test::BlockingPreparationGate> gatePtr)
    : _gatePtr{std::move(gatePtr)}
  {
  }

  PreparationReleaseGuard::~PreparationReleaseGuard()
  {
    if (_gatePtr)
    {
      _gatePtr->release.release();
    }
  }

  void PreparationReleaseGuard::release()
  {
    _gatePtr->release.release();
    _gatePtr.reset();
  }

  audio::DecoderFactoryFn makeActivationProbedDecoderFactory(
    std::shared_ptr<DecoderActivationProbe> probePtr,
    std::shared_ptr<audio::test::BlockingPreparationGate> blockingGatePtr,
    std::filesystem::path blockedFileName,
    bool const failBlockedPreparation,
    bool const blockEveryLookahead,
    std::filesystem::path finalOpenFailureFileName)
  {
    return [probePtr = std::move(probePtr),
            blockingGatePtr = std::move(blockingGatePtr),
            blockedFileName = std::move(blockedFileName),
            failBlockedPreparation,
            blockEveryLookahead,
            finalOpenFailureFileName = std::move(finalOpenFailureFileName)](
             std::filesystem::path const& path, std::optional<audio::SampleEncoding> optOutputEncoding)
    {
      auto const blocks =
        blockingGatePtr &&
        (path.filename() == blockedFileName ||
         (blockEveryLookahead && path.filename() != std::filesystem::path{"transport-playable-0.flac"}));

      if (blocks)
      {
        blockingGatePtr->enterAndWait();
      }

      auto const sourceFormat = audio::SignalFormat{
        .sampleRate = 44100,
        .channels = 2,
        .precisionBits = 16,
      };
      auto const outputFormat =
        audio::pcmFormat(sourceFormat, optOutputEncoding.value_or(audio::SampleEncoding::Signed16Le));
      auto decoderPtr = std::make_unique<audio::test::ScriptedDecoderSession>(audio::DecodedStreamInfo{
        .sourceFormat = sourceFormat,
        .outputFormat = outputFormat,
        .duration = std::chrono::seconds{2},
        .isLossy = false,
        .codec = AudioCodec::Flac,
      });
      decoderPtr->setReadScript(
        {{.data = std::vector<std::byte>(100000, std::byte{0}), .endOfStream = false}, {.endOfStream = true}});

      if (blocks && failBlockedPreparation)
      {
        decoderPtr->setOpenResult(makeError(Error::Code::IoError, "Scripted lookahead preparation failure"));
      }
      else if (!finalOpenFailureFileName.empty() && path.filename() == finalOpenFailureFileName && optOutputEncoding)
      {
        decoderPtr->setOpenResult(makeError(Error::Code::IoError, "Scripted final decoder setup failure"));
      }

      decoderPtr->setReadObserver(
        [probePtr, path](std::size_t const readCount)
        {
          if (readCount == 2)
          {
            probePtr->notify(path);
          }
        });

      if (blocks)
      {
        blockingGatePtr->createdPtr->fetch_add(1, std::memory_order_relaxed);
        decoderPtr->setDestroyCounter(blockingGatePtr->destroyedPtr);
      }

      return decoderPtr;
    };
  }

  PlaybackSuccessionTransportFixture::PlaybackSuccessionTransportFixture(
    PlaybackSuccessionTransportFixtureConfig config)
    : decoderProbePtr{std::make_shared<DecoderActivationProbe>()}
    , transport{makeActivationProbedDecoderFactory(decoderProbePtr,
                                                   std::move(config.blockingGatePtr),
                                                   std::move(config.blockedFileName),
                                                   config.failBlockedPreparation,
                                                   config.blockEveryLookahead,
                                                   std::move(config.finalOpenFailureFileName))}
    , asyncRuntime{transport.executor, 1, {}, &sleeper}
    , changes{libraryChangesExecutor, 0}
    , writerFixture{transport.libraryFixture.library(), changes}
    , sources{transport.libraryFixture.library(), changes}
    , views{transport.executor, transport.libraryFixture.library(), sources}
    , workspace{transport.executor, views, changes}
  {
    transport.onDevicesChangedCb(transport.status.devices);
    transport.executor.drain();
  }

  PlaybackSuccessionTransportFixture::~PlaybackSuccessionTransportFixture() = default;

  LibraryWriter& PlaybackSuccessionTransportFixture::writer()
  {
    return writerFixture.writer();
  }

  TrackId PlaybackSuccessionTransportFixture::addPlayableTrack(std::string title)
  {
    auto const libraryUri = std::format("transport-playable-{}.flac", nextPlayableFile++);
    auto const fixtureUri =
      audio::test::installAudioFixture(transport.libraryFixture.root(), "basic_metadata.flac", libraryUri);
    auto const created =
      ao::test::requireValue(writer().createTrackFromFile(transport.libraryFixture.root() / fixtureUri));
    transport.executor.drain();
    REQUIRE(writerFixture.updateMetadata(std::array{created.trackId}, MetadataPatch{.optTitle = title}));
    transport.executor.drain();
    auto const trackId = created.trackId;
    auto const path = transport.libraryFixture.root() / fixtureUri;
    decoderProbePtr->registerPath(path);
    trackPaths.insert_or_assign(trackId, path);
    return trackId;
  }

  void PlaybackSuccessionTransportFixture::openManualView(std::span<TrackId const> const trackIds)
  {
    auto const membershipTag = std::array{std::string{"transportorder"}};
    REQUIRE(writerFixture.editTags(trackIds, membershipTag, {}));
    sources.reloadAllTracks();
    listId = ao::test::requireValue(writer().createList(LibraryWriter::ListDraft{
      .name = "Transport order",
      .expression = "#transportorder",
    }));
    viewId = ao::test::requireValue(workspace.navigate(navigationRequest(TrackListViewConfig{
      .listId = listId,
      .optPresentation = TrackPresentationSpec{.id = std::string{kListOrderTrackPresentationId}},
    })));
    successionPtr = std::make_unique<PlaybackSuccession>(transport.executor,
                                                         views,
                                                         sources,
                                                         transport.libraryFixture.library(),
                                                         transport.playbackTransport,
                                                         transport.notificationService,
                                                         asyncRuntime);
  }

  void PlaybackSuccessionTransportFixture::buildThreeTrackManualView()
  {
    firstTrackId = addPlayableTrack("First");
    secondTrackId = addPlayableTrack("Second");
    thirdTrackId = addPlayableTrack("Third");
    openManualView(std::array{firstTrackId, secondTrackId, thirdTrackId});
  }

  void PlaybackSuccessionTransportFixture::buildTwoTrackManualView()
  {
    firstTrackId = addPlayableTrack("First");
    secondTrackId = addPlayableTrack("Second");
    openManualView(std::array{firstTrackId, secondTrackId});
  }

  void PlaybackSuccessionTransportFixture::buildFourTrackManualView()
  {
    firstTrackId = addPlayableTrack("First");
    secondTrackId = addPlayableTrack("Second");
    thirdTrackId = addPlayableTrack("Third");
    fourthTrackId = addPlayableTrack("Fourth");
    openManualView(std::array{firstTrackId, secondTrackId, thirdTrackId, fourthTrackId});
  }

  void PlaybackSuccessionTransportFixture::queueNaturalAdvance()
  {
    transport.executor.drain();
    REQUIRE(transport.renderTarget != nullptr);
    auto output = std::array<std::byte, 4096>{};
    REQUIRE(driveRenderUntilTaskQueued(*transport.renderTarget, transport.executor, output));
  }

  Result<> PlaybackSuccessionTransportFixture::playAndWait(TrackId const trackId)
  {
    auto const activationCounts = decoderProbePtr->snapshot();
    auto startedRes = playFromViewAndWait(*successionPtr, transport.executor, viewId, trackId);

    if (!startedRes || !successionPtr->state().optResolvedSuccessor)
    {
      return startedRes;
    }

    auto const successorId = *successionPtr->state().optResolvedSuccessor;
    auto const& successorPath = trackPaths.at(successorId);
    auto const it = activationCounts.find(successorPath);
    auto const previousCount = it == activationCounts.end() ? 0 : it->second;

    if (!waitForLookaheadAfter(successorId, previousCount))
    {
      return makeError(Error::Code::InvalidState, "Timed out waiting for playback lookahead activation");
    }

    return {};
  }

  std::size_t PlaybackSuccessionTransportFixture::lookaheadActivationCount(TrackId const trackId) const
  {
    return decoderProbePtr->count(trackPaths.at(trackId));
  }

  bool PlaybackSuccessionTransportFixture::waitForLookaheadAfter(TrackId const trackId, std::size_t const previousCount)
  {
    auto const& path = trackPaths.at(trackId);
    return transport.executor.drainUntil(
      [&] { return decoderProbePtr->count(path) > previousCount; }, std::chrono::seconds{5});
  }
} // namespace ao::rt::test::playback_succession
