// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/runtime/PlaybackUiTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/Transport.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/uimodel/playback/command/PlaybackCommandSurface.h>
#include <ao/uimodel/playback/transport/TransportViewModel.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>

namespace ao::uimodel::test
{
  using namespace ao::rt;
  using namespace ao::rt::test;

  TEST_CASE("TransportViewModel - renders presentation for actions", "[uimodel][unit][playback]")
  {
    auto fixture = PlaybackUiFixture{};
    auto& playback = fixture.runtime.playback();
    auto commands = PlaybackCommandSurface{playback, [] {}};

    SECTION("Play action uses play icon and disabled command state")
    {
      auto log = ao::test::RenderLog<TransportViewState>{};
      auto vm =
        TransportViewModel{playback, commands, PlaybackCommand::Play, false, [&log](auto const& v) { log.render(v); }};

      REQUIRE(!log.empty());
      CHECK(log.last().enabled == false);
      CHECK(log.last().icon == TransportIcon::Play);
      CHECK(log.last().tooltip == "Play");
    }

    SECTION("PlayPause switches icon and label from playback transport")
    {
      auto log = ao::test::RenderLog<TransportViewState>{};
      auto vm = TransportViewModel{
        playback, commands, PlaybackCommand::PlayPause, true, [&log](auto const& v) { log.render(v); }};

      CHECK(log.last().icon == TransportIcon::Play);
      CHECK(log.last().tooltip == "Play");
      CHECK(log.last().label == "Play");
      CHECK(log.last().playing == false);
      CHECK(log.last().enabled == false);
    }

    SECTION("Repeat and shuffle render engaged state from sequence modes")
    {
      playback.commands().setShuffleMode(ShuffleMode::On);
      playback.commands().setRepeatMode(RepeatMode::One);

      auto shuffleLog = ao::test::RenderLog<TransportViewState>{};
      auto repeatLog = ao::test::RenderLog<TransportViewState>{};
      auto shuffleVm = TransportViewModel{playback,
                                          commands,
                                          PlaybackCommand::ToggleShuffle,
                                          true,
                                          [&shuffleLog](auto const& v) { shuffleLog.render(v); }};
      auto repeatVm = TransportViewModel{
        playback, commands, PlaybackCommand::CycleRepeat, true, [&repeatLog](auto const& v) { repeatLog.render(v); }};

      CHECK(shuffleLog.last().engaged == true);
      CHECK(shuffleLog.last().label == "Shuffle");
      CHECK(repeatLog.last().engaged == true);
      CHECK(repeatLog.last().icon == TransportIcon::RepeatOne);
      CHECK(repeatLog.last().label == "Repeat");
    }
  }

  TEST_CASE("TransportViewModel - renders command-surface enablement", "[uimodel][unit][playback][sequence]")
  {
    auto fixture = PlaybackUiFixture{};
    fixture.makePlaybackReady();
    auto const firstTrack = fixture.addPlayableTrack("First");
    fixture.addPlayableTrack("Second");
    auto& playback = fixture.runtime.playback();
    auto commands = PlaybackCommandSurface{playback, [] {}};
    REQUIRE(fixture.playFromView(firstTrack));

    auto nextLog = ao::test::RenderLog<TransportViewState>{};
    auto previousLog = ao::test::RenderLog<TransportViewState>{};
    auto nextVm = TransportViewModel{
      playback, commands, PlaybackCommand::Next, false, [&nextLog](auto const& v) { nextLog.render(v); }};
    auto previousVm = TransportViewModel{
      playback, commands, PlaybackCommand::Previous, false, [&previousLog](auto const& v) { previousLog.render(v); }};

    CHECK(nextLog.last().enabled == true);
    CHECK(previousLog.last().enabled == false);
  }

