// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/runtime/PlaybackServiceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/Transport.h>
#include <ao/rt/PlaybackSessionState.h>
#include <ao/rt/PlaybackState.h>
#include <ao/uimodel/playback/command/PlaybackCommandSurface.h>
#include <ao/uimodel/playback/queue/PlaybackQueueModel.h>
#include <ao/uimodel/playback/transport/TransportViewModel.h>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <memory>

namespace ao::uimodel::test
{
  using namespace ao::rt;
  using namespace ao::rt::test;

  TEST_CASE("TransportViewModel - renders presentation for actions", "[uimodel][unit][playback]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};
    auto commands = PlaybackCommandSurface{fixture.playbackService, nullptr, [] {}};

    SECTION("Play action uses play icon and disabled command state")
    {
      auto log = ao::test::RenderLog<TransportViewState>{};
      auto vm = TransportViewModel{
        fixture.playbackService, commands, TransportAction::Play, false, [&log](auto const& v) { log.render(v); }};

      REQUIRE(!log.empty());
      CHECK(log.last().enabled == false);
      CHECK(log.last().icon == TransportIcon::Play);
      CHECK(log.last().tooltip == "Play");
    }

    SECTION("PlayPause switches icon and label from playback transport")
    {
      auto log = ao::test::RenderLog<TransportViewState>{};
      auto vm = TransportViewModel{
        fixture.playbackService, commands, TransportAction::PlayPause, true, [&log](auto const& v) { log.render(v); }};

      CHECK(log.last().icon == TransportIcon::Play);
      CHECK(log.last().tooltip == "Play");
      CHECK(log.last().label == "Play");
      CHECK(log.last().playing == false);
      CHECK(log.last().enabled == false);
    }

    SECTION("Repeat and shuffle render engaged state from playback modes")
    {
      fixture.playbackService.setShuffleMode(rt::ShuffleMode::On);
      fixture.playbackService.setRepeatMode(rt::RepeatMode::One);

      auto shuffleLog = ao::test::RenderLog<TransportViewState>{};
      auto repeatLog = ao::test::RenderLog<TransportViewState>{};
      auto shuffleVm = TransportViewModel{fixture.playbackService,
                                          commands,
                                          TransportAction::Shuffle,
                                          true,
                                          [&shuffleLog](auto const& v) { shuffleLog.render(v); }};
      auto repeatVm = TransportViewModel{fixture.playbackService,
                                         commands,
                                         TransportAction::Repeat,
                                         true,
                                         [&repeatLog](auto const& v) { repeatLog.render(v); }};

      CHECK(shuffleLog.last().engaged == true);
      CHECK(shuffleLog.last().label == "Shuffle");
      CHECK(repeatLog.last().engaged == true);
      CHECK(repeatLog.last().icon == TransportIcon::RepeatOne);
      CHECK(repeatLog.last().label == "Repeat");
    }
  }

  TEST_CASE("TransportViewModel - renders command-surface enablement", "[uimodel][unit][playback][queue]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const firstTrack = fixture.testLib.addTrack({.title = "First", .uri = fixturePath});
    auto const secondTrack = fixture.testLib.addTrack({.title = "Second", .uri = fixturePath});
    auto queue = PlaybackQueueModel{fixture.playbackService, fixture.notificationService};
    auto commands = PlaybackCommandSurface{fixture.playbackService, &queue, [] {}};
    REQUIRE(queue.playQueue({firstTrack, secondTrack}, firstTrack, ListId{9}));

    auto nextLog = ao::test::RenderLog<TransportViewState>{};
    auto previousLog = ao::test::RenderLog<TransportViewState>{};
    auto nextVm = TransportViewModel{fixture.playbackService,
                                     commands,
                                     TransportAction::Next,
                                     false,
                                     [&nextLog](auto const& v) { nextLog.render(v); }};
    auto previousVm = TransportViewModel{fixture.playbackService,
                                         commands,
                                         TransportAction::Previous,
                                         false,
                                         [&previousLog](auto const& v) { previousLog.render(v); }};

    CHECK(nextLog.last().enabled == true);
    CHECK(previousLog.last().enabled == false);
  }

