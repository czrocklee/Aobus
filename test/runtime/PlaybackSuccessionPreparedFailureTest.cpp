// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/NullBackend.h"
#include "runtime/playback/PlaybackSuccession.h"
#include "runtime/playback/PlaybackTransport.h"
#include "test/TestFixtureSupport.h"
#include "test/audio/BackendTestSupport.h"
#include "test/audio/EngineTestSupport.h"
#include "test/runtime/PlaybackSuccessionTransportTestSupport.h"
#include <ao/audio/RenderTarget.h>
#include <ao/audio/Transport.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/projection/TrackListProjection.h>

#include <catch2/catch_test_macros.hpp>
#include <fakeit.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <string>

namespace ao::rt::test
{
  TEST_CASE("PlaybackSuccession - a failed prepared successor remains eligible for foreground playback",
            "[concurrency]")
  {
    auto failureGate = audio::test::StagedFailureGate{};
    auto fixture = playback_succession::PlaybackSuccessionTransportFixture{
      playback_succession::PlaybackSuccessionTransportFixtureConfig{
        .stagedFailureFileNames = {"transport-playable-1.flac"},
        // Only the first B output decoder fails. An unexpected retry can finish and cannot hang teardown.
        .stagedFailureGateSequence = {&failureGate},
      }};
    auto releaseGuard = audio::test::StagedFailureReleaseGuard{failureGate};
    fixture.buildTwoTrackManualView();
    auto const a = fixture.firstTrackId;
    auto const b = fixture.secondTrackId;
    auto const& bPath = fixture.trackPaths.at(b);
    auto& seq = *fixture.successionPtr;
    auto& transport = fixture.transport.playbackTransport;
    auto& executor = fixture.transport.executor;
    audio::test::SpyBackend<audio::NullBackend>& spyBackend = *fixture.transport.spyBackendPtr;
    auto& backend = spyBackend.mock();

    REQUIRE(fixture.playAndWait(a));
    REQUIRE(failureGate.tryWaitForRead());
    auto const occurrence = transport.state().occurrenceId;
    REQUIRE(occurrence != PlaybackOccurrenceId{});
    REQUIRE(transport.state().transport == audio::Transport::Playing);
    REQUIRE(seq.state().optResolvedSuccessor == b);

    releaseGuard.release();
    fixture.settlePreparedFailure();
    REQUIRE(fixture.decoderProbePtr->snapshot().outputDecoderConstructions.at(bPath) == 1);
    REQUIRE(fixture.decoderActivationCount(b) == 1);

    // Refresh the same semantic A -> B edge (even a forced lookahead refresh must not re-open B).
    seq.setRepeatMode(RepeatMode::All);
    fixture.settleLookaheadPreparation();
    REQUIRE(fixture.decoderProbePtr->snapshot().outputDecoderConstructions.at(bPath) == 1);
    REQUIRE(seq.state().optResolvedSuccessor == b);
    REQUIRE(seq.state().sourceState == PlaybackSuccessionSourceState::Live);
    REQUIRE(seq.state().currentTrackId == a);
    REQUIRE(transport.state().nowPlaying.trackId == a);
    REQUIRE(transport.state().occurrenceId == occurrence);
    REQUIRE(transport.state().transport == audio::Transport::Playing);
    fakeit::Verify(Method(backend, start)).Once();
    REQUIRE(fixture.transport.notificationService.feed().entries.empty());

    auto const checkForegroundB = [&]
    {
      REQUIRE(fixture.tryWaitForDecoderActivationAfter(b, 1));
      REQUIRE(fixture.decoderProbePtr->snapshot().outputDecoderConstructions.at(bPath) == 2);
      CHECK(seq.state().currentTrackId == b);
      CHECK(transport.state().nowPlaying.trackId == b);
      CHECK(transport.state().occurrenceId != occurrence);
      CHECK(transport.state().transport == audio::Transport::Playing);
      fakeit::Verify(Method(backend, start)).Exactly(2);
      CHECK(fixture.transport.notificationService.feed().entries.empty());
    };

    SECTION("a final seek within A does not retry its failed prepared B")
    {
      constexpr auto kTargetPosition = std::chrono::milliseconds{100};
      REQUIRE(transport.elapsed() < kTargetPosition);
      REQUIRE(kTargetPosition < transport.state().duration);
      bool applied = false;
      auto const seekSub = transport.onSeekUpdate(
        [&](PlaybackTransport::SeekUpdate const& update) noexcept
        { applied = update.mode == PlaybackTransport::SeekMode::Final && update.elapsed == kTargetPosition; });
      REQUIRE(transport.trySeek(occurrence, kTargetPosition, PlaybackTransport::SeekMode::Final));
      REQUIRE(applied);
      fixture.settleLookaheadPreparation();
      REQUIRE(transport.elapsed() == kTargetPosition);
      REQUIRE(fixture.decoderProbePtr->snapshot().outputDecoderConstructions.at(bPath) == 1);

      seq.setRepeatMode(RepeatMode::Off);
      fixture.settleLookaheadPreparation();
      CHECK(seq.state().optResolvedSuccessor == b);
      CHECK(seq.state().currentTrackId == a);
      CHECK(transport.state().nowPlaying.trackId == a);
      CHECK(transport.state().occurrenceId == occurrence);
      CHECK(transport.state().transport == audio::Transport::Playing);
      CHECK(fixture.decoderProbePtr->snapshot().outputDecoderConstructions.at(bPath) == 1);
      CHECK(fixture.decoderActivationCount(b) == 1);
      CHECK(fixture.transport.notificationService.feed().entries.empty());
    }

    SECTION("A drains and B gets a fresh foreground start")
    {
      auto output = std::array<std::byte, 4096>{};
      auto* const target = fixture.transport.renderTarget;
      REQUIRE(target != nullptr);
      REQUIRE(executor.tryDrainUntil([&] { return target->renderPcm(output).drained; }));
      target->handleDrainComplete();
      REQUIRE(executor.tryDrainUntil([&] { return seq.state().currentTrackId == b; }));
      checkForegroundB();
    }

    SECTION("an explicit launch of B is not suppressed")
    {
      REQUIRE(fixture.playAndWait(b));
      checkForegroundB();
    }

    SECTION("replaying A arms a healthy B on the new occurrence")
    {
      REQUIRE(fixture.playAndWait(a));
      REQUIRE(transport.state().occurrenceId != occurrence);
      REQUIRE(seq.state().currentTrackId == a);
      REQUIRE(transport.state().nowPlaying.trackId == a);
      REQUIRE(transport.state().transport == audio::Transport::Playing);
      REQUIRE(transport.clearPreparedNext());
      REQUIRE(fixture.decoderActivationCount(b) == 2);
      REQUIRE(fixture.decoderProbePtr->snapshot().outputDecoderConstructions.at(bPath) == 2);
      fakeit::Verify(Method(backend, start)).Exactly(2);
      CHECK(fixture.transport.notificationService.feed().entries.empty());
    }
  }

