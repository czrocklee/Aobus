// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/runtime/TestUtils.h"
#include <ao/async/Runtime.h>
#include <ao/audio/Types.h>
#include <ao/rt/LibraryMutationService.h>
#include <ao/rt/ListSourceStore.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/ViewService.h>
#include <ao/uimodel/playback/NowPlayingViewModel.h>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <string>

namespace ao::uimodel::playback::test
{
  using namespace ao::rt::test;
  using namespace ao::rt;

  TEST_CASE("NowPlayingViewModel - view state generation", "[unit][uimodel][playback]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = MockExecutor{};
    auto runtime = async::Runtime{executor};
    auto mutationService = LibraryMutationService{runtime, testLib.library()};
    auto listSourceStore = ListSourceStore{testLib.library(), mutationService};
    auto viewService = ViewService{executor, testLib.library(), listSourceStore};
    auto playback = PlaybackService{executor, viewService, testLib.library()};

    auto log = RenderLog<NowPlayingViewState>{};
    auto const viewModel = NowPlayingViewModel{playback, [&log](auto const& view) { log.render(view); }};

    SECTION("Initial render when idle")
    {
      REQUIRE(!log.empty());
      CHECK(log.last().isActive == false);
      CHECK(log.last().title == "Not Playing");
    }

    SECTION("Metadata formatting")
    {
      auto desc = audio::TrackPlaybackDescriptor{
        .trackId = TrackId{1}, .title = "Song", .artist = "Artist", .duration = std::chrono::seconds{1}};

      playback.play(desc, ListId{1});
      REQUIRE(!log.empty());
      CHECK(log.last().title == "Song");
      CHECK(log.last().artist == "Artist");
      CHECK(log.last().combinedStatus == "Artist - Song");

      CHECK(NowPlayingViewModel::fieldText(log.last(), rt::TrackField::Title) == "Song");
      CHECK(NowPlayingViewModel::fieldText(log.last(), rt::TrackField::Artist) == "Artist");
    }

    SECTION("Metadata with empty artist shows Unknown Artist")
    {
      auto desc = audio::TrackPlaybackDescriptor{
        .trackId = TrackId{1}, .title = "Instrumental", .duration = std::chrono::seconds{1}};
      playback.play(desc, ListId{1});
      CHECK(log.last().title == "Instrumental");
      CHECK(log.last().artist == "Unknown Artist");
      CHECK(log.last().combinedStatus == "Instrumental");
    }

    SECTION("fieldText returns empty for unrelated field")
    {
      auto desc =
        audio::TrackPlaybackDescriptor{.trackId = TrackId{1}, .title = "Song", .duration = std::chrono::seconds{1}};
      playback.play(desc, ListId{1});
      CHECK(NowPlayingViewModel::fieldText(log.last(), rt::TrackField::Year).empty());
    }

    SECTION("Reveal action")
    {
      auto const cmd = viewModel.resolveAction(NowPlayingFieldAction::Reveal, rt::TrackField::Title);
      CHECK(cmd.type == NowPlayingActionCommand::Type::Reveal);
    }

    SECTION("PlayPause action")
    {
      auto const cmd = viewModel.resolveAction(NowPlayingFieldAction::PlayPause, rt::TrackField::Title);
      CHECK(cmd.type == NowPlayingActionCommand::Type::PlayPause);
    }

    SECTION("FilterByField with Title")
    {
      auto desc =
        audio::TrackPlaybackDescriptor{.trackId = TrackId{1}, .title = "Song", .duration = std::chrono::seconds{1}};
      playback.play(desc, ListId{1});

      auto const cmd = viewModel.resolveAction(NowPlayingFieldAction::FilterByField, rt::TrackField::Title);
      CHECK(cmd.type == NowPlayingActionCommand::Type::Navigate);
      CHECK(cmd.navigateQuery == "$title = \"Song\"");
    }

    SECTION("FilterByField with empty artist produces no action")
    {
      auto const cmd = viewModel.resolveAction(NowPlayingFieldAction::FilterByField, rt::TrackField::Artist);
      CHECK(cmd.type == NowPlayingActionCommand::Type::None);
    }

    SECTION("Quoting with both quote types replaces double quotes with single")
    {
      auto desc = audio::TrackPlaybackDescriptor{
        .trackId = TrackId{1}, .title = "A \"Song\" 'Name'", .duration = std::chrono::seconds{1}};
      playback.play(desc, ListId{1});

      auto const cmd = viewModel.resolveAction(NowPlayingFieldAction::FilterByField, rt::TrackField::Title);
      CHECK(cmd.type == NowPlayingActionCommand::Type::Navigate);
      CHECK(cmd.navigateQuery == "$title = \"A 'Song' 'Name'\"");
    }

    SECTION("Action resolution")
    {
      auto desc = audio::TrackPlaybackDescriptor{
        .trackId = TrackId{1}, .title = "Song", .artist = "Artist", .duration = std::chrono::seconds{1}};
      playback.play(desc, ListId{1});

      auto const cmd = viewModel.resolveAction(NowPlayingFieldAction::FilterByField, rt::TrackField::Artist);
      CHECK(cmd.type == NowPlayingActionCommand::Type::Navigate);
      CHECK(cmd.navigateQuery == "$artist = \"Artist\"");
    }

    SECTION("Quoting and escaping in actions")
    {
      auto desc = audio::TrackPlaybackDescriptor{
        .trackId = TrackId{1}, .title = "A \"Song\"", .duration = std::chrono::seconds{1}};
      playback.play(desc, ListId{1});

      auto const cmd = viewModel.resolveAction(NowPlayingFieldAction::FilterByField, rt::TrackField::Title);
      CHECK(cmd.navigateQuery == "$title = 'A \"Song\"'");
    }
  }
} // namespace ao::uimodel::playback::test
