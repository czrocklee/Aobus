// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/playback/transport/TransportViewModel.h>

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/runtime/PlaybackUiTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/Transport.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/playback/command/PlaybackActions.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <string_view>

namespace ao::uimodel::test
{
  using namespace ao::rt;
  using namespace ao::rt::test;

  TEST_CASE("TransportViewModel - renders presentation for actions", "[uimodel][unit][playback]")
  {
    auto fixture = PlaybackUiFixture{};
    auto& playback = fixture.runtime().playback();
    auto actions = PlaybackActions{playback, [] {}};

    SECTION("every command has complete semantic icon and label policy")
    {
      struct ExpectedPresentation final
      {
        PlaybackCommand command;
        TransportIcon icon;
        std::string_view text;
      };

      constexpr auto kExpectedPresentations = std::array{
        ExpectedPresentation{PlaybackCommand::Play, TransportIcon::Play, "Play"},
        ExpectedPresentation{PlaybackCommand::Pause, TransportIcon::Pause, "Pause"},
        ExpectedPresentation{PlaybackCommand::PlayPause, TransportIcon::Play, "Play"},
        ExpectedPresentation{PlaybackCommand::Stop, TransportIcon::Stop, "Stop"},
        ExpectedPresentation{PlaybackCommand::Next, TransportIcon::Next, "Next Track"},
        ExpectedPresentation{PlaybackCommand::Previous, TransportIcon::Previous, "Previous Track"},
        ExpectedPresentation{PlaybackCommand::ToggleShuffle, TransportIcon::Shuffle, "Shuffle"},
        ExpectedPresentation{PlaybackCommand::CycleRepeat, TransportIcon::Repeat, "Repeat"},
      };

      for (auto const& expected : kExpectedPresentations)
      {
        CAPTURE(playbackCommandId(expected.command));
        auto withoutLabel = ao::test::RenderLog<TransportViewState>{};
        auto hiddenLabelViewModel =
          TransportViewModel{playback,
                             actions,
                             ao::test::englishMessageCatalog(),
                             expected.command,
                             false,
                             [&withoutLabel](auto const& view) { withoutLabel.render(view); }};
        auto withLabel = ao::test::RenderLog<TransportViewState>{};
        auto shownLabelViewModel = TransportViewModel{playback,
                                                      actions,
                                                      ao::test::englishMessageCatalog(),
                                                      expected.command,
                                                      true,
                                                      [&withLabel](auto const& view) { withLabel.render(view); }};

        REQUIRE_FALSE(withoutLabel.empty());
        REQUIRE_FALSE(withLabel.empty());
        CHECK(withoutLabel.last().icon == expected.icon);
        CHECK(withoutLabel.last().tooltip == expected.text);
        CHECK(withoutLabel.last().label.empty());
        CHECK_FALSE(withoutLabel.last().enabled);
        CHECK_FALSE(withoutLabel.last().engaged);
        CHECK_FALSE(withoutLabel.last().playing);
        CHECK(withLabel.last().icon == expected.icon);
        CHECK(withLabel.last().tooltip == expected.text);
        CHECK(withLabel.last().label == expected.text);
        CHECK_FALSE(withLabel.last().enabled);
        CHECK_FALSE(withLabel.last().engaged);
        CHECK_FALSE(withLabel.last().playing);
      }
    }

    SECTION("PlayPause follows reachable idle playing and paused states")
    {
      fixture.makePlaybackReady();
      auto const trackId = fixture.addPlayableTrack("Presentation Track");
      auto log = ao::test::RenderLog<TransportViewState>{};
      auto viewModel = TransportViewModel{playback,
                                          actions,
                                          ao::test::englishMessageCatalog(),
                                          PlaybackCommand::PlayPause,
                                          true,
                                          [&log](auto const& view) { log.render(view); }};

      REQUIRE_FALSE(log.empty());
      CHECK(log.last().icon == TransportIcon::Play);
      CHECK(log.last().tooltip == "Play");
      CHECK(log.last().label == "Play");
      CHECK_FALSE(log.last().playing);

      REQUIRE(fixture.playFromView(trackId));
      CHECK(log.last().icon == TransportIcon::Pause);
      CHECK(log.last().tooltip == "Pause");
      CHECK(log.last().label == "Pause");
      CHECK(log.last().playing);

      playback.commands().pause();
      CHECK(log.last().icon == TransportIcon::Play);
      CHECK(log.last().tooltip == "Play");
      CHECK(log.last().label == "Play");
      CHECK_FALSE(log.last().playing);
    }

    SECTION("Shuffle presents both engagement states")
    {
      auto log = ao::test::RenderLog<TransportViewState>{};
      auto viewModel = TransportViewModel{playback,
                                          actions,
                                          ao::test::englishMessageCatalog(),
                                          PlaybackCommand::ToggleShuffle,
                                          true,
                                          [&log](auto const& view) { log.render(view); }};

      REQUIRE_FALSE(log.empty());
      CHECK_FALSE(log.last().engaged);
      CHECK(log.last().icon == TransportIcon::Shuffle);
      CHECK(log.last().label == "Shuffle");

      playback.commands().setShuffleMode(ShuffleMode::On);
      CHECK(log.last().engaged);
      CHECK(log.last().icon == TransportIcon::Shuffle);

      playback.commands().setShuffleMode(ShuffleMode::Off);
      CHECK_FALSE(log.last().engaged);
    }

    SECTION("Repeat presents Off All and One states")
    {
      auto log = ao::test::RenderLog<TransportViewState>{};
      auto viewModel = TransportViewModel{playback,
                                          actions,
                                          ao::test::englishMessageCatalog(),
                                          PlaybackCommand::CycleRepeat,
                                          true,
                                          [&log](auto const& view) { log.render(view); }};

      REQUIRE_FALSE(log.empty());
      CHECK_FALSE(log.last().engaged);
      CHECK(log.last().icon == TransportIcon::Repeat);
      CHECK(log.last().tooltip == "Repeat");
      CHECK(log.last().label == "Repeat");

      playback.commands().setRepeatMode(RepeatMode::All);
      CHECK(log.last().engaged);
      CHECK(log.last().icon == TransportIcon::Repeat);

      playback.commands().setRepeatMode(RepeatMode::One);
      CHECK(log.last().engaged);
      CHECK(log.last().icon == TransportIcon::RepeatOne);
    }
  }

