// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/audio/EngineTestSupport.h"
#include "test/runtime/PlaybackSuccessionTransportTestSupport.h"
#include <ao/audio/Device.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/Transport.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackService.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <vector>

namespace ao::rt::test
{
  using playback_succession::PlaybackSuccessionTransportFixture;
  using playback_succession::PlaybackSuccessionTransportFixtureConfig;
  using playback_succession::PreparationReleaseGuard;

  TEST_CASE("PlaybackService - elapsed reads a live correlated clock without publishing")
  {
    auto fixture = PlaybackSuccessionTransportFixture{};
    fixture.buildThreeTrackManualView();
    auto playback = fixture.createPlayback();
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    fixture.transport.executor.drain();
    auto const before = playback.snapshot().transport;
    std::size_t publicationCount = 0;
    auto const subscription = playback.events().onSnapshot([&](PlaybackSnapshot const&) { ++publicationCount; });
    auto output = std::array<std::byte, 4096>{};
    REQUIRE(fixture.transport.renderTarget != nullptr);
    auto const rendered = fixture.transport.renderTarget->renderPcm(output);
    REQUIRE(rendered.bytesWritten > 0);
    REQUIRE(rendered.positionFrames > 0);
    fixture.transport.renderTarget->handlePositionAdvanced(rendered.positionFrames);
    auto const liveElapsed = fixture.transport.playbackTransport.elapsed();
    REQUIRE(liveElapsed > before.elapsed);

    CHECK(playback.elapsed() == liveElapsed);
    CHECK(playback.snapshot().transport.elapsed == before.elapsed);
    CHECK(playback.snapshot().transport.positionRevision == before.positionRevision);
    CHECK(publicationCount == 0);

    playback.commands().pause();
    auto const paused = playback.snapshot().transport;
    CHECK(playback.elapsed() == paused.elapsed);
    playback.commands().stop();
    CHECK(playback.elapsed() == std::chrono::milliseconds{0});
  }

  TEST_CASE("PlaybackService - elapsed does not attribute a pending realtime splice to old metadata", "[concurrency]")
  {
    auto fixture = PlaybackSuccessionTransportFixture{};
    fixture.buildThreeTrackManualView();
    auto playback = fixture.createPlayback();
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    auto const activationCount = fixture.decoderActivationCount(fixture.secondTrackId);
    playback.commands().seek(std::chrono::milliseconds{100});
    REQUIRE(fixture.tryWaitForDecoderActivationAfter(fixture.secondTrackId, activationCount));
    fixture.transport.executor.drain();
    auto const before = playback.snapshot().transport;
    REQUIRE(before.elapsed == std::chrono::milliseconds{100});

    fixture.queueNaturalAdvance();
    REQUIRE(fixture.transport.playbackTransport.state().occurrenceId == before.occurrenceId);
    REQUIRE(fixture.transport.playbackTransport.elapsed() != before.elapsed);
    CHECK(playback.elapsed() == before.elapsed);
    CHECK(playback.snapshot().transport.occurrenceId == before.occurrenceId);

    fixture.transport.executor.drain();
    CHECK(playback.snapshot().transport.nowPlaying.trackId == fixture.secondTrackId);
    CHECK(playback.snapshot().transport.occurrenceId != before.occurrenceId);
    CHECK(playback.elapsed() == fixture.transport.playbackTransport.elapsed());
    CHECK(playback.snapshot().transport.finalSeekRevision == before.finalSeekRevision);
  }

