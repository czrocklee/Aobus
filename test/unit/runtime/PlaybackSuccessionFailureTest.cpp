// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/NullBackend.h"
#include "runtime/playback/PlaybackSuccession.h"
#include "runtime/playback/PlaybackTransport.h"
#include "test/unit/audio/BackendTestSupport.h"
#include "test/unit/audio/EngineTestSupport.h"
#include "test/unit/runtime/PlaybackSuccessionBaseTestSupport.h"
#include "test/unit/runtime/PlaybackSuccessionSeekTestSupport.h"
#include "test/unit/runtime/PlaybackSuccessionTransportTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/audio/OpenedPcmMode.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/Transport.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackMode.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <fakeit.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  using playback_succession::PlaybackSuccessionFixture;
  using playback_succession::PlaybackSuccessionSeekFixture;
  using playback_succession::PlaybackSuccessionTransportFixture;
  using playback_succession::PlaybackSuccessionTransportFixtureConfig;

  namespace
  {
    void countCommittedStarts(PlaybackSuccessionTransportFixture& fixture, std::size_t& count)
    {
      audio::test::SpyBackend<audio::NullBackend>& backend = *fixture.transport.spyBackendPtr;
      fakeit::When(Method(backend.mock(), open))
        .AlwaysDo(
          [&](audio::SignalFormat const& format, audio::RenderTarget& target) -> Result<audio::OpenedPcmMode>
          {
            ++count;
            fixture.transport.renderTarget = &target;
            return audio::OpenedPcmMode{.clientFormat = audio::pcmFormat(format, audio::SampleEncoding::Signed16Le)};
          });
    }
  } // namespace

  TEST_CASE("PlaybackSuccession - accepted final decoder failures stop after three committed attempts",
            "[runtime][regression][playback-succession][concurrency]")
  {
    auto const repeatMode = GENERATE(RepeatMode::One, RepeatMode::Off);
    CAPTURE(repeatMode);
    std::size_t committedStarts = 0;
    auto fixture = PlaybackSuccessionTransportFixture{PlaybackSuccessionTransportFixtureConfig{
      .finalOpenFailureFileNames = {"transport-playable-0.flac",
                                    "transport-playable-1.flac",
                                    "transport-playable-2.flac"},
    }};
    fixture.buildFourTrackManualView();
    countCommittedStarts(fixture, committedStarts);
    fixture.successionPtr->setRepeatMode(repeatMode);

    bool settled = false;
    std::size_t startedEvents = 0;
    auto nowPlayingTracks = std::vector<TrackId>{};
    auto const settledSubscription = fixture.successionPtr->onExplicitStartSettled([&] { settled = true; });
    auto const startedSubscription = fixture.transport.playbackTransport.onStarted([&] { ++startedEvents; });
    auto const nowPlayingSubscription = fixture.transport.playbackTransport.onNowPlayingChanged(
      [&](PlaybackTransport::NowPlayingChanged const& event) { nowPlayingTracks.push_back(event.trackId); });
    REQUIRE(fixture.successionPtr->playFromView(fixture.viewId, fixture.firstTrackId));
    auto const completed = fixture.transport.executor.drainUntil(
      [&]
      {
        return (settled && fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Inactive) ||
               committedStarts >= 4;
      });
    CHECK(completed);
    CHECK(committedStarts == 3);
    CHECK(fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Inactive);
    CHECK(fixture.transport.playbackTransport.state().transport == audio::Transport::Idle);
    CHECK(startedEvents == 0);
    CHECK(nowPlayingTracks == std::vector{kInvalidTrackId});

    // The attempt limit keeps this regression finite if Repeat One retries
    // indefinitely again. Retire recovery before inspecting the report or destroying captures.
    auto const feed = fixture.transport.notificationService.feed();
    std::ignore = fixture.transport.playbackTransport.stop();
    fixture.transport.executor.drain();
    NotificationEntry const* failureLimit = nullptr;

    for (auto const& entry : feed.entries)
    {
      if (auto const* report = std::get_if<NotificationReport>(&entry.message);
          report != nullptr && report->templateId == NotificationReportTemplate::PlaybackStoppedAfterFailures)
      {
        failureLimit = &entry;
        CHECK(report->count == 3);
      }
    }

    REQUIRE(failureLimit != nullptr);
    CHECK(failureLimit->severity == NotificationSeverity::Error);
    CHECK(failureLimit->lifetime == NotificationLifetime::pinned());
  }

  TEST_CASE("PlaybackSuccession - unannounced successful recovery resets the failure streak",
            "[runtime][regression][playback-succession][concurrency]")
  {
    std::size_t committedStarts = 0;
    auto fixture = PlaybackSuccessionTransportFixture{PlaybackSuccessionTransportFixtureConfig{
      .finalOpenFailureFileNames = {"transport-playable-0.flac",
                                    "transport-playable-1.flac",
                                    "transport-playable-3.flac",
                                    "transport-playable-4.flac"},
    }};
    auto const tracks = std::array{fixture.addPlayableTrack("First failure"),
                                   fixture.addPlayableTrack("Second failure"),
                                   fixture.addPlayableTrack("First recovery"),
                                   fixture.addPlayableTrack("Third failure"),
                                   fixture.addPlayableTrack("Fourth failure"),
                                   fixture.addPlayableTrack("Second recovery")};
    fixture.openManualView(tracks);
    countCommittedStarts(fixture, committedStarts);
    REQUIRE(fixture.successionPtr->playFromView(fixture.viewId, tracks[0]));
    REQUIRE(fixture.transport.executor.drainUntil(
      [&]
      {
        return fixture.successionPtr->state().currentTrackId == tracks[2] &&
               fixture.transport.playbackTransport.state().transport == audio::Transport::Playing;
      }));
    CHECK(committedStarts == 3);

    fixture.successionPtr->next();
    REQUIRE(fixture.transport.executor.drainUntil(
      [&]
      {
        return fixture.successionPtr->state().currentTrackId == tracks[5] ||
               fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Inactive;
      }));
    CHECK(committedStarts == 6);
    CHECK(fixture.successionPtr->state().currentTrackId == tracks[5]);
    CHECK(fixture.transport.playbackTransport.state().transport == audio::Transport::Playing);
    auto const feed = fixture.transport.notificationService.feed();

    for (auto const& entry : feed.entries)
    {
      if (auto const* report = std::get_if<NotificationReport>(&entry.message); report != nullptr)
      {
        CHECK(report->templateId != NotificationReportTemplate::PlaybackStoppedAfterFailures);
      }
    }
  }

  TEST_CASE("PlaybackSuccession - accepted final decoder failure skips the failed candidate",
            "[runtime][regression][playback-succession][failure]")
  {
    auto fixture = PlaybackSuccessionTransportFixture{PlaybackSuccessionTransportFixtureConfig{
      .finalOpenFailureFileNames = {"transport-playable-2.flac"},
    }};
    fixture.buildFourTrackManualView();
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));

    REQUIRE(fixture.successionPtr->playFromView(fixture.viewId, fixture.thirdTrackId));
    auto const recovered = fixture.transport.executor.drainUntil(
      [&]
      {
        return fixture.successionPtr->state().currentTrackId == fixture.fourthTrackId &&
               fixture.transport.playbackTransport.state().transport == audio::Transport::Playing;
      },
      std::chrono::seconds{5});
    REQUIRE(recovered);

    CHECK(fixture.successionPtr->state().currentTrackId == fixture.fourthTrackId);
    CHECK(fixture.transport.playbackTransport.state().nowPlaying.trackId == fixture.fourthTrackId);
    auto const feed = fixture.transport.notificationService.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().severity == NotificationSeverity::Warning);
    CHECK(feed.entries.front().lifetime == NotificationLifetime::history());
    REQUIRE(std::holds_alternative<NotificationReport>(feed.entries.front().message));
    auto const& report = std::get<NotificationReport>(feed.entries.front().message);
    CHECK(report.templateId == NotificationReportTemplate::PlaybackTracksSkipped);
    CHECK(report.count == 1);
  }

  TEST_CASE("PlaybackSuccession - navigation stops after three consecutive unplayable candidates",
            "[runtime][unit][playback-succession][failure]")
  {
    auto fixture = PlaybackSuccessionFixture{};
    auto const playable = fixture.addPlayableTrack("Current");
    auto const brokenOne = fixture.addPlayableTrack("Broken one");
    auto const brokenTwo = fixture.addPlayableTrack("Broken two");
    auto const brokenThree = fixture.addPlayableTrack("Broken three");
    fixture.removePlayableFile(brokenOne);
    fixture.removePlayableFile(brokenTwo);
    fixture.removePlayableFile(brokenThree);
    auto const unreachable = fixture.addPlayableTrack("Unreachable");
    fixture.openManualView(std::array{playable, brokenOne, brokenTwo, brokenThree, unreachable});
    REQUIRE(fixture.playAndWait(playable));

    fixture.successionPtr->next();

    CHECK(fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Inactive);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Idle);
    CHECK(fixture.playbackTransport.state().nowPlaying.trackId != unreachable);
    auto const feed = fixture.notifications.feed();
    NotificationEntry const* skipSummary = nullptr;
    NotificationEntry const* failureLimit = nullptr;

    for (auto const& entry : feed.entries)
    {
      auto const* report = std::get_if<NotificationReport>(&entry.message);

      if (report != nullptr && report->templateId == NotificationReportTemplate::PlaybackTracksSkipped &&
          report->count == 3)
      {
        skipSummary = &entry;
      }

      if (report != nullptr && report->templateId == NotificationReportTemplate::PlaybackStoppedAfterFailures &&
          report->count == 3)
      {
        failureLimit = &entry;
      }
    }

    REQUIRE(skipSummary != nullptr);
    CHECK(skipSummary->severity == NotificationSeverity::Warning);
    CHECK(skipSummary->lifetime == NotificationLifetime::history());
    REQUIRE(failureLimit != nullptr);
    CHECK(failureLimit->severity == NotificationSeverity::Error);
    CHECK(failureLimit->lifetime == NotificationLifetime::pinned());
  }

  TEST_CASE("PlaybackSuccession - non-recoverable device failure terminates the active session",
            "[runtime][unit][playback-succession][failure]")
  {
    auto fixture = PlaybackSuccessionTransportFixture{};
    fixture.buildThreeTrackManualView();
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    REQUIRE(fixture.transport.renderTarget != nullptr);

    fixture.transport.renderTarget->handleBackendError("device lost during succession playback");
    REQUIRE(fixture.transport.executor.drainUntil(
      [&] { return fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Inactive; }));

    CHECK(fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Inactive);
    CHECK(fixture.transport.playbackTransport.state().transport == audio::Transport::Idle);

    auto const feed = fixture.transport.notificationService.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().severity == NotificationSeverity::Error);
  }

  TEST_CASE("PlaybackSuccession - track failure on an invalidated source posts one terminal succession error",
            "[runtime][unit][playback-succession][concurrency]")
  {
    auto failureGate = audio::test::StagedFailureGate{};
    auto fixture = PlaybackSuccessionSeekFixture{&failureGate};
    auto releaseGuard = audio::test::StagedFailureReleaseGuard{failureGate};
    fixture.buildSingleTrackManualView();
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    REQUIRE(failureGate.waitForRead());

    REQUIRE(fixture.commandsFixture.runTask(fixture.commands().deleteList(fixture.listId)));
    REQUIRE(fixture.executor.drainUntil(
      [&] { return fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Invalidated; }));
    releaseGuard.release();
    REQUIRE(fixture.executor.drainUntil(
      [&] { return fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Inactive; }));
    CHECK(fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Inactive);

    auto const feed = fixture.notifications.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().severity == NotificationSeverity::Error);
    CHECK(feed.entries.front().lifetime == NotificationLifetime::pinned());
    REQUIRE(std::holds_alternative<NotificationReport>(feed.entries.front().message));
    auto const& report = std::get<NotificationReport>(feed.entries.front().message);
    CHECK(report.templateId == NotificationReportTemplate::PlaybackStoppedForTrack);
    CHECK(report.subject == "Failing current");
    CHECK(report.detail == "gated staged decode failure");
  }

  TEST_CASE("PlaybackSuccession - previous restart uses a strict greater-than three-second final seek",
            "[runtime][unit][playback-succession][previous]")
  {
    auto fixture = PlaybackSuccessionSeekFixture{};
    fixture.buildThreeTrackManualView();
    auto& succession = *fixture.successionPtr;
    auto& playbackTransport = *fixture.transportPtr;
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    fixture.executor.drain();
    playbackTransport.pause();
    REQUIRE(playbackTransport.state().transport == audio::Transport::Paused);

    playbackTransport.seek(std::chrono::milliseconds{3000}, PlaybackTransport::SeekMode::Final);
    CHECK(playbackTransport.elapsed() == std::chrono::milliseconds{3000});
    CHECK_FALSE(succession.state().hasPrevious);

    playbackTransport.seek(std::chrono::milliseconds{3001}, PlaybackTransport::SeekMode::Final);
    CHECK(playbackTransport.elapsed() == std::chrono::milliseconds{3001});
    CHECK(succession.state().hasPrevious);

    succession.previous();
    CHECK(succession.state().currentTrackId == fixture.firstTrackId);
    CHECK_FALSE(succession.state().hasPrevious);
    CHECK(playbackTransport.elapsed() == std::chrono::milliseconds{0});
    CHECK(playbackTransport.state().transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSuccession - commands and dedicated mode signals follow cursor resolution",
            "[runtime][unit][playback-succession][command]")
  {
    auto fixture = PlaybackSuccessionFixture{};
    fixture.buildThreeTrackManualView();
    auto& succession = *fixture.successionPtr;
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));

    std::uint32_t shuffleEvents = 0;
    std::uint32_t repeatEvents = 0;
    auto const shuffleSubscription =
      succession.onShuffleModeChanged([&](PlaybackSuccession::ShuffleModeChanged const&) noexcept { ++shuffleEvents; });
    auto const repeatSubscription =
      succession.onRepeatModeChanged([&](PlaybackSuccession::RepeatModeChanged const&) noexcept { ++repeatEvents; });

    succession.next();
    CHECK(succession.state().currentTrackId == fixture.secondTrackId);
    CHECK(succession.state().hasPrevious);

    succession.previous();
    CHECK(succession.state().currentTrackId == fixture.firstTrackId);

    succession.setRepeatMode(RepeatMode::One);
    CHECK(succession.state().repeat == RepeatMode::One);
    CHECK(succession.state().optResolvedSuccessor == fixture.firstTrackId);
    CHECK(repeatEvents == 1);
    succession.setRepeatMode(RepeatMode::One);
    CHECK(repeatEvents == 1);

    succession.setShuffleMode(ShuffleMode::On);
    CHECK(succession.state().shuffle == ShuffleMode::On);
    CHECK(shuffleEvents == 1);

    succession.clear();
    CHECK(succession.state().sourceState == PlaybackSuccessionSourceState::Inactive);
    CHECK(succession.state().currentTrackId == kInvalidTrackId);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSuccession - shuffle failure walks preserve shuffle direction and semantic parity",
            "[runtime][unit][playback-succession][shuffle]")
  {
    auto fixture = PlaybackSuccessionFixture{};
    fixture.buildThreeTrackManualView();
    auto& succession = *fixture.successionPtr;
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    succession.setShuffleMode(ShuffleMode::On);

    SECTION("failed forward candidate is excluded before the sticky candidate is re-resolved")
    {
      auto const optFailedCandidate = succession.state().optResolvedSuccessor;
      REQUIRE(optFailedCandidate);
      fixture.removePlayableFile(*optFailedCandidate);

      succession.next();

      CHECK(succession.state().currentTrackId != fixture.firstTrackId);
      CHECK(succession.state().currentTrackId != *optFailedCandidate);
      CHECK(fixture.playbackTransport.state().transport == audio::Transport::Playing);
    }

    SECTION("failed history previous does not fall through to sequential previous")
    {
      succession.next();
      auto const currentTrackId = succession.state().currentTrackId;
      REQUIRE(currentTrackId != fixture.firstTrackId);
      REQUIRE(succession.state().hasPrevious);
      fixture.removePlayableFile(fixture.firstTrackId);

      succession.previous();

      CHECK(succession.state().currentTrackId == currentTrackId);
      CHECK_FALSE(succession.state().hasPrevious);
      CHECK(fixture.playbackTransport.state().transport == audio::Transport::Playing);
    }
  }
} // namespace ao::rt::test
