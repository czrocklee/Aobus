// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/playback/PlaybackSuccession.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/runtime/PlaybackSuccessionBaseTestSupport.h"
#include <ao/audio/Transport.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <variant>

namespace ao::rt::test
{
  using playback_succession::navigationRequest;
  using playback_succession::PlaybackSuccessionFixture;

  TEST_CASE("PlaybackSuccession - List reorder updates only the live semantic tuple",
            "[runtime][unit][playback-succession][projection]")
  {
    auto fixture = PlaybackSuccessionFixture{};
    fixture.buildThreeTrackManualView();
    auto& succession = *fixture.successionPtr;
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    std::uint32_t changedCount = 0;
    auto const subscription = succession.onChanged([&](PlaybackSuccessionState const&) noexcept { ++changedCount; });
    auto const beforeMove = succession.state();

    auto const moved = fixture.moveListOrder(std::array{fixture.thirdTrackId}, fixture.secondTrackId);
    REQUIRE(moved);
    REQUIRE(moved->status == ListOrderAuthoringStatus::Applied);

    CHECK(succession.state().currentTrackId == fixture.firstTrackId);
    CHECK(succession.state().optResolvedSuccessor == fixture.thirdTrackId);
    CHECK(succession.state() != beforeMove);
    CHECK(changedCount == 1);
    CHECK(fixture.playbackTransport.state().nowPlaying.trackId == fixture.firstTrackId);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Playing);