  TEST_CASE("TransportViewModel - renders command-surface enablement", "[uimodel][unit][playback][sequence]")
  {
    auto fixture = PlaybackUiFixture{};
    fixture.makePlaybackReady();
    auto const firstTrack = fixture.addPlayableTrack("First");
    fixture.addPlayableTrack("Second");
    auto& playback = fixture.runtime().playback();
    auto actions = PlaybackActions{playback, [] {}};
    REQUIRE(fixture.playFromView(firstTrack));

    auto nextLog = ao::test::RenderLog<TransportViewState>{};
    auto previousLog = ao::test::RenderLog<TransportViewState>{};
    auto nextVm = TransportViewModel{playback,
                                     actions,
                                     ao::test::englishMessageCatalog(),
                                     PlaybackCommand::Next,
                                     false,
                                     [&nextLog](auto const& v) { nextLog.render(v); }};
    auto previousVm = TransportViewModel{playback,
                                         actions,
                                         ao::test::englishMessageCatalog(),
                                         PlaybackCommand::Previous,
                                         false,
                                         [&previousLog](auto const& v) { previousLog.render(v); }};

    CHECK(nextLog.last().enabled == true);
    CHECK(previousLog.last().enabled == false);
  }

  TEST_CASE("TransportViewModel - refreshes only the command it presents", "[uimodel][unit][playback]")
  {
    auto fixture = PlaybackUiFixture{};
    fixture.makePlaybackReady();
    auto& playback = fixture.runtime().playback();
    auto actions = PlaybackActions{playback, [] {}};

    auto playLog = ao::test::RenderLog<TransportViewState>{};
    auto shuffleLog = ao::test::RenderLog<TransportViewState>{};
    auto playVm = TransportViewModel{playback,
                                     actions,
                                     ao::test::englishMessageCatalog(),
                                     PlaybackCommand::Play,
                                     false,
                                     [&playLog](auto const& v) { playLog.render(v); }};
    auto shuffleVm = TransportViewModel{playback,
                                        actions,
                                        ao::test::englishMessageCatalog(),
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

  TEST_CASE("TransportViewModel - clicks delegate to command surface", "[uimodel][integration][playback]")
  {
    auto fixture = PlaybackUiFixture{};
    fixture.makePlaybackReady();
    auto const firstTrack = fixture.addPlayableTrack("First");
    auto const secondTrack = fixture.addPlayableTrack("Second");
    auto& playback = fixture.runtime().playback();
    auto actions = PlaybackActions{playback, [] {}};

    SECTION("PlayPause resumes paused playback")
    {
      REQUIRE(fixture.playFromView(firstTrack));
      playback.commands().pause();

      auto log = ao::test::RenderLog<TransportViewState>{};
      auto vm = TransportViewModel{playback,
                                   actions,
                                   ao::test::englishMessageCatalog(),
                                   PlaybackCommand::PlayPause,
                                   false,
                                   [&log](auto const& v) { log.render(v); }};

      vm.handleClick();

      CHECK(playback.snapshot().transport.transport == audio::Transport::Playing);
      CHECK(playback.snapshot().transport.nowPlaying.trackId == firstTrack);
    }

    SECTION("Next delegates to sequence command")
    {
      REQUIRE(fixture.playFromView(firstTrack));
      auto log = ao::test::RenderLog<TransportViewState>{};
      auto vm = TransportViewModel{playback,
                                   actions,
                                   ao::test::englishMessageCatalog(),
                                   PlaybackCommand::Next,
                                   false,
                                   [&log](auto const& v) { log.render(v); }};

      vm.handleClick();

      CHECK(playback.snapshot().succession.currentTrackId == secondTrack);
    }

    SECTION("Shuffle delegates to sequence mode command")
    {
      auto log = ao::test::RenderLog<TransportViewState>{};
      auto vm = TransportViewModel{playback,
                                   actions,
                                   ao::test::englishMessageCatalog(),
                                   PlaybackCommand::ToggleShuffle,
                                   false,
                                   [&log](auto const& v) { log.render(v); }};

      vm.handleClick();

      CHECK(playback.snapshot().succession.shuffle == ShuffleMode::On);
      CHECK(log.last().engaged == true);
    }
  }

  TEST_CASE("TransportViewModel - stops rendering after destruction", "[uimodel][unit][playback]")
  {
    auto fixture = PlaybackUiFixture{};
    fixture.makePlaybackReady();
    auto& playback = fixture.runtime().playback();
    auto actions = PlaybackActions{playback, [] {}};

    auto log = ao::test::RenderLog<TransportViewState>{};
    auto viewModelPtr = std::make_unique<TransportViewModel>(playback,
                                                             actions,
                                                             ao::test::englishMessageCatalog(),
                                                             PlaybackCommand::ToggleShuffle,
                                                             false,
                                                             [&log](auto const& view) { log.render(view); });

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

  TEST_CASE("TransportViewModel - renders control semantics in the selected locale", "[uimodel][unit][playback]")
  {
    auto fixture = PlaybackUiFixture{};
    auto& playback = fixture.runtime().playback();
    auto actions = PlaybackActions{playback, [] {}};
    auto catalog = ao::test::messageCatalog("de-DE");
    auto log = ao::test::RenderLog<TransportViewState>{};
    auto viewModel = TransportViewModel{
      playback, actions, catalog, PlaybackCommand::Next, true, [&log](auto const& view) { log.render(view); }};

    REQUIRE(!log.empty());
    CHECK(log.last().tooltip == "Nächster Titel");
    CHECK(log.last().label == "Nächster Titel");
  }
} // namespace ao::uimodel::test
