// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/playback/command/PlaybackActions.h>

#include "test/runtime/AppRuntimeTestSupport.h"
#include "test/runtime/ExecutorTestSupport.h"
#include "test/runtime/PlaybackUiTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/audio/Transport.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <tuple>
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

  TEST_CASE("PlaybackActions - executes transport policy")
  {
    auto fixture = PlaybackUiFixture{};
    fixture.makePlaybackReady();
    auto const trackId = fixture.addPlayableTrack("Command Track");
    auto& playback = fixture.runtime().playback();

    std::int32_t playSelectionCount = 0;
    auto actions = PlaybackActions{playback, [&playSelectionCount] { ++playSelectionCount; }};

    actions.setSelectionAvailable(true);

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

  TEST_CASE("PlaybackActions - primary Play needs a selection or a resumable subject")
  {
    auto fixture = PlaybackUiFixture{};
    fixture.makePlaybackReady();
    auto const trackA = fixture.addPlayableTrack("Current Track");
    auto const trackB = fixture.addPlayableTrack("Browsed Track");
    auto& runtime = fixture.runtime();
    auto& playback = runtime.playback();
    auto actions = PlaybackActions{playback, [&runtime] { std::ignore = runtime.playSelectionInFocusedView(); }};
    actions.bindFocusedSelection(runtime.workspace(), runtime.views());
    auto availability = AvailabilityLog{actions};
    CHECK_FALSE(actions.isEnabled(PlaybackCommand::PlayPause));
    CHECK_FALSE(actions.tryExecute(PlaybackCommand::Play));
    CHECK(playback.snapshot().transport.nowPlaying.trackId == kInvalidTrackId);

    availability.clear();
    REQUIRE(runtime.views().setSelection(fixture.viewId, {trackA}));
    checkAffected(availability, {PlaybackCommand::Play, PlaybackCommand::PlayPause});
    availability.clear();
    REQUIRE(runtime.views().setFilter(fixture.viewId, "$title = \"Browsed Track\""));
    fixture.executor->drain();
    checkAffected(availability, {PlaybackCommand::Play, PlaybackCommand::PlayPause});
    CHECK_FALSE(actions.isEnabled(PlaybackCommand::Play));
    CHECK_FALSE(actions.tryExecute(PlaybackCommand::PlayPause));
    availability.clear();
    REQUIRE(runtime.views().setFilter(fixture.viewId, ""));
    fixture.executor->drain();
    checkAffected(availability, {PlaybackCommand::Play, PlaybackCommand::PlayPause});
    REQUIRE(actions.tryExecute(PlaybackCommand::PlayPause));
    REQUIRE(fixture.tryWaitForPlayback(trackA));
    auto const offset = std::chrono::milliseconds{250};
    playback.commands().seek(offset);
    fixture.executor->drain();
    REQUIRE(playback.snapshot().transport.elapsed == offset);
    REQUIRE(runtime.views().setSelection(fixture.viewId, {trackB}));

    SECTION("Paused")
    {
      playback.commands().pause();
      REQUIRE(playback.snapshot().transport.transport == audio::Transport::Paused);
    }

    SECTION("Restored idle")
    {
      REQUIRE(runtime.savePlaybackSession());
      playback.commands().stop();
      auto const restoredRes = runtime.restorePlaybackSession();
      REQUIRE(restoredRes);
      REQUIRE(restoredRes->restored);
      REQUIRE(playback.snapshot().transport.transport == audio::Transport::Idle);
    }

    REQUIRE(actions.tryExecute(PlaybackCommand::Play));
    fixture.executor->drain();
    CHECK(playback.snapshot().transport.transport == audio::Transport::Playing);
    CHECK(playback.snapshot().transport.nowPlaying.trackId == trackA);
    CHECK(playback.snapshot().transport.elapsed == offset);
    playback.commands().stop();
    fixture.observedPositionRevision = playback.snapshot().transport.positionRevision;
    REQUIRE(actions.tryExecute(PlaybackCommand::Play));
    CHECK(fixture.tryWaitForPlayback(trackB));
  }

  TEST_CASE("PlaybackActions - local and system Play retain deferred audio after clearing the sequence")
  {
    auto fixture = PlaybackUiFixture{};
    fixture.makePlaybackReady();
    auto const trackA = fixture.addPlayableTrack("Restored Track");
    auto const trackB = fixture.addPlayableTrack("Selected Track");
    auto& runtime = fixture.runtime();
    auto& playback = runtime.playback();
    auto actions = PlaybackActions{playback, [&runtime] { std::ignore = runtime.playSelectionInFocusedView(); }};
    actions.bindFocusedSelection(runtime.workspace(), runtime.views());

    REQUIRE(fixture.playFromView(trackA));
    auto const offset = std::chrono::milliseconds{250};
    playback.commands().seek(offset);
    fixture.executor->drain();
    REQUIRE(playback.snapshot().transport.elapsed == offset);
    REQUIRE(runtime.savePlaybackSession());
    playback.commands().stop();
    auto const restoredRes = runtime.restorePlaybackSession();
    REQUIRE(restoredRes);
    REQUIRE(restoredRes->restored);
    playback.commands().clearSequence();
    REQUIRE(playback.snapshot().succession.currentTrackId == kInvalidTrackId);
    REQUIRE(playback.snapshot().transport.transport == audio::Transport::Idle);
    REQUIRE(playback.snapshot().transport.nowPlaying.trackId == trackA);
    REQUIRE(playback.snapshot().transport.elapsed == offset);

    REQUIRE(playback.snapshot().transport.hasDeferredResume);
    auto const origin = GENERATE(PlaybackActionOrigin::Primary, PlaybackActionOrigin::SystemMedia);
    CAPTURE(origin);

    SECTION("Play resumes without a selected row")
    {
      REQUIRE(runtime.views().setSelection(fixture.viewId, {}));
      REQUIRE(actions.tryExecute(PlaybackCommand::Play, origin));
    }

    SECTION("PlayPause resumes despite browsing a different row")
    {
      REQUIRE(runtime.views().setSelection(fixture.viewId, {trackB}));
      REQUIRE(actions.tryExecute(PlaybackCommand::PlayPause, origin));
    }

    SECTION("A pending output leaves the deferred track and the selection alone")
    {
      REQUIRE(runtime.views().setSelection(fixture.viewId, {trackB}));
      auto const output = playback.snapshot().transport.output.selectedDevice;
      playback.commands().setOutputDevice(output.backendId, audio::DeviceId{"missing"}, output.profileId);
      fixture.executor->drain();
      auto const held = playback.snapshot().transport;
      REQUIRE(held.transport == audio::Transport::Idle);
      REQUIRE(held.hasDeferredResume);
      REQUIRE(held.output.optPendingOutputSelection);
      CHECK_FALSE(actions.isEnabled(PlaybackCommand::Play, origin));
      CHECK_FALSE(actions.tryExecute(PlaybackCommand::PlayPause, origin));
      CHECK(playback.snapshot().transport == held);
      CHECK(runtime.views().firstVisibleSelectedTrack(fixture.viewId) == trackB);

      playback.commands().setOutputDevice(output);
      fixture.executor->drain();
      REQUIRE(actions.tryExecute(PlaybackCommand::Play, origin));
    }

    fixture.executor->drain();
    CHECK(playback.snapshot().transport.transport == audio::Transport::Playing);
    CHECK(playback.snapshot().transport.nowPlaying.trackId == trackA);
    CHECK(playback.snapshot().transport.elapsed == offset);
  }

  TEST_CASE("PlaybackActions - only system media may use the host fallback without a selected row")
  {
    auto fixture = PlaybackUiFixture{};
    fixture.makePlaybackReady();
    std::int32_t selectedStarts = 0;
    std::int32_t remoteStarts = 0;
    bool remoteAvailable = false;
    bool remoteStarted = true;
    auto actions = PlaybackActions{fixture.runtime().playback(),
                                   [&] { ++selectedStarts; },
                                   SystemMediaPlayHandler{
                                     .canPlay = [&] { return remoteAvailable; },
                                     .play =
                                       [&]
                                     {
                                       ++remoteStarts;
                                       return remoteStarted;
                                     },
                                   }};

    CHECK_FALSE(actions.tryExecute(PlaybackCommand::PlayPause));
    CHECK_FALSE(actions.isEnabled(PlaybackCommand::Play, PlaybackActionOrigin::SystemMedia));
    CHECK_FALSE(actions.tryExecute(PlaybackCommand::Play, PlaybackActionOrigin::SystemMedia));
    CHECK(selectedStarts == 0);
    CHECK(remoteStarts == 0);
    REQUIRE(actions.isCapable(PlaybackCommand::Play));
    auto selectedOnly = PlaybackActions{fixture.runtime().playback(), [&] { ++selectedStarts; }};
    CHECK_FALSE(selectedOnly.tryExecute(PlaybackCommand::Play, PlaybackActionOrigin::SystemMedia));
    CHECK(selectedStarts == 0);

    remoteAvailable = true;
    CHECK(actions.isEnabled(PlaybackCommand::Play, PlaybackActionOrigin::SystemMedia));
    CHECK_FALSE(actions.isEnabled(PlaybackCommand::Play));
    remoteStarted = false;
    CHECK_FALSE(actions.tryExecute(PlaybackCommand::Play, PlaybackActionOrigin::SystemMedia));
    CHECK(remoteStarts == 1);
    remoteStarted = true;
    REQUIRE(actions.tryExecute(PlaybackCommand::Play, PlaybackActionOrigin::SystemMedia));
    CHECK(remoteStarts == 2);
    CHECK(selectedStarts == 0);
    actions.setSelectionAvailable(true);
    REQUIRE(actions.tryExecute(PlaybackCommand::PlayPause));
    CHECK(remoteStarts == 2);
    CHECK(selectedStarts == 1);
  }

  TEST_CASE("PlaybackActions - focused selection publishes system media availability changes without a selection")
  {
    auto fixture = PlaybackUiFixture{};
    fixture.makePlaybackReady();
    auto const trackId = fixture.addPlayableTrack("Projected Track");
    auto& runtime = fixture.runtime();
    auto const canStartRemote = [&runtime]
    {
      auto const viewId = runtime.workspace().snapshot().activeViewId;
      auto& views = runtime.views();

      if (!views.selectedTrackIds(viewId).empty())
      {
        return views.firstVisibleSelectedTrack(viewId) != kInvalidTrackId;
      }

      auto const projectionRes = views.findTrackListProjection(viewId);
      return projectionRes && (*projectionRes)->size() > 0;
    };
    auto actions = PlaybackActions{runtime.playback(),
                                   [] {},
                                   SystemMediaPlayHandler{
                                     .canPlay = canStartRemote,
                                     .play = canStartRemote,
                                   }};
    actions.bindFocusedSelection(runtime.workspace(), runtime.views());
    auto availability = AvailabilityLog{actions};

    CHECK_FALSE(actions.isEnabled(PlaybackCommand::Play));
    CHECK(actions.isEnabled(PlaybackCommand::Play, PlaybackActionOrigin::SystemMedia));

    availability.clear();
    REQUIRE(runtime.views().setFilter(fixture.viewId, R"($title = "Missing")"));
    fixture.executor->drain();
    checkAffected(availability, {PlaybackCommand::Play, PlaybackCommand::PlayPause});
    CHECK_FALSE(actions.isEnabled(PlaybackCommand::Play, PlaybackActionOrigin::SystemMedia));
    CHECK_FALSE(actions.tryExecute(PlaybackCommand::Play, PlaybackActionOrigin::SystemMedia));

    availability.clear();
    REQUIRE(runtime.views().setFilter(fixture.viewId, ""));
    fixture.executor->drain();
    checkAffected(availability, {PlaybackCommand::Play, PlaybackCommand::PlayPause});
    CHECK(actions.isEnabled(PlaybackCommand::Play, PlaybackActionOrigin::SystemMedia));
    CHECK(actions.tryExecute(PlaybackCommand::Play, PlaybackActionOrigin::SystemMedia));
    CHECK(runtime.views().firstVisibleSelectedTrack(fixture.viewId) == kInvalidTrackId);

    REQUIRE(runtime.views().setSelection(fixture.viewId, {trackId}));
    availability.clear();
    REQUIRE(runtime.views().setFilter(fixture.viewId, R"($title = "Missing")"));
    fixture.executor->drain();
    checkAffected(availability, {PlaybackCommand::Play, PlaybackCommand::PlayPause});
    CHECK_FALSE(actions.isEnabled(PlaybackCommand::Play, PlaybackActionOrigin::SystemMedia));
  }

  TEST_CASE("PlaybackActions - manual play resumes the interrupted track when its output appears")
  {
    auto fixture = PlaybackUiFixture{};
    auto& runtime = fixture.runtime();
    auto& playback = runtime.playback();
    auto& commands = playback.commands();
    auto const fallbackCountsPtr = std::make_shared<SilentRouteCounts>();
    runtime.addAudioProvider(makeReadyAudioProvider(makeReadyAudioStatus(), fallbackCountsPtr));
    fixture.executor->drain();
    auto const trackId = fixture.addPlayableTrack("Interrupted Track");
    auto const selectedTrackId = fixture.addPlayableTrack("Different Selection");
    auto actions = PlaybackActions{playback, [&runtime] { std::ignore = runtime.playSelectionInFocusedView(); }};

    REQUIRE(fixture.playFromView(trackId));
    auto const offset = std::chrono::milliseconds{250};
    REQUIRE(playback.snapshot().transport.duration > offset);
    commands.seek(offset);
    fixture.executor->drain();
    REQUIRE(playback.snapshot().transport.transport == audio::Transport::Playing);
    REQUIRE(playback.snapshot().transport.elapsed == offset);
    auto const fallbackOpens = fallbackCountsPtr->opens.load();
    auto const fallbackStarts = fallbackCountsPtr->starts.load();
    REQUIRE(fallbackStarts > 0);

    auto const dac = audio::OutputDeviceSelection{.backendId = audio::BackendId{"usb"},
                                                  .deviceId = audio::DeviceId{"studio-dac"},
                                                  .profileId = audio::kProfileShared};
    commands.setOutputDevice(dac);
    fixture.executor->drain();

    auto const held = playback.snapshot().transport;
    CHECK(held.output.optPendingOutputSelection == dac);
    CHECK_FALSE(held.ready);
    CHECK(held.transport == audio::Transport::Paused);
    CHECK(held.nowPlaying.trackId == trackId);
    CHECK(held.elapsed == offset);
    CHECK_FALSE(actions.isEnabled(PlaybackCommand::Play));
    CHECK_FALSE(actions.isEnabled(PlaybackCommand::PlayPause));
    CHECK_FALSE(actions.tryExecute(PlaybackCommand::Play));
    CHECK_FALSE(actions.tryExecute(PlaybackCommand::PlayPause));

    commands.resume();
    fixture.executor->drain();
    CHECK(playback.snapshot().transport.transport == audio::Transport::Paused);
    CHECK(fallbackCountsPtr->opens.load() == fallbackOpens);
    CHECK(fallbackCountsPtr->starts.load() == fallbackStarts);

    commands.clearSequence();
    REQUIRE(playback.snapshot().succession.currentTrackId == kInvalidTrackId);
    REQUIRE(runtime.views().setSelection(fixture.viewId, {selectedTrackId}));
    auto const dacCountsPtr = std::make_shared<SilentRouteCounts>();
    runtime.addAudioProvider(makeReadyAudioProvider(
      {.descriptor = {.id = dac.backendId, .supportedProfiles = {{.id = dac.profileId}}},
       .devices = {{.id = dac.deviceId, .displayName = "Studio DAC", .backendId = dac.backendId}}},
      dacCountsPtr));
    fixture.executor->drain();

    auto const ready = playback.snapshot().transport;
    REQUIRE(ready.ready);
    CHECK(ready.output.selectedDevice == dac);
    CHECK(ready.transport == audio::Transport::Paused);
    CHECK(ready.nowPlaying.trackId == trackId);
    CHECK(ready.elapsed == offset);
    CHECK(dacCountsPtr->opens.load() == 0);
    CHECK(dacCountsPtr->starts.load() == 0);

    SECTION("Play")
    {
      REQUIRE(actions.tryExecute(PlaybackCommand::Play));
    }

    SECTION("PlayPause")
    {
      REQUIRE(actions.tryExecute(PlaybackCommand::PlayPause));
    }

    fixture.executor->drain();
    auto const resumed = playback.snapshot().transport;
    CHECK(resumed.transport == audio::Transport::Playing);
    CHECK(resumed.nowPlaying.trackId == trackId);
    CHECK(resumed.elapsed == offset);
    CHECK(dacCountsPtr->starts.load() == 1);
    CHECK(fallbackCountsPtr->starts.load() == fallbackStarts);
  }

  TEST_CASE("PlaybackActions - owns availability and live-sequence command policy")
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

  TEST_CASE("PlaybackActions - separates UI enablement from protocol capability")
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
                         CommandPolicy{false, false, false, false, false, false, true, true},
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
                         CommandPolicy{false, false, false, true, false, false, false, false},
                         CommandPolicy{true, true, false, true, false, false, false, false});
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

  TEST_CASE("PlaybackActions - emits availability when playback becomes ready")
  {
    auto fixture = PlaybackUiFixture{};
    std::int32_t playSelectionCount = 0;
    auto actions = PlaybackActions{fixture.runtime().playback(), [&playSelectionCount] { ++playSelectionCount; }};
    actions.setSelectionAvailable(true);
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

  TEST_CASE("PlaybackActions - emits one availability event for playback command inputs")
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
