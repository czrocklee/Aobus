// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "runtime/playback/PlaybackTransport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/runtime/ApplicationPlaybackTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/Transport.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/PlaybackState.h>
#include <ao/uimodel/playback/seek/PlaybackPosition.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <vector>

namespace ao::uimodel::test
{
  using namespace ao::rt::test;
  using namespace ao::rt;

  TEST_CASE("PlaybackPositionViewModel - reactive updates", "[uimodel][unit][playback]")
  {
    auto fixture = ApplicationPlaybackFixtureT<QueuedExecutor>{};
    auto& playback = fixture.playback;
    auto& playbackTransport = fixture.playbackTransport;
    fixture.addReadyProvider();
    REQUIRE(fixture.executor.drainUntil([&] { return playbackTransport.state().ready; }));

    auto log = ao::test::RenderLog<PlaybackPositionViewState>{};
    auto viewModel = PlaybackPositionViewModel{playback, [&log](auto const& state) { log.render(state); }};

    SECTION("Initial state is insensitive when idle")
    {
      REQUIRE(!log.empty());
      CHECK(log.last().seekable == false);
      CHECK(log.last().duration == std::chrono::milliseconds{0});
      CHECK(log.last().elapsed == std::chrono::milliseconds{0});

      log.clear();
      fixture.commands().setShuffleMode(ShuffleMode::On);
      CHECK(fixture.playback.snapshot().succession.shuffle == ShuffleMode::On);
      CHECK(log.empty());
    }

    SECTION("seek commands")
    {
      auto const trackId =
        fixture.libraryFixture.addTrack({.title = "Seek Test", .artist = "Artist", .album = "Album"});
      auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
      auto const desc = PlaybackTransport::PlaybackRequest{
        .item = NowPlayingInfo{.trackId = trackId, .title = "Seek Test", .artist = "Artist"},
        .input = audio::PlaybackInput{.filePath = fixturePath, .duration = std::chrono::seconds{1}},
      };
      REQUIRE(playbackTransport.play(desc, kInvalidListId));
      REQUIRE(
        fixture.executor.drainUntil([&] { return playbackTransport.state().transport == audio::Transport::Playing; }));
      fixture.executor.drain();
      auto const expectedDuration = playbackTransport.state().duration;
      REQUIRE(expectedDuration > std::chrono::milliseconds{0});

      log.clear();
      viewModel.seekPreview(std::chrono::milliseconds{250});

      REQUIRE(!log.empty());
      CHECK(log.last().duration == expectedDuration);
      CHECK(log.last().elapsed == std::chrono::milliseconds{250});
      CHECK(log.last().isPlaying == true);
      CHECK(log.last().seekable == true);
      CHECK(log.last().immediateUpdate == false);
      CHECK(playbackTransport.state().elapsed == std::chrono::milliseconds{0});

      viewModel.seekFinal(std::chrono::milliseconds{500});
      fixture.executor.drain();

      CHECK(log.last().duration == expectedDuration);
      CHECK(log.last().elapsed == std::chrono::milliseconds{500});
      CHECK(log.last().isPlaying == true);
      CHECK(log.last().seekable == true);
      CHECK(log.last().immediateUpdate == true);
      CHECK(playbackTransport.state().duration == expectedDuration);

      auto seekEvents = std::vector<PlaybackTransport::SeekUpdate>{};
      auto seekSub = playbackTransport.onSeekUpdate([&seekEvents](PlaybackTransport::SeekUpdate const& event) noexcept
                                                    { seekEvents.push_back(event); });

      viewModel.seekBy(std::chrono::milliseconds{200});
      REQUIRE(seekEvents.size() == 1);
      CHECK(seekEvents.back().mode == PlaybackTransport::SeekMode::Final);
      CHECK(seekEvents.back().elapsed == std::chrono::milliseconds{700});

      viewModel.seekBy(-std::chrono::seconds{1});
      REQUIRE(seekEvents.size() == 2);
      CHECK(seekEvents.back().mode == PlaybackTransport::SeekMode::Final);
      CHECK(seekEvents.back().elapsed == std::chrono::milliseconds{0});

      viewModel.seekBy(expectedDuration + std::chrono::seconds{1});
      REQUIRE(seekEvents.size() == 3);
      CHECK(seekEvents.back().elapsed == expectedDuration);

      viewModel.seekBy(std::chrono::milliseconds::max());
      REQUIRE(seekEvents.size() == 4);
      CHECK(seekEvents.back().elapsed == expectedDuration);

      viewModel.seekBy(std::chrono::milliseconds::min());
      REQUIRE(seekEvents.size() == 5);
      CHECK(seekEvents.back().elapsed == std::chrono::milliseconds{0});
    }

    SECTION("relative seek is unavailable without a known duration")
    {
      auto seekEvents = std::vector<PlaybackTransport::SeekUpdate>{};
      auto seekSub = playbackTransport.onSeekUpdate([&seekEvents](PlaybackTransport::SeekUpdate const& event) noexcept
                                                    { seekEvents.push_back(event); });

      viewModel.seekBy(std::chrono::seconds{5});

      CHECK(seekEvents.empty());
    }
  }

  TEST_CASE("PlaybackPositionViewModel - initial view state", "[uimodel][unit][playback]")
  {
    auto fixture = ApplicationPlaybackFixture{};

    auto log = ao::test::RenderLog<PlaybackPositionViewState>{};
    auto const viewModel = PlaybackPositionViewModel{fixture.playback, [&log](auto const& view) { log.render(view); }};

    REQUIRE(!log.empty());
    CHECK(log.last().elapsed == std::chrono::milliseconds{0});
    CHECK(log.last().duration == std::chrono::milliseconds{0});

    log.clear();
    fixture.commands().setShuffleMode(ShuffleMode::On);
    CHECK(fixture.playback.snapshot().succession.shuffle == ShuffleMode::On);
    CHECK(log.empty());
  }

  TEST_CASE("PlaybackPositionViewModel - transport seeks render preview and final modes", "[uimodel][unit][playback]")
  {
    auto fixture = ApplicationPlaybackFixtureT<QueuedExecutor>{};
    auto& playback = fixture.playback;
    auto& playbackTransport = fixture.playbackTransport;
    fixture.addReadyProvider();
    REQUIRE(fixture.executor.drainUntil([&] { return playbackTransport.state().ready; }));

    auto log = ao::test::RenderLog<PlaybackPositionViewState>{};
    auto const viewModel = PlaybackPositionViewModel{playback, [&log](auto const& view) { log.render(view); }};

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
} // namespace ao::uimodel::test
