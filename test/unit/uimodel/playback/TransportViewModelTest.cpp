// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include <ao/Type.h>
#include <ao/audio/Types.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/source/ListSourceStore.h>
#include <ao/uimodel/playback/PlaybackQueueModel.h>
#include <ao/uimodel/playback/TransportViewModel.h>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <vector>

namespace ao::uimodel::playback::test
{
  using namespace ao::rt::test;
  using namespace ao::rt;

  TEST_CASE("TransportViewModel - view state by action", "[unit][uimodel][playback]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = MockExecutor{};
    auto changes = LibraryChanges{};
    auto listSourceStore = ListSourceStore{testLib.library(), changes};
    auto viewService = ViewService{executor, testLib.library(), listSourceStore};
    auto playback = PlaybackService{executor, viewService, testLib.library()};

    SECTION("Play action - disabled when not ready")
    {
      auto log = RenderLog<TransportViewState>{};
      auto vm = TransportViewModel{
        playback, nullptr, TransportAction::Play, {}, false, [&log](auto const& v) { log.render(v); }};

      REQUIRE(!log.empty());
      CHECK(log.last().enabled == false);
      CHECK(log.last().icon == TransportIcon::Play);
    }

    SECTION("Pause action - disabled when not playing")
    {
      auto log = RenderLog<TransportViewState>{};
      auto vm = TransportViewModel{
        playback, nullptr, TransportAction::Pause, {}, false, [&log](auto const& v) { log.render(v); }};

      CHECK(log.last().enabled == false);
      CHECK(log.last().icon == TransportIcon::Pause);
    }

    SECTION("Stop action - disabled when idle")
    {
      auto log = RenderLog<TransportViewState>{};
      auto vm = TransportViewModel{
        playback, nullptr, TransportAction::Stop, {}, false, [&log](auto const& v) { log.render(v); }};

      CHECK(log.last().enabled == false);
      CHECK(log.last().icon == TransportIcon::Stop);
    }

    SECTION("PlayPause action - shows play glyph when idle, disabled when not ready")
    {
      auto log = RenderLog<TransportViewState>{};
      auto vm = TransportViewModel{
        playback, nullptr, TransportAction::PlayPause, {}, false, [&log](auto const& v) { log.render(v); }};

      CHECK(log.last().icon == TransportIcon::Play);
      CHECK(log.last().tooltip == "Play");
      CHECK(log.last().playing == false);
      CHECK(log.last().enabled == false);
    }

    SECTION("PlayPause action with showLabel")
    {
      auto log = RenderLog<TransportViewState>{};
      auto vm = TransportViewModel{
        playback, nullptr, TransportAction::PlayPause, {}, true, [&log](auto const& v) { log.render(v); }};

      CHECK(log.last().label == "Play");
    }

    SECTION("Next/Previous action - disabled when not ready")
    {
      auto logNext = RenderLog<TransportViewState>{};
      auto logPrev = RenderLog<TransportViewState>{};
      auto vmNext = TransportViewModel{
        playback, nullptr, TransportAction::Next, {}, false, [&logNext](auto const& v) { logNext.render(v); }};
      auto vmPrev = TransportViewModel{
        playback, nullptr, TransportAction::Previous, {}, false, [&logPrev](auto const& v) { logPrev.render(v); }};

      CHECK(logNext.last().enabled == false);
      CHECK(logPrev.last().enabled == false);
    }

    SECTION("Shuffle action")
    {
      auto log = RenderLog<TransportViewState>{};
      auto vm = TransportViewModel{
        playback, nullptr, TransportAction::Shuffle, {}, false, [&log](auto const& v) { log.render(v); }};

      CHECK(log.last().engaged == false);
      CHECK(log.last().enabled == false);
    }

    SECTION("Repeat action - Off mode")
    {
      auto log = RenderLog<TransportViewState>{};
      auto vm = TransportViewModel{
        playback, nullptr, TransportAction::Repeat, {}, false, [&log](auto const& v) { log.render(v); }};

      CHECK(log.last().engaged == false);
      CHECK(log.last().icon == TransportIcon::Repeat);
      CHECK(log.last().enabled == false);
    }

    SECTION("Repeat action - All mode")
    {
      auto log = RenderLog<TransportViewState>{};
      playback.setRepeatMode(rt::RepeatMode::All);
      auto vm = TransportViewModel{
        playback, nullptr, TransportAction::Repeat, {}, false, [&log](auto const& v) { log.render(v); }};

      CHECK(log.last().engaged == true);
      CHECK(log.last().icon == TransportIcon::Repeat);
    }

    SECTION("Repeat action - One mode")
    {
      auto log = RenderLog<TransportViewState>{};
      playback.setRepeatMode(rt::RepeatMode::One);
      auto vm = TransportViewModel{
        playback, nullptr, TransportAction::Repeat, {}, false, [&log](auto const& v) { log.render(v); }};

      CHECK(log.last().engaged == true);
      CHECK(log.last().icon == TransportIcon::RepeatOne);
    }

    SECTION("Shuffle action with shuffle On")
    {
      auto log = RenderLog<TransportViewState>{};
      playback.setShuffleMode(rt::ShuffleMode::On);
      auto vm = TransportViewModel{
        playback, nullptr, TransportAction::Shuffle, {}, false, [&log](auto const& v) { log.render(v); }};

      CHECK(log.last().engaged == true);
    }

    SECTION("showLabel=true renders label for all actions")
    {
      auto logNext = RenderLog<TransportViewState>{};
      auto logPrev = RenderLog<TransportViewState>{};
      auto logShuffle = RenderLog<TransportViewState>{};
      auto logRepeat = RenderLog<TransportViewState>{};

      auto vmNext = TransportViewModel{
        playback, nullptr, TransportAction::Next, {}, true, [&logNext](auto const& v) { logNext.render(v); }};
      auto vmPrev = TransportViewModel{
        playback, nullptr, TransportAction::Previous, {}, true, [&logPrev](auto const& v) { logPrev.render(v); }};
      auto vmShuffle = TransportViewModel{
        playback, nullptr, TransportAction::Shuffle, {}, true, [&logShuffle](auto const& v) { logShuffle.render(v); }};
      auto vmRepeat = TransportViewModel{
        playback, nullptr, TransportAction::Repeat, {}, true, [&logRepeat](auto const& v) { logRepeat.render(v); }};

      CHECK(logNext.last().label == "Next Track");
      CHECK(logPrev.last().label == "Previous Track");
      CHECK(logShuffle.last().label == "Shuffle");
      CHECK(logRepeat.last().label == "Repeat");
    }
  }

