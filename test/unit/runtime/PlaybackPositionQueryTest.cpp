// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/audio/EngineTestSupport.h"
#include "test/unit/runtime/PlaybackSuccessionTransportTestSupport.h"
#include <ao/audio/RenderTarget.h>
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

  TEST_CASE("PlaybackService - elapsed reads a live correlated clock without publishing",
            "[runtime][unit][playback][seek]")
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

  TEST_CASE("PlaybackService - elapsed does not attribute a pending realtime splice to old metadata",
            "[runtime][unit][playback][concurrency]")
  {
    auto fixture = PlaybackSuccessionTransportFixture{};
    fixture.buildThreeTrackManualView();
    auto playback = fixture.createPlayback();
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    auto const activationCount = fixture.lookaheadActivationCount(fixture.secondTrackId);
    playback.commands().seek(std::chrono::milliseconds{100});
    REQUIRE(fixture.tryWaitForLookaheadAfter(fixture.secondTrackId, activationCount));
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

  TEST_CASE("PlaybackCommands seekBy - past-end Next cancels an older pending explicit preparation",
            "[runtime][unit][playback][concurrency]")
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
    auto const activationCount = fixture.lookaheadActivationCount(fixture.thirdTrackId);
    releaseGuard.release();
    // The single worker must retire the old inspection before it can prepare
    // the winner's third-track lookahead. That legitimate decoder stays alive.
    REQUIRE(fixture.tryWaitForLookaheadAfter(fixture.thirdTrackId, activationCount));
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
