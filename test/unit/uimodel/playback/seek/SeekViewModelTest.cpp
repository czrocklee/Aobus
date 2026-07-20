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
#include <ao/uimodel/playback/seek/SeekViewModel.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace ao::uimodel::test
{
  using namespace ao::rt::test;
  using namespace ao::rt;

  TEST_CASE("SeekViewModel - reactive updates", "[uimodel][unit][playback]")
  {
    auto fixture = ApplicationPlaybackFixtureT<QueuedExecutor>{};
    auto& playback = fixture.playback;
    auto& playbackTransport = fixture.playbackTransport;
    fixture.addReadyProvider();
    REQUIRE(fixture.executor.drainUntil([&] { return playbackTransport.state().ready; }));

    auto log = ao::test::RenderLog<SeekViewState>{};
    auto viewModel = SeekViewModel{playback, [&log](auto const& state) { log.render(state); }};

    SECTION("Initial state is insensitive when idle")
    {
      REQUIRE(!log.empty());
      CHECK(log.last().enabled == false);
      CHECK(log.last().duration == std::chrono::milliseconds{0});
      CHECK(log.last().elapsed == std::chrono::milliseconds{0});

      log.clear();
      auto const revisionBefore = fixture.playback.snapshot().revision;
      fixture.commands().setShuffleMode(ShuffleMode::On);
      CHECK(fixture.playback.snapshot().revision > revisionBefore);
      CHECK(log.empty());
    }

    SECTION("refresh with override")
    {
      log.clear();
      viewModel.refresh(true, std::chrono::seconds{5});
      REQUIRE(!log.empty());
      CHECK(log.last().elapsed == std::chrono::seconds{5});
      CHECK(log.last().immediateUpdate == true);
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
      CHECK(log.last().enabled == true);
      CHECK(log.last().immediateUpdate == false);
      CHECK(playbackTransport.state().elapsed == std::chrono::milliseconds{0});

      viewModel.seekFinal(std::chrono::milliseconds{500});
      fixture.executor.drain();

      CHECK(log.last().duration == expectedDuration);
      CHECK(log.last().elapsed == std::chrono::milliseconds{500});
      CHECK(log.last().isPlaying == true);
      CHECK(log.last().enabled == true);
      CHECK(log.last().immediateUpdate == true);
      CHECK(playbackTransport.state().duration == expectedDuration);

      auto seekEvents = std::vector<PlaybackTransport::SeekUpdate>{};
      auto seekSub = playbackTransport.onSeekUpdate([&seekEvents](PlaybackTransport::SeekUpdate const& event)
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
      auto seekSub = playbackTransport.onSeekUpdate([&seekEvents](PlaybackTransport::SeekUpdate const& event)
                                                    { seekEvents.push_back(event); });

      viewModel.seekBy(std::chrono::seconds{5});

      CHECK(seekEvents.empty());
    }
  }
} // namespace ao::uimodel::test