  TEST_CASE("PlaybackCommands - navigation preserves the track and offset held for a missing output")
  {
    auto fixture = PlaybackSuccessionTransportFixture{};
    fixture.buildThreeTrackManualView();
    auto playback = fixture.createPlayback();
    auto& commands = playback.commands();
    REQUIRE(fixture.playAndWait(fixture.secondTrackId));
    fixture.transport.executor.drain();

    auto const selected = playback.snapshot().transport.output.selectedDevice;
    auto const dac = audio::Device{.id = audio::DeviceId{"studio-dac"}, .backendId = selected.backendId};
    auto const offset = std::chrono::milliseconds{100};
    commands.seek(offset);
    commands.setOutputDevice(dac.backendId, dac.id, selected.profileId);
    fixture.transport.executor.drain();
    auto const held = playback.snapshot();
    REQUIRE(held.transport.transport == audio::Transport::Paused);
    REQUIRE_FALSE(held.transport.ready);
    REQUIRE(held.transport.output.optPendingOutputSelection);
    REQUIRE(held.transport.elapsed == offset);
    REQUIRE(held.succession.hasNext);
    REQUIRE(held.succession.hasPrevious);

    std::size_t publicationCount = 0;
    auto const subscription = playback.events().onSnapshot([&](PlaybackSnapshot const&) { ++publicationCount; });

    SECTION("Next")
    {
      commands.next();
    }

    SECTION("Previous")
    {
      commands.previous();
    }

    SECTION("Occurrence-guarded Next")
    {
      CHECK_FALSE(commands.tryNext(held.transport.occurrenceId));
    }

    fixture.transport.executor.drain();
    CHECK(playback.snapshot() == held);
    CHECK(playback.elapsed() == offset);
    CHECK(publicationCount == 0);
    CHECK(fixture.transport.notificationService.feed().entries.empty());

    fixture.transport.onDevicesChangedCb({dac});
    fixture.transport.executor.drain();
    REQUIRE(playback.snapshot().transport.ready);
    REQUIRE(playback.snapshot().transport.transport == audio::Transport::Paused);
    CHECK(playback.snapshot().transport.nowPlaying.trackId == fixture.secondTrackId);
    CHECK(playback.elapsed() == offset);

    commands.resume();
    REQUIRE(playback.snapshot().transport.transport == audio::Transport::Playing);
    CHECK(playback.snapshot().transport.nowPlaying.trackId == fixture.secondTrackId);
    CHECK(playback.elapsed() == offset);

    commands.next();
    REQUIRE(playback.snapshot().transport.nowPlaying.trackId == fixture.thirdTrackId);
    commands.previous();
    CHECK(playback.snapshot().transport.nowPlaying.trackId == fixture.secondTrackId);
    CHECK(playback.snapshot().succession.currentTrackId == fixture.secondTrackId);
  }

  TEST_CASE("PlaybackCommands - queued navigation checks output readiness when it executes")
  {
    auto fixture = PlaybackSuccessionTransportFixture{};
    fixture.buildThreeTrackManualView();
    auto playback = fixture.createPlayback();
    auto& commands = playback.commands();
    REQUIRE(fixture.playAndWait(fixture.secondTrackId));
    fixture.transport.executor.drain();

    auto const before = playback.snapshot().transport;
    REQUIRE(before.ready);
    auto const selected = before.output.selectedDevice;
    auto const dac = audio::Device{.id = audio::DeviceId{"studio-dac"}, .backendId = selected.backendId};
    auto const offset = std::chrono::milliseconds{100};
    commands.seek(offset);
    bool queued = false;
    auto const subscription = playback.events().onSnapshot(
      [&](PlaybackSnapshot const& snapshot)
      {
        if (!queued && snapshot.transport.ready)
        {
          queued = true;
          commands.setOutputDevice(dac.backendId, dac.id, selected.profileId);
          commands.next();
        }
      });

    commands.setRepeatMode(RepeatMode::All);
    REQUIRE(queued);
    REQUIRE(playback.snapshot().transport.ready);
    fixture.transport.executor.drain();

    auto const held = playback.snapshot();
    CHECK_FALSE(held.transport.ready);
    CHECK(held.transport.transport == audio::Transport::Paused);
    CHECK(held.transport.occurrenceId == before.occurrenceId);
    CHECK(held.transport.nowPlaying.trackId == fixture.secondTrackId);
    CHECK(held.succession.currentTrackId == fixture.secondTrackId);
    CHECK(playback.elapsed() == offset);
    CHECK(fixture.transport.notificationService.feed().entries.empty());
  }

  TEST_CASE("PlaybackCommands - navigation rejected on missing output does not supersede a queued start")
  {
    auto fixture = PlaybackSuccessionTransportFixture{};
    fixture.buildThreeTrackManualView();
    auto playback = fixture.createPlayback();
    auto& commands = playback.commands();
    REQUIRE(fixture.playAndWait(fixture.secondTrackId));
    fixture.transport.executor.drain();

    auto const selected = playback.snapshot().transport.output.selectedDevice;
    commands.setOutputDevice(selected.backendId, audio::DeviceId{"studio-dac"}, selected.profileId);
    REQUIRE_FALSE(playback.snapshot().transport.ready);

    bool queued = false;
    bool startAdmitted = false;
    auto const subscription = playback.events().onSnapshot(
      [&](PlaybackSnapshot const& snapshot)
      {
        if (!queued && !snapshot.transport.ready)
        {
          queued = true;
          // The route command queued ahead of the start makes it executable.
          commands.setOutputDevice(selected);
          startAdmitted = commands.startFromView(fixture.viewId, fixture.thirdTrackId).has_value();
          commands.next();
          commands.previous();
        }
      });

    commands.setRepeatMode(RepeatMode::All);
    REQUIRE(startAdmitted);
    REQUIRE(fixture.transport.executor.tryDrainUntil(
      [&] { return playback.snapshot().transport.transport == audio::Transport::Playing; }));
    CHECK(playback.snapshot().transport.nowPlaying.trackId == fixture.thirdTrackId);
    CHECK(playback.snapshot().succession.currentTrackId == fixture.thirdTrackId);
    CHECK(fixture.transport.notificationService.feed().entries.empty());
  }

