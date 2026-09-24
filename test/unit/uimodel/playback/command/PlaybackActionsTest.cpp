// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/playback/command/PlaybackActions.h>

#include "test/unit/runtime/PlaybackUiTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/Transport.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace ao::uimodel::test
{
  using namespace ao::rt;
  using namespace ao::rt::test;

  namespace
  {
    constexpr std::size_t commandIndex(PlaybackCommand const command) noexcept
    {
      return static_cast<std::size_t>(command);
    }

    using CommandPolicy = std::array<bool, 8>;

    void checkCommandPolicy(PlaybackActions const& actions,
                            CommandPolicy const& expectedEnabled,
                            CommandPolicy const& expectedCapable)
    {
      for (auto const command : playbackCommands())
      {
        CAPTURE(playbackCommandId(command));
        CHECK(actions.isEnabled(command) == expectedEnabled[commandIndex(command)]);
        CHECK(actions.isCapable(command) == expectedCapable[commandIndex(command)]);
      }
    }

    struct AvailabilityLog final
    {
      explicit AvailabilityLog(PlaybackActions& actions)
      {
        commandSubscriptions.reserve(playbackCommands().size());

        for (auto const command : playbackCommands())
        {
          commandSubscriptions.push_back(actions.onAvailabilityChanged(
            command, [this, command] noexcept { ++commandCounts[commandIndex(command)]; }));
        }

        aggregateSubscription = actions.onAvailabilityChanged([this] noexcept { ++aggregateCount; });
      }

      void clear() noexcept
      {
        commandCounts.fill(0);
        aggregateCount = 0;
      }

      std::vector<PlaybackCommand> affectedCommands() const
      {
        auto affected = std::vector<PlaybackCommand>{};

        for (auto const command : playbackCommands())
        {
          if (commandCounts[commandIndex(command)] > 0)
          {
            affected.push_back(command);
          }
        }

        return affected;
      }

      std::int32_t count(PlaybackCommand const command) const noexcept { return commandCounts[commandIndex(command)]; }

      std::array<std::int32_t, 8> commandCounts{};
      std::int32_t aggregateCount = 0;
      std::vector<async::Subscription> commandSubscriptions;
      async::Subscription aggregateSubscription;
    };

    void checkAffected(AvailabilityLog const& log, std::initializer_list<PlaybackCommand> const expected)
    {
      CHECK(log.affectedCommands() == std::vector<PlaybackCommand>(expected));
    }
  } // namespace

  TEST_CASE("PlaybackActions - executes transport policy", "[uimodel][integration][playback][command]")
  {
    auto fixture = PlaybackUiFixture{};
    fixture.makePlaybackReady();
    auto const trackId = fixture.addPlayableTrack("Command Track");
    auto& playback = fixture.runtime().playback();

    std::int32_t playSelectionCount = 0;
    auto actions = PlaybackActions{playback, [&playSelectionCount] { ++playSelectionCount; }};

    SECTION("Play uses selection when idle without a current track")
    {
      CHECK(actions.tryExecute(PlaybackCommand::Play));

      CHECK(playSelectionCount == 1);
      CHECK(playback.snapshot().transport.transport == audio::Transport::Idle);
    }

    SECTION("PlayPause toggles between playing and paused")
    {
      REQUIRE(fixture.playFromView(trackId));

      CHECK(actions.tryExecute(PlaybackCommand::PlayPause));

      CHECK(playback.snapshot().transport.transport == audio::Transport::Paused);

      CHECK(actions.tryExecute(PlaybackCommand::PlayPause));

      CHECK(playback.snapshot().transport.transport == audio::Transport::Playing);
    }

    SECTION("PlayPause pauses while a new output selection is pending")
    {
      REQUIRE(fixture.playFromView(trackId));
      auto const selected = playback.snapshot().transport.output.selectedDevice;

      playback.commands().setOutputDevice(selected.backendId, audio::DeviceId{"pending-device"}, selected.profileId);

      REQUIRE_FALSE(playback.snapshot().transport.ready);
      REQUIRE(playback.snapshot().transport.transport == audio::Transport::Playing);
      CHECK(actions.isEnabled(PlaybackCommand::Pause));
      CHECK(actions.isEnabled(PlaybackCommand::PlayPause));
      CHECK(actions.tryExecute(PlaybackCommand::PlayPause));
      CHECK(playback.snapshot().transport.transport == audio::Transport::Paused);
    }

    SECTION("PlayPause resumes a restored sequence track")
    {
      REQUIRE(fixture.playFromView(trackId));
      REQUIRE(fixture.runtime().savePlaybackSession());
      playback.commands().stop();
      auto const restoredRes = fixture.runtime().restorePlaybackSession();

      REQUIRE(restoredRes);
      REQUIRE(restoredRes->restored);
      REQUIRE(playback.snapshot().transport.transport == audio::Transport::Idle);
      REQUIRE(playback.snapshot().transport.nowPlaying.trackId == trackId);
      REQUIRE(playback.snapshot().succession.currentTrackId == trackId);

      CHECK(actions.tryExecute(PlaybackCommand::PlayPause));
      CHECK(playback.snapshot().transport.transport == audio::Transport::Playing);
      CHECK(playSelectionCount == 0);
    }

    SECTION("PlayPause starts the selection for an idle track outside the sequence")
    {
      REQUIRE(fixture.playFromView(trackId));
      REQUIRE(fixture.runtime().savePlaybackSession());
      playback.commands().stop();
      auto const restoredRes = fixture.runtime().restorePlaybackSession();

      REQUIRE(restoredRes);
      REQUIRE(restoredRes->restored);
      playback.commands().clearSequence();
      REQUIRE(playback.snapshot().transport.transport == audio::Transport::Idle);
      REQUIRE(playback.snapshot().transport.nowPlaying.trackId == trackId);
      REQUIRE(playback.snapshot().succession.currentTrackId == kInvalidTrackId);

      CHECK(actions.tryExecute(PlaybackCommand::PlayPause));
      CHECK(playback.snapshot().transport.transport == audio::Transport::Idle);
      CHECK(playSelectionCount == 1);
    }

    SECTION("Stop is enabled only outside idle")
    {
      CHECK_FALSE(actions.isEnabled(PlaybackCommand::Stop));
      CHECK_FALSE(actions.tryExecute(PlaybackCommand::Stop));

      REQUIRE(fixture.playFromView(trackId));

      CHECK(actions.isEnabled(PlaybackCommand::Stop));

      CHECK(actions.tryExecute(PlaybackCommand::Stop));

      CHECK(playback.snapshot().transport.transport == audio::Transport::Idle);
      CHECK_FALSE(actions.isEnabled(PlaybackCommand::Stop));
    }
  }

  TEST_CASE("PlaybackActions - owns availability and live-sequence command policy",
            "[uimodel][integration][playback][command][sequence]")
  {
    auto fixture = PlaybackUiFixture{};
    fixture.makePlaybackReady();
    auto const firstTrack = fixture.addPlayableTrack("Sequence First");
    auto const secondTrack = fixture.addPlayableTrack("Sequence Second");
    auto& playback = fixture.runtime().playback();
    auto actions = PlaybackActions{playback, [] {}};

    SECTION("Next and Previous enablement follows live sequence targets")
    {
      REQUIRE(fixture.playFromView(firstTrack));

      CHECK(actions.isEnabled(PlaybackCommand::Next));
      CHECK_FALSE(actions.isEnabled(PlaybackCommand::Previous));

      actions.tryExecute(PlaybackCommand::Next);

      CHECK(playback.snapshot().succession.currentTrackId == secondTrack);
      CHECK_FALSE(actions.isEnabled(PlaybackCommand::Next));
      CHECK(actions.isEnabled(PlaybackCommand::Previous));

      actions.tryExecute(PlaybackCommand::Next);

      CHECK(playback.snapshot().succession.currentTrackId == secondTrack);
      CHECK(playback.snapshot().transport.transport == audio::Transport::Playing);

      actions.tryExecute(PlaybackCommand::Previous);

      CHECK(playback.snapshot().succession.currentTrackId == firstTrack);
    }

    SECTION("Shuffle and repeat actions write through the live sequence")
    {
      REQUIRE(fixture.playFromView(secondTrack));
      REQUIRE_FALSE(playback.snapshot().succession.hasNext);

      actions.tryExecute(PlaybackCommand::CycleRepeat);

      CHECK(playback.snapshot().succession.repeat == RepeatMode::All);
      CHECK(playback.snapshot().succession.hasNext);

      actions.tryExecute(PlaybackCommand::CycleRepeat);
      CHECK(playback.snapshot().succession.repeat == RepeatMode::One);

      actions.tryExecute(PlaybackCommand::CycleRepeat);
      CHECK(playback.snapshot().succession.repeat == RepeatMode::Off);
      CHECK_FALSE(playback.snapshot().succession.hasNext);

      actions.tryExecute(PlaybackCommand::ToggleShuffle);

      CHECK(playback.snapshot().succession.shuffle == ShuffleMode::On);
      CHECK(playback.snapshot().succession.hasNext);
    }
  }

  TEST_CASE("PlaybackActions - separates UI enablement from protocol capability", "[uimodel][unit][playback][command]")
  {
    auto fixture = PlaybackUiFixture{};
    auto& playback = fixture.runtime().playback();
    auto actions = PlaybackActions{playback, [] {}};

    SECTION("no track distinguishes unavailable from ready playback")
    {
      checkCommandPolicy(actions,
                         CommandPolicy{false, false, false, false, false, false, false, false},
                         CommandPolicy{false, false, false, false, false, false, false, false});

      fixture.makePlaybackReady();

      checkCommandPolicy(actions,
                         CommandPolicy{true, false, true, false, false, false, true, true},
                         CommandPolicy{true, false, true, false, false, false, true, true});
    }

    SECTION("current track keeps protocol controls capable while output is pending")
    {
      fixture.makePlaybackReady();
      auto const firstTrack = fixture.addPlayableTrack("Capability First");
      fixture.addPlayableTrack("Capability Second");
      REQUIRE(fixture.playFromView(firstTrack));

      checkCommandPolicy(actions,
                         CommandPolicy{false, true, true, true, true, false, true, true},
                         CommandPolicy{true, true, true, true, true, false, true, true});

      auto const selected = playback.snapshot().transport.output.selectedDevice;
      playback.commands().setOutputDevice(
        selected.backendId, audio::DeviceId{"pending-capability-device"}, selected.profileId);
      REQUIRE_FALSE(playback.snapshot().transport.ready);

      checkCommandPolicy(actions,
                         CommandPolicy{false, true, true, true, false, false, false, false},
                         CommandPolicy{true, true, true, true, false, false, false, false});
    }

    SECTION("restored idle track is playable and pause-capable without being pause-enabled")
    {
      fixture.makePlaybackReady();
      auto const trackId = fixture.addPlayableTrack("Capability Restored");
      REQUIRE(fixture.playFromView(trackId));
      REQUIRE(fixture.runtime().savePlaybackSession());
      playback.commands().stop();
      auto const restoredRes = fixture.runtime().restorePlaybackSession();

      REQUIRE(restoredRes);
      REQUIRE(restoredRes->restored);
      REQUIRE(playback.snapshot().transport.transport == audio::Transport::Idle);
      REQUIRE(playback.snapshot().transport.nowPlaying.trackId == trackId);

      checkCommandPolicy(actions,
                         CommandPolicy{true, false, true, false, false, false, true, true},
                         CommandPolicy{true, true, true, false, false, false, true, true});
    }
  }

  TEST_CASE("PlaybackActions - emits availability when playback becomes ready", "[uimodel][unit][playback][command]")
  {
    auto fixture = PlaybackUiFixture{};
    std::int32_t playSelectionCount = 0;
    auto actions = PlaybackActions{fixture.runtime().playback(), [&playSelectionCount] { ++playSelectionCount; }};
    auto availability = AvailabilityLog{actions};

    CHECK_FALSE(actions.isEnabled(PlaybackCommand::Play));
    CHECK_FALSE(actions.isEnabled(PlaybackCommand::PlayPause));
    CHECK_FALSE(actions.tryExecute(PlaybackCommand::PlayPause));
    CHECK(playSelectionCount == 0);

    fixture.makePlaybackReady();

    CHECK(actions.isEnabled(PlaybackCommand::Play));
    CHECK(actions.isEnabled(PlaybackCommand::PlayPause));
    checkAffected(availability,
                  {PlaybackCommand::Play,
                   PlaybackCommand::Pause,
                   PlaybackCommand::PlayPause,
                   PlaybackCommand::Next,
                   PlaybackCommand::Previous,
                   PlaybackCommand::ToggleShuffle,
                   PlaybackCommand::CycleRepeat});
    CHECK(availability.aggregateCount > 0);
  }

  TEST_CASE("PlaybackActions - emits one availability event for playback command inputs",
            "[uimodel][unit][playback][command]")
  {
    auto fixture = PlaybackUiFixture{};
    fixture.makePlaybackReady();
    auto const firstTrack = fixture.addPlayableTrack("Event First");
    auto const secondTrack = fixture.addPlayableTrack("Event Second");
    auto& playback = fixture.runtime().playback();
    auto actions = PlaybackActions{playback, [] {}};
    auto availability = AvailabilityLog{actions};

    REQUIRE(fixture.playFromView(firstTrack));
    checkAffected(availability,
                  {PlaybackCommand::Play,
                   PlaybackCommand::Pause,
                   PlaybackCommand::PlayPause,
                   PlaybackCommand::Stop,
                   PlaybackCommand::Next,
                   PlaybackCommand::Previous});
    CHECK(availability.aggregateCount > 0);

    availability.clear();
    playback.commands().pause();
    checkAffected(
      availability, {PlaybackCommand::Play, PlaybackCommand::Pause, PlaybackCommand::PlayPause, PlaybackCommand::Stop});
    CHECK(availability.aggregateCount > 0);

    playback.commands().resume();
    availability.clear();
    playback.commands().setShuffleMode(ShuffleMode::On);
    checkAffected(availability, {PlaybackCommand::ToggleShuffle});
    CHECK(availability.count(PlaybackCommand::ToggleShuffle) == 1);
    CHECK(availability.aggregateCount == 1);

    availability.clear();
    playback.commands().setRepeatMode(RepeatMode::All);
    checkAffected(availability, {PlaybackCommand::CycleRepeat});
    CHECK(availability.count(PlaybackCommand::CycleRepeat) == 1);
    CHECK(availability.aggregateCount == 1);

    availability.clear();
    playback.commands().clearSequence();
    checkAffected(availability, {PlaybackCommand::Next, PlaybackCommand::Previous});
    CHECK(availability.aggregateCount > 0);

    availability.clear();
    REQUIRE(playback.snapshot().transport.transport == audio::Transport::Playing);
    REQUIRE(fixture.playFromView(secondTrack));
    REQUIRE(playback.snapshot().transport.transport == audio::Transport::Playing);
    checkAffected(availability,
                  {PlaybackCommand::Play,
                   PlaybackCommand::Pause,
                   PlaybackCommand::PlayPause,
                   PlaybackCommand::Stop,
                   PlaybackCommand::Next,
                   PlaybackCommand::Previous});
    CHECK(availability.aggregateCount > 0);

    availability.clear();
    playback.commands().seek(std::chrono::milliseconds{5}, PlaybackSeekMode::Preview);
    checkAffected(availability, {});
    CHECK(availability.aggregateCount == 0);

    playback.commands().seek(std::chrono::milliseconds{10}, PlaybackSeekMode::Final);
    checkAffected(availability, {});
    CHECK(availability.aggregateCount == 0);
  }
} // namespace ao::uimodel::test
