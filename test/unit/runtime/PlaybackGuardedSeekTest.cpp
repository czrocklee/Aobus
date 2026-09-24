// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/playback/PlaybackTransport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/ApplicationPlaybackTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    PlaybackTransport::PlaybackRequest installPlaybackRequest(ApplicationPlaybackFixtureT<ManualExecutor>& fixture,
                                                              std::string const& title)
    {
      auto const uri =
        audio::test::installAudioFixture(fixture.libraryFixture.root(), "basic_metadata.flac", "guarded-seek.flac");
      auto spec = library::test::TrackSpec{};
      spec.title = title;
      spec.uri = uri;
      spec.duration = std::chrono::minutes{3};
      auto const trackId = fixture.libraryFixture.addTrack(spec);
      auto requestRes = playbackRequestForTrack(fixture.libraryFixture.library(), trackId);
      REQUIRE(requestRes);
      return std::move(*requestRes);
    }

    void preparePlayback(ApplicationPlaybackFixtureT<ManualExecutor>& fixture)
    {
      fixture.addReadyProvider();
      fixture.executor.runUntilIdle();
    }
  } // namespace

  TEST_CASE("PlaybackCommands guarded seek - same-track lower replay rejects a stale published occurrence",
            "[runtime][unit][playback][async]")
  {
    auto fixture = ApplicationPlaybackFixtureT<ManualExecutor>{};
    preparePlayback(fixture);
    auto const request = installPlaybackRequest(fixture, "Repeated subject");
    REQUIRE(request.input.duration > std::chrono::milliseconds{100});
    REQUIRE(fixture.playbackTransport.play(request, ListId{7}));
    fixture.executor.runUntilIdle();

    auto const published = fixture.playback.snapshot();
    REQUIRE(published.transport.occurrenceId.value != 0);
    auto previews = std::vector<std::chrono::milliseconds>{};
    auto const previewSubscription = fixture.playback.events().onSeekPreview(
      [&previews](std::chrono::milliseconds const elapsed) noexcept { previews.push_back(elapsed); });

    // Replay through the lower collaborator. Transport commits a fresh
    // occurrence synchronously while PlaybackService still exposes the prior
    // same-TrackId snapshot until its deferred publication runs.
    REQUIRE(fixture.playbackTransport.play(request, ListId{7}));
    auto const lowerOccurrence = fixture.playbackTransport.state().occurrenceId;
    REQUIRE(lowerOccurrence != published.transport.occurrenceId);
    CHECK(fixture.playback.snapshot().transport.occurrenceId == published.transport.occurrenceId);
    CHECK(fixture.playback.snapshot().transport.nowPlaying.trackId ==
          fixture.playbackTransport.state().nowPlaying.trackId);

    auto const lowerElapsed = fixture.playbackTransport.state().elapsed;

    SECTION("synchronous final seek")
    {
      CHECK_FALSE(fixture.commands().trySeek(published.transport.occurrenceId, std::chrono::milliseconds{100}));
    }

    SECTION("preview seek")
    {
      fixture.commands().seek(
        published.transport.occurrenceId, std::chrono::milliseconds{100}, PlaybackSeekMode::Preview);
    }

    CHECK(fixture.playbackTransport.state().elapsed == lowerElapsed);
    CHECK(fixture.playback.snapshot().transport.occurrenceId == lowerOccurrence);
    CHECK(fixture.playback.snapshot().transport.finalSeekRevision == published.transport.finalSeekRevision);
    CHECK(fixture.playback.snapshot().transport.elapsed != std::chrono::milliseconds{100});
    CHECK(previews.empty());

    fixture.executor.runUntilIdle();
    CHECK(fixture.playback.snapshot().transport.occurrenceId == lowerOccurrence);
    CHECK(fixture.playback.snapshot().transport.finalSeekRevision == published.transport.finalSeekRevision);
    CHECK(previews.empty());
  }

  TEST_CASE("PlaybackCommands trySeek - valid occurrence accepts previews and consecutive final seeks",
            "[runtime][unit][playback][seek]")
  {
    auto fixture = ApplicationPlaybackFixtureT<ManualExecutor>{};
    preparePlayback(fixture);
    auto const request = installPlaybackRequest(fixture, "Seekable subject");
    REQUIRE(request.input.duration > std::chrono::milliseconds{300});
    REQUIRE(fixture.playbackTransport.play(request, ListId{7}));
    fixture.executor.runUntilIdle();

    auto const started = fixture.playback.snapshot();
    auto const occurrenceId = started.transport.occurrenceId;
    REQUIRE(occurrenceId.value != 0);
    auto snapshots = std::vector<PlaybackSnapshot>{};
    auto previews = std::vector<std::chrono::milliseconds>{};
    auto const snapshotSubscription = fixture.playback.events().onSnapshot(
      [&snapshots](PlaybackSnapshot const& snapshot) noexcept { snapshots.push_back(snapshot); });
    auto const previewSubscription = fixture.playback.events().onSeekPreview(
      [&previews](std::chrono::milliseconds const elapsed) noexcept { previews.push_back(elapsed); });

    CHECK_FALSE(fixture.commands().trySeek({}, std::chrono::milliseconds{100}));
    CHECK_FALSE(fixture.commands().trySeek(occurrenceId, std::chrono::milliseconds{-1}));
    CHECK_FALSE(fixture.commands().trySeek(occurrenceId, request.input.duration + std::chrono::milliseconds{1}));
    fixture.commands().seek({}, std::chrono::milliseconds{100});
    fixture.commands().seek(occurrenceId, std::chrono::milliseconds{-1});
    fixture.commands().seek(occurrenceId, request.input.duration + std::chrono::milliseconds{1});
    fixture.commands().seek({}, std::chrono::milliseconds{100}, PlaybackSeekMode::Preview);
    fixture.commands().seek(occurrenceId, std::chrono::milliseconds{-1}, PlaybackSeekMode::Preview);
    fixture.commands().seek(
      occurrenceId, request.input.duration + std::chrono::milliseconds{1}, PlaybackSeekMode::Preview);
    CHECK(snapshots.empty());
    CHECK(previews.empty());
    CHECK(fixture.playback.snapshot().transport.finalSeekRevision == started.transport.finalSeekRevision);

    fixture.commands().seek(occurrenceId, std::chrono::milliseconds{75}, PlaybackSeekMode::Preview);
    REQUIRE(previews.size() == 1);
    CHECK(previews.front() == std::chrono::milliseconds{75});
    CHECK(snapshots.empty());
    CHECK(fixture.playback.snapshot().transport.elapsed == started.transport.elapsed);
    CHECK(fixture.playback.snapshot().transport.finalSeekRevision == started.transport.finalSeekRevision);

    fixture.commands().pause();
    CHECK(fixture.playback.snapshot().transport.occurrenceId == occurrenceId);
    snapshots.clear();

    CHECK(fixture.commands().trySeek(occurrenceId, std::chrono::milliseconds{100}));
    CHECK(fixture.commands().trySeek(occurrenceId, std::chrono::milliseconds{200}));

    REQUIRE(snapshots.size() == 2);
    CHECK(fixture.playback.snapshot().transport.occurrenceId == occurrenceId);
    CHECK(fixture.playback.snapshot().transport.elapsed == std::chrono::milliseconds{200});
    CHECK(fixture.playback.snapshot().transport.finalSeekRevision.value ==
          started.transport.finalSeekRevision.value + 2);
  }

  TEST_CASE("PlaybackCommands guarded seek - orthogonal backlog preserves preview and final settlement",
            "[runtime][unit][playback][async]")
  {
    auto fixture = ApplicationPlaybackFixtureT<ManualExecutor>{};
    preparePlayback(fixture);
    auto const request = installPlaybackRequest(fixture, "Backlogged subject");
    REQUIRE(request.input.duration > std::chrono::milliseconds{100});
    REQUIRE(fixture.playbackTransport.play(request, ListId{7}));
    fixture.executor.runUntilIdle();

    auto const started = fixture.playback.snapshot();
    bool queuedMute = false;
    bool finalAcceptedInsidePublication = true;
    auto previews = std::vector<std::chrono::milliseconds>{};
    auto const previewSubscription = fixture.playback.events().onSeekPreview(
      [&previews](std::chrono::milliseconds const elapsed) noexcept { previews.push_back(elapsed); });
    auto const snapshotSubscription = fixture.playback.events().onSnapshot(
      [&](PlaybackSnapshot const&) noexcept
      {
        if (!queuedMute)
        {
          queuedMute = true;
          fixture.commands().setMuted(true);
          fixture.commands().seek(
            started.transport.occurrenceId, std::chrono::milliseconds{50}, PlaybackSeekMode::Preview);
          fixture.commands().seek(started.transport.occurrenceId, std::chrono::milliseconds{100});
          finalAcceptedInsidePublication =
            fixture.commands().trySeek(started.transport.occurrenceId, std::chrono::milliseconds{75});
        }
      });

    fixture.commands().setVolume(0.5F);
    REQUIRE(queuedMute);
    CHECK_FALSE(finalAcceptedInsidePublication);
    CHECK_FALSE(fixture.commands().trySeek(started.transport.occurrenceId, std::chrono::milliseconds{150}));

    fixture.executor.runUntilIdle();
    CHECK(fixture.playback.snapshot().transport.volume.muted);
    CHECK(fixture.playback.snapshot().transport.occurrenceId == started.transport.occurrenceId);
    CHECK(fixture.playback.snapshot().transport.finalSeekRevision.value ==
          started.transport.finalSeekRevision.value + 1);
    CHECK(fixture.playback.snapshot().transport.elapsed == std::chrono::milliseconds{100});
    REQUIRE(previews.size() == 1);
    CHECK(previews.front() == std::chrono::milliseconds{50});
  }

  TEST_CASE("PlaybackService - elapsed retains its committed anchor across lower same-track replay",
            "[runtime][unit][playback][async]")
  {
    auto fixture = ApplicationPlaybackFixtureT<ManualExecutor>{};
    preparePlayback(fixture);
    auto const request = installPlaybackRequest(fixture, "Replayed clock");
    REQUIRE(fixture.playbackTransport.play(request, ListId{7}));
    fixture.executor.runUntilIdle();
    fixture.commands().seek(std::chrono::milliseconds{100});
    fixture.executor.runUntilIdle();
    auto const before = fixture.playback.snapshot().transport;
    REQUIRE(before.elapsed == std::chrono::milliseconds{100});

    REQUIRE(fixture.playbackTransport.play(request, ListId{7}));
    REQUIRE(fixture.playbackTransport.state().occurrenceId != before.occurrenceId);
    REQUIRE(fixture.playbackTransport.elapsed() == std::chrono::milliseconds{0});
    CHECK(fixture.playback.elapsed() == before.elapsed);
    CHECK(fixture.playback.snapshot().transport.occurrenceId == before.occurrenceId);
    fixture.executor.runUntilIdle();
    CHECK(fixture.playback.elapsed() == std::chrono::milliseconds{0});
  }

  TEST_CASE("PlaybackCommands guarded seek - service destruction discards already queued positioning",
            "[runtime][unit][playback][async]")
  {
    auto fixture = ApplicationPlaybackFixtureT<ManualExecutor>{};
    preparePlayback(fixture);
    auto const request = installPlaybackRequest(fixture, "Retired service");
    REQUIRE(fixture.playbackTransport.play(request, ListId{7}));
    fixture.executor.runUntilIdle();
    fixture.commands().pause();
    auto const before = fixture.playback.snapshot().transport;
    bool handedOff = false;
    auto const subscription = fixture.playback.events().onSnapshot(
      [&](PlaybackSnapshot const&)
      {
        if (!handedOff)
        {
          handedOff = true;
          fixture.commands().seek(before.occurrenceId, std::chrono::milliseconds{100});
        }
      });
    fixture.commands().setMuted(true);
    REQUIRE(handedOff);
    fixture.playbackStoragePtr.reset();
    fixture.executor.runUntilIdle();
    CHECK(fixture.playbackTransport.state().occurrenceId == before.occurrenceId);
    CHECK(fixture.playbackTransport.elapsed() == before.elapsed);
  }
} // namespace ao::rt::test