  TEST_CASE("PlaybackCommands seekBy - past-end Next preserves a track held for a missing output")
  {
    auto fixture = PlaybackSuccessionTransportFixture{};
    fixture.buildTwoTrackManualView();
    auto playback = fixture.createPlayback();
    auto& commands = playback.commands();
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    fixture.transport.executor.drain();

    auto const selected = playback.snapshot().transport.output.selectedDevice;
    auto const dac = audio::Device{.id = audio::DeviceId{"studio-dac"}, .backendId = selected.backendId};
    commands.setOutputDevice(dac.backendId, dac.id, selected.profileId);
    auto const held = playback.snapshot().transport;
    REQUIRE(held.transport == audio::Transport::Paused);
    REQUIRE_FALSE(held.ready);
    REQUIRE(playback.snapshot().succession.hasNext);

    auto const delta = std::chrono::milliseconds{100};
    auto const offset = held.elapsed + delta;
    REQUIRE(offset < held.duration);
    commands.seekBy(held.occurrenceId, delta, PlaybackRelativeSeekEndBehavior::Next);
    REQUIRE(playback.elapsed() == offset);

    commands.seekBy(held.occurrenceId, std::chrono::milliseconds::max(), PlaybackRelativeSeekEndBehavior::Next);
    CHECK(playback.snapshot().transport.occurrenceId == held.occurrenceId);
    CHECK(playback.snapshot().succession.currentTrackId == fixture.firstTrackId);
    CHECK(playback.elapsed() == offset);

    fixture.transport.onDevicesChangedCb({dac});
    fixture.transport.executor.drain();
    REQUIRE(playback.snapshot().transport.ready);
    REQUIRE(playback.snapshot().transport.transport == audio::Transport::Paused);
    commands.resume();
    CHECK(playback.snapshot().transport.transport == audio::Transport::Playing);
    CHECK(playback.snapshot().transport.nowPlaying.trackId == fixture.firstTrackId);
    CHECK(playback.elapsed() == offset);
  }

  TEST_CASE("PlaybackCommands seekBy - past-end Next cancels an older pending explicit preparation", "[concurrency]")
  {
    auto gatePtr = std::make_shared<audio::test::BlockingPreparationGate>();
    auto fixture = PlaybackSuccessionTransportFixture{PlaybackSuccessionTransportFixtureConfig{
      .blockingGatePtr = gatePtr,
      .blockedFileName = "transport-playable-2.flac",
    }};
    auto releaseGuard = PreparationReleaseGuard{gatePtr};
    fixture.buildThreeTrackManualView();
    auto playback = fixture.createPlayback();
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    fixture.transport.executor.drain();
    auto const before = playback.snapshot().transport;
    auto publishedTracks = std::vector<TrackId>{};
    auto const subscription = playback.events().onSnapshot(
      [&](PlaybackSnapshot const& snapshot) { publishedTracks.push_back(snapshot.transport.nowPlaying.trackId); });
    REQUIRE(playback.commands().startFromView(fixture.viewId, fixture.thirdTrackId));
    REQUIRE(gatePtr->tryWaitForEntry());

    playback.commands().seekBy(
      before.occurrenceId, std::chrono::milliseconds::max(), PlaybackRelativeSeekEndBehavior::Next);
    auto const activationCount = fixture.decoderActivationCount(fixture.thirdTrackId);
    releaseGuard.release();
    // The single worker must retire the old inspection before it can prepare
    // the winner's third-track lookahead. That legitimate decoder stays alive.
    REQUIRE(fixture.tryWaitForDecoderActivationAfter(fixture.thirdTrackId, activationCount));
    auto const created = gatePtr->createdPtr->load(std::memory_order_relaxed);
    auto const destroyed = gatePtr->destroyedPtr->load(std::memory_order_relaxed);
    CHECK(destroyed > 0);
    CHECK(created == destroyed + 1);
    CHECK(playback.snapshot().transport.nowPlaying.trackId == fixture.secondTrackId);
    CHECK(playback.snapshot().transport.occurrenceId != before.occurrenceId);
    CHECK(playback.snapshot().transport.finalSeekRevision == before.finalSeekRevision);

    for (auto const trackId : publishedTracks)
    {
      CHECK(trackId != fixture.thirdTrackId);
    }
  }
} // namespace ao::rt::test
