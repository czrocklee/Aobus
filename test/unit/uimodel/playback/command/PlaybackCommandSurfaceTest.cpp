// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/runtime/PlaybackSequenceUiTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/Transport.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/PlaybackSequenceService.h>
#include <ao/rt/PlaybackState.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/uimodel/playback/command/PlaybackCommandSurface.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>

namespace ao::uimodel::test
{
  using namespace ao::rt;
  using namespace ao::rt::test;

  TEST_CASE("PlaybackCommandSurface - executes transport policy", "[uimodel][unit][playback][command]")
  {
    auto fixture = PlaybackSequenceUiFixture{};
    fixture.makePlaybackReady();
    auto const trackId = fixture.addPlayableTrack("Command Track");
    auto& playback = fixture.runtime.playback();
    auto& sequence = fixture.runtime.playbackSequence();

    std::int32_t playSelectionCount = 0;
    auto commands = PlaybackCommandSurface{playback, sequence, [&playSelectionCount] { ++playSelectionCount; }};

    SECTION("Play uses selection when idle without a current track")
    {
      commands.execute(PlaybackCommand::Play);

      CHECK(playSelectionCount == 1);
      CHECK(playback.state().transport == audio::Transport::Idle);
    }

    SECTION("PlayPause toggles between playing and paused")
    {
      REQUIRE(playback.playTrack(trackId, ListId{5}));

      commands.execute(PlaybackCommand::PlayPause);

      CHECK(playback.state().transport == audio::Transport::Paused);

      commands.execute(PlaybackCommand::PlayPause);

      CHECK(playback.state().transport == audio::Transport::Playing);
    }

    SECTION("Stop is enabled only outside idle")
    {
      CHECK_FALSE(commands.isEnabled(PlaybackCommand::Stop));

      REQUIRE(playback.playTrack(trackId, ListId{5}));

      CHECK(commands.isEnabled(PlaybackCommand::Stop));

      commands.execute(PlaybackCommand::Stop);

      CHECK(playback.state().transport == audio::Transport::Idle);
      CHECK_FALSE(commands.isEnabled(PlaybackCommand::Stop));
    }
  }

  TEST_CASE("PlaybackCommandSurface - owns availability and live-sequence command policy",
            "[uimodel][unit][playback-command][sequence]")
  {
    auto fixture = PlaybackSequenceUiFixture{};
    fixture.makePlaybackReady();
    auto const firstTrack = fixture.addPlayableTrack("Sequence First");
    auto const secondTrack = fixture.addPlayableTrack("Sequence Second");
    auto& playback = fixture.runtime.playback();
    auto& sequence = fixture.runtime.playbackSequence();
    auto commands = PlaybackCommandSurface{playback, sequence, [] {}};

    SECTION("Next and Previous enablement follows live sequence targets")
    {
      REQUIRE(fixture.playFromView(firstTrack));

      CHECK(commands.isEnabled(PlaybackCommand::Next));
      CHECK_FALSE(commands.isEnabled(PlaybackCommand::Previous));

      commands.execute(PlaybackCommand::Next);

      CHECK(sequence.state().currentTrackId == secondTrack);
      CHECK_FALSE(commands.isEnabled(PlaybackCommand::Next));
      CHECK(commands.isEnabled(PlaybackCommand::Previous));

      commands.execute(PlaybackCommand::Next);

      CHECK(sequence.state().currentTrackId == secondTrack);
      CHECK(playback.state().transport == audio::Transport::Playing);

      commands.execute(PlaybackCommand::Previous);

      CHECK(sequence.state().currentTrackId == firstTrack);
    }

    SECTION("Shuffle and repeat commands write through the live sequence")
    {
      REQUIRE(fixture.playFromView(secondTrack));
      REQUIRE_FALSE(sequence.state().hasNext);
      REQUIRE_FALSE(sequence.state().optResolvedSuccessor);

      commands.execute(PlaybackCommand::CycleRepeat);

      CHECK(sequence.state().repeat == RepeatMode::All);
      CHECK(sequence.state().hasNext);
      CHECK(sequence.state().optResolvedSuccessor == firstTrack);

      commands.execute(PlaybackCommand::CycleRepeat);
      CHECK(sequence.state().repeat == RepeatMode::One);

      commands.execute(PlaybackCommand::CycleRepeat);
      CHECK(sequence.state().repeat == RepeatMode::Off);
      CHECK_FALSE(sequence.state().hasNext);
      CHECK_FALSE(sequence.state().optResolvedSuccessor);

      commands.execute(PlaybackCommand::ToggleShuffle);

      CHECK(sequence.state().shuffle == ShuffleMode::On);
      CHECK(sequence.state().hasNext);
      CHECK(sequence.state().optResolvedSuccessor == firstTrack);
    }
  }

