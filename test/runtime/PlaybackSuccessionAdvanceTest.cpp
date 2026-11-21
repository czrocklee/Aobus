// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/PlaybackSessionPersistence.h"
#include "runtime/PlaybackSessionState.h"
#include "runtime/PlaybackSessionYamlSchema.h"
#include "runtime/playback/PlaybackSuccession.h"
#include "runtime/playback/PlaybackTransport.h"
#include "test/audio/BackendTestSupport.h"
#include "test/audio/EngineTestSupport.h"
#include "test/runtime/PlaybackSuccessionBaseTestSupport.h"
#include "test/runtime/PlaybackSuccessionSeekTestSupport.h"
#include "test/runtime/PlaybackSuccessionTransportTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/Transport.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/utility/ScopedRegistration.h>
#include <ao/utility/ThreadName.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <fakeit.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <tuple>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  using playback_succession::DecoderActivationGate;
  using playback_succession::DecoderActivationProbe;
  using playback_succession::PlaybackSuccessionFixture;
  using playback_succession::PlaybackSuccessionSeekFixture;
  using playback_succession::PlaybackSuccessionTransportFixture;
  using playback_succession::PlaybackSuccessionTransportFixtureConfig;
  using playback_succession::PreparationReleaseGuard;

  namespace
  {
    std::size_t totalDecoderActivations(DecoderActivationProbe::Snapshot const& snapshot)
    {
      std::size_t count = 0;

      for (auto const& entry : snapshot.activations)
      {
        count += entry.second;
      }

      return count;
    }
  } // namespace

  TEST_CASE("PlaybackSuccession - natural end without a prepared successor respects output availability",
            "[concurrency]")
  {
    auto fixture = PlaybackSuccessionTransportFixture{};
    fixture.buildThreeTrackManualView();
    auto& playbackTransport = fixture.transport.playbackTransport;
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    fixture.transport.executor.drain();
    REQUIRE(playbackTransport.clearPreparedNext());
    REQUIRE(fixture.transport.renderTarget != nullptr);

    auto output = std::array<std::byte, 4096>{};
    auto& executor = fixture.transport.executor;
    REQUIRE(executor.tryDrainUntil([&] { return fixture.transport.renderTarget->renderPcm(output).drained; }));
    executor.drain();

    SECTION("a ready route advances to the next track")
    {
      fixture.transport.renderTarget->handleDrainComplete();
      REQUIRE(
        executor.tryDrainUntil([&] { return fixture.successionPtr->state().currentTrackId == fixture.secondTrackId; }));
      CHECK(playbackTransport.state().nowPlaying.trackId == fixture.secondTrackId);
      CHECK(playbackTransport.state().transport == audio::Transport::Playing);
    }

    SECTION("a missing route retires the sequence without skipping or replaying on rediscovery")
    {
      auto const occurrence = playbackTransport.state().occurrenceId;
      auto missing = playbackTransport.state().output.selectedDevice;
      missing.deviceId = audio::DeviceId{"missing"};
      std::size_t idleCount = 0;
      auto idleSub = playbackTransport.onIdle([&] { ++idleCount; });

      // Discovery will run *before* the queued terminal notifications, but
      // cannot make the requested route available until we deliver that turn.
      fixture.transport.onDevicesChangedCb({audio::Device{.id = missing.deviceId, .backendId = missing.backendId}});
      fixture.transport.renderTarget->handleDrainComplete();
      REQUIRE(executor.tryWaitUntilQueuedCount(4)); // discovery, state, route, end
      REQUIRE_FALSE(playbackTransport.canAdvanceFrom(occurrence));
      playbackTransport.setOutputDevice(missing);
      REQUIRE(playbackTransport.state().output.optPendingOutputSelection == missing);
      REQUIRE_FALSE(playbackTransport.state().ready);
      executor.drain();

      CHECK(fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Inactive);
      REQUIRE(playbackTransport.state().ready);
      CHECK(playbackTransport.state().output.selectedDevice == missing);
      playbackTransport.resume();
      executor.drain();
      CHECK(playbackTransport.state().transport == audio::Transport::Idle);
      CHECK(playbackTransport.state().occurrenceId == PlaybackOccurrenceId{});
      CHECK(idleCount == 1);
    }

    CHECK(fixture.transport.notificationService.feed().entries.empty());
  }

  TEST_CASE("PlaybackSuccession - output and final seek replace the disarmed lookahead token")
  {
    auto fixture = PlaybackSuccessionTransportFixture{};
    fixture.buildThreeTrackManualView();
    auto& playbackTransport = fixture.transport.playbackTransport;
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));

    auto const optFirstToken = playbackTransport.clearPreparedNext();
    REQUIRE(optFirstToken);
    auto const activationCount = fixture.decoderActivationCount(fixture.secondTrackId);

    SECTION("output edge")
    {
      playbackTransport.setOutputDevice(
        audio::BackendId{"mock_backend"}, audio::DeviceId{"mock_device"}, audio::ProfileId{audio::kProfileShared});
    }

    SECTION("final seek edge")
    {
      playbackTransport.seek(std::chrono::milliseconds{0}, PlaybackTransport::SeekMode::Final);
    }

    REQUIRE(fixture.tryWaitForDecoderActivationAfter(fixture.secondTrackId, activationCount));
    auto const optReplacementToken = playbackTransport.clearPreparedNext();
    REQUIRE(optReplacementToken);
    CHECK(*optReplacementToken != *optFirstToken);
    CHECK(fixture.successionPtr->state().currentTrackId == fixture.firstTrackId);
    CHECK(playbackTransport.state().transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSuccession - waiting for output preserves the shuffle successor until resume")
  {
    auto fixture = PlaybackSuccessionTransportFixture{};
    fixture.buildThreeTrackManualView();
    auto& succession = *fixture.successionPtr;
    auto& transport = fixture.transport.playbackTransport;
    succession.setShuffleMode(ShuffleMode::On);
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    auto const optSuccessor = succession.state().optResolvedSuccessor;
    REQUIRE(optSuccessor);

    auto const selected = transport.state().output.selectedDevice;
    transport.setOutputDevice(selected.backendId, audio::DeviceId{"missing"}, selected.profileId);
    REQUIRE_FALSE(transport.state().ready);
    CHECK(succession.state().optResolvedSuccessor == optSuccessor);

    transport.setOutputDevice(selected);
    REQUIRE(transport.state().ready);
    REQUIRE(transport.state().transport == audio::Transport::Paused);
    CHECK(succession.state().optResolvedSuccessor == optSuccessor);
    transport.resume();
    REQUIRE(succession.tryMoveNext());
    CHECK(transport.state().nowPlaying.trackId == *optSuccessor);
  }

  TEST_CASE("PlaybackSuccession - paired mode update replaces lookahead once from the final state", "[concurrency]")
  {
    auto probePtr = std::shared_ptr<DecoderActivationProbe>{};
    auto expected = DecoderActivationProbe::Snapshot{};
    std::size_t beforeActivations = 0;

    {
      auto fixture = PlaybackSuccessionTransportFixture{};
      fixture.buildThreeTrackManualView();
      probePtr = fixture.decoderProbePtr;
      auto& succession = *fixture.successionPtr;
      REQUIRE(fixture.playAndWait(fixture.firstTrackId));
      // The aggregate baseline includes the independently scheduled current decoder.
      REQUIRE(fixture.tryWaitForDecoderActivationAfter(fixture.firstTrackId, 0));

      auto signalStates = std::vector<PlaybackSuccessionState>{};
      auto const shuffleSubscription = succession.onShuffleModeChanged(
        [&](PlaybackSuccession::ShuffleModeChanged const&) { signalStates.push_back(succession.state()); });
      auto const repeatSubscription = succession.onRepeatModeChanged([&](PlaybackSuccession::RepeatModeChanged const&)
                                                                     { signalStates.push_back(succession.state()); });
      auto const before = probePtr->snapshot();
      beforeActivations = totalDecoderActivations(before);

      succession.setPlaybackMode(ShuffleMode::On, RepeatMode::All);

      auto const optSuccessorId = succession.state().optResolvedSuccessor;
      REQUIRE(optSuccessorId);
      REQUIRE(*optSuccessorId != fixture.firstTrackId);
      auto const& successorPath = fixture.trackPaths.at(*optSuccessorId);
      expected = before;
      ++expected.activations.at(successorPath);
      ++expected.outputDecoderConstructions.at(successorPath);
      REQUIRE(fixture.tryWaitForDecoderActivationAfter(*optSuccessorId, before.activations.at(successorPath)));
      fixture.transport.executor.drain();
      REQUIRE(signalStates.size() == 2);

      for (auto const& state : signalStates)
      {
        CHECK(state.shuffle == ShuffleMode::On);
        CHECK(state.repeat == RepeatMode::All);
      }

      CHECK(succession.state().shuffle == ShuffleMode::On);
      CHECK(succession.state().repeat == RepeatMode::All);
      auto const after = probePtr->snapshot();
      CAPTURE(
        after.activations, expected.activations, after.outputDecoderConstructions, expected.outputDecoderConstructions);
      CHECK(after == expected);
      CHECK(totalDecoderActivations(after) == beforeActivations + 1);
    }

    // Full fixture teardown joins preparation and decode producers. Output opens
    // also catch an extra source whose second-read probe never ran before stop.
    auto const settled = probePtr->snapshot();
    CAPTURE(settled.activations,
            expected.activations,
            settled.outputDecoderConstructions,
            expected.outputDecoderConstructions);
    CHECK(settled == expected);
    CHECK(totalDecoderActivations(settled) == beforeActivations + 1);
  }

  TEST_CASE("PlaybackSuccession - paired mode replacement is independent of delayed current decoder activation",
            "[concurrency]")
  {
    auto const gatePtr = std::make_shared<DecoderActivationGate>();
    auto probePtr = std::shared_ptr<DecoderActivationProbe>{};
    auto expected = DecoderActivationProbe::Snapshot{};
    std::size_t beforeActivations = 0;

    {
      auto fixture = PlaybackSuccessionTransportFixture{PlaybackSuccessionTransportFixtureConfig{
        .activationGatePtr = gatePtr,
      }};
      auto releaseGuard = utility::ScopedRegistration{[gatePtr] { gatePtr->release(); }};
      fixture.buildThreeTrackManualView();
      gatePtr->blockPath(fixture.trackPaths.at(fixture.firstTrackId));
      probePtr = fixture.decoderProbePtr;
      auto& succession = *fixture.successionPtr;
      INFO("The first scripted PCM block must cover preroll so the current gate is reached only after preparation");
      REQUIRE(fixture.playAndWait(fixture.firstTrackId));
      REQUIRE(gatePtr->tryWaitForEntry());
      REQUIRE(gatePtr->isHeld());

      auto const before = probePtr->snapshot();
      beforeActivations = totalDecoderActivations(before);
      auto const& currentPath = fixture.trackPaths.at(fixture.firstTrackId);
      REQUIRE(before.activations.at(currentPath) == 0);
      REQUIRE(before.activations.at(fixture.trackPaths.at(fixture.secondTrackId)) == 1);
      REQUIRE(before.activations.at(fixture.trackPaths.at(fixture.thirdTrackId)) == 0);

      succession.setPlaybackMode(ShuffleMode::On, RepeatMode::All);

      auto const optSuccessorId = succession.state().optResolvedSuccessor;
      REQUIRE(optSuccessorId);
      REQUIRE(*optSuccessorId != fixture.firstTrackId);
      auto const& successorPath = fixture.trackPaths.at(*optSuccessorId);
      expected = before;
      ++expected.activations.at(currentPath);
      ++expected.activations.at(successorPath);
      ++expected.outputDecoderConstructions.at(successorPath);
      REQUIRE(fixture.tryWaitForDecoderActivationAfter(*optSuccessorId, before.activations.at(successorPath)));
      fixture.transport.executor.drain();
      // The replacement reached its probe while the first current decoder was
      // held. A duplicate current decoder bypasses this one-shot gate and fails.
      REQUIRE(gatePtr->isHeld());
      CHECK(fixture.decoderActivationCount(fixture.firstTrackId) == before.activations.at(currentPath));

      // Let the current decoder finish before its source is destroyed.
      releaseGuard.reset();
      REQUIRE(fixture.tryWaitForDecoderActivationAfter(fixture.firstTrackId, before.activations.at(currentPath)));
      CHECK_FALSE(gatePtr->hasTimedOut());
      auto const after = probePtr->snapshot();
      CAPTURE(
        after.activations, expected.activations, after.outputDecoderConstructions, expected.outputDecoderConstructions);
      CHECK(after == expected);
      CHECK(succession.state().currentTrackId == fixture.firstTrackId);
      CHECK(succession.state().shuffle == ShuffleMode::On);
      CHECK(succession.state().repeat == RepeatMode::All);
    }

    auto const settled = probePtr->snapshot();
    CAPTURE(settled.activations,
            expected.activations,
            settled.outputDecoderConstructions,
            expected.outputDecoderConstructions);
    CHECK(settled == expected);
    CHECK(totalDecoderActivations(settled) == beforeActivations + 2);
    CHECK_FALSE(gatePtr->hasTimedOut());
  }

  TEST_CASE("PlaybackSuccession - activation gate reports timeout and tolerates repeated release", "[concurrency]")
  {
    auto gate = DecoderActivationGate{std::chrono::milliseconds{0}};
    gate.blockPath("current.flac");
    auto worker = std::jthread{[&]
                               {
                                 setCurrentThreadName("ao-gate-timeout");
                                 gate.enterAndWait("current.flac");
                               }};
    REQUIRE(gate.tryWaitForEntry());
    worker.join();
    CHECK(gate.hasTimedOut());
    CHECK_FALSE(gate.isHeld());
    gate.release();
    gate.release();
    gate.enterAndWait("current.flac");
    CHECK(gate.hasTimedOut());
  }

  TEST_CASE("PlaybackSuccession - activation gate blocks only the selected decoder once", "[concurrency]")
  {
    auto gate = DecoderActivationGate{};
    gate.blockPath("current.flac");
    auto worker = std::jthread{};
    auto const releaseGuard = utility::ScopedRegistration{[&] { gate.release(); }};
    worker = std::jthread{[&]
                          {
                            setCurrentThreadName("ao-gate-current");
                            gate.enterAndWait("current.flac");
                          }};
    REQUIRE(gate.tryWaitForEntry());
    REQUIRE(gate.isHeld());
    gate.enterAndWait("other.flac");
    gate.enterAndWait("current.flac");
    CHECK(gate.isHeld());
    gate.release();
    worker.join();
    CHECK_FALSE(gate.isHeld());
    CHECK_FALSE(gate.hasTimedOut());
  }

  TEST_CASE("PlaybackSuccession - natural prepared winner is adopted exactly once", "[concurrency]")
  {
    auto fixture = PlaybackSuccessionTransportFixture{};
    fixture.buildThreeTrackManualView();
    auto events = std::vector<PlaybackTransport::NowPlayingChanged>{};
    auto const subscription = fixture.transport.playbackTransport.onNowPlayingChanged(
      [&](PlaybackTransport::NowPlayingChanged const& event) noexcept { events.push_back(event); });
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    fixture.transport.executor.drain();
    events.clear();

    fixture.queueNaturalAdvance();
    fixture.transport.executor.drain();

    REQUIRE(events.size() == 1);
    REQUIRE(events.front().optPreparedNextToken);
    CHECK(events.front().trackId == fixture.secondTrackId);
    CHECK(fixture.successionPtr->state().currentTrackId == fixture.secondTrackId);
    CHECK(fixture.transport.playbackTransport.state().transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSuccession - switching to missing output settles a failed prepared winner once", "[concurrency]")
  {
    auto const expectedReport =
      GENERATE(NotificationReportTemplate::PlaybackDeviceLost, NotificationReportTemplate::PlaybackStoppedForTrack);
    CAPTURE(expectedReport);
    auto const decodeFailure = expectedReport == NotificationReportTemplate::PlaybackStoppedForTrack;
    auto const* const deviceLostDetail = "device lost after gapless advance";
    auto failureGate = audio::test::StagedFailureGate{};
    auto renderMutex = std::mutex{};
    auto fixture = PlaybackSuccessionTransportFixture{PlaybackSuccessionTransportFixtureConfig{
      .stagedFailureFileName = "transport-playable-1.flac",
      .stagedFailureGate = decodeFailure ? &failureGate : nullptr,
    }};
    auto releaseGuard = audio::test::StagedFailureReleaseGuard{failureGate};
    fixture.buildThreeTrackManualView();
    auto& transport = fixture.transport.playbackTransport;
    auto& executor = fixture.transport.executor;
    auto& backend = fixture.transport.spyBackendPtr->mock();
    fakeit::When(Method(backend, close))
      .AlwaysDo(
        [&]
        {
          // Engine destroys the borrowed render target after the backend closes.
          auto const lock = std::scoped_lock{renderMutex};
          fixture.transport.renderTarget = nullptr;
        });
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    auto const firstOccurrence = transport.state().occurrenceId;

    auto announcements = std::vector<PlaybackTransport::NowPlayingChanged>{};
    std::size_t startedCount = 0;
    auto const nowPlayingSub = transport.onNowPlayingChanged(
      [&](PlaybackTransport::NowPlayingChanged const& event)
      {
        if (event.trackId != kInvalidTrackId)
        {
          announcements.push_back(event);
        }
      });
    auto const startedSub = transport.onStarted([&] { ++startedCount; });

    fixture.queueNaturalAdvance();
    REQUIRE(executor.tryWaitUntilQueuedCount(3)); // advance, state, route
    REQUIRE_FALSE(transport.canAdvanceFrom(firstOccurrence));

    if (decodeFailure)
    {
      // Fail only after B's splice, so this is not a failed lookahead.
      REQUIRE(failureGate.tryWaitForRead());
      releaseGuard.release();
    }
    else
    {
      auto const lock = std::scoped_lock{renderMutex};
      REQUIRE(fixture.transport.renderTarget != nullptr);
      fixture.transport.renderTarget->handleBackendError(deviceLostDetail);
    }

    // Engine has closed B and queued failure and state (and end for Decode).
    REQUIRE(executor.tryWaitUntilQueuedCount(decodeFailure ? 6 : 5));

    if (decodeFailure)
    {
      // Let one unexpected reopen of B fail instead of hanging at the decoder gate.
      failureGate.release();
    }

    REQUIRE(transport.state().nowPlaying.trackId == fixture.firstTrackId);
    REQUIRE(announcements.empty());
    REQUIRE(fixture.transport.notificationService.feed().entries.empty());

    auto missing = transport.state().output.selectedDevice;
    missing.deviceId = audio::DeviceId{"missing"};
    transport.setOutputDevice(missing);
    executor.drain();
    CHECK(transport.state().output.optPendingOutputSelection == missing);
    CHECK(fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Inactive);
    REQUIRE(transport.state().transport == audio::Transport::Idle);

    auto const feed = fixture.transport.notificationService.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().severity == NotificationSeverity::Error);
    auto const* report = std::get_if<NotificationReport>(&feed.entries.front().message);
    REQUIRE(report != nullptr);
    CHECK(report->templateId == expectedReport);
    CHECK(report->trackId == fixture.secondTrackId);
    CHECK(report->detail == (decodeFailure ? "gated staged decode failure" : deviceLostDetail));

    fixture.transport.onDevicesChangedCb({audio::Device{.id = missing.deviceId, .backendId = missing.backendId}});
    executor.drain();
    REQUIRE(transport.state().ready);
    transport.resume();
    executor.drain();

    CHECK(transport.state().transport == audio::Transport::Idle);
    CHECK(transport.state().occurrenceId == PlaybackOccurrenceId{});
    CHECK(startedCount == 0);
    fakeit::Verify(Method(backend, open)).Once();
    REQUIRE(announcements.size() == 1);
    CHECK(announcements.front().trackId == fixture.secondTrackId);
    CHECK(announcements.front().optPreparedNextToken);
  }

  TEST_CASE("PlaybackSuccession - switching output after a prepared successor ends settles that successor once",
            "[concurrency]")
  {
    auto fixture = PlaybackSuccessionTransportFixture{};
    fixture.buildThreeTrackManualView();
    auto& transport = fixture.transport.playbackTransport;
    auto& executor = fixture.transport.executor;
    auto& succession = *fixture.successionPtr;
    auto alternate = transport.state().output.selectedDevice;
    alternate.deviceId = audio::DeviceId{"alternate"};
    auto devices = fixture.transport.status.devices;
    devices.push_back(audio::Device{.id = alternate.deviceId, .backendId = alternate.backendId});
    fixture.transport.onDevicesChangedCb(devices);
    executor.drain();
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    auto const firstOccurrence = transport.state().occurrenceId;

    auto secondTrackEvents = std::vector<PlaybackTransport::NowPlayingChanged>{};
    auto const subscription = transport.onNowPlayingChanged(
      [&](PlaybackTransport::NowPlayingChanged const& event)
      {
        if (event.trackId == fixture.secondTrackId)
        {
          secondTrackEvents.push_back(event);
        }
      });
    fixture.queueNaturalAdvance();
    // Render B to EOF without running the executor: succession cannot yet arm C.
    auto output = std::array<std::byte, 4096>{};
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    auto drained = fixture.transport.renderTarget->renderPcm(output).drained;

    while (!drained && std::chrono::steady_clock::now() < deadline)
    {
      std::this_thread::yield();
      drained = fixture.transport.renderTarget->renderPcm(output).drained;
    }

    REQUIRE(drained);
    REQUIRE_FALSE(transport.canAdvanceFrom(firstOccurrence));
    fixture.transport.renderTarget->handleDrainComplete();
    // Splice (advance/state/route) and EOF (state/route/end) await delivery.
    REQUIRE(executor.tryWaitUntilQueuedCount(6));
    REQUIRE(transport.state().nowPlaying.trackId == fixture.firstTrackId);
    REQUIRE(secondTrackEvents.empty());
    bool supersededByNext = false;

    SECTION("a ready route plays C rather than replaying B")
    {
      transport.setOutputDevice(alternate);
      executor.drain();
      CHECK(transport.state().output.selectedDevice == alternate);
      CHECK(transport.state().transport == audio::Transport::Playing);
      CHECK(transport.state().nowPlaying.trackId == fixture.thirdTrackId);
      CHECK(succession.state().currentTrackId == fixture.thirdTrackId);
    }

    SECTION("a missing route retains B only as the last restorable track")
    {
      auto missing = alternate;
      missing.deviceId = audio::DeviceId{"missing"};
      transport.setOutputDevice(missing);
      executor.drain();
      REQUIRE(transport.state().output.optPendingOutputSelection == missing);
      CHECK(succession.state().sourceState == PlaybackSuccessionSourceState::Inactive);

      devices.push_back(audio::Device{.id = missing.deviceId, .backendId = missing.backendId});
      fixture.transport.onDevicesChangedCb(devices);
      executor.drain();
      REQUIRE(transport.state().ready);
      transport.resume();
      executor.drain();
      CHECK(transport.state().transport == audio::Transport::Idle);
      CHECK(transport.state().occurrenceId == PlaybackOccurrenceId{});

      auto playback = fixture.createPlayback();
      CHECK(playback.snapshot().transport.nowPlaying.trackId == fixture.secondTrackId);
      auto store = ConfigStore{fixture.transport.libraryFixture.root() / "session.yaml"};
      auto persistence = PlaybackSessionPersistence{
        store, fixture.commandsFixture.library(), succession, transport, playback, fixture.asyncRuntime};
      REQUIRE(persistence.checkpoint());
      auto saved = PlaybackSessionState{};
      auto const loadedRes = store.load(kPlaybackSessionConfigGroup, saved, PlaybackSessionYamlSchema{});
      REQUIRE(loadedRes);
      REQUIRE(*loadedRes);
      CHECK(saved.currentTrackId == fixture.secondTrackId);
      CHECK(saved.anchorIndex == 1);
    }

    SECTION("Stop from B's deferred now-playing observer prevents C from starting")
    {
      bool stoppedFromB = false;
      auto const stopSubscription = transport.onNowPlayingChanged(
        [&](PlaybackTransport::NowPlayingChanged const& event)
        {
          if (event.trackId == fixture.secondTrackId && event.optPreparedNextToken)
          {
            stoppedFromB = true;
            std::ignore = transport.stop();
          }
        });
      // Switch outputs while Transport is already draining its notifications.
      auto const volumeSubscription = transport.onVolumeChanged([&](float) { transport.setOutputDevice(alternate); });
      transport.setVolume(0.5F);
      executor.drain();

      CHECK(stoppedFromB);
      CHECK(transport.state().output.selectedDevice == alternate);
      CHECK(transport.state().transport == audio::Transport::Idle);
      CHECK(transport.state().occurrenceId == PlaybackOccurrenceId{});
      CHECK(transport.state().nowPlaying.trackId == kInvalidTrackId);
      CHECK(succession.state().sourceState == PlaybackSuccessionSourceState::Inactive);
    }

    SECTION("Next from the outer observer supersedes the queued prepared notice")
    {
      auto const volumeSubscription = transport.onVolumeChanged(
        [&](float)
        {
          transport.setOutputDevice(alternate);
          supersededByNext = succession.tryMoveNext();
        });
      transport.setVolume(0.5F);
      executor.drain();

      CHECK(supersededByNext);
      CHECK(transport.state().output.selectedDevice == alternate);
      CHECK(transport.state().transport == audio::Transport::Playing);
      CHECK(transport.state().nowPlaying.trackId != fixture.firstTrackId);
      CHECK(transport.state().nowPlaying.trackId == succession.state().currentTrackId);
    }

    if (supersededByNext)
    {
      for (auto const& event : secondTrackEvents)
      {
        // An explicit replay of B is allowed, but not the retired prepared notice.
        CHECK_FALSE(event.optPreparedNextToken);
      }
    }
    else
    {
      REQUIRE(secondTrackEvents.size() == 1);
      CHECK(secondTrackEvents.front().optPreparedNextToken);
    }
  }

  TEST_CASE("PlaybackSuccession - positioning commands retain a naturally advanced winner", "[concurrency]")
  {
    auto fixture = PlaybackSuccessionTransportFixture{};
    fixture.buildThreeTrackManualView();
    auto events = std::vector<PlaybackTransport::NowPlayingChanged>{};
    std::size_t finalSeekCount = 0;
    auto const nowPlayingSubscription = fixture.transport.playbackTransport.onNowPlayingChanged(
      [&](PlaybackTransport::NowPlayingChanged const& event) noexcept { events.push_back(event); });
    auto const seekSubscription = fixture.transport.playbackTransport.onSeekUpdate(
      [&](PlaybackTransport::SeekUpdate const& update) noexcept
      {
        if (update.mode == PlaybackTransport::SeekMode::Final)
        {
          ++finalSeekCount;
        }
      });
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    fixture.transport.executor.drain();
    events.clear();
    auto const retiredOccurrence = fixture.transport.playbackTransport.state().occurrenceId;

    fixture.queueNaturalAdvance();
    std::size_t expectedFinalSeekCount = 0;

    SECTION("unconditional final seek preserves the prepared winner")
    {
      fixture.transport.playbackTransport.seek(std::chrono::milliseconds{0}, PlaybackTransport::SeekMode::Final);
      expectedFinalSeekCount = 1;
    }

    SECTION("guarded seek rejects the retired occurrence without a seek event")
    {
      CHECK_FALSE(fixture.transport.playbackTransport.trySeek(retiredOccurrence, std::chrono::milliseconds{0}));
    }

    SECTION("guarded Next observes the realtime winner and preserves its successor")
    {
      CHECK_FALSE(fixture.transport.playbackTransport.canAdvanceFrom(retiredOccurrence));
    }

    CHECK(finalSeekCount == expectedFinalSeekCount);
    fixture.transport.executor.drain();

    REQUIRE(events.size() == 1);
    REQUIRE(events.front().optPreparedNextToken);
    CHECK(events.front().trackId == fixture.secondTrackId);
    CHECK(fixture.successionPtr->state().currentTrackId == fixture.secondTrackId);
    CHECK(fixture.successionPtr->state().optResolvedSuccessor == fixture.thirdTrackId);
    CHECK(fixture.transport.playbackTransport.state().occurrenceId != retiredOccurrence);
    CHECK(fixture.transport.playbackTransport.state().transport == audio::Transport::Playing);
    CHECK(finalSeekCount == expectedFinalSeekCount);
  }

  TEST_CASE("PlaybackSuccession - natural gapless advance invalidates a pending explicit start", "[concurrency]")
  {
    auto gatePtr = std::make_shared<audio::test::BlockingPreparationGate>();
    auto fixture = PlaybackSuccessionTransportFixture{PlaybackSuccessionTransportFixtureConfig{
      .blockingGatePtr = gatePtr,
      .blockedFileName = "transport-playable-2.flac",
    }};
    auto releaseGuard = PreparationReleaseGuard{gatePtr};
    fixture.buildThreeTrackManualView();
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    fixture.queueNaturalAdvance();

    REQUIRE(fixture.successionPtr->playFromView(fixture.viewId, fixture.thirdTrackId));
    REQUIRE(gatePtr->tryWaitForEntry());
    fixture.transport.executor.drain();
    REQUIRE(fixture.successionPtr->state().currentTrackId == fixture.secondTrackId);
    auto const activationCount = fixture.decoderActivationCount(fixture.thirdTrackId);

    releaseGuard.release();
    // On this fixture's single preparation worker, the winner's new lookahead
    // settles after the canceled inspection. Its live decoder must not be
    // mistaken for an unreclaimed explicit candidate for the same track.
    REQUIRE(fixture.tryWaitForDecoderActivationAfter(fixture.thirdTrackId, activationCount));
    auto const created = gatePtr->createdPtr->load(std::memory_order_relaxed);
    auto const destroyed = gatePtr->destroyedPtr->load(std::memory_order_relaxed);
    CHECK(destroyed > 0);
    CHECK(created == destroyed + 1);

    CHECK(fixture.successionPtr->state().currentTrackId == fixture.secondTrackId);
    CHECK(fixture.transport.playbackTransport.state().nowPlaying.trackId == fixture.secondTrackId);
    CHECK(fixture.transport.playbackTransport.state().transport == audio::Transport::Playing);
    CHECK(fixture.transport.notificationService.feed().entries.empty());
  }

  TEST_CASE("PlaybackSuccession - destruction disconnects a queued natural-advance callback", "[concurrency]")
  {
    auto fixture = PlaybackSuccessionTransportFixture{};
    fixture.buildThreeTrackManualView();
    std::uint32_t changedCount = 0;
    auto const subscription =
      fixture.successionPtr->onChanged([&](PlaybackSuccessionState const&) noexcept { ++changedCount; });
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    REQUIRE(changedCount == 1);
    fixture.queueNaturalAdvance();

    fixture.successionPtr.reset();
    fixture.transport.executor.drain();

    CHECK(changedCount == 1);
    CHECK(fixture.transport.playbackTransport.state().nowPlaying.trackId == fixture.secondTrackId);
  }

  TEST_CASE("PlaybackSuccession - previous restart uses a strict greater-than three-second final seek")
  {
    auto fixture = PlaybackSuccessionSeekFixture{};
    fixture.buildThreeTrackManualView();
    auto& succession = *fixture.successionPtr;
    auto& playbackTransport = *fixture.transportPtr;
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    fixture.executor.drain();
    playbackTransport.pause();
    REQUIRE(playbackTransport.state().transport == audio::Transport::Paused);

    playbackTransport.seek(std::chrono::milliseconds{3000}, PlaybackTransport::SeekMode::Final);
    CHECK(playbackTransport.elapsed() == std::chrono::milliseconds{3000});
    CHECK_FALSE(succession.state().hasPrevious);

    playbackTransport.seek(std::chrono::milliseconds{3001}, PlaybackTransport::SeekMode::Final);
    CHECK(playbackTransport.elapsed() == std::chrono::milliseconds{3001});
    CHECK(succession.state().hasPrevious);

    succession.tryMovePrevious();
    CHECK(succession.state().currentTrackId == fixture.firstTrackId);
    CHECK_FALSE(succession.state().hasPrevious);
    CHECK(playbackTransport.elapsed() == std::chrono::milliseconds{0});
    CHECK(playbackTransport.state().transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSuccession - Previous restarts a resumed interrupted track beyond three seconds")
  {
    auto fixture = PlaybackSuccessionSeekFixture{};
    fixture.buildSingleTrackManualView();
    auto& succession = *fixture.successionPtr;
    auto& transport = *fixture.transportPtr;
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    auto const offset = std::chrono::milliseconds{3001};
    transport.seek(offset);
    auto const selected = transport.state().output.selectedDevice;
    transport.setOutputDevice(selected.backendId, audio::DeviceId{"missing"}, selected.profileId);
    REQUIRE(transport.state().transport == audio::Transport::Paused);
    REQUIRE(transport.elapsed() == offset);

    transport.setOutputDevice(selected);
    transport.resume();
    REQUIRE(transport.state().transport == audio::Transport::Playing);
    REQUIRE(transport.elapsed() == offset);
    REQUIRE(succession.tryMovePrevious());
    CHECK(transport.state().nowPlaying.trackId == fixture.firstTrackId);
    CHECK(transport.elapsed() == std::chrono::milliseconds{0});
  }

  TEST_CASE("PlaybackSuccession - Next and Previous follow the cursor")
  {
    auto fixture = PlaybackSuccessionFixture{};
    fixture.buildThreeTrackManualView();
    auto& succession = *fixture.successionPtr;
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));

    succession.tryMoveNext();
    CHECK(succession.state().currentTrackId == fixture.secondTrackId);
    CHECK(succession.state().hasPrevious);

    succession.tryMovePrevious();
    CHECK(succession.state().currentTrackId == fixture.firstTrackId);
  }

  TEST_CASE("PlaybackSuccession - dedicated mode signals notify only on changes")
  {
    auto fixture = PlaybackSuccessionFixture{};
    fixture.buildThreeTrackManualView();
    auto& succession = *fixture.successionPtr;
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));

    std::uint32_t shuffleEvents = 0;
    std::uint32_t repeatEvents = 0;
    auto const shuffleSubscription =
      succession.onShuffleModeChanged([&](PlaybackSuccession::ShuffleModeChanged const&) noexcept { ++shuffleEvents; });
    auto const repeatSubscription =
      succession.onRepeatModeChanged([&](PlaybackSuccession::RepeatModeChanged const&) noexcept { ++repeatEvents; });

    succession.setRepeatMode(RepeatMode::One);
    CHECK(succession.state().repeat == RepeatMode::One);
    CHECK(succession.state().optResolvedSuccessor == fixture.firstTrackId);
    CHECK(repeatEvents == 1);
    succession.setRepeatMode(RepeatMode::One);
    CHECK(repeatEvents == 1);

    succession.setShuffleMode(ShuffleMode::On);
    CHECK(succession.state().shuffle == ShuffleMode::On);
    CHECK(shuffleEvents == 1);
  }

  TEST_CASE("PlaybackSuccession - clear deactivates succession without stopping transport")
  {
    auto fixture = PlaybackSuccessionFixture{};
    fixture.buildThreeTrackManualView();
    auto& succession = *fixture.successionPtr;
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));

    succession.clear();
    CHECK(succession.state().sourceState == PlaybackSuccessionSourceState::Inactive);
    CHECK(succession.state().currentTrackId == kInvalidTrackId);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Playing);
    CHECK(fixture.playbackTransport.state().nowPlaying.trackId == fixture.firstTrackId);
  }
} // namespace ao::rt::test