  TEST_CASE("PlaybackSuccession - a failed prepared repeat is not reopened when its row rejoins the projection",
            "[concurrency]")
  {
    auto failureGate = audio::test::StagedFailureGate{};
    auto fixture = playback_succession::PlaybackSuccessionTransportFixture{
      playback_succession::PlaybackSuccessionTransportFixtureConfig{
        .stagedFailureFileNames = {"transport-playable-0.flac"},
        // Only the prepared copy fails; the current source and all later opens are healthy.
        .stagedFailureGateSequence = {nullptr, &failureGate},
      }};
    auto releaseGuard = audio::test::StagedFailureReleaseGuard{failureGate};
    auto const track = fixture.addPlayableTrack("Repeated track");
    auto const tracks = std::array{track};
    fixture.openManualView(tracks);
    auto const membershipTag = std::array{std::string{"transportorder"}};
    auto const projectionPtr = ao::test::requireValue(fixture.views.findTrackListProjection(fixture.viewId));
    TrackListProjection const& projection = *projectionPtr;
    auto const& path = fixture.trackPaths.at(track);
    auto& succession = *fixture.successionPtr;
    auto& transport = fixture.transport.playbackTransport;
    audio::test::SpyBackend<audio::NullBackend>& backend = *fixture.transport.spyBackendPtr;
    succession.setRepeatMode(RepeatMode::One);
    REQUIRE(fixture.playAndWait(track));
    REQUIRE(fixture.tryWaitForDecoderActivationAfter(track, 1));
    REQUIRE(failureGate.tryWaitForRead());
    REQUIRE(fixture.decoderProbePtr->snapshot().outputDecoderConstructions.at(path) == 2);
    auto const occurrence = transport.state().occurrenceId;
    REQUIRE(occurrence != PlaybackOccurrenceId{});
    REQUIRE(transport.state().transport == audio::Transport::Playing);

    REQUIRE(projection.indexOf(track));
    REQUIRE(fixture.commandsFixture.editTags(tracks, {}, membershipTag));
    REQUIRE_FALSE(projection.indexOf(track));
    REQUIRE(succession.state().sourceState == PlaybackSuccessionSourceState::Live);
    REQUIRE(succession.state().optResolvedSuccessor == track);
    releaseGuard.release();
    fixture.settlePreparedFailure();
    REQUIRE(transport.state().occurrenceId == occurrence);
    REQUIRE(fixture.decoderProbePtr->snapshot().outputDecoderConstructions.at(path) == 2);

    // Rejoining refreshes lookahead for the unchanged playback input on the same occurrence.
    REQUIRE(fixture.commandsFixture.editTags(tracks, membershipTag, {}));
    REQUIRE(projection.indexOf(track));
    fixture.settleLookaheadPreparation();
    REQUIRE(fixture.decoderProbePtr->snapshot().outputDecoderConstructions.at(path) == 2);
    REQUIRE(fixture.decoderActivationCount(track) == 2);
    REQUIRE(succession.state().optResolvedSuccessor == track);
    REQUIRE(transport.state().nowPlaying.trackId == track);
    REQUIRE(transport.state().occurrenceId == occurrence);
    REQUIRE(transport.state().transport == audio::Transport::Playing);
    fakeit::Verify(Method(backend.mock(), start)).Once();
    REQUIRE(fixture.transport.notificationService.feed().entries.empty());

    REQUIRE(fixture.playAndWait(track));
    CHECK(transport.state().nowPlaying.trackId == track);
    CHECK(transport.state().occurrenceId != occurrence);
    CHECK(transport.state().transport == audio::Transport::Playing);
    fakeit::Verify(Method(backend.mock(), start)).Exactly(2);
    CHECK(fixture.transport.notificationService.feed().entries.empty());
  }
} // namespace ao::rt::test