  TEST_CASE("PlaybackCommandSurface - separates UI enablement from protocol capability",
            "[uimodel][unit][playback][command]")
  {
    auto fixture = PlaybackSequenceUiFixture{};
    fixture.makePlaybackReady();
    auto const trackId = fixture.addPlayableTrack("Capability Track");
    auto& playback = fixture.runtime.playback();
    auto& sequence = fixture.runtime.playbackSequence();
    auto commands = PlaybackCommandSurface{playback, sequence, [] {}};

    REQUIRE(playback.playTrack(trackId, ListId{5}));

    CHECK_FALSE(commands.isEnabled(PlaybackCommand::Play));
    CHECK(commands.isCapable(PlaybackCommand::Play));
    CHECK(commands.isCapable(PlaybackCommand::Pause));
  }

  TEST_CASE("PlaybackCommandSurface - emits availability when playback becomes ready",
            "[uimodel][unit][playback][command]")
  {
    auto fixture = PlaybackSequenceUiFixture{};
    auto& playback = fixture.runtime.playback();
    auto& sequence = fixture.runtime.playbackSequence();
    auto commands = PlaybackCommandSurface{playback, sequence, [] {}};

    std::int32_t playCount = 0;
    auto sub = commands.onAvailabilityChanged(PlaybackCommand::Play, [&playCount] { ++playCount; });

    CHECK_FALSE(commands.isEnabled(PlaybackCommand::Play));

    fixture.makePlaybackReady();

    CHECK(commands.isEnabled(PlaybackCommand::Play));
    CHECK(playCount > 0);
  }

  TEST_CASE("PlaybackCommandSurface - emits one availability event for playback command inputs",
            "[uimodel][unit][playback][command]")
  {
    auto fixture = PlaybackSequenceUiFixture{};
    fixture.makePlaybackReady();
    auto const firstTrack = fixture.addPlayableTrack("Event First");
    auto const secondTrack = fixture.addPlayableTrack("Event Second");
    auto& playback = fixture.runtime.playback();
    auto& sequence = fixture.runtime.playbackSequence();
    auto commands = PlaybackCommandSurface{playback, sequence, [] {}};

    std::int32_t count = 0;
    auto sub = commands.onAvailabilityChanged([&count] { ++count; });

    REQUIRE(playback.playTrack(firstTrack, ListId{5}));
    CHECK(count > 0);

    auto const afterPlay = count;
    sequence.setShuffleMode(ShuffleMode::On);
    CHECK(count == afterPlay + 1);

    auto const afterMode = count;
    REQUIRE(playback.playTrack(secondTrack, ListId{5}));
    CHECK(count > afterMode);

    auto const afterNowPlaying = count;
    playback.seek(std::chrono::milliseconds{5}, PlaybackService::SeekMode::Preview);
    CHECK(count == afterNowPlaying);

    playback.seek(std::chrono::milliseconds{10}, PlaybackService::SeekMode::Final);
    CHECK(count == afterNowPlaying + 1);
  }
} // namespace ao::uimodel::test
