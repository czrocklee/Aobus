// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/NullBackend.h"
#include "runtime/playback/PlaybackSuccession.h"
#include "runtime/playback/PlaybackTransport.h"
#include "test/audio/BackendTestSupport.h"
#include "test/audio/EngineTestSupport.h"
#include "test/runtime/PlaybackSuccessionBaseTestSupport.h"
#include "test/runtime/PlaybackSuccessionSeekTestSupport.h"
#include "test/runtime/PlaybackSuccessionTransportTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/audio/OpenedPcmMode.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/Transport.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/PreparedPlayback.h>
#include <ao/rt/playback/PlaybackService.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <fakeit.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
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
          [&](audio::SignalFormat const& format, audio::RenderTarget& target) -> audio::BackendOpenResult
          {
            ++count;
            fixture.transport.renderTarget = &target;
            return audio::OpenedPcmMode{.clientFormat = audio::pcmFormat(format, audio::SampleEncoding::Signed16Le)};
          });
    }

    bool tryWaitForNextPlaying(PlaybackSuccessionTransportFixture& fixture, PlaybackOccurrenceId const previous)
    {
      auto const& state = fixture.transport.playbackTransport.state();
      auto const inactive = [&]
      { return fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Inactive; };
      REQUIRE(fixture.transport.executor.tryDrainUntil(
        [&]
        { return inactive() || (state.transport == audio::Transport::Playing && state.occurrenceId != previous); }));
      return !inactive();
    }

    std::chrono::milliseconds renderAndAdvancePosition(PlaybackSuccessionTransportFixture& fixture)
    {
      auto* const target = fixture.transport.renderTarget;
      REQUIRE(target != nullptr);
      auto output = std::array<std::byte, 4096>{};
      auto const block = target->renderPcm(output);
      REQUIRE(block.bytesWritten > 0);
      REQUIRE(block.positionFrames > 0);
      target->handlePositionAdvanced(block.positionFrames);
      auto const position = fixture.transport.playbackTransport.elapsed();
      REQUIRE(position > std::chrono::milliseconds{0});
      return position;
    }

    auto playbackReportCount(NotificationFeedState const& feed)
    {
      return std::ranges::count_if(feed.entries,
                                   [](NotificationEntry const& entry)
                                   { return std::holds_alternative<NotificationReport>(entry.message); });
    }

    std::vector<std::size_t> reportCounts(NotificationFeedState const& feed, NotificationReportTemplate const id)
    {
      auto counts = std::vector<std::size_t>{};

      for (auto const& entry : feed.entries)
      {
        if (auto const* report = std::get_if<NotificationReport>(&entry.message);
            report != nullptr && report->templateId == id)
        {
          counts.push_back(report->count);
        }
      }

      return counts;
    }

    // Both typed refusal causes deliberately share one Error code and message:
    // recovery must discriminate on BackendOpenFailure::cause, never on a
    // code-string classifier.
    Error scriptedRouteRefusal()
    {
      return Error{.code = Error::Code::NotSupported, .message = "scripted output route refusal"};
    }

    // Backend-open outcomes for one fixture, keyed by 1-based committed-open
    // number. `incompatibleOpenNumbers` fail as a skippable track/route
    // incompatibility; `outputFaultOpenNumber` fails as a terminal output
    // fault. Counters are atomic because open() runs on the engine's control
    // domain while the test thread observes them through bounded drains.
    struct BackendOpenScript final
    {
      std::vector<std::size_t> incompatibleOpenNumbers{};
      std::size_t outputFaultOpenNumber = 0;
      std::atomic<std::size_t> committedOpens{0};
      std::atomic<std::size_t> succeededOpens{0};
      std::atomic<std::size_t> startedRoutes{0};
    };

    void scriptBackendOpens(PlaybackSuccessionTransportFixture& fixture, BackendOpenScript& script)
    {
      audio::test::SpyBackend<audio::NullBackend>& backend = *fixture.transport.spyBackendPtr;
      fakeit::When(Method(backend.mock(), start))
        .AlwaysDo([&script] { script.startedRoutes.fetch_add(1, std::memory_order_relaxed); });
      fakeit::When(Method(backend.mock(), open))
        .AlwaysDo(
          [&](audio::SignalFormat const& format, audio::RenderTarget& target) -> audio::BackendOpenResult
          {
            auto const openNumber = script.committedOpens.fetch_add(1, std::memory_order_relaxed) + 1;
            fixture.transport.renderTarget = &target;

            if (openNumber == script.outputFaultOpenNumber)
            {
              return std::unexpected{audio::BackendOpenFailure{
                .error = scriptedRouteRefusal(),
                .cause = audio::BackendOpenFailure::Cause::OutputFailure,
              }};
            }

            if (std::ranges::contains(script.incompatibleOpenNumbers, openNumber))
            {
              return std::unexpected{audio::BackendOpenFailure{
                .error = scriptedRouteRefusal(),
                .cause = audio::BackendOpenFailure::Cause::TrackRouteIncompatible,
              }};
            }

            script.succeededOpens.fetch_add(1, std::memory_order_relaxed);
            return audio::OpenedPcmMode{.clientFormat = audio::pcmFormat(format, audio::SampleEncoding::Signed16Le)};
          });
    }
  } // namespace

  TEST_CASE("PlaybackSuccession - accepted final decoder failures stop after three committed attempts")
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
    auto const completed = fixture.transport.executor.tryDrainUntil(
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

  TEST_CASE("PlaybackSuccession - repeating stops after three post-start decode failures", "[concurrency]")
  {
    auto const [repeatMode, twoTracks] = GENERATE(Catch::Generators::table<RepeatMode, bool>(
      {{RepeatMode::One, false}, {RepeatMode::All, false}, {RepeatMode::All, true}}));
    CAPTURE(repeatMode, twoTracks);
    auto failureGate = audio::test::StagedFailureGate{};
    auto fixture = PlaybackSuccessionTransportFixture{PlaybackSuccessionTransportFixtureConfig{
      .stagedFailureFileNames = {"transport-playable-0.flac", "transport-playable-1.flac"},
      .stagedFailureGate = &failureGate,
      // Every transition includes the lossy A, so no lookahead takes the read gate.
      .lossyFileName = "transport-playable-0.flac",
    }};
    auto releaseGuard = audio::test::StagedFailureReleaseGuard{failureGate};
    auto tracks = std::vector{fixture.addPlayableTrack("First bad track")};

    if (twoTracks)
    {
      tracks.push_back(fixture.addPlayableTrack("Second bad track"));
    }

    fixture.openManualView(tracks);
    auto& succession = *fixture.successionPtr;
    auto& transport = fixture.transport.playbackTransport;
    auto& executor = fixture.transport.executor;
    succession.setRepeatMode(repeatMode);

    REQUIRE(playback_succession::playFromViewAndWait(succession, executor, fixture.viewId, tracks.front()));
    auto startedTracks = std::vector<TrackId>{};
    auto occurrence = PlaybackOccurrenceId{};

    while (tryWaitForNextPlaying(fixture, occurrence))
    {
      REQUIRE(failureGate.tryWaitForRead());
      occurrence = transport.state().occurrenceId;
      startedTracks.push_back(transport.state().nowPlaying.trackId);

      // At the fourth start leave the reader to teardown, without dispatching
      // another recovery turn on an unbounded repeat regression.
      if (startedTracks.size() == 4)
      {
        break;
      }

      if (twoTracks && startedTracks.size() == 2)
      {
        transport.pause();
        REQUIRE(transport.state().transport == audio::Transport::Paused);
        transport.resume();
        REQUIRE(transport.state().transport == audio::Transport::Playing);
        REQUIRE(transport.state().occurrenceId == occurrence);
      }

      failureGate.release();
    }

    CHECK(startedTracks == std::vector<TrackId>{tracks.front(), tracks.back(), tracks.front()});
    CHECK(succession.state().sourceState == PlaybackSuccessionSourceState::Inactive);
    CHECK(transport.state().transport == audio::Transport::Idle);
    auto const feed = fixture.transport.notificationService.feed();
    CHECK(playbackReportCount(feed) == 2);
    CHECK(reportCounts(feed, NotificationReportTemplate::PlaybackTracksSkipped) == std::vector<std::size_t>{2});
    CHECK(reportCounts(feed, NotificationReportTemplate::PlaybackStoppedAfterFailures) == std::vector<std::size_t>{3});
  }

  TEST_CASE("PlaybackSuccession - in-track seeking and a ready output switch preserve the decode failure limit",
            "[concurrency]")
  {
    auto const seekBeforeSwitch = GENERATE(false, true);
    CAPTURE(seekBeforeSwitch);
    auto firstFailures = audio::test::StagedFailureGate{};
    auto reopened = audio::test::StagedFailureGate{};
    auto fixture = PlaybackSuccessionTransportFixture{PlaybackSuccessionTransportFixtureConfig{
      .stagedFailureFileNames = {"transport-playable-0.flac"},
      // The third occurrence can seek or close without a blocked reader. Its reopen supplies the third failure.
      .stagedFailureGateSequence = {&firstFailures, &firstFailures, nullptr, &reopened},
      .lossyFileName = "transport-playable-0.flac", // Keep lookahead from opening another decoder.
    }};
    auto releaseFirst = audio::test::StagedFailureReleaseGuard{firstFailures};
    auto releaseReopen = audio::test::StagedFailureReleaseGuard{reopened};
    auto const track = fixture.addPlayableTrack("Repeating track");
    fixture.openManualView(std::array{track});
    auto& succession = *fixture.successionPtr;
    auto& transport = fixture.transport.playbackTransport;
    auto const selected = transport.state().output.selectedDevice;
    auto const alternate = audio::Device{.id = audio::DeviceId{"alternate"}, .backendId = selected.backendId};
    fixture.transport.onDevicesChangedCb({fixture.transport.status.devices.front(), alternate});
    fixture.transport.executor.drain();
    REQUIRE(transport.state().output.selectedDevice.deviceId != alternate.id);
    succession.setRepeatMode(RepeatMode::One);
    REQUIRE(playback_succession::playFromViewAndWait(succession, fixture.transport.executor, fixture.viewId, track));

    auto occurrence = PlaybackOccurrenceId{};

    for (std::size_t i = 0; i < 2; ++i)
    {
      REQUIRE(tryWaitForNextPlaying(fixture, occurrence));
      occurrence = transport.state().occurrenceId;
      REQUIRE(firstFailures.tryWaitForRead());
      firstFailures.release();
    }

    REQUIRE(tryWaitForNextPlaying(fixture, occurrence));
    occurrence = transport.state().occurrenceId;
    auto const position = seekBeforeSwitch ? std::chrono::milliseconds{100} : std::chrono::milliseconds{0};

    if (seekBeforeSwitch)
    {
      REQUIRE(position < transport.state().duration);
      REQUIRE(transport.trySeek(occurrence, position, PlaybackTransport::SeekMode::Final));
      REQUIRE(transport.elapsed() == position);
      REQUIRE(transport.state().occurrenceId == occurrence);
    }

    transport.setOutputDevice(alternate.backendId, alternate.id, selected.profileId);
    REQUIRE(transport.state().output.selectedDevice.deviceId == alternate.id);
    REQUIRE(transport.state().ready);
    REQUIRE(transport.state().transport == audio::Transport::Playing);
    REQUIRE(transport.canAdvanceFrom(occurrence));
    REQUIRE(transport.elapsed() == position);
    REQUIRE(reopened.tryWaitForRead());
    releaseReopen.release();

    CHECK_FALSE(tryWaitForNextPlaying(fixture, occurrence));
    CHECK(succession.state().sourceState == PlaybackSuccessionSourceState::Inactive);
    CHECK(transport.state().transport == audio::Transport::Idle);
    auto& backend = fixture.transport.spyBackendPtr->mock();
    fakeit::Verify(Method(backend, open)).Exactly(4); // Three occurrences, one native reopen.
    auto const feed = fixture.transport.notificationService.feed();
    CHECK(reportCounts(feed, NotificationReportTemplate::PlaybackTracksSkipped) == std::vector<std::size_t>{2});
    CHECK(reportCounts(feed, NotificationReportTemplate::PlaybackStoppedAfterFailures) == std::vector<std::size_t>{3});
  }

  TEST_CASE("PlaybackSuccession - Repeat Off stops before the fourth track after three rendered decode failures",
            "[concurrency]")
  {
    auto failureGate = audio::test::StagedFailureGate{};
    auto fixture = PlaybackSuccessionTransportFixture{PlaybackSuccessionTransportFixtureConfig{
      .stagedFailureFileNames = {"transport-playable-0.flac", "transport-playable-1.flac", "transport-playable-2.flac"},
      .stagedFailureGate = &failureGate,
      // Lossy B prevents A -> B and B -> C splices. C can prepare D, but D's reader is not gated.
      .lossyFileName = "transport-playable-1.flac",
    }};
    auto releaseGuard = audio::test::StagedFailureReleaseGuard{failureGate};
    fixture.buildFourTrackManualView();
    auto& succession = *fixture.successionPtr;
    auto& transport = fixture.transport.playbackTransport;
    succession.setRepeatMode(RepeatMode::Off);
    REQUIRE(playback_succession::playFromViewAndWait(
      succession, fixture.transport.executor, fixture.viewId, fixture.firstTrackId));
    auto occurrence = PlaybackOccurrenceId{};

    for (auto const track : {fixture.firstTrackId, fixture.secondTrackId, fixture.thirdTrackId})
    {
      CAPTURE(track);
      REQUIRE(tryWaitForNextPlaying(fixture, occurrence));
      REQUIRE(transport.state().nowPlaying.trackId == track);
      occurrence = transport.state().occurrenceId;
      REQUIRE(failureGate.tryWaitForRead());
      std::ignore = renderAndAdvancePosition(fixture);
      failureGate.release();
    }

    CHECK_FALSE(tryWaitForNextPlaying(fixture, occurrence));
    CHECK(succession.state().sourceState == PlaybackSuccessionSourceState::Inactive);
    CHECK(transport.state().transport == audio::Transport::Idle);
    auto& backend = fixture.transport.spyBackendPtr->mock();
    fakeit::Verify(Method(backend, start)).Exactly(3);
    auto const feed = fixture.transport.notificationService.feed();
    CHECK(playbackReportCount(feed) == 2);
    CHECK(reportCounts(feed, NotificationReportTemplate::PlaybackTracksSkipped) == std::vector<std::size_t>{2});
    CHECK(reportCounts(feed, NotificationReportTemplate::PlaybackStoppedAfterFailures) == std::vector<std::size_t>{3});
  }

  TEST_CASE("PlaybackSuccession - an empty track does not clear post-start decode failures on repeat all",
            "[concurrency]")
  {
    auto failureGate = audio::test::StagedFailureGate{};
    auto fixture = PlaybackSuccessionTransportFixture{PlaybackSuccessionTransportFixtureConfig{
      .stagedFailureFileNames = {"transport-playable-0.flac"},
      .stagedFailureGate = &failureGate,
      .lossyFileName = "transport-playable-0.flac",
      .immediateEofFileNames = {"transport-playable-1.flac"},
    }};
    auto releaseGuard = audio::test::StagedFailureReleaseGuard{failureGate};
    fixture.buildTwoTrackManualView();
    auto& succession = *fixture.successionPtr;
    auto& transport = fixture.transport.playbackTransport;
    auto& executor = fixture.transport.executor;
    succession.setRepeatMode(RepeatMode::All);
    std::size_t naturalEnds = 0;
    auto const idleSubscription = transport.onIdle([&] { ++naturalEnds; });
    REQUIRE(playback_succession::playFromViewAndWait(succession, executor, fixture.viewId, fixture.firstTrackId));
    auto occurrence = PlaybackOccurrenceId{};

    for (std::size_t i = 0; i < 3; ++i)
    {
      REQUIRE(tryWaitForNextPlaying(fixture, occurrence));
      REQUIRE(transport.state().nowPlaying.trackId == fixture.firstTrackId);
      occurrence = transport.state().occurrenceId;
      REQUIRE(failureGate.tryWaitForRead());
      // Between A starts, B ended naturally without a render or a TrackOpen failure.
      REQUIRE(naturalEnds == i);
      failureGate.release();
    }

    // Stop driving recovery on a fourth Playing occurrence. Its blocked A reader
    // is released by the guard before the fixture joins it, including on failure.
    CHECK_FALSE(tryWaitForNextPlaying(fixture, occurrence));
    CHECK(succession.state().sourceState == PlaybackSuccessionSourceState::Inactive);
    CHECK(transport.state().transport == audio::Transport::Idle);
    auto const feed = fixture.transport.notificationService.feed();
    CHECK(playbackReportCount(feed) == 2);
    CHECK(reportCounts(feed, NotificationReportTemplate::PlaybackTracksSkipped) == std::vector<std::size_t>{2});
    CHECK(reportCounts(feed, NotificationReportTemplate::PlaybackStoppedAfterFailures) == std::vector<std::size_t>{3});
    auto& backend = fixture.transport.spyBackendPtr->mock();
    fakeit::Verify(Method(backend, open)).Exactly(5);
    fakeit::Verify(Method(backend, start)).Exactly(3);
  }

  TEST_CASE("PlaybackSuccession - a route refusal shares the post-start decode failure limit", "[concurrency]")
  {
    auto failureGate = audio::test::StagedFailureGate{};
    auto script = BackendOpenScript{.incompatibleOpenNumbers = {2}};
    auto fixture = PlaybackSuccessionTransportFixture{PlaybackSuccessionTransportFixtureConfig{
      .stagedFailureFileNames = {"transport-playable-0.flac"},
      .stagedFailureGate = &failureGate,
      .lossyFileName = "transport-playable-0.flac",
    }};
    auto releaseGuard = audio::test::StagedFailureReleaseGuard{failureGate};
    fixture.buildTwoTrackManualView();
    scriptBackendOpens(fixture, script);
    auto& succession = *fixture.successionPtr;
    auto& transport = fixture.transport.playbackTransport;
    auto& executor = fixture.transport.executor;
    succession.setRepeatMode(RepeatMode::All);
    REQUIRE(playback_succession::playFromViewAndWait(succession, executor, fixture.viewId, fixture.firstTrackId));

    // A starts and fails, B's route refuses to open, then A starts and fails again.
    auto occurrence = PlaybackOccurrenceId{};

    for (std::size_t i = 0; i < 2; ++i)
    {
      REQUIRE(tryWaitForNextPlaying(fixture, occurrence));
      REQUIRE(transport.state().nowPlaying.trackId == fixture.firstTrackId);
      occurrence = transport.state().occurrenceId;
      REQUIRE(failureGate.tryWaitForRead());
      failureGate.release();
    }

    REQUIRE(executor.tryDrainUntil(
      [&]
      {
        return succession.state().sourceState == PlaybackSuccessionSourceState::Inactive ||
               script.committedOpens.load(std::memory_order_relaxed) >= 4;
      }));
    CHECK(script.committedOpens.load(std::memory_order_relaxed) == 3);
    CHECK(script.succeededOpens.load(std::memory_order_relaxed) == 2);
    CHECK(succession.state().sourceState == PlaybackSuccessionSourceState::Inactive);
    CHECK(transport.state().transport == audio::Transport::Idle);
    auto const feed = fixture.transport.notificationService.feed();
    CHECK(reportCounts(feed, NotificationReportTemplate::PlaybackTracksSkipped) == std::vector<std::size_t>{2});
    CHECK(reportCounts(feed, NotificationReportTemplate::PlaybackStoppedAfterFailures) == std::vector<std::size_t>{3});
  }

  TEST_CASE("PlaybackSuccession - a new playback intent or natural end retires post-start decode failures",
            "[concurrency]")
  {
    auto failureGate = audio::test::StagedFailureGate{};
    auto script = BackendOpenScript{.incompatibleOpenNumbers = {1, 2}};
    auto fixture = PlaybackSuccessionTransportFixture{PlaybackSuccessionTransportFixtureConfig{
      .stagedFailureFileNames = {"transport-playable-0.flac"},
      .stagedFailureGate = &failureGate,
      .lossyFileName = "transport-playable-0.flac",
    }};
    auto releaseGuard = audio::test::StagedFailureReleaseGuard{failureGate};
    fixture.buildTwoTrackManualView();
    auto& succession = *fixture.successionPtr;
    auto& transport = fixture.transport.playbackTransport;
    auto& executor = fixture.transport.executor;
    succession.setRepeatMode(RepeatMode::All);
    auto occurrence = PlaybackOccurrenceId{};
    auto const replayA = [&]
    { REQUIRE(playback_succession::playFromViewAndWait(succession, executor, fixture.viewId, fixture.firstTrackId)); };
    auto const requirePlaying = [&](TrackId const track)
    {
      REQUIRE(tryWaitForNextPlaying(fixture, occurrence));
      REQUIRE(transport.state().nowPlaying.trackId == track);
      occurrence = transport.state().occurrenceId;
    };
    auto const failAAndReachB = [&]
    {
      requirePlaying(fixture.firstTrackId);
      REQUIRE(failureGate.tryWaitForRead());
      failureGate.release();
      requirePlaying(fixture.secondTrackId);
    };

    replayA();
    failAAndReachB();
    auto expectedSkips = std::vector<std::size_t>{1, 1, 1};

    SECTION("replaying A before B renders starts a fresh streak")
    {
      for (std::size_t i = 0; i < 2; ++i)
      {
        replayA();
        failAAndReachB();
      }
    }

    SECTION("an accepted failed launch retires Decode recovery before an open-only streak")
    {
      // Relaunch B and refuse its route. A's successful automatic start must clear that refusal.
      script.incompatibleOpenNumbers = {1, 3};
      scriptBackendOpens(fixture, script);
      REQUIRE(playback_succession::playFromViewAndWait(succession, executor, fixture.viewId, fixture.secondTrackId));
      requirePlaying(fixture.firstTrackId);
      REQUIRE(failureGate.tryWaitForRead());
      failureGate.release();

      // A fails decoding, then B refuses again. These two failures must still allow A to start.
      CHECK(tryWaitForNextPlaying(fixture, occurrence));
      CHECK(succession.state().sourceState == PlaybackSuccessionSourceState::Live);
      CHECK(transport.state().transport == audio::Transport::Playing);
      CHECK(transport.state().nowPlaying.trackId == fixture.firstTrackId);
      CHECK(script.committedOpens.load(std::memory_order_relaxed) == 4);
      expectedSkips = {1, 1, 2};
      CHECK(playbackReportCount(fixture.transport.notificationService.feed()) == 3);
    }

    SECTION("letting B play to its natural end starts a fresh streak")
    {
      auto output = std::array<std::byte, 4096>{};

      for (std::size_t i = 0; i < 2; ++i)
      {
        auto* const target = fixture.transport.renderTarget;
        REQUIRE(target != nullptr);
        REQUIRE(executor.tryDrainUntil([&] { return target->renderPcm(output).drained; }));
        target->handleDrainComplete();
        failAAndReachB();
      }
    }

    SECTION("seeking playing B to its end resets the streak before opening its successor")
    {
      // Both successors refuse to open. Counting the earlier Decode would stop before A starts again.
      scriptBackendOpens(fixture, script);
      REQUIRE(transport.trySeek(occurrence, transport.state().duration));
      requirePlaying(fixture.firstTrackId);
      CHECK(script.committedOpens.load(std::memory_order_relaxed) == 3);
      expectedSkips = {1, 2};
    }

    SECTION("resuming a held B starts a fresh streak even if reopening B fails")
    {
      auto const position = renderAndAdvancePosition(fixture);
      scriptBackendOpens(fixture, script);
      auto const selectMissingOutputAndRestore = [&]
      {
        auto const selected = transport.state().output.selectedDevice;
        transport.setOutputDevice(selected.backendId, audio::DeviceId{"missing"}, selected.profileId);
        REQUIRE_FALSE(transport.state().ready);
        REQUIRE(transport.hasDeferredResume());
        CHECK(transport.elapsed() == position);
        CHECK(transport.state().occurrenceId == occurrence);
        transport.setOutputDevice(selected);
        executor.drain();
        REQUIRE(transport.state().ready);
      };

      SECTION("seeking paused B to its end releases the decoder")
      {
        transport.pause();
        REQUIRE(transport.state().transport == audio::Transport::Paused);
        REQUIRE(transport.trySeek(occurrence, transport.state().duration));
        // Resume inside B so it must reopen, rather than consuming the end intent.
        REQUIRE(transport.trySeek(occurrence, position));
      }

      SECTION("selecting a missing output while B is already paused releases the decoder")
      {
        transport.pause();
        REQUIRE(transport.state().transport == audio::Transport::Paused);
        selectMissingOutputAndRestore();
      }

      SECTION("selecting a missing output while B is playing releases the decoder")
      {
        REQUIRE(transport.state().transport == audio::Transport::Playing);
        selectMissingOutputAndRestore();
      }

      REQUIRE(transport.state().transport == audio::Transport::Paused);
      REQUIRE(transport.hasDeferredResume());
      CHECK(transport.state().occurrenceId == occurrence);
      CHECK(transport.state().nowPlaying.trackId == fixture.secondTrackId);
      CHECK(transport.elapsed() == position);
      REQUIRE(script.committedOpens.load(std::memory_order_relaxed) == 0);
      // Refuse both the explicit reopen of B and the automatic open of A.
      // Retaining A's earlier Decode would stop here, before B can start again.
      transport.resume();
      CHECK(tryWaitForNextPlaying(fixture, occurrence));
      CHECK(transport.state().nowPlaying.trackId == fixture.secondTrackId);
      CHECK(script.committedOpens.load(std::memory_order_relaxed) == 3);
      CHECK(script.startedRoutes.load(std::memory_order_relaxed) == 1);
      expectedSkips = {1, 2};
    }

    auto const feed = fixture.transport.notificationService.feed();
    CHECK(reportCounts(feed, NotificationReportTemplate::PlaybackTracksSkipped) == expectedSkips);
    CHECK(reportCounts(feed, NotificationReportTemplate::PlaybackStoppedAfterFailures).empty());
  }

  TEST_CASE("PlaybackSuccession - unannounced successful recovery resets the failure streak", "[concurrency]")
  {
    std::size_t committedStarts = 0;
    auto failureGate = audio::test::StagedFailureGate{};
    auto fixture = PlaybackSuccessionTransportFixture{PlaybackSuccessionTransportFixtureConfig{
      .finalOpenFailureFileNames = {"transport-playable-0.flac",
                                    "transport-playable-1.flac",
                                    "transport-playable-3.flac"},
      .stagedFailureFileNames = {"transport-playable-2.flac"},
      .stagedFailureGate = &failureGate,
    }};
    auto releaseGuard = audio::test::StagedFailureReleaseGuard{failureGate};
    auto const tracks = std::array{fixture.addPlayableTrack("First failure"),
                                   fixture.addPlayableTrack("Second failure"),
                                   fixture.addPlayableTrack("First recovery"),
                                   fixture.addPlayableTrack("Final setup failure after recovery"),
                                   fixture.addPlayableTrack("Second recovery")};
    fixture.openManualView(tracks);
    countCommittedStarts(fixture, committedStarts);
    std::size_t nowPlayingEvents = 0;
    auto const nowPlayingSubscription = fixture.transport.playbackTransport.onNowPlayingChanged(
      [&](PlaybackTransport::NowPlayingChanged const&) { ++nowPlayingEvents; });
    REQUIRE(fixture.successionPtr->playFromView(fixture.viewId, tracks[0]));
    REQUIRE(fixture.transport.executor.tryDrainUntil(
      [&]
      {
        return fixture.successionPtr->state().currentTrackId == tracks[2] &&
               fixture.transport.playbackTransport.state().transport == audio::Transport::Playing;
      }));
    REQUIRE(failureGate.tryWaitForRead());
    REQUIRE(fixture.transport.playbackTransport.state().nowPlaying.trackId == tracks[2]);
    CHECK(committedStarts == 3);
    CHECK(nowPlayingEvents == 0);

    // A navigation command resets the streak itself. Fail the running decoder
    // instead, so only the successful recovery can have cleared the old failures.
    releaseGuard.release();
    REQUIRE(fixture.transport.executor.tryDrainUntil(
      [&]
      {
        return fixture.successionPtr->state().currentTrackId == tracks[4] ||
               fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Inactive;
      }));
    CHECK(committedStarts == 5);
    CHECK(fixture.successionPtr->state().currentTrackId == tracks[4]);
    CHECK(fixture.transport.playbackTransport.state().nowPlaying.trackId == tracks[4]);
    CHECK(fixture.transport.playbackTransport.state().transport == audio::Transport::Playing);
    CHECK(nowPlayingEvents == 0);
    auto const feed = fixture.transport.notificationService.feed();

    for (auto const& entry : feed.entries)
    {
      if (auto const* report = std::get_if<NotificationReport>(&entry.message); report != nullptr)
      {
        CHECK(report->templateId != NotificationReportTemplate::PlaybackStoppedAfterFailures);
      }
    }
  }

  TEST_CASE("PlaybackSuccession - backend route refusals skip ahead to the next playable route")
  {
    // The script must be declared before the fixture: the SpyBackend open
    // stub captures it by reference, and the fixture teardown (including the
    // engine join) can still invoke open() after the test body unwinds.
    auto script = BackendOpenScript{.incompatibleOpenNumbers = {1, 2}};
    auto fixture = PlaybackSuccessionTransportFixture{};
    fixture.buildFourTrackManualView();
    scriptBackendOpens(fixture, script);
    bool settled = false;
    auto const settledSubscription = fixture.successionPtr->onExplicitStartSettled([&] { settled = true; });
    REQUIRE(fixture.successionPtr->playFromView(fixture.viewId, fixture.firstTrackId));
    REQUIRE(fixture.transport.executor.tryDrainUntil(
      [&]
      {
        return (fixture.successionPtr->state().currentTrackId == fixture.thirdTrackId &&
                fixture.transport.playbackTransport.state().transport == audio::Transport::Playing) ||
               script.committedOpens.load(std::memory_order_relaxed) >= 4 ||
               (settled && fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Inactive);
      }));

    CHECK(script.committedOpens.load(std::memory_order_relaxed) == 3);
    CHECK(script.succeededOpens.load(std::memory_order_relaxed) == 1);
    CHECK(fixture.successionPtr->state().currentTrackId == fixture.thirdTrackId);
    CHECK(fixture.transport.playbackTransport.state().nowPlaying.trackId == fixture.thirdTrackId);
    CHECK(fixture.transport.playbackTransport.state().transport == audio::Transport::Playing);

    auto const feed = fixture.transport.notificationService.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().severity == NotificationSeverity::Warning);
    CHECK(feed.entries.front().lifetime == NotificationLifetime::history());
    REQUIRE(std::holds_alternative<NotificationReport>(feed.entries.front().message));
    auto const& report = std::get<NotificationReport>(feed.entries.front().message);
    CHECK(report.templateId == NotificationReportTemplate::PlaybackTracksSkipped);
    CHECK(report.count == 2);
  }

  TEST_CASE("PlaybackSuccession - three backend route refusals stop before a fourth committed start")
  {
    auto script = BackendOpenScript{.incompatibleOpenNumbers = {1, 2, 3}};
    auto fixture = PlaybackSuccessionTransportFixture{};
    fixture.buildFourTrackManualView();
    scriptBackendOpens(fixture, script);
    bool settled = false;
    auto const settledSubscription = fixture.successionPtr->onExplicitStartSettled([&] { settled = true; });
    std::size_t startedEvents = 0;
    auto nowPlayingTracks = std::vector<TrackId>{};
    auto const startedSubscription = fixture.transport.playbackTransport.onStarted([&] { ++startedEvents; });
    auto const nowPlayingSubscription = fixture.transport.playbackTransport.onNowPlayingChanged(
      [&](PlaybackTransport::NowPlayingChanged const& event) { nowPlayingTracks.push_back(event.trackId); });
    REQUIRE(fixture.successionPtr->playFromView(fixture.viewId, fixture.firstTrackId));
    REQUIRE(fixture.transport.executor.tryDrainUntil(
      [&]
      {
        return (settled && fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Inactive) ||
               script.committedOpens.load(std::memory_order_relaxed) >= 4;
      }));

    CHECK(script.committedOpens.load(std::memory_order_relaxed) == 3);
    CHECK(fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Inactive);
    CHECK(fixture.transport.playbackTransport.state().transport == audio::Transport::Idle);
    CHECK(startedEvents == 0);
    CHECK(nowPlayingTracks == std::vector{kInvalidTrackId});

    // The committed-open bound keeps this regression finite if the streak
    // ever retries indefinitely. Retire recovery before inspecting the report.
    std::ignore = fixture.transport.playbackTransport.stop();
    fixture.transport.executor.drain();
    auto const feed = fixture.transport.notificationService.feed();
    NotificationEntry const* skipSummary = nullptr;
    NotificationEntry const* failureLimit = nullptr;

    for (auto const& entry : feed.entries)
    {
      auto const* report = std::get_if<NotificationReport>(&entry.message);

      if (report != nullptr && report->templateId == NotificationReportTemplate::PlaybackTracksSkipped)
      {
        skipSummary = &entry;
        CHECK(report->count == 2);
      }

      if (report != nullptr && report->templateId == NotificationReportTemplate::PlaybackStoppedAfterFailures)
      {
        failureLimit = &entry;
        CHECK(report->count == 3);
      }
    }

    REQUIRE(skipSummary != nullptr);
    CHECK(skipSummary->severity == NotificationSeverity::Warning);
    CHECK(skipSummary->lifetime == NotificationLifetime::history());
    REQUIRE(failureLimit != nullptr);
    CHECK(failureLimit->severity == NotificationSeverity::Error);
    CHECK(failureLimit->lifetime == NotificationLifetime::pinned());
  }

  TEST_CASE("PlaybackSuccession - backend output fault stops immediately despite the shared error")
  {
    auto script = BackendOpenScript{.outputFaultOpenNumber = 1};
    auto fixture = PlaybackSuccessionTransportFixture{};
    fixture.buildTwoTrackManualView();
    scriptBackendOpens(fixture, script);
    bool settled = false;
    auto const settledSubscription = fixture.successionPtr->onExplicitStartSettled([&] { settled = true; });
    std::size_t startedEvents = 0;
    auto nowPlayingTracks = std::vector<TrackId>{};
    auto const startedSubscription = fixture.transport.playbackTransport.onStarted([&] { ++startedEvents; });
    auto const nowPlayingSubscription = fixture.transport.playbackTransport.onNowPlayingChanged(
      [&](PlaybackTransport::NowPlayingChanged const& event) { nowPlayingTracks.push_back(event.trackId); });
    REQUIRE(fixture.successionPtr->playFromView(fixture.viewId, fixture.firstTrackId));
    REQUIRE(fixture.transport.executor.tryDrainUntil(
      [&]
      {
        return (settled && fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Inactive) ||
               script.committedOpens.load(std::memory_order_relaxed) >= 2;
      }));

    CHECK(script.committedOpens.load(std::memory_order_relaxed) == 1);
    CHECK(fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Inactive);
    CHECK(fixture.transport.playbackTransport.state().transport == audio::Transport::Idle);
    CHECK(startedEvents == 0);
    CHECK(nowPlayingTracks == std::vector{kInvalidTrackId});
    CHECK(fixture.transport.playbackTransport.state().nowPlaying.trackId == kInvalidTrackId);

    std::ignore = fixture.transport.playbackTransport.stop();
    fixture.transport.executor.drain();
    auto const feed = fixture.transport.notificationService.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().severity == NotificationSeverity::Error);
    CHECK(feed.entries.front().lifetime == NotificationLifetime::pinned());
    REQUIRE(std::holds_alternative<NotificationReport>(feed.entries.front().message));
    auto const& report = std::get<NotificationReport>(feed.entries.front().message);
    CHECK(report.templateId == NotificationReportTemplate::PlaybackRouteActivationFailed);
    CHECK(report.trackId == fixture.firstTrackId);
    CHECK(report.detail == "scripted output route refusal");
  }

  TEST_CASE("PlaybackSuccession - selecting a listed output reports failure when its native open fails")
  {
    auto disconnectedDac = PlaybackFixtureSpyBackend{};
    auto fixture = PlaybackSuccessionTransportFixture{};
    auto const trackId = fixture.addPlayableTrack("Playing Track");
    fixture.openManualView(std::array{trackId});
    auto playback = fixture.createPlayback();
    auto& transport = fixture.transport;
    auto const original = transport.status.devices.front();
    auto const dac = audio::Device{.id = audio::DeviceId{"disconnected-dac"}, .backendId = original.backendId};
    fakeit::When(Method(disconnectedDac.mock(), backendId)).AlwaysReturn(dac.backendId);
    fakeit::When(Method(disconnectedDac.mock(), profileId)).AlwaysReturn(audio::kProfileShared);
    fakeit::When(Method(disconnectedDac.mock(), open))
      .AlwaysReturn(std::unexpected{audio::BackendOpenFailure{
        .error = Error{.code = Error::Code::DeviceNotFound, .message = "DAC disconnected"},
        .cause = audio::BackendOpenFailure::Cause::OutputFailure,
      }});
    fakeit::When(Method(transport.mockProvider, createBackend))
      .AlwaysDo([&transport, &disconnectedDac, dacId = dac.id](audio::Device const& device, audio::ProfileId const&)
                { return device.id == dacId ? disconnectedDac.makeProxy() : transport.spyBackendPtr->makeProxy(); });
    transport.onDevicesChangedCb({original, dac});
    transport.executor.drain();
    REQUIRE(playback.commands().startFromView(fixture.viewId, trackId));
    REQUIRE(transport.executor.tryDrainUntil(
      [&] { return playback.snapshot().transport.transport == audio::Transport::Playing; }));
    REQUIRE(playback.snapshot().transport.output.selectedDevice.deviceId == original.id);
    REQUIRE(transport.notificationService.feed().entries.empty());

    playback.commands().setOutputDevice(dac.backendId, dac.id, audio::kProfileShared);

    fakeit::Verify(Method(disconnectedDac.mock(), open)).Once();
    fakeit::Verify(Method(disconnectedDac.mock(), start)).Never();
    auto const failed = playback.snapshot().transport;
    CHECK(failed.output.selectedDevice.deviceId == dac.id);
    CHECK(failed.ready);
    CHECK(failed.transport == audio::Transport::Error);
    CHECK(failed.nowPlaying.trackId == trackId);

    REQUIRE(transport.executor.tryDrainUntil([&] { return !transport.notificationService.feed().entries.empty(); }));
    auto const feed = transport.notificationService.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().severity == NotificationSeverity::Error);
    REQUIRE(std::holds_alternative<NotificationReport>(feed.entries.front().message));
    auto const& report = std::get<NotificationReport>(feed.entries.front().message);
    CHECK(report.templateId == NotificationReportTemplate::PlaybackRouteActivationFailed);
    CHECK(report.trackId == trackId);
    CHECK(report.detail == "DAC disconnected");
  }

  TEST_CASE("PlaybackSuccession - prepared drain fallback preserves typed backend refusal semantics", "[concurrency]")
  {
    auto const cause = GENERATE(
      audio::BackendOpenFailure::Cause::TrackRouteIncompatible, audio::BackendOpenFailure::Cause::OutputFailure);
    CAPTURE(cause);
    bool const recoverable = cause == audio::BackendOpenFailure::Cause::TrackRouteIncompatible;
    auto script = BackendOpenScript{};

    if (recoverable)
    {
      script.incompatibleOpenNumbers = {2};
    }
    else
    {
      script.outputFaultOpenNumber = 2;
    }

    // A lossy current stream prepares a logical DrainFallback synchronously,
    // before explicit-start settlement, without activating an audio successor.
    auto fixture = PlaybackSuccessionTransportFixture{PlaybackSuccessionTransportFixtureConfig{
      .lossyFileName = "transport-playable-0.flac",
    }};
    fixture.buildThreeTrackManualView();
    scriptBackendOpens(fixture, script);
    auto announcedTracks = std::vector<PlaybackTransport::NowPlayingChanged>{};
    auto const nowPlayingSubscription = fixture.transport.playbackTransport.onNowPlayingChanged(
      [&announcedTracks](PlaybackTransport::NowPlayingChanged const& event) { announcedTracks.push_back(event); });
    REQUIRE(playback_succession::playFromViewAndWait(
      *fixture.successionPtr, fixture.transport.executor, fixture.viewId, fixture.firstTrackId));
    REQUIRE(fixture.transport.playbackTransport.state().transport == audio::Transport::Playing);
    REQUIRE(fixture.successionPtr->state().currentTrackId == fixture.firstTrackId);
    REQUIRE(fixture.successionPtr->state().optResolvedSuccessor == fixture.secondTrackId);
    CHECK(script.committedOpens.load(std::memory_order_relaxed) == 1);
    CHECK(script.startedRoutes.load(std::memory_order_relaxed) == 1);
    CHECK(fixture.decoderActivationCount(fixture.secondTrackId) == 0);

    SECTION("the shared preparation has an exact logical-token disarm acknowledgement")
    {
      auto const optToken = fixture.transport.playbackTransport.clearPreparedNext();
      REQUIRE(optToken);
      CHECK(optToken->value > 0);
      CHECK(optToken->issuedGeneration > 0);
      CHECK_FALSE(fixture.transport.playbackTransport.clearPreparedNext());
      CHECK(script.committedOpens.load(std::memory_order_relaxed) == 1);
      CHECK(script.startedRoutes.load(std::memory_order_relaxed) == 1);
      CHECK(fixture.decoderActivationCount(fixture.secondTrackId) == 0);
    }

    SECTION("untouched preparation reaches the authoritative open only after natural drain")
    {
      // Do not disarm, seek, navigate or replace the commitment in this branch.
      REQUIRE(fixture.transport.renderTarget != nullptr);
      auto* const currentTarget = fixture.transport.renderTarget;
      auto output = std::array<std::byte, 4096>{};
      REQUIRE(fixture.transport.executor.tryDrainUntil([&] { return currentTarget->renderPcm(output).drained; }));
      CHECK(script.committedOpens.load(std::memory_order_relaxed) == 1);
      CHECK(script.startedRoutes.load(std::memory_order_relaxed) == 1);
      CHECK(fixture.successionPtr->state().currentTrackId == fixture.firstTrackId);
      currentTarget->handleDrainComplete();
      REQUIRE(fixture.transport.executor.tryDrainUntil(
        [&]
        {
          return (fixture.successionPtr->state().currentTrackId == fixture.thirdTrackId &&
                  fixture.transport.playbackTransport.state().transport == audio::Transport::Playing) ||
                 fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Inactive ||
                 script.committedOpens.load(std::memory_order_relaxed) >= 4;
        }));
      CHECK_FALSE(fixture.transport.playbackTransport.clearPreparedNext());
      auto const feed = fixture.transport.notificationService.feed();
      REQUIRE(feed.entries.size() == 1);
      auto const& entry = feed.entries.front();
      REQUIRE(std::holds_alternative<NotificationReport>(entry.message));
      auto const& report = std::get<NotificationReport>(entry.message);

      if (recoverable)
      {
        CHECK(announcedTracks.empty());
        CHECK(script.committedOpens.load(std::memory_order_relaxed) == 3);
        CHECK(script.succeededOpens.load(std::memory_order_relaxed) == 2);
        CHECK(script.startedRoutes.load(std::memory_order_relaxed) == 2);
        CHECK(fixture.successionPtr->state().currentTrackId == fixture.thirdTrackId);
        CHECK(fixture.transport.playbackTransport.state().nowPlaying.trackId == fixture.thirdTrackId);
        CHECK(fixture.transport.playbackTransport.state().transport == audio::Transport::Playing);
        CHECK(entry.severity == NotificationSeverity::Warning);
        CHECK(entry.lifetime == NotificationLifetime::history());
        CHECK(report.templateId == NotificationReportTemplate::PlaybackTracksSkipped);
        CHECK(report.count == 1);
      }
      else
      {
        REQUIRE(announcedTracks.size() == 1);
        CHECK(announcedTracks[0].trackId == kInvalidTrackId);
        CHECK_FALSE(announcedTracks[0].optPreparedNextToken);
        CHECK(script.committedOpens.load(std::memory_order_relaxed) == 2);
        CHECK(script.succeededOpens.load(std::memory_order_relaxed) == 1);
        CHECK(script.startedRoutes.load(std::memory_order_relaxed) == 1);
        CHECK(fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Inactive);
        CHECK(fixture.transport.playbackTransport.state().transport == audio::Transport::Idle);
        CHECK(fixture.transport.playbackTransport.state().nowPlaying.trackId == kInvalidTrackId);
        CHECK(entry.severity == NotificationSeverity::Error);
        CHECK(entry.lifetime == NotificationLifetime::pinned());
        CHECK(report.templateId == NotificationReportTemplate::PlaybackRouteActivationFailed);
        CHECK(report.trackId == fixture.secondTrackId);
        CHECK(report.detail == "scripted output route refusal");
      }
    }
  }

  TEST_CASE("PlaybackSuccession - a successful backend open alone does not clear the failure streak")
  {
    // The third route opens successfully, but its final decoder setup still
    // fails as a recoverable track failure. A mere successful open must not
    // clear the streak, so this third consecutive failure stops the sequence
    // before the fourth track is ever committed.
    auto script = BackendOpenScript{.incompatibleOpenNumbers = {1, 2}};
    auto fixture = PlaybackSuccessionTransportFixture{PlaybackSuccessionTransportFixtureConfig{
      .finalOpenFailureFileNames = {"transport-playable-2.flac"},
    }};
    fixture.buildFourTrackManualView();
    scriptBackendOpens(fixture, script);
    bool settled = false;
    auto const settledSubscription = fixture.successionPtr->onExplicitStartSettled([&] { settled = true; });
    REQUIRE(fixture.successionPtr->playFromView(fixture.viewId, fixture.firstTrackId));
    REQUIRE(fixture.transport.executor.tryDrainUntil(
      [&]
      {
        return (settled && fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Inactive) ||
               script.committedOpens.load(std::memory_order_relaxed) >= 4;
      }));

    CHECK(script.committedOpens.load(std::memory_order_relaxed) == 3);
    CHECK(script.succeededOpens.load(std::memory_order_relaxed) == 1);
    CHECK(fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Inactive);
    CHECK(fixture.transport.playbackTransport.state().transport == audio::Transport::Idle);

    std::ignore = fixture.transport.playbackTransport.stop();
    fixture.transport.executor.drain();
    auto const feed = fixture.transport.notificationService.feed();
    NotificationEntry const* skipSummary = nullptr;
    NotificationEntry const* failureLimit = nullptr;

    for (auto const& entry : feed.entries)
    {
      auto const* report = std::get_if<NotificationReport>(&entry.message);

      if (report != nullptr && report->templateId == NotificationReportTemplate::PlaybackTracksSkipped)
      {
        skipSummary = &entry;
        CHECK(report->count == 2);
      }

      if (report != nullptr && report->templateId == NotificationReportTemplate::PlaybackStoppedAfterFailures)
      {
        failureLimit = &entry;
        CHECK(report->count == 3);
      }
    }

    REQUIRE(skipSummary != nullptr);
    REQUIRE(failureLimit != nullptr);
    CHECK(failureLimit->severity == NotificationSeverity::Error);
    CHECK(failureLimit->lifetime == NotificationLifetime::pinned());
  }

  TEST_CASE("PlaybackSuccession - an actually started backend route clears the failure streak", "[concurrency]")
  {
    auto script = BackendOpenScript{.incompatibleOpenNumbers = {1, 2, 4}};
    auto failureGate = audio::test::StagedFailureGate{};
    auto fixture = PlaybackSuccessionTransportFixture{PlaybackSuccessionTransportFixtureConfig{
      .stagedFailureFileNames = {"transport-playable-2.flac"},
      .stagedFailureGate = &failureGate,
    }};
    auto releaseGuard = audio::test::StagedFailureReleaseGuard{failureGate};
    auto const tracks = std::array{fixture.addPlayableTrack("First refusal"),
                                   fixture.addPlayableTrack("Second refusal"),
                                   fixture.addPlayableTrack("Started route"),
                                   fixture.addPlayableTrack("Third refusal"),
                                   fixture.addPlayableTrack("Second start")};
    fixture.openManualView(tracks);
    scriptBackendOpens(fixture, script);
    bool settled = false;
    auto const settledSubscription = fixture.successionPtr->onExplicitStartSettled([&] { settled = true; });
    REQUIRE(fixture.successionPtr->playFromView(fixture.viewId, tracks[0]));
    REQUIRE(fixture.transport.executor.tryDrainUntil(
      [&]
      {
        return (fixture.successionPtr->state().currentTrackId == tracks[2] &&
                fixture.transport.playbackTransport.state().transport == audio::Transport::Playing) ||
               script.committedOpens.load(std::memory_order_relaxed) >= 4 ||
               (settled && fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Inactive);
      }));
    REQUIRE(fixture.successionPtr->state().currentTrackId == tracks[2]);
    REQUIRE(fixture.transport.playbackTransport.state().transport == audio::Transport::Playing);
    REQUIRE(failureGate.tryWaitForRead());

    // The running track's decoder fails after the real start. Only that
    // started route can have cleared the earlier backend refusals; a navigation
    // command or a mere successful open must not.
    releaseGuard.release();
    REQUIRE(fixture.transport.executor.tryDrainUntil(
      [&]
      {
        return (fixture.successionPtr->state().currentTrackId == tracks[4] &&
                fixture.transport.playbackTransport.state().transport == audio::Transport::Playing) ||
               script.committedOpens.load(std::memory_order_relaxed) >= 6 ||
               fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Inactive;
      }));

    CHECK(script.committedOpens.load(std::memory_order_relaxed) == 5);
    CHECK(script.succeededOpens.load(std::memory_order_relaxed) == 2);
    CHECK(fixture.successionPtr->state().currentTrackId == tracks[4]);
    CHECK(fixture.transport.playbackTransport.state().nowPlaying.trackId == tracks[4]);
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

  TEST_CASE("PlaybackSuccession - accepted final decoder failure skips the failed candidate")
  {
    auto fixture = PlaybackSuccessionTransportFixture{PlaybackSuccessionTransportFixtureConfig{
      .finalOpenFailureFileNames = {"transport-playable-2.flac"},
    }};
    fixture.buildFourTrackManualView();
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));

    REQUIRE(fixture.successionPtr->playFromView(fixture.viewId, fixture.thirdTrackId));
    auto const recovered = fixture.transport.executor.tryDrainUntil(
      [&]
      {
        return fixture.successionPtr->state().currentTrackId == fixture.fourthTrackId &&
               fixture.transport.playbackTransport.state().transport == audio::Transport::Playing;
      },
      std::chrono::seconds{10});
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

  TEST_CASE("PlaybackSuccession - navigation stops after three consecutive unplayable candidates")
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

    fixture.successionPtr->tryMoveNext();

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

  TEST_CASE("PlaybackSuccession - non-recoverable device failure terminates the active session")
  {
    auto fixture = PlaybackSuccessionTransportFixture{};
    fixture.buildThreeTrackManualView();
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    REQUIRE(fixture.transport.renderTarget != nullptr);

    fixture.transport.renderTarget->handleBackendError("device lost during succession playback");
    REQUIRE(fixture.transport.executor.tryDrainUntil(
      [&] { return fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Inactive; }));

    CHECK(fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Inactive);
    CHECK(fixture.transport.playbackTransport.state().transport == audio::Transport::Idle);

    auto const feed = fixture.transport.notificationService.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().severity == NotificationSeverity::Error);
    CHECK(feed.entries.front().lifetime == NotificationLifetime::pinned());
    REQUIRE(std::holds_alternative<NotificationReport>(feed.entries.front().message));
    auto const& report = std::get<NotificationReport>(feed.entries.front().message);
    CHECK(report.templateId == NotificationReportTemplate::PlaybackDeviceLost);
    CHECK(report.detail == "device lost during succession playback");
  }

  TEST_CASE("PlaybackSuccession - track failure on an invalidated source posts one terminal succession error",
            "[concurrency]")
  {
    auto failureGate = audio::test::StagedFailureGate{};
    auto fixture = PlaybackSuccessionSeekFixture{&failureGate};
    auto releaseGuard = audio::test::StagedFailureReleaseGuard{failureGate};
    fixture.buildSingleTrackManualView();
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    REQUIRE(failureGate.tryWaitForRead());

    REQUIRE(fixture.commandsFixture.runTask(fixture.commands().deleteListAsync(fixture.listId)));
    REQUIRE(fixture.executor.tryDrainUntil(
      [&] { return fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Invalidated; }));
    releaseGuard.release();
    REQUIRE(fixture.executor.tryDrainUntil(
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

  TEST_CASE("PlaybackSuccession - shuffle failure walks preserve shuffle direction and semantic parity")
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

      succession.tryMoveNext();

      CHECK(succession.state().currentTrackId != fixture.firstTrackId);
      CHECK(succession.state().currentTrackId != *optFailedCandidate);
      CHECK(fixture.playbackTransport.state().transport == audio::Transport::Playing);
    }

    SECTION("failed history previous does not fall through to sequential previous")
    {
      succession.tryMoveNext();
      auto const currentTrackId = succession.state().currentTrackId;
      REQUIRE(currentTrackId != fixture.firstTrackId);
      REQUIRE(succession.state().hasPrevious);
      fixture.removePlayableFile(fixture.firstTrackId);

      succession.tryMovePrevious();

      CHECK(succession.state().currentTrackId == currentTrackId);
      CHECK_FALSE(succession.state().hasPrevious);
      CHECK(fixture.playbackTransport.state().transport == audio::Transport::Playing);
    }
  }
} // namespace ao::rt::test
