// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/runtime/PlaybackServiceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/Transport.h>
#include <ao/rt/PlaybackQueueService.h>
#include <ao/rt/PlaybackState.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/uimodel/playback/command/PlaybackCommandSurface.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <vector>

namespace ao::uimodel::test
{
  using namespace ao::rt;
  using namespace ao::rt::test;

  TEST_CASE("PlaybackCommandSurface - executes transport policy", "[uimodel][unit][playback][command]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const trackId = fixture.libraryFixture.addTrack({.title = "Command Track", .uri = fixturePath});
    auto queue = PlaybackQueueService{fixture.executor, fixture.playbackService, fixture.notificationService};

    std::int32_t playSelectionCount = 0;
    auto commands =
      PlaybackCommandSurface{fixture.playbackService, queue, [&playSelectionCount] { ++playSelectionCount; }};

    SECTION("Play uses selection when idle without a current track")
    {
      commands.execute(PlaybackCommand::Play);

      CHECK(playSelectionCount == 1);
      CHECK(fixture.playbackService.state().transport == audio::Transport::Idle);
    }

    SECTION("PlayPause toggles between playing and paused")
    {
      REQUIRE(fixture.playbackService.playTrack(trackId, ListId{5}));

      commands.execute(PlaybackCommand::PlayPause);

      CHECK(fixture.playbackService.state().transport == audio::Transport::Paused);

      commands.execute(PlaybackCommand::PlayPause);

      CHECK(fixture.playbackService.state().transport == audio::Transport::Playing);
    }

    SECTION("Stop is enabled only outside idle")
    {
      CHECK_FALSE(commands.isEnabled(PlaybackCommand::Stop));

      REQUIRE(fixture.playbackService.playTrack(trackId, ListId{5}));

      CHECK(commands.isEnabled(PlaybackCommand::Stop));

      commands.execute(PlaybackCommand::Stop);

      CHECK(fixture.playbackService.state().transport == audio::Transport::Idle);
      CHECK_FALSE(commands.isEnabled(PlaybackCommand::Stop));
    }
  }

  TEST_CASE("PlaybackCommandSurface - owns availability and queue command policy",
            "[uimodel][unit][playback-command][queue]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const firstTrack = fixture.libraryFixture.addTrack({.title = "Queue First", .uri = fixturePath});
    auto const secondTrack = fixture.libraryFixture.addTrack({.title = "Queue Second", .uri = fixturePath});
    auto queue = PlaybackQueueService{fixture.executor, fixture.playbackService, fixture.notificationService};
    auto commands = PlaybackCommandSurface{fixture.playbackService, queue, [] {}};

    SECTION("Next and Previous enablement follows real queue targets")
    {
      REQUIRE(queue.playQueue({firstTrack, secondTrack}, firstTrack, ListId{9}));

      CHECK(commands.isEnabled(PlaybackCommand::Next));
      CHECK_FALSE(commands.isEnabled(PlaybackCommand::Previous));

      commands.execute(PlaybackCommand::Next);

      REQUIRE(queue.state().optCurrentIndex);
      CHECK(queue.state().trackIds[*queue.state().optCurrentIndex] == secondTrack);
      CHECK_FALSE(commands.isEnabled(PlaybackCommand::Next));
      CHECK(commands.isEnabled(PlaybackCommand::Previous));

      commands.execute(PlaybackCommand::Next);

      REQUIRE(queue.state().optCurrentIndex);
      CHECK(queue.state().trackIds[*queue.state().optCurrentIndex] == secondTrack);
      CHECK(fixture.playbackService.state().transport == audio::Transport::Playing);

      commands.execute(PlaybackCommand::Previous);

      REQUIRE(queue.state().optCurrentIndex);
      CHECK(queue.state().trackIds[*queue.state().optCurrentIndex] == firstTrack);
    }

    SECTION("Shuffle and repeat commands write through playback and re-prepare the queue")
    {
      REQUIRE(queue.playQueue({firstTrack, secondTrack}, secondTrack, ListId{9}));
      REQUIRE_FALSE(queue.hasNext());
      REQUIRE_FALSE(queue.state().optPendingNextIndex);

      commands.execute(PlaybackCommand::CycleRepeat);

      CHECK(fixture.playbackService.state().mode.repeat == rt::RepeatMode::All);
      CHECK(queue.hasNext());
      REQUIRE(queue.state().optPendingNextIndex);
      CHECK(queue.state().trackIds[*queue.state().optPendingNextIndex] == firstTrack);

      commands.execute(PlaybackCommand::CycleRepeat);
      CHECK(fixture.playbackService.state().mode.repeat == rt::RepeatMode::One);

      commands.execute(PlaybackCommand::CycleRepeat);
      CHECK(fixture.playbackService.state().mode.repeat == rt::RepeatMode::Off);
      REQUIRE_FALSE(queue.hasNext());
      REQUIRE_FALSE(queue.state().optPendingNextIndex);

      commands.execute(PlaybackCommand::ToggleShuffle);

      CHECK(fixture.playbackService.state().mode.shuffle == rt::ShuffleMode::On);
      CHECK(queue.hasNext());
      REQUIRE(queue.state().optPendingNextIndex);
      CHECK(queue.state().trackIds[*queue.state().optPendingNextIndex] == firstTrack);
    }
  }

