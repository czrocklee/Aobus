// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureUtils.h"
#include <ao/CoreIds.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/Transport.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/source/ListSourceStore.h>
#include <ao/uimodel/playback/queue/PlaybackQueueModel.h>
#include <ao/uimodel/playback/transport/TransportViewModel.h>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <memory>
#include <vector>

namespace ao::uimodel::test
{
  using namespace ao::rt::test;
  using namespace ao::rt;

  TEST_CASE("TransportViewModel - view state by action", "[uimodel][unit][playback]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = MockExecutor{};
    auto changes = LibraryChanges{};
    auto listSourceStore = ListSourceStore{testLib.library(), changes};
    auto viewService = ViewService{executor, testLib.library(), listSourceStore};
    auto notificationService = NotificationService{};
    auto playback = PlaybackService{executor, viewService, testLib.library(), notificationService};

    SECTION("Play action - disabled when not ready")
    {
      auto log = ao::test::RenderLog<TransportViewState>{};
      auto vm = TransportViewModel{
        playback, nullptr, TransportAction::Play, {}, false, [&log](auto const& v) { log.render(v); }};

      REQUIRE(!log.empty());
      CHECK(log.last().enabled == false);
      CHECK(log.last().icon == TransportIcon::Play);
    }

    SECTION("Pause action - disabled when not playing")
    {
      auto log = ao::test::RenderLog<TransportViewState>{};
      auto vm = TransportViewModel{
        playback, nullptr, TransportAction::Pause, {}, false, [&log](auto const& v) { log.render(v); }};

      CHECK(log.last().enabled == false);
      CHECK(log.last().icon == TransportIcon::Pause);
    }

    SECTION("Stop action - disabled when idle")
    {
      auto log = ao::test::RenderLog<TransportViewState>{};
      auto vm = TransportViewModel{
        playback, nullptr, TransportAction::Stop, {}, false, [&log](auto const& v) { log.render(v); }};

      CHECK(log.last().enabled == false);
      CHECK(log.last().icon == TransportIcon::Stop);
    }

    SECTION("PlayPause action - shows play glyph when idle, disabled when not ready")
    {
      auto log = ao::test::RenderLog<TransportViewState>{};
      auto vm = TransportViewModel{
        playback, nullptr, TransportAction::PlayPause, {}, false, [&log](auto const& v) { log.render(v); }};

      CHECK(log.last().icon == TransportIcon::Play);
      CHECK(log.last().tooltip == "Play");
      CHECK(log.last().playing == false);
      CHECK(log.last().enabled == false);
    }

    SECTION("PlayPause action with showLabel")
    {
      auto log = ao::test::RenderLog<TransportViewState>{};
      auto vm = TransportViewModel{
        playback, nullptr, TransportAction::PlayPause, {}, true, [&log](auto const& v) { log.render(v); }};

      CHECK(log.last().label == "Play");
    }

    SECTION("Next/Previous action - disabled when not ready")
    {
      auto logNext = ao::test::RenderLog<TransportViewState>{};
      auto logPrev = ao::test::RenderLog<TransportViewState>{};
      auto vmNext = TransportViewModel{
        playback, nullptr, TransportAction::Next, {}, false, [&logNext](auto const& v) { logNext.render(v); }};
      auto vmPrev = TransportViewModel{
        playback, nullptr, TransportAction::Previous, {}, false, [&logPrev](auto const& v) { logPrev.render(v); }};

      CHECK(logNext.last().enabled == false);
      CHECK(logPrev.last().enabled == false);
    }

    SECTION("Shuffle action")
    {
      auto log = ao::test::RenderLog<TransportViewState>{};
      auto vm = TransportViewModel{
        playback, nullptr, TransportAction::Shuffle, {}, false, [&log](auto const& v) { log.render(v); }};

      CHECK(log.last().engaged == false);
      CHECK(log.last().enabled == false);
    }

    SECTION("Repeat action - Off mode")
    {
      auto log = ao::test::RenderLog<TransportViewState>{};
      auto vm = TransportViewModel{
        playback, nullptr, TransportAction::Repeat, {}, false, [&log](auto const& v) { log.render(v); }};

      CHECK(log.last().engaged == false);
      CHECK(log.last().icon == TransportIcon::Repeat);
      CHECK(log.last().enabled == false);
    }

    SECTION("Repeat action - All mode")
    {
      auto log = ao::test::RenderLog<TransportViewState>{};
      playback.setRepeatMode(rt::RepeatMode::All);
      auto vm = TransportViewModel{
        playback, nullptr, TransportAction::Repeat, {}, false, [&log](auto const& v) { log.render(v); }};

      CHECK(log.last().engaged == true);
      CHECK(log.last().icon == TransportIcon::Repeat);
    }

    SECTION("Repeat action - One mode")
    {
      auto log = ao::test::RenderLog<TransportViewState>{};
      playback.setRepeatMode(rt::RepeatMode::One);
      auto vm = TransportViewModel{
        playback, nullptr, TransportAction::Repeat, {}, false, [&log](auto const& v) { log.render(v); }};

      CHECK(log.last().engaged == true);
      CHECK(log.last().icon == TransportIcon::RepeatOne);
    }

    SECTION("Shuffle action with shuffle On")
    {
      auto log = ao::test::RenderLog<TransportViewState>{};
      playback.setShuffleMode(rt::ShuffleMode::On);
      auto vm = TransportViewModel{
        playback, nullptr, TransportAction::Shuffle, {}, false, [&log](auto const& v) { log.render(v); }};

      CHECK(log.last().engaged == true);
    }

    SECTION("showLabel=true renders label for all actions")
    {
      auto logNext = ao::test::RenderLog<TransportViewState>{};
      auto logPrev = ao::test::RenderLog<TransportViewState>{};
      auto logShuffle = ao::test::RenderLog<TransportViewState>{};
      auto logRepeat = ao::test::RenderLog<TransportViewState>{};

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

  TEST_CASE("TransportViewModel - handleClick command resolution", "[uimodel][unit][playback]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = MockExecutor{};
    auto changes = LibraryChanges{};
    auto listSourceStore = ListSourceStore{testLib.library(), changes};
    auto viewService = ViewService{executor, testLib.library(), listSourceStore};
    auto notificationService = NotificationService{};
    auto playback = PlaybackService{executor, viewService, testLib.library(), notificationService};
    addReadyAudioProvider(playback);

    bool playSelectionCalled = false;
    auto onPlaySelection = [&playSelectionCalled] { playSelectionCalled = true; };

    SECTION("Play fires onPlaySelection when idle")
    {
      auto log = ao::test::RenderLog<TransportViewState>{};
      auto vm = TransportViewModel{
        playback, nullptr, TransportAction::Play, onPlaySelection, false, [&log](auto const& v) { log.render(v); }};

      playSelectionCalled = false;
      vm.handleClick();
      CHECK(playSelectionCalled);
    }

    SECTION("PlayPause fires onPlaySelection when idle")
    {
      auto log = ao::test::RenderLog<TransportViewState>{};
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
      auto log = ao::test::RenderLog<TransportViewState>{};
      auto vm = TransportViewModel{
        playback, nullptr, TransportAction::Stop, {}, false, [&log](auto const& v) { log.render(v); }};

      vm.handleClick();
      CHECK(playback.state().transport == audio::Transport::Idle);
    }

    SECTION("Shuffle click with null queue is no-op")
    {
      auto log = ao::test::RenderLog<TransportViewState>{};
      auto vm = TransportViewModel{
        playback, nullptr, TransportAction::Shuffle, {}, false, [&log](auto const& v) { log.render(v); }};
      auto const initialCount = log.states.size();

      vm.handleClick();

      CHECK(playback.state().shuffleMode == rt::ShuffleMode::Off);
      CHECK(log.states.size() == initialCount);
      CHECK(log.last().engaged == false);
    }

    SECTION("Repeat click with null queue is no-op")
    {
      auto log = ao::test::RenderLog<TransportViewState>{};
      auto vm = TransportViewModel{
        playback, nullptr, TransportAction::Repeat, {}, false, [&log](auto const& v) { log.render(v); }};
      auto const initialCount = log.states.size();

      vm.handleClick();

      CHECK(playback.state().repeatMode == rt::RepeatMode::Off);
      CHECK(log.states.size() == initialCount);
      CHECK(log.last().engaged == false);
      CHECK(log.last().icon == TransportIcon::Repeat);
    }

    SECTION("Next/Previous with null queue is no-op")
    {
      auto log = ao::test::RenderLog<TransportViewState>{};
      auto vm = TransportViewModel{
        playback, nullptr, TransportAction::Next, {}, false, [&log](auto const& v) { log.render(v); }};
      auto const initialCount = log.states.size();

      vm.handleClick();

      CHECK(playback.state().transport == audio::Transport::Idle);
      CHECK(playback.state().trackId == kInvalidTrackId);
      CHECK(log.states.size() == initialCount);
    }

    SECTION("Next/Previous/Shuffle/CycleRepeat with queue")
    {
      auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
      auto const firstTrackId =
        testLib.addTrack({.title = "Q Test 1", .artist = "Artist", .album = "Album", .uri = fixturePath});
      auto const secondTrackId =
        testLib.addTrack({.title = "Q Test 2", .artist = "Artist", .album = "Album", .uri = fixturePath});
      auto const thirdTrackId =
        testLib.addTrack({.title = "Q Test 3", .artist = "Artist", .album = "Album", .uri = fixturePath});
      auto queueModel = PlaybackQueueModel{playback, notificationService};

      auto const trackIds = std::vector{firstTrackId, secondTrackId, thirdTrackId};
      REQUIRE(queueModel.playQueue(trackIds, firstTrackId, kInvalidListId));
      REQUIRE(queueModel.nowPlayingTrackId());
      CHECK(*queueModel.nowPlayingTrackId() == firstTrackId);

      SECTION("Next with queue")
      {
        auto log = ao::test::RenderLog<TransportViewState>{};
        auto vm = TransportViewModel{
          playback, &queueModel, TransportAction::Next, {}, false, [&log](auto const& v) { log.render(v); }};

        vm.handleClick();

        REQUIRE(queueModel.nowPlayingTrackId());
        CHECK(*queueModel.nowPlayingTrackId() == secondTrackId);
        CHECK(playback.state().trackId == secondTrackId);
        CHECK(log.last().enabled == true);
      }

      SECTION("Previous with queue")
      {
        REQUIRE(queueModel.playQueue(trackIds, secondTrackId, kInvalidListId));
        auto log = ao::test::RenderLog<TransportViewState>{};
        auto vm = TransportViewModel{
          playback, &queueModel, TransportAction::Previous, {}, false, [&log](auto const& v) { log.render(v); }};

        vm.handleClick();

        REQUIRE(queueModel.nowPlayingTrackId());
        CHECK(*queueModel.nowPlayingTrackId() == firstTrackId);
        CHECK(playback.state().trackId == firstTrackId);
        CHECK(log.last().enabled == true);
      }

      SECTION("Shuffle click with queue toggles mode")
      {
        auto log = ao::test::RenderLog<TransportViewState>{};
        auto vm = TransportViewModel{
          playback, &queueModel, TransportAction::Shuffle, {}, false, [&log](auto const& v) { log.render(v); }};
        vm.handleClick();

        CHECK(playback.state().shuffleMode == rt::ShuffleMode::On);
        CHECK(log.last().engaged == true);
        CHECK(log.last().enabled == true);
      }

      SECTION("Repeat click with queue cycles modes")
      {
        auto log = ao::test::RenderLog<TransportViewState>{};
        auto vm = TransportViewModel{
          playback, &queueModel, TransportAction::Repeat, {}, false, [&log](auto const& v) { log.render(v); }};
        vm.handleClick();

        CHECK(playback.state().repeatMode == rt::RepeatMode::All);
        CHECK(log.last().engaged == true);
        CHECK(log.last().icon == TransportIcon::Repeat);

        vm.handleClick();

        CHECK(playback.state().repeatMode == rt::RepeatMode::One);
        CHECK(log.last().engaged == true);
        CHECK(log.last().icon == TransportIcon::RepeatOne);

        vm.handleClick();

        CHECK(playback.state().repeatMode == rt::RepeatMode::Off);
        CHECK(log.last().engaged == false);
        CHECK(log.last().icon == TransportIcon::Repeat);
      }
    }
  }

  TEST_CASE("TransportViewModel - subscription callbacks refresh view", "[uimodel][unit][playback]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = MockExecutor{};
    auto changes = LibraryChanges{};
    auto listSourceStore = ListSourceStore{testLib.library(), changes};
    auto viewService = ViewService{executor, testLib.library(), listSourceStore};
    auto notificationService = NotificationService{};
    auto playback = PlaybackService{executor, viewService, testLib.library(), notificationService};
    addReadyAudioProvider(playback);

    auto const trackId = testLib.addTrack({.title = "Sub Test", .artist = "Sub Artist", .album = "Sub Album"});
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();

    auto log = ao::test::RenderLog<TransportViewState>{};
    auto vm = TransportViewModel{
      playback, nullptr, TransportAction::PlayPause, {}, false, [&log](auto const& v) { log.render(v); }};

    auto const initialCount = log.states.size();

    SECTION("playback callbacks trigger refresh")
    {
      auto desc = PlaybackService::PlaybackRequest{
        .trackId = trackId,
        .input = audio::PlaybackInput{.filePath = fixturePath, .duration = std::chrono::seconds{5}},
        .title = "Sub Test",
        .artist = "Sub Artist",
      };
      REQUIRE(playback.play(desc, kInvalidListId));

      CHECK(log.states.size() > initialCount);
      CHECK(log.last().enabled == true);
      CHECK(log.last().icon == TransportIcon::Pause);
      CHECK(log.last().tooltip == "Pause");
      CHECK(log.last().playing == true);
      CHECK(playback.state().ready == true);
      CHECK(playback.state().trackId == trackId);
    }
  }

  TEST_CASE("TransportViewModel stops rendering after destruction", "[uimodel][unit][playback]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = MockExecutor{};
    auto changes = LibraryChanges{};
    auto listSourceStore = ListSourceStore{testLib.library(), changes};
    auto viewService = ViewService{executor, testLib.library(), listSourceStore};
    auto notificationService = NotificationService{};
    auto playback = PlaybackService{executor, viewService, testLib.library(), notificationService};
    addReadyAudioProvider(playback);

    auto log = ao::test::RenderLog<TransportViewState>{};
    auto viewModelPtr = std::make_unique<TransportViewModel>(playback,
                                                             nullptr,
                                                             TransportAction::Shuffle,
                                                             std::function<void()>{},
                                                             false,
                                                             [&log](auto const& view) { log.render(view); });

    REQUIRE(!log.empty());
    log.clear();

    playback.setShuffleMode(rt::ShuffleMode::On);
    REQUIRE(log.states.size() == 1);
    CHECK(log.last().engaged == true);

    log.clear();
    viewModelPtr.reset();

    playback.setShuffleMode(rt::ShuffleMode::Off);
    CHECK(log.empty());
  }
} // namespace ao::uimodel::test
