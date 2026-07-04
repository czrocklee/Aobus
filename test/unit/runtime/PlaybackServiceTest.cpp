// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/audio/AudioFixtureUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/PlaybackServiceTestSupport.h"
#include <ao/audio/IRenderTarget.h>
#include <ao/audio/Transport.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("PlaybackService playback - playTrack fails when track does not exist", "[runtime][unit][playback][play]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};

    CHECK_FALSE(fixture.playbackService.playTrack(TrackId{99999}, ListId{7}));
  }

  TEST_CASE("PlaybackService playback - playTrack resolves track metadata", "[runtime][unit][playback][play]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};

    // Prime the device list. The first notification auto-selects the default
    // output; the duplicate exercises the "already selected" early return, and the
    // empty list exercises the no-devices guard.
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.onDevicesChangedCb(fixture.status.devices);
    auto emptyStatus = fixture.status;
    emptyStatus.devices.clear();
    fixture.onDevicesChangedCb(emptyStatus.devices);

    auto spec = library::test::TrackSpec{};
    spec.title = "Playable Track";
    spec.artist = "Queue Artist";
    spec.album = "Queue Album";
    spec.duration = std::chrono::minutes{3};
    auto const trackId = fixture.testLib.addTrack(spec);

    CHECK(fixture.playbackService.playTrack(trackId, ListId{7}));
    CHECK(fixture.playbackService.state().trackId == trackId);
    CHECK(fixture.playbackService.state().sourceListId == ListId{7});
    CHECK(fixture.playbackService.state().trackTitle == "Playable Track");
    CHECK(fixture.playbackService.state().trackArtist == "Queue Artist");
    CHECK(fixture.playbackService.state().duration == std::chrono::minutes{3});
  }

  TEST_CASE("PlaybackService playback - prepareNext does not replace current state", "[runtime][unit][playback]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();

    auto const currentTrack = fixture.testLib.addTrack({.title = "Current Track", .uri = fixturePath});
    auto const nextTrack = fixture.testLib.addTrack({.title = "Prepared Track", .uri = fixturePath});

    REQUIRE(fixture.playbackService.playTrack(currentTrack, ListId{7}));
    REQUIRE(fixture.playbackService.prepareNext(nextTrack, ListId{7}));

    CHECK(fixture.playbackService.state().trackId == currentTrack);
    CHECK(fixture.playbackService.state().trackTitle == "Current Track");
    CHECK_FALSE(fixture.playbackService.prepareNext(TrackId{99999}, ListId{7}));
    CHECK(fixture.playbackService.state().trackId == currentTrack);
  }

  TEST_CASE("PlaybackService playback - drain emits idle when playback is actually idle",
            "[runtime][unit][playback][drain]")
  {
    auto fixture = PlaybackFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const trackId = fixture.testLib.addTrack({.title = "Terminal Track", .uri = fixturePath});

    std::size_t idleCount = 0;
    auto idleSub = fixture.playbackService.onIdle([&] { ++idleCount; });

    REQUIRE(fixture.playbackService.playTrack(trackId, ListId{7}));
    REQUIRE(fixture.renderTarget != nullptr);

    auto buffer = std::array<std::byte, 4096>{};
    bool isDrained = false;

    for (std::int32_t i = 0; i < 100000 && !isDrained; ++i)
    {
      isDrained = fixture.renderTarget->renderPcm(buffer).drained;
    }

    REQUIRE(isDrained);
    fixture.renderTarget->onDrainComplete();

    for (std::int32_t i = 0; i < 100000 && idleCount == 0; ++i)
    {
      fixture.executor.drain();
    }

    REQUIRE(idleCount > 0);
    CHECK(idleCount == 1);
    CHECK(fixture.playbackService.state().transport == audio::Transport::Idle);
  }

  TEST_CASE("PlaybackService playback - natural advance commits prepared track without idle",
            "[runtime][unit][playback][gapless]")
  {
    auto fixture = PlaybackFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const currentTrack = fixture.testLib.addTrack({.title = "Current Track", .uri = fixturePath});
    auto const nextTrack = fixture.testLib.addTrack({.title = "Prepared Track", .uri = fixturePath});

    auto nowPlaying = std::vector<PlaybackService::NowPlayingChanged>{};
    std::size_t idleCount = 0;
    auto nowPlayingSub = fixture.playbackService.onNowPlayingChanged([&](PlaybackService::NowPlayingChanged const& ev)
                                                                     { nowPlaying.push_back(ev); });
    auto idleSub = fixture.playbackService.onIdle([&] { ++idleCount; });

    REQUIRE(fixture.playbackService.playTrack(currentTrack, ListId{7}));
    REQUIRE(fixture.playbackService.prepareNext(nextTrack, ListId{7}));
    REQUIRE(fixture.renderTarget != nullptr);

    // Only observe the natural advance below, not playTrack's own emission.
    nowPlaying.clear();

    // Drive the render side to the end of the current track. Both tracks are
    // the same lossless FLAC, so the engine splices into the prepared successor
    // and reports the advance with the caller-supplied item id; the player
    // marshals it onto the executor, which drain() runs on this thread.
    auto buffer = std::array<std::byte, 4096>{};

    for (std::int32_t i = 0; i < 100000 && nowPlaying.empty(); ++i)
    {
      fixture.renderTarget->renderPcm(buffer);
      fixture.executor.drain();
    }

    REQUIRE_FALSE(nowPlaying.empty());

    // Item-id match: the prepared request is committed as now-playing, exactly
    // once, without an idle in between (idle would send the queue down the
    // explicit-restart fallback).
    REQUIRE(nowPlaying.size() == 1);
    CHECK(nowPlaying[0].trackId == nextTrack);
    CHECK(nowPlaying[0].sourceListId == ListId{7});
    CHECK(fixture.playbackService.state().trackId == nextTrack);
    CHECK(fixture.playbackService.state().trackTitle == "Prepared Track");
    CHECK(idleCount == 0);
  }

  TEST_CASE("PlaybackService playback - final seek before advanced callback keeps prepared metadata",
            "[runtime][unit][playback][gapless]")
  {
    auto fixture = PlaybackFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const currentTrack = fixture.testLib.addTrack({.title = "Current Track", .uri = fixturePath});
    auto const nextTrack = fixture.testLib.addTrack({.title = "Prepared Track", .uri = fixturePath});

    auto nowPlaying = std::vector<PlaybackService::NowPlayingChanged>{};
    auto nowPlayingSub = fixture.playbackService.onNowPlayingChanged([&](PlaybackService::NowPlayingChanged const& ev)
                                                                     { nowPlaying.push_back(ev); });

    REQUIRE(fixture.playbackService.playTrack(currentTrack, ListId{7}));
    REQUIRE(fixture.playbackService.prepareNext(nextTrack, ListId{7}));
    REQUIRE(fixture.renderTarget != nullptr);
    nowPlaying.clear();

    auto buffer = std::array<std::byte, 4096>{};

    for (std::int32_t i = 0; i < 100000 && fixture.executor.queuedCount() == 0; ++i)
    {
      fixture.renderTarget->renderPcm(buffer);
    }

    REQUIRE(fixture.executor.queuedCount() > 0);

    fixture.playbackService.seek(std::chrono::milliseconds{0}, PlaybackService::SeekMode::Final);
    fixture.executor.drain();

    REQUIRE(nowPlaying.size() == 1);
    CHECK(nowPlaying[0].trackId == nextTrack);
    CHECK(nowPlaying[0].sourceListId == ListId{7});
    CHECK(fixture.playbackService.state().trackId == nextTrack);
    CHECK(fixture.playbackService.state().trackTitle == "Prepared Track");
  }
} // namespace ao::rt::test
