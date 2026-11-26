// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/playback/PlaybackSuccession.h"
#include "runtime/playback/PlaybackTransport.h"
#include "test/audio/BackendTestSupport.h"
#include "test/audio/EngineTestSupport.h"
#include "test/runtime/PlaybackSuccessionTransportTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/Transport.h>
#include <ao/rt/NotificationIds.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/playback/PlaybackEvents.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <fakeit.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    using ReportCount = std::pair<NotificationReportTemplate, std::size_t>;
    constexpr auto kSkipped = NotificationReportTemplate::PlaybackTracksSkipped;
    constexpr auto kLimit = NotificationReportTemplate::PlaybackStoppedAfterFailures;

    NotificationReport const* changedReport(NotificationFeedUpdate const& update)
    {
      auto const& entries = update.feedPtr->entries;
      auto const it = std::ranges::find(entries, update.id, &NotificationEntry::id);
      return it == entries.end() ? nullptr : std::get_if<NotificationReport>(&it->message);
    }

    struct FailureObserverFixture final
    {
      explicit FailureObserverFixture(std::vector<std::filesystem::path> finalOpenFailures = {})
        : tracks{playback_succession::PlaybackSuccessionTransportFixtureConfig{
            .finalOpenFailureFileNames = std::move(finalOpenFailures),
            .stagedFailureFileNames = {"transport-playable-0.flac", "transport-playable-2.flac"},
            .stagedFailureGate = &gate,
            // Lossy B cannot claim the shared read gate as a prepared successor.
            .lossyFileName = "transport-playable-2.flac",
            .immediateEofFileName = "transport-playable-1.flac",
          }}
      {
        audio::test::SpyBackend<>& backend = *tracks.transport.spyBackendPtr;
        fakeit::When(Method(backend.mock(), start))
          .AlwaysDo([this] { starts.fetch_add(1, std::memory_order_relaxed); });
      }

      PlaybackService createPlayback(bool throughEmpty = false)
      {
        tracks.firstTrackId = tracks.addPlayableTrack("A");
        auto const empty = tracks.addPlayableTrack("Empty");
        tracks.secondTrackId = tracks.addPlayableTrack("B");
        tracks.thirdTrackId = tracks.addPlayableTrack("C");
        tracks.fourthTrackId = tracks.addPlayableTrack("D");
        auto projection =
          std::vector{tracks.firstTrackId, tracks.secondTrackId, tracks.thirdTrackId, tracks.fourthTrackId};

        if (throughEmpty)
        {
          emptyTrackId = empty;
          projection.insert(projection.begin() + 1, empty);
        }

        tracks.openManualView(projection);
        return tracks.createPlayback();
      }

      void issueOnReport(PlaybackService& playback, NotificationReportTemplate trigger, auto issueCommand)
      {
        auto guard = audio::test::StagedFailureReleaseGuard{gate};
        auto postedSkipId = kInvalidNotificationId;
        bool sawGroupedSkip = false;
        bool issued = false;
        bool publishedInline = false;
        bool startedInline = false;
        std::size_t publications = 0;
        auto const snapshots = playback.events().onSnapshot([&](PlaybackSnapshot const&) { ++publications; });
        auto const subscription = tracks.transport.notificationService.onFeedUpdated(
          [&](NotificationFeedUpdate const& update)
          {
            auto const* report = changedReport(update);

            if (issued || report == nullptr)
            {
              return;
            }

            auto const counted = ReportCount{report->templateId, report->count};
            auto const posted = update.mutationKind == NotificationFeedMutationKind::Posted;

            if (posted && counted == ReportCount{kSkipped, 1})
            {
              postedSkipId = update.id;
              return;
            }

            if (update.mutationKind == NotificationFeedMutationKind::ReportUpdated && update.id == postedSkipId &&
                counted == ReportCount{kSkipped, 2})
            {
              sawGroupedSkip = true;
            }
            else if (!posted || counted != ReportCount{kLimit, 3})
            {
              return;
            }

            if (!sawGroupedSkip || report->templateId != trigger)
            {
              return;
            }

            issued = true;
            auto const before = publications;
            auto const startsBefore = starts.load(std::memory_order_relaxed);
            issueCommand();
            startedInline = starts.load(std::memory_order_relaxed) != startsBefore;
            publishedInline = publications != before;
          });

        auto const& lower = tracks.transport.playbackTransport.state();
        bool sawEmptyOccurrence = false;
        auto const changed = tracks.successionPtr->onChanged(
          [&](PlaybackSuccessionState const& state)
          {
            if (emptyTrackId != kInvalidTrackId && state.currentTrackId == emptyTrackId &&
                lower.nowPlaying.trackId == emptyTrackId && lower.occurrenceId != PlaybackOccurrenceId{})
            {
              sawEmptyOccurrence = true;
            }
          });
        REQUIRE(playback.commands().startFromView(tracks.viewId, tracks.firstTrackId));
        auto& executor = tracks.transport.executor;

        for (auto const trackId : {tracks.firstTrackId, tracks.secondTrackId})
        {
          REQUIRE(executor.tryDrainUntil(
            [&]
            {
              auto const& state = playback.snapshot().transport;
              return state.transport == audio::Transport::Playing && state.nowPlaying.trackId == trackId;
            }));
          lastFailedOccurrence = playback.snapshot().transport.occurrenceId;
          CHECK(sawEmptyOccurrence == (emptyTrackId != kInvalidTrackId && trackId == tracks.secondTrackId));
          REQUIRE(gate.tryWaitForRead());
          gate.release();
        }

        REQUIRE(executor.tryDrainUntil([&] { return issued; }));

        if (trigger == kSkipped)
        {
          // Recovery completed in the report's turn; the observer command cannot run until a later turn.
          REQUIRE(tracks.successionPtr->state().currentTrackId == tracks.thirdTrackId);
          REQUIRE(lower.nowPlaying.trackId == tracks.thirdTrackId);
          REQUIRE(lower.occurrenceId != PlaybackOccurrenceId{});
          REQUIRE(lower.occurrenceId != lastFailedOccurrence);
        }

        executor.drain();
        CHECK_FALSE(publishedInline);
        CHECK_FALSE(startedInline);

        for (auto const& entry : tracks.transport.notificationService.feed().entries)
        {
          if (auto const* report = std::get_if<NotificationReport>(&entry.message); report != nullptr)
          {
            reports.emplace_back(report->templateId, report->count);
          }
        }
      }

      std::atomic<std::size_t> starts{0};
      audio::test::StagedFailureGate gate;
      playback_succession::PlaybackSuccessionTransportFixture tracks;
      TrackId emptyTrackId = kInvalidTrackId;
      PlaybackOccurrenceId lastFailedOccurrence{};
      std::vector<ReportCount> reports;
    };
  } // namespace

  TEST_CASE("PlaybackService - grouped skip observer commands follow recovery to the next track", "[concurrency]")
  {
    auto fixture = FailureObserverFixture{};
    auto& tracks = fixture.tracks;
    auto state = PlaybackSnapshot{};

    SECTION("Next advances from the recovered track")
    {
      auto const throughEmpty = GENERATE(false, true);
      CAPTURE(throughEmpty);
      auto playback = fixture.createPlayback(throughEmpty);
      fixture.issueOnReport(playback, kSkipped, [&] { playback.commands().next(); });
      state = playback.snapshot();
      CHECK(state.succession.sourceState == PlaybackSourceState::Live);
      CHECK(state.succession.currentTrackId == tracks.fourthTrackId);
      CHECK(state.transport.nowPlaying.trackId == tracks.fourthTrackId);
      CHECK(fixture.starts.load(std::memory_order_relaxed) == 4);
    }

    SECTION("clear removes the cursor without stopping the recovered audio")
    {
      auto playback = fixture.createPlayback();
      fixture.issueOnReport(playback, kSkipped, [&] { playback.commands().clearSequence(); });
      state = playback.snapshot();
      CHECK(state.succession.sourceState == PlaybackSourceState::Inactive);
      CHECK(state.succession.currentTrackId == kInvalidTrackId);
      CHECK_FALSE(state.succession.hasNext);
      CHECK(state.transport.nowPlaying.trackId == tracks.thirdTrackId);
      CHECK(fixture.starts.load(std::memory_order_relaxed) == 3);
    }

    auto const& transport = state.transport;
    CHECK(transport.transport == audio::Transport::Playing);
    CHECK(transport.occurrenceId != PlaybackOccurrenceId{});
    CHECK(transport.occurrenceId != fixture.lastFailedOccurrence);
    CHECK(fixture.reports == std::vector<ReportCount>{{kSkipped, 2}});
  }

  TEST_CASE("PlaybackService - failure limit observer Next does not start a track after stop", "[concurrency]")
  {
    // C fails final decoder setup. Unlike A and B it never starts; healthy D must not start either.
    auto fixture = FailureObserverFixture{{"transport-playable-3.flac"}};
    auto playback = fixture.createPlayback();
    fixture.issueOnReport(playback, kLimit, [&] { playback.commands().next(); });

    auto const& state = playback.snapshot();
    CHECK(state.succession.sourceState == PlaybackSourceState::Inactive);
    CHECK(state.succession.currentTrackId == kInvalidTrackId);
    CHECK(state.transport.transport == audio::Transport::Idle);
    CHECK(state.transport.occurrenceId == PlaybackOccurrenceId{});
    CHECK(fixture.starts.load(std::memory_order_relaxed) == 2);
    CHECK(fixture.reports == std::vector<ReportCount>{{kSkipped, 2}, {kLimit, 3}});
  }

  TEST_CASE("PlaybackService - terminal report observer commands precede later commands", "[concurrency]")
  {
    auto gate = audio::test::StagedFailureGate{};
    auto tracks = playback_succession::PlaybackSuccessionTransportFixture{
      playback_succession::PlaybackSuccessionTransportFixtureConfig{
        .stagedFailureFileNames = {"transport-playable-0.flac"},
        .stagedFailureGate = &gate,
        .lossyFileName = "transport-playable-0.flac",
      }};
    tracks.buildTwoTrackManualView();
    auto playback = tracks.createPlayback();
    auto guard = audio::test::StagedFailureReleaseGuard{gate};
    auto& executor = tracks.transport.executor;
    auto reveals = std::vector<TrackId>{};
    auto const revealed = playback.events().onRevealTrackRequested([&](PlaybackRevealTrackRequest const& request)
                                                                   { reveals.push_back(request.trackId); });
    auto trigger = NotificationReportTemplate::PlaybackSequenceFinished;
    bool issued = false;
    auto const subscription = tracks.transport.notificationService.onFeedUpdated(
      [&](NotificationFeedUpdate const& update)
      {
        if (auto const* report = changedReport(update); !issued && report != nullptr && report->templateId == trigger &&
                                                        update.mutationKind == NotificationFeedMutationKind::Posted)
        {
          issued = true;
          playback.commands().revealTrack(tracks.firstTrackId);
        }
      });

    SECTION("the last track finishes naturally")
    {
      REQUIRE(playback.commands().startFromView(tracks.viewId, tracks.secondTrackId));
      REQUIRE(executor.tryDrainUntil(
        [&] { return playback.snapshot().transport.nowPlaying.trackId == tracks.secondTrackId; }));
      auto* const target = tracks.transport.renderTarget;
      REQUIRE(target != nullptr);
      auto pcm = std::array<std::byte, 4096>{};
      REQUIRE(executor.tryDrainUntil([&] { return target->renderPcm(pcm).drained; }));
      target->handleDrainComplete();
    }

    SECTION("the current track fails after its source is deleted")
    {
      trigger = NotificationReportTemplate::PlaybackStoppedForTrack;
      REQUIRE(playback.commands().startFromView(tracks.viewId, tracks.firstTrackId));
      REQUIRE(executor.tryDrainUntil(
        [&] { return playback.snapshot().transport.nowPlaying.trackId == tracks.firstTrackId; }));
      REQUIRE(gate.tryWaitForRead());
      REQUIRE(tracks.commandsFixture.runTask(tracks.commands().deleteListAsync(tracks.listId)));
      REQUIRE(executor.tryDrainUntil(
        [&] { return playback.snapshot().succession.sourceState == PlaybackSourceState::Invalidated; }));
      guard.release();
    }

    REQUIRE(executor.tryDrainUntil([&] { return issued; }));
    REQUIRE(reveals.empty());
    // The first command is waiting for a later executor turn, not merely delayed inside notification delivery.
    playback.commands().revealTrack(tracks.secondTrackId);
    executor.drain();
    CHECK(reveals == std::vector<TrackId>{tracks.firstTrackId, tracks.secondTrackId});
    CHECK(playback.snapshot().transport.transport == audio::Transport::Idle);
    CHECK(playback.snapshot().succession.sourceState == PlaybackSourceState::Inactive);
  }
} // namespace ao::rt::test
