// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/tui/EventControllerTestSupport.h"
#include "test/unit/tui/RenderTestSupport.h"
#include "tui/EventController.h"
#include "tui/LibraryController.h"
#include "tui/PlaybackPanel.h"
#include "tui/ShellInteractionModel.h"
#include <ao/CoreIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/screen/box.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace ao::tui::test
{
  namespace
  {
    void submitNavigation(EventController& events, std::string_view const command)
    {
      REQUIRE(events.tryHandleEvent(ftxui::Event::Character(":")));

      for (char const character : command)
      {
        REQUIRE(events.tryHandleEvent(ftxui::Event::Character(std::string{character})));
      }

      REQUIRE(events.tryHandleEvent(ftxui::Event::Return));
    }

    RenderedElement renderPlaybackMetadata(rt::PlaybackTransportSnapshot const& state,
                                           PlaybackMetadataHitRegions& hit,
                                           std::int32_t const columns)
    {
      return renderElement(
        playbackBar(ao::test::englishMessageCatalog(),
                    {.playbackState = &state, .metadataHitRegions = &hit, .terminalColumns = columns}),
        columns,
        1);
    }
  } // namespace

  TEST_CASE("LibraryController - artist links escape literal metadata and preserve filtered history",
            "[tui][unit][playback-navigation]")
  {
    auto fixture = EventControllerFixture{};
    auto const artist = std::string{R"(A "quoted" artist\with slash; B)"};
    auto const target = fixture.addTrack(library::test::TrackSpec{.title = "Target", .artist = artist});
    fixture.addTrack(library::test::TrackSpec{.title = "Other", .artist = "Someone else"});
    auto library = fixture.makeLibrary();
    library.setPresentation("songs");
    library.setFilterDraft("First");
    REQUIRE(library.applyFilter());
    library.toggleVisualSelection();
    auto const previousView = library.activeViewId();
    auto const previousFilter = fixture.runtimePtr->views().trackListState(previousView).filterExpression;

    REQUIRE(library.navigateToArtist(artist));

    CHECK(library.currentListId() == rt::kAllTracksListId);
    REQUIRE(library.tracks().size() == 1);
    CHECK(library.tracks().front().id == target);
    CHECK_FALSE(library.isVisualSelectionActive());
    CHECK(library.markedIds().empty());
    CHECK(fixture.runtimePtr->views().trackListState(previousView).filterExpression == previousFilter);
    REQUIRE(library.navigateHistory(false));
    CHECK(library.activeViewId() == previousView);
    CHECK(library.filterDraft() == previousFilter);
    CHECK(library.activePresentationId() == "songs");
    REQUIRE(library.tracks().size() == 1);
    CHECK(library.tracks().front().row.title == "First");
    REQUIRE(library.navigateHistory(true));
    REQUIRE(library.tracks().size() == 1);
    CHECK(library.tracks().front().id == target);
  }

  TEST_CASE("LibraryController - album links reveal the subject in album groups and restore the previous view",
            "[tui][unit][playback-navigation]")
  {
    auto fixture = EventControllerFixture{};
    auto const target = fixture.addTrack(library::test::TrackSpec{.title = "Target", .album = "Another album"});
    auto library = fixture.makeLibrary();
    library.setPresentation("songs");
    library.setFilterDraft("First");
    REQUIRE(library.applyFilter());
    auto const previousView = library.activeViewId();
    auto const previousFilter = fixture.runtimePtr->views().trackListState(previousView).filterExpression;

    REQUIRE(library.revealAlbum(target));

    CHECK(library.currentListId() == rt::kAllTracksListId);
    CHECK(library.activePresentationId() == "albums");
    CHECK(library.filterDraft().empty());
    CHECK(library.tracks().size() == 3);
    REQUIRE(library.selectedTrackView().track != nullptr);
    CHECK(library.selectedTrackView().track->id == target);
    CHECK(library.sections().size() == 2);
    REQUIRE(library.navigateHistory(false));
    CHECK(library.activeViewId() == previousView);
    CHECK(library.activePresentationId() == "songs");
    CHECK(library.filterDraft() == previousFilter);
  }

  TEST_CASE("LibraryController - absent metadata targets leave navigation and marks untouched",
            "[tui][unit][playback-navigation]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    library.toggleVisualSelection();
    auto const workspace = fixture.runtimePtr->workspace().snapshot();
    auto const marks = library.markedIds();

    CHECK_FALSE(library.navigateToArtist(""));
    CHECK_FALSE(library.revealAlbum(kInvalidTrackId));
    CHECK_FALSE(library.revealAlbum(TrackId{999999}));

    CHECK(fixture.runtimePtr->workspace().snapshot() == workspace);
    CHECK(library.isVisualSelectionActive());
    CHECK(library.markedIds() == marks);
  }

  TEST_CASE("PlaybackPanel - metadata links follow painted text at normal and constrained widths",
            "[tui][unit][playback-navigation]")
  {
    auto state = rt::PlaybackTransportSnapshot{
      .nowPlaying = {.trackId = TrackId{42}, .title = "曲目 Title", .artist = "藝人 Artist", .album = "專輯 Album"},
    };
    auto hit = PlaybackMetadataHitRegions{};

    for (auto const columns : {140, 80, 48, 24})
    {
      auto const rendered = renderPlaybackMetadata(state, hit, columns);
      INFO(rendered.text);
      CHECK(hit.nowPlaying == state.nowPlaying);

      for (auto const& box : {hit.title, hit.artist, hit.album})
      {
        if (box.IsEmpty())
        {
          continue;
        }

        CHECK(box.x_min >= 0);
        CHECK(box.x_max < columns);
        CHECK(box.y_min == 0);
        CHECK(box.y_max == 0);
        CHECK_FALSE(rendered.screen.PixelAt(box.x_min, box.y_min).underlined);
      }

      if (columns >= 80)
      {
        REQUIRE_FALSE(hit.title.IsEmpty());
        REQUIRE_FALSE(hit.artist.IsEmpty());
        REQUIRE_FALSE(hit.album.IsEmpty());
        CHECK(hit.title.x_max < hit.artist.x_min);
        CHECK(hit.artist.x_max < hit.album.x_min);
      }
    }

    state.nowPlaying = {};
    auto const idle = renderPlaybackMetadata(state, hit, 140);
    CHECK_FALSE(idle.text.empty());
    CHECK(hit.title.IsEmpty());
    CHECK(hit.artist.IsEmpty());
    CHECK(hit.album.IsEmpty());
    state.nowPlaying = {.trackId = TrackId{43}, .title = "Title only"};
    auto const sparse = renderPlaybackMetadata(state, hit, 140);
    CHECK(sparse.text.contains("Title only"));
    CHECK_FALSE(hit.title.IsEmpty());
    CHECK(hit.artist.IsEmpty());
    CHECK(hit.album.IsEmpty());
  }

  TEST_CASE("EventController - playback metadata mouse and command navigation agree",
            "[tui][integration][playback-navigation]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto events = fixture.makeEvents(library);
    fixture.addReadyAudioProvider();
    auto& playback = fixture.runtimePtr->playback();
    auto const target = library.tracks().front().id;
    REQUIRE(playback.commands().startFromView(library.activeViewId(), target));
    REQUIRE(fixture.tryWaitForPlayback(target));
    library.setFilterDraft("Second");
    REQUIRE(library.applyFilter());
    auto const previousView = library.activeViewId();
    auto const previousFilter = fixture.runtimePtr->views().trackListState(previousView).filterExpression;
    auto const rendered =
      renderPlaybackMetadata(playback.snapshot().transport, fixture.hitRegions.playbackMetadata, 140);
    INFO(rendered.text);

    SECTION("Title click reveals the playback subject")
    {
      REQUIRE(events.tryHandleEvent(clickBox(fixture.hitRegions.playbackMetadata.title)));
      fixture.executor->drain();
      REQUIRE(library.selectedTrackView().track != nullptr);
      CHECK(library.selectedTrackView().track->id == target);
    }

    SECTION("Artist click navigates all tracks")
    {
      REQUIRE(events.tryHandleEvent(clickBox(fixture.hitRegions.playbackMetadata.artist)));
      CHECK(library.filterDraft() == "$artist = \"Artist\"");
      CHECK(library.tracks().size() == 2);
    }

    SECTION("Artist command navigates all tracks")
    {
      submitNavigation(events, "artist");
      CHECK(library.filterDraft() == "$artist = \"Artist\"");
      CHECK(library.tracks().size() == 2);
    }

    SECTION("Album click reveals the album group")
    {
      REQUIRE(events.tryHandleEvent(clickBox(fixture.hitRegions.playbackMetadata.album)));
      CHECK(library.activePresentationId() == "albums");
      REQUIRE(library.selectedTrackView().track != nullptr);
      CHECK(library.selectedTrackView().track->id == target);
    }

    SECTION("Album command reveals the album group")
    {
      submitNavigation(events, "album");
      CHECK(library.activePresentationId() == "albums");
      REQUIRE(library.selectedTrackView().track != nullptr);
      CHECK(library.selectedTrackView().track->id == target);
    }

    CHECK(playback.snapshot().transport.nowPlaying.trackId == target);
    REQUIRE(library.navigateHistory(false));
    CHECK(library.activeViewId() == previousView);
    CHECK(library.filterDraft() == previousFilter);
  }

  TEST_CASE("EventController - stale playback metadata and modal input cannot navigate",
            "[tui][regression][playback-navigation]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto events = fixture.makeEvents(library);
    fixture.addReadyAudioProvider();
    auto& playback = fixture.runtimePtr->playback();
    auto const target = library.tracks().front().id;
    REQUIRE(playback.commands().startFromView(library.activeViewId(), target));
    REQUIRE(fixture.tryWaitForPlayback(target));
    auto const rendered =
      renderPlaybackMetadata(playback.snapshot().transport, fixture.hitRegions.playbackMetadata, 140);
    INFO(rendered.text);
    auto const before = fixture.runtimePtr->workspace().snapshot();

    SECTION("A new playback subject invalidates all old links")
    {
      auto const next = library.tracks().back().id;
      REQUIRE(playback.commands().startFromView(library.activeViewId(), next));
      REQUIRE(fixture.tryWaitForPlayback(next));
    }

    SECTION("A metadata update invalidates the old text target")
    {
      fixture.hitRegions.playbackMetadata.nowPlaying.artist = "Old artist";
    }

    SECTION("Quick Filter owns pointer input")
    {
      fixture.shell.beginInput(ShellInputMode::QuickFilter, "draft");
    }

    SECTION("Track Properties owns pointer input")
    {
      REQUIRE(events.tryHandleEvent(ftxui::Event::Character("e")));
      REQUIRE(fixture.trackEditPtr->isActive());
    }

    for (auto const& box : {fixture.hitRegions.playbackMetadata.title,
                            fixture.hitRegions.playbackMetadata.artist,
                            fixture.hitRegions.playbackMetadata.album})
    {
      events.tryHandleEvent(clickBox(box));
      fixture.executor->drain();
      CHECK(fixture.runtimePtr->workspace().snapshot() == before);
    }
  }
} // namespace ao::tui::test