  TEST_CASE("TransportViewModel - refreshes only the command it presents", "[uimodel][unit][playback]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    auto commands = PlaybackCommandSurface{fixture.playbackService, nullptr, [] {}};

    auto playLog = ao::test::RenderLog<TransportViewState>{};
    auto shuffleLog = ao::test::RenderLog<TransportViewState>{};
    auto playVm = TransportViewModel{fixture.playbackService,
                                     commands,
                                     TransportAction::Play,
                                     false,
                                     [&playLog](auto const& v) { playLog.render(v); }};
    auto shuffleVm = TransportViewModel{fixture.playbackService,
                                        commands,
                                        TransportAction::Shuffle,
                                        false,
                                        [&shuffleLog](auto const& v) { shuffleLog.render(v); }};

    auto const playCount = playLog.states.size();
    auto const shuffleCount = shuffleLog.states.size();

    fixture.playbackService.setShuffleMode(rt::ShuffleMode::On);

    CHECK(playLog.states.size() == playCount);
    CHECK(shuffleLog.states.size() == shuffleCount + 1);
    CHECK(shuffleLog.last().engaged == true);
  }

  TEST_CASE("TransportViewModel - clicks delegate to command surface", "[uimodel][unit][playback]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const firstTrack = fixture.testLib.addTrack({.title = "First", .uri = fixturePath});
    auto const secondTrack = fixture.testLib.addTrack({.title = "Second", .uri = fixturePath});
    auto queue = PlaybackQueueModel{fixture.playbackService, fixture.notificationService};
    auto commands = PlaybackCommandSurface{fixture.playbackService, &queue, [] {}};

    SECTION("PlayPause resumes restored playback")
    {
      REQUIRE(fixture.playbackService.restoreSession(PlaybackSessionState{
        .sourceListId = ListId{5},
        .trackId = firstTrack,
        .positionMs = 50,
      }));

      auto log = ao::test::RenderLog<TransportViewState>{};
      auto vm = TransportViewModel{
        fixture.playbackService, commands, TransportAction::PlayPause, false, [&log](auto const& v) { log.render(v); }};

      vm.handleClick();

      CHECK(fixture.playbackService.state().transport == audio::Transport::Playing);
      CHECK(fixture.playbackService.state().nowPlaying.trackId == firstTrack);
    }

    SECTION("Next delegates to queue command")
    {
      REQUIRE(queue.playQueue({firstTrack, secondTrack}, firstTrack, ListId{9}));
      auto log = ao::test::RenderLog<TransportViewState>{};
      auto vm = TransportViewModel{
        fixture.playbackService, commands, TransportAction::Next, false, [&log](auto const& v) { log.render(v); }};

      vm.handleClick();

      CHECK(queue.nowPlayingTrackId() == secondTrack);
    }

    SECTION("Shuffle delegates to mode command")
    {
      auto log = ao::test::RenderLog<TransportViewState>{};
      auto vm = TransportViewModel{
        fixture.playbackService, commands, TransportAction::Shuffle, false, [&log](auto const& v) { log.render(v); }};

      vm.handleClick();

      CHECK(fixture.playbackService.state().mode.shuffle == rt::ShuffleMode::On);
      CHECK(log.last().engaged == true);
    }
  }

  TEST_CASE("TransportViewModel - stops rendering after destruction", "[uimodel][unit][playback]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    auto commands = PlaybackCommandSurface{fixture.playbackService, nullptr, [] {}};

    auto log = ao::test::RenderLog<TransportViewState>{};
    auto viewModelPtr = std::make_unique<TransportViewModel>(fixture.playbackService,
                                                             commands,
                                                             TransportAction::Shuffle,
                                                             false,
                                                             [&log](auto const& view) { log.render(view); });

    REQUIRE(!log.empty());
    log.clear();

    fixture.playbackService.setShuffleMode(rt::ShuffleMode::On);
    REQUIRE(log.states.size() == 1);
    CHECK(log.last().engaged == true);

    log.clear();
    viewModelPtr.reset();

    fixture.playbackService.setShuffleMode(rt::ShuffleMode::Off);
    CHECK(log.empty());
  }
} // namespace ao::uimodel::test
