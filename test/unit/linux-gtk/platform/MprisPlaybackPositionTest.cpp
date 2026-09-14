// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "platform/MprisBridge.h"
#include "platform/MprisPlaybackEndpoint.h"
#include "runtime/playback/PlaybackSuccession.h"
#include "test/unit/runtime/PlaybackSuccessionTransportTestSupport.h"
#include <ao/audio/RenderTarget.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/playback/command/PlaybackActions.h>

#include <catch2/catch_test_macros.hpp>
#include <glibmm/variant.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace ao::gtk::platform::test
{
  namespace
  {
    struct MprisPositionFixture final
    {
      MprisPositionFixture()
        : playback{createPlayback(application)}, actions{playback, [] {}}, endpoint{playback, actions, callbacks}
      {
        REQUIRE(application.playAndWait(application.firstTrackId));
        application.transport.executor.drain();
      }

      static rt::PlaybackService createPlayback(
        rt::test::playback_succession::PlaybackSuccessionTransportFixture& application)
      {
        application.buildThreeTrackManualView();
        return application.createPlayback();
      }

      std::chrono::milliseconds renderBlockElapsed() const
      {
        auto output = std::array<std::byte, 4096>{};
        auto* const target = application.transport.renderTarget;
        REQUIRE(target != nullptr);
        auto const rendered = target->renderPcm(output);
        REQUIRE(rendered.bytesWritten > 0);
        REQUIRE(rendered.positionFrames > 0);
        target->handlePositionAdvanced(rendered.positionFrames);
        return application.transport.playbackTransport.elapsed();
      }

      rt::test::playback_succession::PlaybackSuccessionTransportFixture application;
      rt::PlaybackService playback;
      uimodel::PlaybackActions actions;
      MprisBridge::Callbacks callbacks;
      MprisPlaybackEndpoint endpoint;
    };
  } // namespace

  TEST_CASE("MprisPlaybackEndpoint - valid SetPosition survives an orthogonal backlog",
            "[gtk][regression][mpris][concurrency]")
  {
    auto fixture = MprisPositionFixture{};
    auto& playback = fixture.playback;
    playback.commands().pause();
    playback.commands().seek(std::chrono::milliseconds{100});
    fixture.application.transport.executor.drain();
    auto const before = playback.snapshot().transport;
    auto const path = MprisBridge::trackObjectPath(before.nowPlaying.trackId, before.occurrenceId);
    auto snapshots = std::vector<rt::PlaybackTransportSnapshot>{};
    bool queuedMute = false;
    auto const subscription = playback.events().onSnapshot(
      [&](rt::PlaybackSnapshot const& snapshot)
      {
        snapshots.push_back(snapshot.transport);

        if (!queuedMute)
        {
          queuedMute = true;
          playback.commands().setMuted(true);
        }
      });

    playback.commands().setVolume(0.5F);
    REQUIRE(queuedMute);
    REQUIRE_FALSE(playback.snapshot().transport.volume.muted);
    fixture.endpoint.handleSetPosition(path, 400'000);
    fixture.application.transport.executor.drain();

    REQUIRE(snapshots.size() == 3);
    CHECK(snapshots[1].volume.muted);
    CHECK(snapshots[1].elapsed == before.elapsed);
    CHECK(snapshots[2].occurrenceId == before.occurrenceId);
    CHECK(snapshots[2].elapsed == std::chrono::milliseconds{400});
    CHECK(snapshots[2].finalSeekRevision.value == before.finalSeekRevision.value + 1);
  }

  TEST_CASE("MprisPlaybackEndpoint - relative Seek uses live playback progress", "[gtk][regression][mpris]")
  {
    auto fixture = MprisPositionFixture{};
    auto const before = fixture.playback.snapshot().transport;
    auto const liveElapsed = fixture.renderBlockElapsed();
    REQUIRE(liveElapsed > before.elapsed);
    REQUIRE(fixture.playback.snapshot().transport.elapsed == before.elapsed);

    fixture.endpoint.handleSeek(100'000);
    auto const after = fixture.playback.snapshot().transport;
    CHECK(after.occurrenceId == before.occurrenceId);
    // PCM frame conversion may truncate the published audio position by 1 ms.
    CHECK(after.elapsed <= liveElapsed + std::chrono::milliseconds{100});
    CHECK(after.elapsed >= liveElapsed + std::chrono::milliseconds{99});
    CHECK(after.finalSeekRevision.value == before.finalSeekRevision.value + 1);
  }

  TEST_CASE("MprisPlaybackEndpoint - relative past-end Seek decides from live playback progress",
            "[gtk][regression][mpris]")
  {
    auto fixture = MprisPositionFixture{};
    auto const before = fixture.playback.snapshot().transport;
    REQUIRE(fixture.renderBlockElapsed() > before.elapsed);
    REQUIRE(fixture.playback.snapshot().transport.elapsed == before.elapsed);

    // This only reaches the cached endpoint, but passes the live endpoint.
    fixture.endpoint.handleSeek(MprisBridge::microsecondsFromMilliseconds(before.duration - before.elapsed));
    fixture.application.transport.executor.drain();
    auto const after = fixture.playback.snapshot().transport;
    CHECK(after.nowPlaying.trackId == fixture.application.secondTrackId);
    CHECK(after.occurrenceId != before.occurrenceId);
    CHECK(after.finalSeekRevision == before.finalSeekRevision);
  }

  TEST_CASE("MprisPlaybackEndpoint - queued relative Seek samples at execution rather than admission",
            "[gtk][regression][mpris][concurrency]")
  {
    auto fixture = MprisPositionFixture{};
    auto& playback = fixture.playback;
    auto const before = playback.snapshot().transport;
    auto const admittedElapsed = fixture.renderBlockElapsed();
    bool handedOff = false;
    auto const subscription = playback.events().onSnapshot(
      [&](rt::PlaybackSnapshot const&)
      {
        if (!handedOff)
        {
          handedOff = true;
          playback.commands().setMuted(true);
          fixture.endpoint.handleSeek(100'000);
        }
      });
    playback.commands().setVolume(0.5F);
    REQUIRE(handedOff);
    auto const liveElapsed = fixture.renderBlockElapsed();
    REQUIRE(liveElapsed > admittedElapsed);
    fixture.application.transport.executor.drain();
    auto const after = playback.snapshot().transport;
    CHECK(after.occurrenceId == before.occurrenceId);
    CHECK(after.volume.muted);
    CHECK(after.elapsed <= liveElapsed + std::chrono::milliseconds{100});
    CHECK(after.elapsed >= liveElapsed + std::chrono::milliseconds{99});
    CHECK(after.finalSeekRevision.value == before.finalSeekRevision.value + 1);
  }

  TEST_CASE("MprisPlaybackEndpoint - queued positioning cannot act on a replayed occurrence",
            "[gtk][regression][mpris][concurrency]")
  {
    auto fixture = MprisPositionFixture{};
    auto& playback = fixture.playback;
    auto const before = playback.snapshot().transport;
    bool handedOff = false;
    auto const subscription = playback.events().onSnapshot(
      [&](rt::PlaybackSnapshot const&)
      {
        if (!handedOff)
        {
          handedOff = true;
          fixture.endpoint.handleSetPosition(
            MprisBridge::trackObjectPath(before.nowPlaying.trackId, before.occurrenceId), 400'000);
          fixture.endpoint.handleSeek(std::numeric_limits<std::int64_t>::max());
        }
      });
    playback.commands().setVolume(0.5F);
    REQUIRE(handedOff);
    REQUIRE(fixture.application.transport.playbackTransport.playTrack(
      fixture.application.firstTrackId, fixture.application.listId));
    auto const replayedOccurrence = fixture.application.transport.playbackTransport.state().occurrenceId;
    REQUIRE(replayedOccurrence != before.occurrenceId);
    fixture.application.transport.executor.drain();
    auto const after = playback.snapshot().transport;
    CHECK(after.occurrenceId == replayedOccurrence);
    CHECK(after.nowPlaying.trackId == before.nowPlaying.trackId);
    CHECK(after.elapsed == std::chrono::milliseconds{0});
    CHECK(after.finalSeekRevision == before.finalSeekRevision);
    CHECK_FALSE(MprisBridge::shouldEmitSeeked(before, after));
  }

  TEST_CASE("MprisPlaybackEndpoint - past-end Seek does not skip a pending realtime successor",
            "[gtk][regression][mpris][concurrency]")
  {
    auto fixture = MprisPositionFixture{};
    auto const before = fixture.playback.snapshot().transport;
    fixture.application.queueNaturalAdvance();
    fixture.endpoint.handleSeek(std::numeric_limits<std::int64_t>::max());
    fixture.application.transport.executor.drain();
    auto const after = fixture.playback.snapshot().transport;
    CHECK(after.nowPlaying.trackId == fixture.application.secondTrackId);
    CHECK(after.occurrenceId != before.occurrenceId);
    CHECK(after.finalSeekRevision == before.finalSeekRevision);
    CHECK(fixture.application.successionPtr->state().optResolvedSuccessor == fixture.application.thirdTrackId);
  }

  TEST_CASE("MprisPlaybackEndpoint - relative Seek preserves endpoint and overflow behavior", "[gtk][unit][mpris]")
  {
    auto fixture = MprisPositionFixture{};
    auto& playback = fixture.playback;
    playback.commands().pause();
    playback.commands().seek(std::chrono::milliseconds{100});
    auto const before = playback.snapshot().transport;
    auto expectedElapsed = before.elapsed;
    auto expectedTrackId = before.nowPlaying.trackId;
    auto expectedFinalRevision = before.finalSeekRevision.value + 1;

    SECTION("exact endpoint seeks rather than advancing")
    {
      expectedElapsed = before.duration;
      fixture.endpoint.handleSeek(MprisBridge::microsecondsFromMilliseconds(before.duration - before.elapsed));
    }

    SECTION("negative offset clamps at zero without overflow")
    {
      expectedElapsed = std::chrono::milliseconds{0};
      fixture.endpoint.handleSeek(std::numeric_limits<std::int64_t>::min());
    }

    SECTION("zero offset remains a final seek")
    {
      fixture.endpoint.handleSeek(0);
    }

    SECTION("maximum positive offset advances only once")
    {
      expectedTrackId = fixture.application.secondTrackId;
      expectedElapsed = std::chrono::milliseconds{0};
      expectedFinalRevision = before.finalSeekRevision.value;
      fixture.endpoint.handleSeek(std::numeric_limits<std::int64_t>::max());
    }

    fixture.application.transport.executor.drain();
    auto const after = playback.snapshot().transport;
    CHECK(after.nowPlaying.trackId == expectedTrackId);
    CHECK(after.elapsed == expectedElapsed);
    CHECK(after.finalSeekRevision.value == expectedFinalRevision);
  }

  TEST_CASE("MprisPlaybackEndpoint - positive past-end Seek without a successor is a no-op", "[gtk][unit][mpris]")
  {
    auto fixture = MprisPositionFixture{};
    REQUIRE(fixture.application.playAndWait(fixture.application.thirdTrackId));
    fixture.application.transport.executor.drain();
    fixture.playback.commands().pause();
    auto const before = fixture.playback.snapshot().transport;
    REQUIRE_FALSE(fixture.playback.snapshot().succession.hasNext);
    fixture.endpoint.handleSeek(std::numeric_limits<std::int64_t>::max());
    fixture.application.transport.executor.drain();
    auto const after = fixture.playback.snapshot().transport;
    CHECK(after.occurrenceId == before.occurrenceId);
    CHECK(after.elapsed == before.elapsed);
    CHECK(after.finalSeekRevision == before.finalSeekRevision);
  }

  TEST_CASE("MprisBridge - Position reads the live source instead of the snapshot anchor", "[gtk][regression][mpris]")
  {
    auto fixture = MprisPositionFixture{};
    auto snapshot = fixture.playback.snapshot();
    snapshot.transport.elapsed = std::chrono::milliseconds{100};
    auto liveElapsed = std::chrono::milliseconds{900};
    auto source = MprisBridge::PlaybackSource{
      .snapshot = [&] -> rt::PlaybackSnapshot const& { return snapshot; },
      .onSnapshot = [](rt::PlaybackSnapshotObserver) { return async::Subscription{}; },
      .elapsed = [&] { return liveElapsed; },
    };
    auto bridge = MprisBridge{fixture.playback, fixture.actions, {}, std::move(source)};

    // Read the actual exported property mapping without registering a bus name.
    CHECK(bridge.playerProperty("Position").get_dynamic<std::int64_t>() == 900'000);
    liveElapsed = std::chrono::milliseconds{1100};
    CHECK(bridge.playerProperty("Position").get_dynamic<std::int64_t>() == 1'100'000);
    CHECK(snapshot.transport.elapsed == std::chrono::milliseconds{100});
  }
} // namespace ao::gtk::platform::test
