// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/runtime/PlaybackUiTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/Transport.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackService.h>
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
    auto fixture = PlaybackUiFixture{};
    fixture.makePlaybackReady();
    auto const trackId = fixture.addPlayableTrack("Command Track");
    auto& playback = fixture.runtime.playback();

    std::int32_t playSelectionCount = 0;
    auto commands = PlaybackCommandSurface{playback, [&playSelectionCount] { ++playSelectionCount; }};

    SECTION("Play uses selection when idle without a current track")
    {
      CHECK(commands.execute(PlaybackCommand::Play));

      CHECK(playSelectionCount == 1);
      CHECK(playback.snapshot().transport.transport == audio::Transport::Idle);
    }

    SECTION("PlayPause toggles between playing and paused")
    {
      REQUIRE(fixture.playFromView(trackId));

      CHECK(commands.execute(PlaybackCommand::PlayPause));

      CHECK(playback.snapshot().transport.transport == audio::Transport::Paused);

      CHECK(commands.execute(PlaybackCommand::PlayPause));

      CHECK(playback.snapshot().transport.transport == audio::Transport::Playing);
    }

    SECTION("PlayPause pauses while a new output selection is pending")
    {
      REQUIRE(fixture.playFromView(trackId));
      auto const selected = playback.snapshot().transport.output.selectedDevice;

      playback.commands().setOutputDevice(selected.backendId, audio::DeviceId{"pending-device"}, selected.profileId);

      REQUIRE_FALSE(playback.snapshot().transport.ready);
      REQUIRE(playback.snapshot().transport.transport == audio::Transport::Playing);
      CHECK(commands.isEnabled(PlaybackCommand::Pause));
      CHECK(commands.isEnabled(PlaybackCommand::PlayPause));
      CHECK(commands.execute(PlaybackCommand::PlayPause));
      CHECK(playback.snapshot().transport.transport == audio::Transport::Paused);
    }

    SECTION("PlayPause resumes a restored sequence track")
    {
      REQUIRE(fixture.playFromView(trackId));
      REQUIRE(fixture.runtime.savePlaybackSession());
      playback.commands().stop();
      auto const restored = fixture.runtime.restorePlaybackSession();

      REQUIRE(restored);
      REQUIRE(restored->restored);
      REQUIRE(playback.snapshot().transport.transport == audio::Transport::Idle);
      REQUIRE(playback.snapshot().transport.nowPlaying.trackId == trackId);
      REQUIRE(playback.snapshot().succession.currentTrackId == trackId);

      CHECK(commands.execute(PlaybackCommand::PlayPause));
      CHECK(playback.snapshot().transport.transport == audio::Transport::Playing);
      CHECK(playSelectionCount == 0);
    }

    SECTION("PlayPause starts the selection for an idle track outside the sequence")
    {
      REQUIRE(fixture.playFromView(trackId));
      REQUIRE(fixture.runtime.savePlaybackSession());
      playback.commands().stop();
      auto const restored = fixture.runtime.restorePlaybackSession();

      REQUIRE(restored);
      REQUIRE(restored->restored);
      playback.commands().clearSequence();
      REQUIRE(playback.snapshot().transport.transport == audio::Transport::Idle);
      REQUIRE(playback.snapshot().transport.nowPlaying.trackId == trackId);
      REQUIRE(playback.snapshot().succession.currentTrackId == kInvalidTrackId);

      CHECK(commands.execute(PlaybackCommand::PlayPause));
      CHECK(playback.snapshot().transport.transport == audio::Transport::Idle);
      CHECK(playSelectionCount == 1);
    }

    SECTION("Stop is enabled only outside idle")
    {
      CHECK_FALSE(commands.isEnabled(PlaybackCommand::Stop));
      CHECK_FALSE(commands.execute(PlaybackCommand::Stop));

      REQUIRE(fixture.playFromView(trackId));

      CHECK(commands.isEnabled(PlaybackCommand::Stop));

      CHECK(commands.execute(PlaybackCommand::Stop));

      CHECK(playback.snapshot().transport.transport == audio::Transport::Idle);
      CHECK_FALSE(commands.isEnabled(PlaybackCommand::Stop));
    }
  }

  TEST_CASE("PlaybackCommandSurface - owns availability and live-sequence command policy",
            "[uimodel][unit][playback-command][sequence]")
  {
    auto fixture = PlaybackUiFixture{};
    fixture.makePlaybackReady();
    auto const firstTrack = fixture.addPlayableTrack("Sequence First");
    auto const secondTrack = fixture.addPlayableTrack("Sequence Second");
    auto& playback = fixture.runtime.playback();
    auto commands = PlaybackCommandSurface{playback, [] {}};

    SECTION("Next and Previous enablement follows live sequence targets")
    {
      REQUIRE(fixture.playFromView(firstTrack));

      CHECK(commands.isEnabled(PlaybackCommand::Next));
      CHECK_FALSE(commands.isEnabled(PlaybackCommand::Previous));

      commands.execute(PlaybackCommand::Next);

      CHECK(playback.snapshot().succession.currentTrackId == secondTrack);
      CHECK_FALSE(commands.isEnabled(PlaybackCommand::Next));
      CHECK(commands.isEnabled(PlaybackCommand::Previous));

      commands.execute(PlaybackCommand::Next);

      CHECK(playback.snapshot().succession.currentTrackId == secondTrack);
      CHECK(playback.snapshot().transport.transport == audio::Transport::Playing);

      commands.execute(PlaybackCommand::Previous);

      CHECK(playback.snapshot().succession.currentTrackId == firstTrack);
    }

    SECTION("Shuffle and repeat commands write through the live sequence")
    {
      REQUIRE(fixture.playFromView(secondTrack));
      REQUIRE_FALSE(playback.snapshot().succession.hasNext);

      commands.execute(PlaybackCommand::CycleRepeat);

      CHECK(playback.snapshot().succession.repeat == RepeatMode::All);
      CHECK(playback.snapshot().succession.hasNext);

      commands.execute(PlaybackCommand::CycleRepeat);
      CHECK(playback.snapshot().succession.repeat == RepeatMode::One);

      commands.execute(PlaybackCommand::CycleRepeat);
      CHECK(playback.snapshot().succession.repeat == RepeatMode::Off);
      CHECK_FALSE(playback.snapshot().succession.hasNext);

      commands.execute(PlaybackCommand::ToggleShuffle);

      CHECK(playback.snapshot().succession.shuffle == ShuffleMode::On);
      CHECK(playback.snapshot().succession.hasNext);
    }
  }

  TEST_CASE("PlaybackCommandSurface - separates UI enablement from protocol capability",
            "[uimodel][unit][playback][command]")
  {
    auto fixture = PlaybackUiFixture{};
    fixture.makePlaybackReady();
    auto const trackId = fixture.addPlayableTrack("Capability Track");
    auto commands = PlaybackCommandSurface{fixture.runtime.playback(), [] {}};

    REQUIRE(fixture.playFromView(trackId));

    CHECK_FALSE(commands.isEnabled(PlaybackCommand::Play));
    CHECK(commands.isCapable(PlaybackCommand::Play));
    CHECK(commands.isCapable(PlaybackCommand::Pause));
  }

  TEST_CASE("PlaybackCommandSurface - emits availability when playback becomes ready",
            "[uimodel][unit][playback][command]")
  {
    auto fixture = PlaybackUiFixture{};
    std::int32_t playSelectionCount = 0;
    auto commands = PlaybackCommandSurface{fixture.runtime.playback(), [&playSelectionCount] { ++playSelectionCount; }};

    std::int32_t playCount = 0;
    auto sub = commands.onAvailabilityChanged(PlaybackCommand::Play, [&playCount] { ++playCount; });

    CHECK_FALSE(commands.isEnabled(PlaybackCommand::Play));
    CHECK_FALSE(commands.isEnabled(PlaybackCommand::PlayPause));
    CHECK_FALSE(commands.execute(PlaybackCommand::PlayPause));
    CHECK(playSelectionCount == 0);

    fixture.makePlaybackReady();

    CHECK(commands.isEnabled(PlaybackCommand::Play));
    CHECK(commands.isEnabled(PlaybackCommand::PlayPause));
    CHECK(playCount > 0);
  }

  TEST_CASE("PlaybackCommandSurface - emits one availability event for playback command inputs",
            "[uimodel][unit][playback][command]")
  {
    auto fixture = PlaybackUiFixture{};
    fixture.makePlaybackReady();
    auto const firstTrack = fixture.addPlayableTrack("Event First");
    auto const secondTrack = fixture.addPlayableTrack("Event Second");
    auto& playback = fixture.runtime.playback();
    auto commands = PlaybackCommandSurface{playback, [] {}};

    std::int32_t count = 0;
    auto sub = commands.onAvailabilityChanged([&count] { ++count; });

    REQUIRE(fixture.playFromView(firstTrack));
    CHECK(count > 0);

    auto const afterPlay = count;
    playback.commands().setShuffleMode(ShuffleMode::On);
    CHECK(count == afterPlay + 1);

    auto const afterMode = count;
    REQUIRE(fixture.playFromView(secondTrack));
    CHECK(count > afterMode);

    auto const afterNowPlaying = count;
    playback.commands().seek(std::chrono::milliseconds{5}, PlaybackSeekMode::Preview);
    CHECK(count == afterNowPlaying);

    playback.commands().seek(std::chrono::milliseconds{10}, PlaybackSeekMode::Final);
    CHECK(count == afterNowPlaying);
  }
} // namespace ao::uimodel::test