  TEST_CASE("PlaybackCommandSurface - separates UI enablement from protocol capability",
            "[uimodel][unit][playback][command]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const trackId = fixture.libraryFixture.addTrack({.title = "Capability Track", .uri = fixturePath});
    auto queue = PlaybackQueueService{fixture.executor, fixture.playbackService, fixture.notificationService};
    auto commands = PlaybackCommandSurface{fixture.playbackService, queue, [] {}};

    REQUIRE(fixture.playbackService.playTrack(trackId, ListId{5}));

    CHECK_FALSE(commands.isEnabled(PlaybackCommand::Play));
    CHECK(commands.isCapable(PlaybackCommand::Play));
    CHECK(commands.isCapable(PlaybackCommand::Pause));
  }

  TEST_CASE("PlaybackCommandSurface - emits availability when playback becomes ready",
            "[uimodel][unit][playback][command]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};
    auto queue = PlaybackQueueService{fixture.executor, fixture.playbackService, fixture.notificationService};
    auto commands = PlaybackCommandSurface{fixture.playbackService, queue, [] {}};

    std::int32_t playCount = 0;
    auto sub = commands.onAvailabilityChanged(PlaybackCommand::Play, [&playCount] { ++playCount; });

    CHECK_FALSE(commands.isEnabled(PlaybackCommand::Play));

    fixture.onDevicesChangedCb(fixture.status.devices);

    CHECK(commands.isEnabled(PlaybackCommand::Play));
    CHECK(playCount > 0);
  }

  TEST_CASE("PlaybackCommandSurface - emits one availability event for playback command inputs",
            "[uimodel][unit][playback][command]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const firstTrack = fixture.libraryFixture.addTrack({.title = "Event First", .uri = fixturePath});
    auto const secondTrack = fixture.libraryFixture.addTrack({.title = "Event Second", .uri = fixturePath});
    auto queue = PlaybackQueueService{fixture.executor, fixture.playbackService, fixture.notificationService};
    auto commands = PlaybackCommandSurface{fixture.playbackService, queue, [] {}};

    std::int32_t count = 0;
    auto sub = commands.onAvailabilityChanged([&count] { ++count; });

    REQUIRE(fixture.playbackService.playTrack(firstTrack, ListId{5}));
    CHECK(count > 0);

    auto const afterPlay = count;
    fixture.playbackService.setShuffleMode(rt::ShuffleMode::On);
    CHECK(count == afterPlay + 1);

    auto const afterMode = count;
    REQUIRE(fixture.playbackService.playTrack(secondTrack, ListId{5}));
    CHECK(count > afterMode);

    auto const afterNowPlaying = count;
    fixture.playbackService.seek(std::chrono::milliseconds{5}, PlaybackService::SeekMode::Preview);
    CHECK(count == afterNowPlaying);

    fixture.playbackService.seek(std::chrono::milliseconds{10}, PlaybackService::SeekMode::Final);
    CHECK(count == afterNowPlaying + 1);
  }
} // namespace ao::uimodel::test