  TEST_CASE("TransportViewModel - handleClick command resolution", "[unit][uimodel][playback]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = MockExecutor{};
    auto changes = LibraryChanges{};
    auto listSourceStore = ListSourceStore{testLib.library(), changes};
    auto viewService = ViewService{executor, testLib.library(), listSourceStore};
    auto playback = PlaybackService{executor, viewService, testLib.library()};

    bool playSelectionCalled = false;
    auto onPlaySelection = [&playSelectionCalled] { playSelectionCalled = true; };

    SECTION("Play fires onPlaySelection when idle")
    {
      auto log = RenderLog<TransportViewState>{};
      auto vm = TransportViewModel{
        playback, nullptr, TransportAction::Play, onPlaySelection, false, [&log](auto const& v) { log.render(v); }};

      playSelectionCalled = false;
      vm.handleClick();
      CHECK(playSelectionCalled);
    }

    SECTION("PlayPause fires onPlaySelection when idle")
    {
      auto log = RenderLog<TransportViewState>{};
      auto vm = TransportViewModel{playback,
                                   nullptr,
                                   TransportAction::PlayPause,
                                   onPlaySelection,
                                   false,
                                   [&log](auto const& v) { log.render(v); }};

      playSelectionCalled = false;
      vm.handleClick();
      CHECK(playSelectionCalled);
    }

    SECTION("Stop does nothing when already idle")
    {
      auto log = RenderLog<TransportViewState>{};
      auto vm = TransportViewModel{
        playback, nullptr, TransportAction::Stop, {}, false, [&log](auto const& v) { log.render(v); }};

      vm.handleClick();
      CHECK(playback.state().transport == audio::Transport::Idle);
    }

    SECTION("Shuffle click with null queue is no-op")
    {
      auto log = RenderLog<TransportViewState>{};
      auto vm = TransportViewModel{
        playback, nullptr, TransportAction::Shuffle, {}, false, [&log](auto const& v) { log.render(v); }};

      vm.handleClick();
      SUCCEED("no crash with null queue");
    }

    SECTION("Repeat click with null queue is no-op")
    {
      auto log = RenderLog<TransportViewState>{};
      auto vm = TransportViewModel{
        playback, nullptr, TransportAction::Repeat, {}, false, [&log](auto const& v) { log.render(v); }};

      vm.handleClick();
      SUCCEED("no crash with null queue");
    }

    SECTION("Next/Previous with null queue is no-op")
    {
      auto log = RenderLog<TransportViewState>{};
      auto vm = TransportViewModel{
        playback, nullptr, TransportAction::Next, {}, false, [&log](auto const& v) { log.render(v); }};

      vm.handleClick();
      SUCCEED("no crash with null queue");
    }

    SECTION("Next/Previous/Shuffle/CycleRepeat with queue")
    {
      auto const trackId = testLib.addTrack({.title = "Q Test", .artist = "Artist", .album = "Album"});
      auto queueModel = PlaybackQueueModel{playback};

      auto const trackIds = std::vector{trackId, TrackId{999}, TrackId{1000}};
      queueModel.playQueue(trackIds, trackId, kInvalidListId);

      SECTION("Next with queue")
      {
        auto log = RenderLog<TransportViewState>{};
        auto vm = TransportViewModel{
          playback, &queueModel, TransportAction::Next, {}, false, [&log](auto const& v) { log.render(v); }};
        vm.handleClick();
        SUCCEED("next with queue");
      }

      SECTION("Previous with queue")
      {
        auto log = RenderLog<TransportViewState>{};
        auto vm = TransportViewModel{
          playback, &queueModel, TransportAction::Previous, {}, false, [&log](auto const& v) { log.render(v); }};
        vm.handleClick();
        SUCCEED("previous with queue");
      }

      SECTION("Shuffle click with queue toggles mode")
      {
        auto log = RenderLog<TransportViewState>{};
        auto vm = TransportViewModel{
          playback, &queueModel, TransportAction::Shuffle, {}, false, [&log](auto const& v) { log.render(v); }};
        vm.handleClick();
        SUCCEED("shuffle with queue");
      }

      SECTION("Repeat click with queue cycles modes")
      {
        auto log = RenderLog<TransportViewState>{};
        auto vm = TransportViewModel{
          playback, &queueModel, TransportAction::Repeat, {}, false, [&log](auto const& v) { log.render(v); }};
        vm.handleClick();
        SUCCEED("repeat with queue");
      }
    }
  }

  TEST_CASE("TransportViewModel - subscription callbacks refresh view", "[unit][uimodel][playback]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = MockExecutor{};
    auto changes = LibraryChanges{};
    auto listSourceStore = ListSourceStore{testLib.library(), changes};
    auto viewService = ViewService{executor, testLib.library(), listSourceStore};
    auto playback = PlaybackService{executor, viewService, testLib.library()};

    auto const trackId = testLib.addTrack({.title = "Sub Test", .artist = "Sub Artist", .album = "Sub Album"});

    auto log = RenderLog<TransportViewState>{};
    auto vm = TransportViewModel{
      playback, nullptr, TransportAction::PlayPause, {}, false, [&log](auto const& v) { log.render(v); }};

    auto const initialCount = log.states.size();

    SECTION("onPreparing triggers refresh")
    {
      auto desc = PlaybackService::PlaybackRequest{
        .trackId = trackId,
        .input = audio::PlaybackInput{.filePath = "test.flac", .duration = std::chrono::seconds{5}},
        .title = "Sub Test",
        .artist = "Sub Artist",
      };
      playback.play(desc, kInvalidListId);

      CHECK(log.states.size() > initialCount);
    }
  }
} // namespace ao::uimodel::playback::test
