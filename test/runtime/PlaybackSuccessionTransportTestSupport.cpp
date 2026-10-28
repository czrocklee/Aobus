// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/runtime/PlaybackSuccessionTransportTestSupport.h"

#include "runtime/playback/PlaybackBootstrap.h"
#include "runtime/playback/PlaybackSuccession.h"
#include "test/TestFixtureSupport.h"
#include "test/audio/AudioFixtureSupport.h"
#include "test/audio/EngineTestSupport.h"
#include "test/audio/ScriptedDecoderSession.h"
#include "test/runtime/PlaybackSuccessionBaseTestSupport.h"
#include "test/runtime/PlaybackTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/Engine.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/playback/PlaybackService.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
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
    Snapshot counts;
  };

  DecoderActivationProbe::DecoderActivationProbe()
    : _implPtr{std::make_unique<Impl>()}
  {
  }

  DecoderActivationProbe::~DecoderActivationProbe() = default;

  void DecoderActivationProbe::registerPath(std::filesystem::path const& path)
  {
    auto const lock = std::scoped_lock{_implPtr->mutex};
    _implPtr->counts.activations.try_emplace(path, 0);
    _implPtr->counts.outputDecoderConstructions.try_emplace(path, 0);
  }

  void DecoderActivationProbe::notify(std::filesystem::path const& path)
  {
    auto const lock = std::scoped_lock{_implPtr->mutex};

    if (auto const it = _implPtr->counts.activations.find(path); it != _implPtr->counts.activations.end())
    {
      ++it->second;
    }
  }

  void DecoderActivationProbe::notifyOutputDecoderConstruction(std::filesystem::path const& path)
  {
    auto const lock = std::scoped_lock{_implPtr->mutex};
    ++_implPtr->counts.outputDecoderConstructions[path];
  }

  std::size_t DecoderActivationProbe::count(std::filesystem::path const& path) const
  {
    auto const lock = std::scoped_lock{_implPtr->mutex};
    auto const it = _implPtr->counts.activations.find(path);
    return it == _implPtr->counts.activations.end() ? 0 : it->second;
  }

  DecoderActivationProbe::Snapshot DecoderActivationProbe::snapshot() const
  {
    auto const lock = std::scoped_lock{_implPtr->mutex};
    return _implPtr->counts;
  }

  struct DecoderActivationGate::Impl final
  {
    explicit Impl(std::chrono::milliseconds const holdTimeout)
      : timeout{holdTimeout}
    {
    }

    mutable std::mutex mutex;
    std::condition_variable cv;
    std::chrono::milliseconds timeout;
    std::filesystem::path blockedPath;
    bool entered = false;
    bool released = false;
    bool expired = false;
  };

  DecoderActivationGate::DecoderActivationGate(std::chrono::milliseconds const timeout)
    : _implPtr{std::make_unique<Impl>(timeout)}
  {
  }

  DecoderActivationGate::~DecoderActivationGate() = default;

  void DecoderActivationGate::blockPath(std::filesystem::path const& path)
  {
    auto const lock = std::scoped_lock{_implPtr->mutex};
    _implPtr->blockedPath = path;
  }

  void DecoderActivationGate::enterAndWait(std::filesystem::path const& path)
  {
    auto lock = std::unique_lock{_implPtr->mutex};

    // The first matching decoder is held; a duplicate must remain observable.
    if (path != _implPtr->blockedPath || _implPtr->entered || _implPtr->released)
    {
      return;
    }

    _implPtr->entered = true;
    _implPtr->cv.notify_all();

    if (!_implPtr->cv.wait_for(lock, _implPtr->timeout, [&] { return _implPtr->released; }))
    {
      _implPtr->expired = true;
    }
  }

  bool DecoderActivationGate::tryWaitForEntry(std::chrono::milliseconds const timeout)
  {
    auto lock = std::unique_lock{_implPtr->mutex};
    return _implPtr->cv.wait_for(lock, timeout, [&] { return _implPtr->entered; });
  }

  bool DecoderActivationGate::isHeld() const
  {
    auto const lock = std::scoped_lock{_implPtr->mutex};
    return _implPtr->entered && !_implPtr->released && !_implPtr->expired;
  }

  bool DecoderActivationGate::hasTimedOut() const
  {
    auto const lock = std::scoped_lock{_implPtr->mutex};
    return _implPtr->expired;
  }

  void DecoderActivationGate::release()
  {
    {
      auto const lock = std::scoped_lock{_implPtr->mutex};
      _implPtr->released = true;
    }

    _implPtr->cv.notify_all();
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

  namespace
  {
    audio::DecoderFactoryFn makeActivationProbedDecoderFactory(std::shared_ptr<DecoderActivationProbe> probePtr,
                                                               PlaybackSuccessionTransportFixtureConfig config)
    {
      return [probePtr = std::move(probePtr), config = std::move(config)](
               std::filesystem::path const& path,
               std::optional<audio::SampleEncoding> optOutputEncoding) -> Result<std::unique_ptr<audio::DecoderSession>>
      {
        auto const blocks =
          config.blockingGatePtr &&
          (path.filename() == config.blockedFileName ||
           (config.blockEveryLookahead && path.filename() != std::filesystem::path{"transport-playable-0.flac"}));

        if (blocks)
        {
          config.blockingGatePtr->enterAndWait();
        }

        auto const sourceFormat = audio::SignalFormat{
          .sampleRate = 44100,
          .channels = 2,
          .precisionBits = 16,
        };
        auto const outputFormat =
          audio::pcmFormat(sourceFormat, optOutputEncoding.value_or(audio::SampleEncoding::Signed16Le));
        bool const isLossy = path.filename() == config.lossyFileName;
        auto decoderPtr = std::make_unique<audio::test::ScriptedDecoderSession>(audio::DecodedStreamInfo{
          .sourceFormat = sourceFormat,
          .outputFormat = outputFormat,
          .duration = std::chrono::seconds{2},
          .isLossy = isLossy,
          .codec = isLossy ? AudioCodec::Mp3 : AudioCodec::Flac,
        });

        if (optOutputEncoding)
        {
          probePtr->notifyOutputDecoderConstruction(path);
        }

        auto* const failureGate =
          optOutputEncoding && path.filename() == config.stagedFailureFileName ? config.stagedFailureGate : nullptr;
        // One complete second in the selected encoding covers TrackSession's
        // 500 ms preroll using whole frames, even for 24/32-bit output. If that
        // policy grows past this block, the bounded activation gate fails setup.
        auto const firstBlockBytes = audio::bytesPerSecond(outputFormat);
        decoderPtr->setReadScript(
          {{.data = std::vector<std::byte>(firstBlockBytes, std::byte{0}), .endOfStream = false},
           {.endOfStream = failureGate == nullptr,
            .res = failureGate != nullptr ? Result<>{makeError(Error::Code::IoError, "gated staged decode failure")}
                                          : Result<>{}}});

        if (blocks)
        {
          config.blockingGatePtr->createdPtr->fetch_add(1, std::memory_order_relaxed);
          decoderPtr->setDestroyCounter(config.blockingGatePtr->destroyedPtr);
        }

        if (blocks && config.failBlockedPreparation)
        {
          return makeError(Error::Code::IoError, "Scripted lookahead preparation failure");
        }

        if (optOutputEncoding && std::ranges::contains(config.finalOpenFailureFileNames, path.filename()))
        {
          return makeError(Error::Code::IoError, "Scripted final decoder setup failure");
        }

        auto const activationGatePtr = config.activationGatePtr;
        decoderPtr->setReadObserver(
          [probePtr, path, failureGate, activationGatePtr](std::size_t const readCount)
          {
            if (readCount == 2)
            {
              if (activationGatePtr)
              {
                activationGatePtr->enterAndWait(path);
              }

              probePtr->notify(path);

              if (failureGate != nullptr)
              {
                failureGate->notifyReadEntered();
                failureGate->waitForRelease();
              }
            }
          });

        return std::unique_ptr<audio::DecoderSession>{std::move(decoderPtr)};
      };
    }
  } // namespace

  PlaybackSuccessionTransportFixture::PlaybackSuccessionTransportFixture(
    PlaybackSuccessionTransportFixtureConfig config)
    : decoderProbePtr{std::make_shared<DecoderActivationProbe>()}
    , transport{makeActivationProbedDecoderFactory(decoderProbePtr, std::move(config))}
    , asyncRuntime{transport.executor, 1, &sleeper}
    , changes{transport.executor, 0, "test-library"}
    , commandsFixture{transport.libraryFixture.library(), changes, transport.executor}
    , sources{transport.libraryFixture.library(), changes}
    , views{transport.executor, transport.libraryFixture.library(), sources, changes}
    , workspace{transport.executor, views, transport.libraryFixture.library(), changes}
  {
    transport.onDevicesChangedCb(transport.status.devices);
    transport.executor.drain();
  }

  PlaybackSuccessionTransportFixture::~PlaybackSuccessionTransportFixture() = default;

  LibraryCommands& PlaybackSuccessionTransportFixture::commands()
  {
    return commandsFixture.commands();
  }

  TrackId PlaybackSuccessionTransportFixture::addPlayableTrack(std::string title)
  {
    auto const libraryUri = std::format("transport-playable-{}.flac", nextPlayableFile++);
    auto const fixtureUri =
      audio::test::installAudioFixture(transport.libraryFixture.root(), "basic_metadata.flac", libraryUri);
    auto const created = ao::test::requireValue(
      commandsFixture.runTask(commands().createTrackFromFileAsync(transport.libraryFixture.root() / fixtureUri)));
    transport.executor.drain();
    REQUIRE(commandsFixture.updateMetadata(std::array{created.trackId}, MetadataPatch{.optTitle = title}));
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
    REQUIRE(commandsFixture.editTags(trackIds, membershipTag, {}));
    sources.reloadAllTracks();
    listId = ao::test::requireValue(commandsFixture.runTask(commands().createListAsync(ListDraft{
      .name = "Transport order",
      .expression = "#transportorder",
    })));
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
    REQUIRE(tryDriveRenderUntilTaskQueued(*transport.renderTarget, transport.executor, output));
  }

  PlaybackService PlaybackSuccessionTransportFixture::createPlayback()
  {
    REQUIRE(successionPtr);
    return PlaybackBootstrap{transport.playbackTransport}.createPlaybackService(
      transport.executor, *successionPtr, transport.libraryFixture.library(), changes);
  }

  Result<> PlaybackSuccessionTransportFixture::playAndWait(TrackId const trackId)
  {
    auto const activationCounts = decoderProbePtr->snapshot().activations;
    auto startedRes = playFromViewAndWait(*successionPtr, transport.executor, viewId, trackId);

    if (!startedRes || !successionPtr->state().optResolvedSuccessor)
    {
      return startedRes;
    }

    auto const successorId = *successionPtr->state().optResolvedSuccessor;
    auto const& successorPath = trackPaths.at(successorId);
    auto const it = activationCounts.find(successorPath);
    auto const previousCount = it == activationCounts.end() ? 0 : it->second;

    if (!tryWaitForDecoderActivationAfter(successorId, previousCount))
    {
      return makeError(Error::Code::InvalidState, "Timed out waiting for playback lookahead activation");
    }

    return {};
  }

  std::size_t PlaybackSuccessionTransportFixture::decoderActivationCount(TrackId const trackId) const
  {
    return decoderProbePtr->count(trackPaths.at(trackId));
  }

  bool PlaybackSuccessionTransportFixture::tryWaitForDecoderActivationAfter(TrackId const trackId,
                                                                            std::size_t const previousCount)
  {
    auto const& path = trackPaths.at(trackId);
    return transport.executor.tryDrainUntil(
      [&] { return decoderProbePtr->count(path) > previousCount; }, std::chrono::seconds{10});
  }
} // namespace ao::rt::test::playback_succession
