// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/playback/PlaybackSuccession.h"
#include "runtime/playback/PlaybackTransport.h"
#include "test/unit/audio/EngineTestSupport.h"
#include "test/unit/runtime/PlaybackSuccessionTestSupport.h"
#include <ao/audio/BackendIds.h>
#include <ao/audio/Transport.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ao::rt::test
{
  using playback_succession::PlaybackSuccessionTransportFixture;
  using playback_succession::PlaybackSuccessionTransportFixtureConfig;
  using playback_succession::PreparationReleaseGuard;

  TEST_CASE("PlaybackSuccession - idle fallback advances without a prepared successor",
            "[runtime][regression][playback-succession]")
  {
    auto fixture = PlaybackSuccessionTransportFixture{};
    fixture.buildThreeTrackManualView();
    auto& playbackTransport = fixture.transport.playbackTransport;
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    fixture.transport.executor.drain();
    REQUIRE(playbackTransport.clearPreparedNext());
    REQUIRE(fixture.transport.renderTarget != nullptr);

    auto output = std::array<std::byte, 4096>{};
    bool drained = false;

    for (std::int32_t attempt = 0; attempt < 100000 && !drained; ++attempt)
    {
      drained = fixture.transport.renderTarget->renderPcm(output).drained;
    }

    REQUIRE(drained);
    fixture.transport.renderTarget->handleDrainComplete();
    fixture.transport.executor.checkQueued(std::chrono::seconds{5});
    fixture.transport.executor.drain();

    CHECK(fixture.successionPtr->state().currentTrackId == fixture.secondTrackId);
    CHECK(playbackTransport.state().nowPlaying.trackId == fixture.secondTrackId);
    CHECK(playbackTransport.state().transport == audio::Transport::Playing);
    CHECK(fixture.transport.notificationService.feed().entries.empty());
  }

  TEST_CASE("PlaybackSuccession - output and final seek replace the disarmed lookahead token",
            "[runtime][unit][playback-succession][token]")
  {
    auto fixture = PlaybackSuccessionTransportFixture{};
    fixture.buildThreeTrackManualView();
    auto& playbackTransport = fixture.transport.playbackTransport;
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));

    auto const optFirstToken = playbackTransport.clearPreparedNext();
    REQUIRE(optFirstToken);
    auto const activationCount = fixture.lookaheadActivationCount(fixture.secondTrackId);

    SECTION("output edge")
    {
      playbackTransport.setOutputDevice(
        audio::BackendId{"mock_backend"}, audio::DeviceId{"mock_device"}, audio::ProfileId{audio::kProfileShared});
    }

    SECTION("final seek edge")
    {
      playbackTransport.seek(std::chrono::milliseconds{0}, PlaybackTransport::SeekMode::Final);
    }

    REQUIRE(fixture.waitForLookaheadAfter(fixture.secondTrackId, activationCount));
    auto const optReplacementToken = playbackTransport.clearPreparedNext();
    REQUIRE(optReplacementToken);
    CHECK(*optReplacementToken != *optFirstToken);
    CHECK(fixture.successionPtr->state().currentTrackId == fixture.firstTrackId);
    CHECK(playbackTransport.state().transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSuccession - natural prepared winner is adopted exactly once",
            "[runtime][unit][playback-succession][token]")
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

  TEST_CASE("PlaybackSuccession - retired prepared winner survives a final-seek race",
            "[runtime][unit][playback-succession][token]")
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
    fixture.transport.playbackTransport.seek(std::chrono::milliseconds{0}, PlaybackTransport::SeekMode::Final);
    fixture.transport.executor.drain();

    REQUIRE(events.size() == 1);
    REQUIRE(events.front().optPreparedNextToken);
    CHECK(events.front().trackId == fixture.secondTrackId);
    CHECK(fixture.successionPtr->state().currentTrackId == fixture.secondTrackId);
    CHECK(fixture.successionPtr->state().optResolvedSuccessor == fixture.thirdTrackId);
    CHECK(fixture.transport.playbackTransport.state().transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSuccession - queued natural advance settles before an asynchronous explicit start",
            "[runtime][unit][playback-succession][token]")
  {
    auto fixture = PlaybackSuccessionTransportFixture{};
    fixture.buildThreeTrackManualView();
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    fixture.queueNaturalAdvance();

    REQUIRE(fixture.successionPtr->playFromView(fixture.viewId, fixture.thirdTrackId));
    fixture.transport.executor.drain();

    CHECK(fixture.successionPtr->state().currentTrackId == fixture.secondTrackId);
    CHECK(fixture.transport.playbackTransport.state().nowPlaying.trackId == fixture.secondTrackId);
    CHECK(fixture.transport.playbackTransport.state().transport == audio::Transport::Playing);
    CHECK(fixture.transport.notificationService.feed().entries.empty());
  }

  TEST_CASE("PlaybackSuccession - natural gapless advance invalidates a pending explicit start",
            "[runtime][regression][playback-succession][concurrency]")
  {
    auto gatePtr = std::make_shared<audio::test::BlockingPreparationGate>();
    auto fixture = PlaybackSuccessionTransportFixture{PlaybackSuccessionTransportFixtureConfig{
      .blockingGatePtr = gatePtr,
      .blockedFileName = "transport-playable-2.flac",
    }};
    fixture.buildThreeTrackManualView();
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    fixture.queueNaturalAdvance();

    REQUIRE(fixture.successionPtr->playFromView(fixture.viewId, fixture.thirdTrackId));
    REQUIRE(gatePtr->waitForEntry());
    auto releaseGuard = PreparationReleaseGuard{gatePtr};
    fixture.transport.executor.drain();
    REQUIRE(fixture.successionPtr->state().currentTrackId == fixture.secondTrackId);

    releaseGuard.release();
    REQUIRE(fixture.transport.executor.drainUntil(
      [&] { return gatePtr->destroyedPtr->load(std::memory_order_relaxed) > 0; }, std::chrono::seconds{5}));

    CHECK(fixture.successionPtr->state().currentTrackId == fixture.secondTrackId);
    CHECK(fixture.transport.playbackTransport.state().nowPlaying.trackId == fixture.secondTrackId);
    CHECK(fixture.transport.playbackTransport.state().transport == audio::Transport::Playing);
    CHECK(fixture.transport.notificationService.feed().entries.empty());
  }

  TEST_CASE("PlaybackSuccession - destruction disconnects a queued natural-advance callback",
            "[runtime][unit][playback-succession][lifecycle]")
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
} // namespace ao::rt::test
