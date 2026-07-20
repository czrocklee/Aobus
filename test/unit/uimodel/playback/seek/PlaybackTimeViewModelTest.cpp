// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "runtime/playback/PlaybackTransport.h"
#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/Transport.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/PlaybackState.h>
#include <ao/uimodel/playback/seek/PlaybackTimeViewModel.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace ao::uimodel::test
{
  using namespace ao::rt::test;
  using namespace ao::rt;

  TEST_CASE("PlaybackTimeViewModel - initial view state", "[uimodel][unit][playback]")
  {
    auto fixture = ApplicationPlaybackFixture{};

    auto log = ao::test::RenderLog<PlaybackTimeViewState>{};
    auto const viewModel = PlaybackTimeViewModel{fixture.playback, [&log](auto const& view) { log.render(view); }};

    REQUIRE(!log.empty());
    CHECK(log.last().elapsed == std::chrono::milliseconds{0});
    CHECK(log.last().duration == std::chrono::milliseconds{0});

    log.clear();
    auto const revisionBefore = fixture.playback.snapshot().revision;
    fixture.commands().setShuffleMode(ShuffleMode::On);
    CHECK(fixture.playback.snapshot().revision > revisionBefore);
    CHECK(log.empty());
  }

  TEST_CASE("PlaybackTimeViewModel - seek updates render preview and final modes", "[uimodel][unit][playback]")
  {
    auto fixture = ApplicationPlaybackFixtureT<QueuedExecutor>{};
    auto& playback = fixture.playback;
    auto& playbackTransport = fixture.playbackTransport;
    fixture.addReadyProvider();
    REQUIRE(fixture.executor.drainUntil([&] { return playbackTransport.state().ready; }));

    auto log = ao::test::RenderLog<PlaybackTimeViewState>{};
    auto const viewModel = PlaybackTimeViewModel{playback, [&log](auto const& view) { log.render(view); }};

    auto const trackId = fixture.libraryFixture.addTrack({.title = "Seek Test", .artist = "Artist", .album = "Album"});
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto desc = PlaybackTransport::PlaybackRequest{
      .item = NowPlayingInfo{.trackId = trackId, .title = "Seek Test", .artist = "Artist"},
      .input = audio::PlaybackInput{.filePath = fixturePath, .duration = std::chrono::seconds{30}},
    };
    log.clear();
    REQUIRE(playbackTransport.play(desc, kInvalidListId));
    REQUIRE(
      fixture.executor.drainUntil([&] { return playbackTransport.state().transport == audio::Transport::Playing; }));
    fixture.executor.drain();
    auto const expectedDuration = playbackTransport.state().duration;
    REQUIRE(expectedDuration > std::chrono::milliseconds{0});
    // Playback coalesces the preparing and started transitions into one
    // coherent snapshot, so the view model settles on the playing state rather
    // than observing each intermediate transport signal.
    REQUIRE(!log.empty());
    CHECK(log.last().isPlaying);

    log.clear();
    playbackTransport.seek(std::chrono::milliseconds{500}, PlaybackTransport::SeekMode::Final);
    fixture.executor.drain();
    REQUIRE(!log.empty());
    CHECK(log.last().duration == expectedDuration);
    CHECK(log.last().elapsed == std::chrono::milliseconds{500});
    CHECK(log.last().isPlaying == true);
    CHECK(log.last().isPreviewing == false);
    CHECK(log.last().immediateUpdate == true);
    CHECK(playbackTransport.state().elapsed == std::chrono::milliseconds{500});

    log.clear();
    playbackTransport.seek(std::chrono::milliseconds{250}, PlaybackTransport::SeekMode::Preview);
    fixture.executor.drain();
    REQUIRE(!log.empty());
    CHECK(log.last().duration == expectedDuration);
    CHECK(log.last().elapsed == std::chrono::milliseconds{250});
    CHECK(log.last().isPlaying == true);
    CHECK(log.last().isPreviewing == true);
    CHECK(log.last().immediateUpdate == false);
    CHECK(playbackTransport.state().elapsed == std::chrono::milliseconds{500});
  }

  TEST_CASE("PlaybackTimeViewModel - formats display text for each label mode", "[uimodel][unit][playback]")
  {
    SECTION("template text reserves the widest idle label")
    {
      CHECK(PlaybackTimeViewModel::describeTimeTemplate(PlaybackTimeMode::Elapsed) == "00:00");
      CHECK(PlaybackTimeViewModel::describeTimeTemplate(PlaybackTimeMode::Duration) == "00:00");
      CHECK(PlaybackTimeViewModel::describeTimeTemplate(PlaybackTimeMode::Default) == "00:00 / 00:00");
    }

    SECTION("playback time text matches selected label mode")
    {
      auto const elapsed = std::chrono::seconds{65};
      auto const duration = std::chrono::hours{1} + std::chrono::minutes{1} + std::chrono::seconds{1};

      CHECK(PlaybackTimeViewModel::formatPlaybackTime(PlaybackTimeMode::Elapsed, elapsed, duration) == "1:05");
      CHECK(PlaybackTimeViewModel::formatPlaybackTime(PlaybackTimeMode::Duration, elapsed, duration) == "61:01");
      CHECK(PlaybackTimeViewModel::formatPlaybackTime(PlaybackTimeMode::Default, elapsed, duration) == "1:05 / 61:01");
    }
  }
} // namespace ao::uimodel::test