    auto const noOp = fixture.moveListOrder(std::array{fixture.thirdTrackId}, fixture.secondTrackId);
    REQUIRE(noOp);
    CHECK(noOp->status == ListOrderAuthoringStatus::NoOp);
    CHECK(succession.state().optResolvedSuccessor == fixture.thirdTrackId);
    CHECK(changedCount == 1);
  }

  TEST_CASE("PlaybackSuccession - smart membership changes retain current audio and a gap anchor",
            "[runtime][unit][playback-succession][projection]")
  {
    auto fixture = PlaybackSuccessionFixture{};
    fixture.firstTrackId = fixture.addPlayableTrack("First", 1990);
    fixture.secondTrackId = fixture.addPlayableTrack("Second", 2000);
    fixture.thirdTrackId = fixture.addPlayableTrack("Third", 2010);
    fixture.sources.reloadAllTracks();
    fixture.listId = ao::test::requireValue(fixture.writer().createList(LibraryWriter::ListDraft{
      .name = "Recent",
      .expression = "$year >= 2000",
    }));
    fixture.viewId = ao::test::requireValue(fixture.workspace.navigate(navigationRequest(TrackListViewConfig{
      .listId = fixture.listId,
      .optPresentation =
        TrackPresentationSpec{.id = "year-order", .sortBy = {{.field = TrackSortField::Year, .ascending = true}}}})));
    fixture.successionPtr = std::make_unique<PlaybackSuccession>(fixture.executor,
                                                                 fixture.views,
                                                                 fixture.sources,
                                                                 fixture.libraryFixture.library(),
                                                                 fixture.playbackTransport,
                                                                 fixture.notifications,
                                                                 fixture.asyncRuntime);
    auto& succession = *fixture.successionPtr;
    REQUIRE(fixture.playAndWait(fixture.secondTrackId));
    auto const beforeAddition = succession.state();
    REQUIRE(beforeAddition.optResolvedSuccessor == fixture.thirdTrackId);
    std::uint32_t changedCount = 0;
    auto const subscription = succession.onChanged([&](PlaybackSuccessionState const&) noexcept { ++changedCount; });

    REQUIRE(fixture.writerFixture.updateMetadata(
      std::array{fixture.firstTrackId}, MetadataPatch{.optYear = std::uint16_t{2005}}));
    auto const afterAddition = succession.state();
    CHECK(afterAddition.currentTrackId == fixture.secondTrackId);
    CHECK(afterAddition.optResolvedSuccessor == fixture.firstTrackId);
    CHECK(afterAddition != beforeAddition);
    CHECK(changedCount == 1);
    CHECK(fixture.playbackTransport.state().nowPlaying.trackId == fixture.secondTrackId);

    REQUIRE(fixture.writerFixture.updateMetadata(
      std::array{fixture.secondTrackId}, MetadataPatch{.optYear = std::uint16_t{1990}}));
    auto const currentRemoved = succession.state();
    CHECK(currentRemoved.sourceState == PlaybackSuccessionSourceState::Live);
    CHECK(currentRemoved.currentTrackId == fixture.secondTrackId);
    CHECK(currentRemoved.optResolvedSuccessor == fixture.firstTrackId);
    CHECK(currentRemoved == afterAddition);
    CHECK(changedCount == 1);
    CHECK(fixture.playbackTransport.state().nowPlaying.trackId == fixture.secondTrackId);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSuccession - empty live repeat-one remains navigable until invalidation",
            "[runtime][unit][playback-succession][repeat]")
  {
    auto fixture = PlaybackSuccessionFixture{};
    fixture.firstTrackId = fixture.addPlayableTrack("Only");
    fixture.openManualView(std::array{fixture.firstTrackId});
    auto& succession = *fixture.successionPtr;
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    succession.setRepeatMode(RepeatMode::One);

    fixture.removeFromList(std::array{fixture.firstTrackId});
    auto const emptyLive = succession.state();
    CHECK(emptyLive.sourceState == PlaybackSuccessionSourceState::Live);
    CHECK(emptyLive.hasNext);
    CHECK(emptyLive.optResolvedSuccessor == fixture.firstTrackId);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Playing);

    REQUIRE(fixture.writer().deleteList(fixture.listId));
    auto const invalidated = succession.state();
    CHECK(invalidated.sourceState == PlaybackSuccessionSourceState::Invalidated);
    CHECK_FALSE(invalidated.hasNext);
    CHECK_FALSE(invalidated.optResolvedSuccessor);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Playing);

    succession.next();
    CHECK(succession.state().sourceState == PlaybackSuccessionSourceState::Inactive);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Idle);

    auto const feed = fixture.notifications.feed();
    REQUIRE(feed.entries.size() == 1);
    REQUIRE(std::holds_alternative<NotificationReport>(feed.entries.front().message));
    CHECK(std::get<NotificationReport>(feed.entries.front().message).templateId ==
          NotificationReportTemplate::PlaybackSequenceFinished);
    CHECK(feed.entries.front().severity == NotificationSeverity::Info);
    CHECK(feed.entries.front().lifetime == NotificationLifetime::transient());
  }

  TEST_CASE("PlaybackSuccession - empty live source has no successor with repeat off or all",
            "[runtime][unit][playback-succession][repeat]")
  {
    auto fixture = PlaybackSuccessionFixture{};
    fixture.firstTrackId = fixture.addPlayableTrack("Only");
    fixture.openManualView(std::array{fixture.firstTrackId});
    auto& succession = *fixture.successionPtr;
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));

    SECTION("repeat off")
    {
      REQUIRE(succession.state().repeat == RepeatMode::Off);
    }

    SECTION("repeat all")
    {
      succession.setRepeatMode(RepeatMode::All);
    }

    fixture.removeFromList(std::array{fixture.firstTrackId});
    CHECK(succession.state().sourceState == PlaybackSuccessionSourceState::Live);
    CHECK_FALSE(succession.state().hasNext);
    CHECK_FALSE(succession.state().optResolvedSuccessor);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Playing);

    succession.next();
    CHECK(succession.state().sourceState == PlaybackSuccessionSourceState::Inactive);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Idle);

    auto const feed = fixture.notifications.feed();
    REQUIRE(feed.entries.size() == 1);
    REQUIRE(std::holds_alternative<NotificationReport>(feed.entries.front().message));
    CHECK(std::get<NotificationReport>(feed.entries.front().message).templateId ==
          NotificationReportTemplate::PlaybackSequenceFinished);
    CHECK(feed.entries.front().severity == NotificationSeverity::Info);
    CHECK(feed.entries.front().lifetime == NotificationLifetime::transient());
  }
} // namespace ao::rt::test