  TEST_CASE("TransportViewModel - refreshes only the command it presents", "[uimodel][unit][playback]")
  {
    auto fixture = PlaybackUiFixture{};
    fixture.makePlaybackReady();
    auto& playback = fixture.runtime.playback();
    auto commands = PlaybackCommandSurface{playback, [] {}};

    auto playLog = ao::test::RenderLog<TransportViewState>{};
    auto shuffleLog = ao::test::RenderLog<TransportViewState>{};
    auto playVm = TransportViewModel{
      playback, commands, PlaybackCommand::Play, false, [&playLog](auto const& v) { playLog.render(v); }};
    auto shuffleVm = TransportViewModel{playback,
                                        commands,
                                        PlaybackCommand::ToggleShuffle,
                                        false,
                                        [&shuffleLog](auto const& v) { shuffleLog.render(v); }};

    auto const playCount = playLog.states.size();
    auto const shuffleCount = shuffleLog.states.size();

    playback.commands().setShuffleMode(ShuffleMode::On);

    CHECK(playLog.states.size() == playCount);
    CHECK(shuffleLog.states.size() == shuffleCount + 1);
    CHECK(shuffleLog.last().engaged == true);
  }

  TEST_CASE("TransportViewModel - clicks delegate to command surface", "[uimodel][unit][playback]")
  {
    auto fixture = PlaybackUiFixture{};
    fixture.makePlaybackReady();
    auto const firstTrack = fixture.addPlayableTrack("First");
    auto const secondTrack = fixture.addPlayableTrack("Second");
    auto& playback = fixture.runtime.playback();
    auto commands = PlaybackCommandSurface{playback, [] {}};

    SECTION("PlayPause resumes paused playback")
    {
      REQUIRE(fixture.playFromView(firstTrack));
      playback.commands().pause();

      auto log = ao::test::RenderLog<TransportViewState>{};
      auto vm = TransportViewModel{
        playback, commands, PlaybackCommand::PlayPause, false, [&log](auto const& v) { log.render(v); }};

      vm.handleClick();

      CHECK(playback.snapshot().transport.transport == audio::Transport::Playing);
      CHECK(playback.snapshot().transport.nowPlaying.trackId == firstTrack);
    }

    SECTION("Next delegates to sequence command")
    {
      REQUIRE(fixture.playFromView(firstTrack));
      auto log = ao::test::RenderLog<TransportViewState>{};
      auto vm =
        TransportViewModel{playback, commands, PlaybackCommand::Next, false, [&log](auto const& v) { log.render(v); }};

      vm.handleClick();

      CHECK(playback.snapshot().succession.currentTrackId == secondTrack);
    }

    SECTION("Shuffle delegates to sequence mode command")
    {
      auto log = ao::test::RenderLog<TransportViewState>{};
      auto vm = TransportViewModel{
        playback, commands, PlaybackCommand::ToggleShuffle, false, [&log](auto const& v) { log.render(v); }};

      vm.handleClick();

      CHECK(playback.snapshot().succession.shuffle == ShuffleMode::On);
      CHECK(log.last().engaged == true);
    }
  }

  TEST_CASE("TransportViewModel - stops rendering after destruction", "[uimodel][unit][playback]")
  {
    auto fixture = PlaybackUiFixture{};
    fixture.makePlaybackReady();
    auto& playback = fixture.runtime.playback();
    auto commands = PlaybackCommandSurface{playback, [] {}};

    auto log = ao::test::RenderLog<TransportViewState>{};
    auto viewModelPtr = std::make_unique<TransportViewModel>(
      playback, commands, PlaybackCommand::ToggleShuffle, false, [&log](auto const& view) { log.render(view); });

    REQUIRE(!log.empty());
    log.clear();

    playback.commands().setShuffleMode(ShuffleMode::On);
    REQUIRE(log.states.size() == 1);
    CHECK(log.last().engaged == true);

    log.clear();
    viewModelPtr.reset();

    playback.commands().setShuffleMode(ShuffleMode::Off);
    CHECK(log.empty());
  }
} // namespace ao::uimodel::test
