// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/playback/PlaybackSuccession.h"
#include "runtime/playback/PlaybackTransport.h"
#include "test/TestFixtureSupport.h"
#include "test/audio/BackendTestSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/runtime/PlaybackSuccessionBaseTestSupport.h"
#include "test/runtime/PlaybackSuccessionTransportTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/Transport.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/projection/TrackListProjection.h>

#include <catch2/catch_test_macros.hpp>
#include <fakeit.hpp>
#include <gsl-lite/gsl-lite.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <tuple>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    using playback_succession::PlaybackSuccessionTransportFixture;
    using playback_succession::PlaybackSuccessionTransportFixtureConfig;
    using playback_succession::playFromViewAndWait;

    // Each step needs only one queued EOF. Bound both a runaway repeat and a missing callback.
    void drivePastEmpty(PlaybackSuccessionTransportFixture& fixture, auto ready)
    {
      std::size_t ends = 0;
      auto const idle = fixture.transport.playbackTransport.onIdle([&] { ++ends; });
      auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
      REQUIRE(fixture.transport.executor.tryDrainUntil(
        [&]
        {
          return ready() || fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Inactive ||
                 ends > 2 || std::chrono::steady_clock::now() >= deadline;
        }));
      REQUIRE(ready());
    }

    auto stopWalkOnExit(PlaybackSuccessionTransportFixture& fixture)
    {
      return gsl_lite::finally(
        [&fixture]
        {
          fixture.successionPtr->clear();
          std::ignore = fixture.transport.playbackTransport.stop();
        });
    }

    void setProjected(PlaybackSuccessionTransportFixture& fixture, TrackId const trackId, bool const included)
    {
      auto const projectionPtr = ao::test::requireValue(fixture.views.findTrackListProjection(fixture.viewId));
      TrackListProjection const& projection = *projectionPtr;
      REQUIRE(projection.indexOf(trackId).has_value() != included);
      fixture.transport.libraryFixture.updateTrack(trackId,
                                                   [included](library::test::TrackSpec& spec)
                                                   {
                                                     if (included)
                                                     {
                                                       spec.tags.emplace_back("transportorder");
                                                     }
                                                     else
                                                     {
                                                       std::erase(spec.tags, "transportorder");
                                                     }
                                                   });
      fixture.sources.reloadAllTracks();
      REQUIRE(projection.indexOf(trackId).has_value() == included);
    }

    void requirePlaying(PlaybackSuccessionTransportFixture& fixture, TrackId const trackId)
    {
      auto const& state = fixture.transport.playbackTransport.state();
      drivePastEmpty(fixture, [&] { return state.transport == audio::Transport::Playing; });
      CHECK(state.nowPlaying.trackId == trackId);
      CHECK(fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Live);
      CHECK(fixture.transport.notificationService.feed().entries.empty());
      audio::test::SpyBackend<>& backend = *fixture.transport.spyBackendPtr;
      fakeit::Verify(Method(backend.mock(), start)).Once();
    }

    void relinkToPlayable(PlaybackSuccessionTransportFixture& fixture, TrackId const trackId)
    {
      auto const uri = fixture.transport.installAudioFixture("basic_metadata.flac", "relinked.flac");
      fixture.transport.libraryFixture.updateTrack(trackId, [&](library::test::TrackSpec& spec) { spec.uri = uri; });
      fixture.sources.reloadAllTracks();
    }

    void requireFinished(PlaybackSuccessionTransportFixture& fixture)
    {
      drivePastEmpty(
        fixture, [&] { return fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Inactive; });
      CHECK(fixture.transport.playbackTransport.state().transport == audio::Transport::Idle);
      auto const feed = fixture.transport.notificationService.feed();
      REQUIRE(feed.entries.size() == 1);
      auto const* report = std::get_if<NotificationReport>(&feed.entries.front().message);
      REQUIRE(report != nullptr);
      CHECK(report->templateId == NotificationReportTemplate::PlaybackSequenceFinished);
      audio::test::SpyBackend<>& backend = *fixture.transport.spyBackendPtr;
      fakeit::Verify(Method(backend.mock(), start)).Never();
    }
  } // namespace

  TEST_CASE("PlaybackSuccession - empty Repeat One retries a changed playback input but not a changed title",
            "[concurrency]")
  {
    auto fixture = PlaybackSuccessionTransportFixture{PlaybackSuccessionTransportFixtureConfig{
      .immediateEofFileNames = {"transport-playable-0.flac"},
    }};
    auto const trackId = fixture.addPlayableTrack("Empty");
    fixture.openManualView(std::array{trackId});
    fixture.commandsFixture.releaseLibrary(); // The between-turn edits below need the exclusive writer.
    auto& succession = *fixture.successionPtr;
    succession.setRepeatMode(RepeatMode::One);
    auto const cleanup = stopWalkOnExit(fixture);
    REQUIRE(playFromViewAndWait(succession, fixture.transport.executor, fixture.viewId, trackId));
    REQUIRE(succession.state().currentTrackId == trackId);
    audio::test::SpyBackend<>& backend = *fixture.transport.spyBackendPtr;
    fakeit::Verify(Method(backend.mock(), open)).Once();
    fakeit::Verify(Method(backend.mock(), start)).Never();

    // The accepted occurrence opened the original file. Change the library before its queued EOF is delivered.
    SECTION("the same TrackId now resolves to playable audio")
    {
      relinkToPlayable(fixture, trackId);
      requirePlaying(fixture, trackId);
      fakeit::Verify(Method(backend.mock(), open)).Exactly(2);
    }

    SECTION("the same input still exhausts normally after a metadata change")
    {
      fixture.transport.libraryFixture.updateTrack(
        trackId, [](library::test::TrackSpec& spec) { spec.title = "Retitled"; });
      fixture.sources.reloadAllTracks();
      requireFinished(fixture);
      fakeit::Verify(Method(backend.mock(), open)).Once();
    }

    SECTION("Repeat One still resolves the current track after its row leaves the projection")
    {
      setProjected(fixture, trackId, false);
      REQUIRE(succession.state().optResolvedSuccessor == trackId);

      SECTION("the unchanged gap current finishes without reopening")
      {
        requireFinished(fixture);
        fakeit::Verify(Method(backend.mock(), open)).Once();
      }

      SECTION("the relinked gap current starts its new input")
      {
        relinkToPlayable(fixture, trackId);
        requirePlaying(fixture, trackId);
        fakeit::Verify(Method(backend.mock(), open)).Exactly(2);
      }
    }
  }

  TEST_CASE("PlaybackSuccession - empty Repeat All retries only the visited track that left and rejoined",
            "[concurrency]")
  {
    auto fixture = PlaybackSuccessionTransportFixture{PlaybackSuccessionTransportFixtureConfig{
      .immediateEofFileNames = {"transport-playable-0.flac", "transport-playable-1.flac", "transport-playable-2.flac"},
    }};
    fixture.buildThreeTrackManualView();
    fixture.commandsFixture.releaseLibrary();
    auto& succession = *fixture.successionPtr;
    auto const& playback = fixture.transport.playbackTransport.state();
    auto const advanceTo = [&](TrackId const trackId)
    {
      auto const previous = playback.occurrenceId;
      drivePastEmpty(fixture, [&] { return playback.occurrenceId != previous; });
      REQUIRE(playback.occurrenceId != PlaybackOccurrenceId{});
      REQUIRE(playback.nowPlaying.trackId == trackId);
    };
    auto const inputForSecond = [&]
    {
      auto const requestRes =
        playbackRequestForTrack(fixture.transport.libraryFixture.library(), fixture.secondTrackId);
      return ao::test::requireValue(requestRes).input;
    };
    succession.setRepeatMode(RepeatMode::All);
    auto const cleanup = stopWalkOnExit(fixture);
    REQUIRE(playFromViewAndWait(succession, fixture.transport.executor, fixture.viewId, fixture.firstTrackId));
    REQUIRE(playback.nowPlaying.trackId == fixture.firstTrackId);
    advanceTo(fixture.secondTrackId);
    advanceTo(fixture.thirdTrackId);
    auto const thirdOccurrence = playback.occurrenceId;
    auto const originalInput = inputForSecond();

    // Publish two distinct membership changes without pumping the third occurrence's queued EOF.
    setProjected(fixture, fixture.secondTrackId, false);
    setProjected(fixture, fixture.secondTrackId, true);
    REQUIRE(inputForSecond() == originalInput);
    REQUIRE(playback.occurrenceId == thirdOccurrence);
    REQUIRE(succession.state().optResolvedSuccessor == fixture.firstTrackId);

    advanceTo(fixture.secondTrackId);
    requireFinished(fixture);
    audio::test::SpyBackend<>& backend = *fixture.transport.spyBackendPtr;
    fakeit::Verify(Method(backend.mock(), open)).Exactly(4);
  }

  TEST_CASE("PlaybackSuccession - empty Repeat All rechecks eligible inputs on wrap", "[concurrency]")
  {
    auto fixture = PlaybackSuccessionTransportFixture{PlaybackSuccessionTransportFixtureConfig{
      .immediateEofFileNames = {"transport-playable-0.flac", "transport-playable-2.flac"},
    }};
    auto const first = fixture.addPlayableTrack("Empty first");
    auto const middle = fixture.addPlayableTrack("Playable middle");
    auto const last = fixture.addPlayableTrack("Empty last");
    fixture.openManualView(std::array{first, last});
    fixture.commandsFixture.releaseLibrary();
    auto& succession = *fixture.successionPtr;
    succession.setRepeatMode(RepeatMode::All);
    REQUIRE(playFromViewAndWait(succession, fixture.transport.executor, fixture.viewId, first));
    drivePastEmpty(fixture, [&] { return succession.state().currentTrackId == last; });

    audio::test::SpyBackend<>& backend = *fixture.transport.spyBackendPtr;
    fakeit::Verify(Method(backend.mock(), open)).Exactly(2);
    fakeit::Verify(Method(backend.mock(), start)).Never();

    SECTION("the previously visited row now resolves to playable audio")
    {
      relinkToPlayable(fixture, first);
      requirePlaying(fixture, first);
      fakeit::Verify(Method(backend.mock(), open)).Exactly(3);
    }

    SECTION("a title-only change does not reopen the previously visited row")
    {
      fixture.transport.libraryFixture.updateTrack(
        first, [](library::test::TrackSpec& spec) { spec.title = "Retitled"; });
      fixture.sources.reloadAllTracks();
      requireFinished(fixture);
      fakeit::Verify(Method(backend.mock(), open)).Exactly(2);
    }

    SECTION("a newly eligible row is reachable beyond the unchanged visited row")
    {
      fixture.transport.libraryFixture.updateTrack(
        middle, [](library::test::TrackSpec& spec) { spec.tags.emplace_back("transportorder"); });
      fixture.sources.reloadAllTracks();
      requirePlaying(fixture, middle);
      fakeit::Verify(Method(backend.mock(), open)).Exactly(3);
    }
  }
} // namespace ao::rt::test
