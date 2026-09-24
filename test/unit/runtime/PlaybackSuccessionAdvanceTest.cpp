// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/playback/PlaybackSuccession.h"
#include "runtime/playback/PlaybackTransport.h"
#include "test/unit/audio/EngineTestSupport.h"
#include "test/unit/runtime/PlaybackSuccessionBaseTestSupport.h"
#include "test/unit/runtime/PlaybackSuccessionSeekTestSupport.h"
#include "test/unit/runtime/PlaybackSuccessionTransportTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/Transport.h>
#include <ao/rt/PlaybackMode.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ao::rt::test
{
  using playback_succession::PlaybackSuccessionFixture;
  using playback_succession::PlaybackSuccessionSeekFixture;
  using playback_succession::PlaybackSuccessionTransportFixture;
  using playback_succession::PlaybackSuccessionTransportFixtureConfig;
  using playback_succession::PreparationReleaseGuard;

  TEST_CASE("PlaybackSuccession - idle fallback advances without a prepared successor",
            "[runtime][unit][playback-succession]")
  {
    auto fixture = PlaybackSuccessionTransportFixture{};
    fixture.buildThreeTrackManualView();
    auto& playbackTransport = fixture.transport.playbackTransport;
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    fixture.transport.executor.drain();
    REQUIRE(playbackTransport.clearPreparedNext());
    REQUIRE(fixture.transport.renderTarget != nullptr);

    auto output = std::array<std::byte, 4096>{};
    bool drained = false;

    for (std::int32_t attempt = 0; attempt < 100000 && !drained; ++attempt)
    {
      drained = fixture.transport.renderTarget->renderPcm(output).drained;
    }

    REQUIRE(drained);
    fixture.transport.renderTarget->handleDrainComplete();

    // The drain only enqueues a render-thread signal, so the advance lands an
    // executor hop after the first queued task. Waiting for the advance itself
    // rather than for a queued task keeps the pump thread out of the race.
    REQUIRE(fixture.transport.executor.tryDrainUntil(
      [&]
      {
        return fixture.successionPtr->state().currentTrackId == fixture.secondTrackId &&
               playbackTransport.state().transport == audio::Transport::Playing;
      },
      std::chrono::seconds{10}));

    CHECK(fixture.successionPtr->state().currentTrackId == fixture.secondTrackId);
    CHECK(playbackTransport.state().nowPlaying.trackId == fixture.secondTrackId);
    CHECK(playbackTransport.state().transport == audio::Transport::Playing);
    CHECK(fixture.transport.notificationService.feed().entries.empty());
  }

  TEST_CASE("PlaybackSuccession - output and final seek replace the disarmed lookahead token",
            "[runtime][unit][playback-succession][token]")
  {
    auto fixture = PlaybackSuccessionTransportFixture{};
    fixture.buildThreeTrackManualView();
    auto& playbackTransport = fixture.transport.playbackTransport;
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));

    auto const optFirstToken = playbackTransport.clearPreparedNext();
    REQUIRE(optFirstToken);
    auto const activationCount = fixture.lookaheadActivationCount(fixture.secondTrackId);

    SECTION("output edge")
    {
      playbackTransport.setOutputDevice(
        audio::BackendId{"mock_backend"}, audio::DeviceId{"mock_device"}, audio::ProfileId{audio::kProfileShared});
    }

    SECTION("final seek edge")
    {
      playbackTransport.seek(std::chrono::milliseconds{0}, PlaybackTransport::SeekMode::Final);
    }

    REQUIRE(fixture.tryWaitForLookaheadAfter(fixture.secondTrackId, activationCount));
    auto const optReplacementToken = playbackTransport.clearPreparedNext();
    REQUIRE(optReplacementToken);
    CHECK(*optReplacementToken != *optFirstToken);
    CHECK(fixture.successionPtr->state().currentTrackId == fixture.firstTrackId);
    CHECK(playbackTransport.state().transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSuccession - paired mode update replaces lookahead once from the final state",
            "[runtime][unit][playback-succession][token]")
  {
    auto fixture = PlaybackSuccessionTransportFixture{};
    fixture.buildThreeTrackManualView();
    auto& succession = *fixture.successionPtr;
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));

    auto signalStates = std::vector<PlaybackSuccessionState>{};
    auto const shuffleSubscription = succession.onShuffleModeChanged([&](PlaybackSuccession::ShuffleModeChanged const&)
                                                                     { signalStates.push_back(succession.state()); });
    auto const repeatSubscription = succession.onRepeatModeChanged([&](PlaybackSuccession::RepeatModeChanged const&)
                                                                   { signalStates.push_back(succession.state()); });
    auto activationCount = [&]
    {
      std::size_t count = 0;

      for (auto const& entry : fixture.decoderProbePtr->snapshot())
      {
        count += entry.second;
      }

      return count;
    };
    auto const beforeActivations = activationCount();

    succession.setPlaybackMode(ShuffleMode::On, RepeatMode::All);

    REQUIRE(fixture.transport.executor.tryDrainUntil(
      [&] { return activationCount() > beforeActivations; }, std::chrono::seconds{10}));
    fixture.transport.executor.drain();
    REQUIRE(signalStates.size() == 2);

    for (auto const& state : signalStates)
    {
      CHECK(state.shuffle == ShuffleMode::On);
      CHECK(state.repeat == RepeatMode::All);
    }

    CHECK(succession.state().shuffle == ShuffleMode::On);
    CHECK(succession.state().repeat == RepeatMode::All);
    CHECK(activationCount() == beforeActivations + 1);
  }

  TEST_CASE("PlaybackSuccession - natural prepared winner is adopted exactly once",
            "[runtime][unit][playback-succession][token][concurrency]")
  {
    auto fixture = PlaybackSuccessionTransportFixture{};
    fixture.buildThreeTrackManualView();
    auto events = std::vector<PlaybackTransport::NowPlayingChanged>{};
    auto const subscription = fixture.transport.playbackTransport.onNowPlayingChanged(
      [&](PlaybackTransport::NowPlayingChanged const& event) noexcept { events.push_back(event); });
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    fixture.transport.executor.drain();
    events.clear();

    fixture.queueNaturalAdvance();
    fixture.transport.executor.drain();

    REQUIRE(events.size() == 1);
    REQUIRE(events.front().optPreparedNextToken);
    CHECK(events.front().trackId == fixture.secondTrackId);
    CHECK(fixture.successionPtr->state().currentTrackId == fixture.secondTrackId);
    CHECK(fixture.transport.playbackTransport.state().transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSuccession - positioning commands retain a naturally advanced winner",
            "[runtime][unit][playback-succession][concurrency]")
  {
    auto fixture = PlaybackSuccessionTransportFixture{};
    fixture.buildThreeTrackManualView();
    auto events = std::vector<PlaybackTransport::NowPlayingChanged>{};
    std::size_t finalSeekCount = 0;
    auto const nowPlayingSubscription = fixture.transport.playbackTransport.onNowPlayingChanged(
      [&](PlaybackTransport::NowPlayingChanged const& event) noexcept { events.push_back(event); });
    auto const seekSubscription = fixture.transport.playbackTransport.onSeekUpdate(
      [&](PlaybackTransport::SeekUpdate const& update) noexcept
      {
        if (update.mode == PlaybackTransport::SeekMode::Final)
        {
          ++finalSeekCount;
        }
      });
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    fixture.transport.executor.drain();
    events.clear();
    auto const retiredOccurrence = fixture.transport.playbackTransport.state().occurrenceId;

    fixture.queueNaturalAdvance();
    std::size_t expectedFinalSeekCount = 0;

    SECTION("unconditional final seek preserves the prepared winner")
    {
      fixture.transport.playbackTransport.seek(std::chrono::milliseconds{0}, PlaybackTransport::SeekMode::Final);
      expectedFinalSeekCount = 1;
    }

    SECTION("guarded seek rejects the retired occurrence without a seek event")
    {
      CHECK_FALSE(fixture.transport.playbackTransport.trySeek(retiredOccurrence, std::chrono::milliseconds{0}));
    }

    SECTION("guarded Next observes the realtime winner and preserves its successor")
    {
      CHECK_FALSE(fixture.transport.playbackTransport.canAdvanceFrom(retiredOccurrence));
    }

    CHECK(finalSeekCount == expectedFinalSeekCount);
    fixture.transport.executor.drain();

    REQUIRE(events.size() == 1);
    REQUIRE(events.front().optPreparedNextToken);
    CHECK(events.front().trackId == fixture.secondTrackId);
    CHECK(fixture.successionPtr->state().currentTrackId == fixture.secondTrackId);
    CHECK(fixture.successionPtr->state().optResolvedSuccessor == fixture.thirdTrackId);
    CHECK(fixture.transport.playbackTransport.state().occurrenceId != retiredOccurrence);
    CHECK(fixture.transport.playbackTransport.state().transport == audio::Transport::Playing);
    CHECK(finalSeekCount == expectedFinalSeekCount);
  }

  TEST_CASE("PlaybackSuccession - natural gapless advance invalidates a pending explicit start",
            "[runtime][unit][playback-succession][concurrency]")
  {
    auto gatePtr = std::make_shared<audio::test::BlockingPreparationGate>();
    auto fixture = PlaybackSuccessionTransportFixture{PlaybackSuccessionTransportFixtureConfig{
      .blockingGatePtr = gatePtr,
      .blockedFileName = "transport-playable-2.flac",
    }};
    auto releaseGuard = PreparationReleaseGuard{gatePtr};
    fixture.buildThreeTrackManualView();
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    fixture.queueNaturalAdvance();

    REQUIRE(fixture.successionPtr->playFromView(fixture.viewId, fixture.thirdTrackId));
    REQUIRE(gatePtr->tryWaitForEntry());
    fixture.transport.executor.drain();
    REQUIRE(fixture.successionPtr->state().currentTrackId == fixture.secondTrackId);
    auto const activationCount = fixture.lookaheadActivationCount(fixture.thirdTrackId);

    releaseGuard.release();
    // On this fixture's single preparation worker, the winner's new lookahead
    // settles after the canceled inspection. Its live decoder must not be
    // mistaken for an unreclaimed explicit candidate for the same track.
    REQUIRE(fixture.tryWaitForLookaheadAfter(fixture.thirdTrackId, activationCount));
    auto const created = gatePtr->createdPtr->load(std::memory_order_relaxed);
    auto const destroyed = gatePtr->destroyedPtr->load(std::memory_order_relaxed);
    CHECK(destroyed > 0);
    CHECK(created == destroyed + 1);

    CHECK(fixture.successionPtr->state().currentTrackId == fixture.secondTrackId);
    CHECK(fixture.transport.playbackTransport.state().nowPlaying.trackId == fixture.secondTrackId);
    CHECK(fixture.transport.playbackTransport.state().transport == audio::Transport::Playing);
    CHECK(fixture.transport.notificationService.feed().entries.empty());
  }

  TEST_CASE("PlaybackSuccession - destruction disconnects a queued natural-advance callback",
            "[runtime][unit][playback-succession][concurrency]")
  {
    auto fixture = PlaybackSuccessionTransportFixture{};
    fixture.buildThreeTrackManualView();
    std::uint32_t changedCount = 0;
    auto const subscription =
      fixture.successionPtr->onChanged([&](PlaybackSuccessionState const&) noexcept { ++changedCount; });
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    REQUIRE(changedCount == 1);
    fixture.queueNaturalAdvance();

    fixture.successionPtr.reset();
    fixture.transport.executor.drain();

    CHECK(changedCount == 1);
    CHECK(fixture.transport.playbackTransport.state().nowPlaying.trackId == fixture.secondTrackId);
  }

  TEST_CASE("PlaybackSuccession - previous restart uses a strict greater-than three-second final seek",
            "[runtime][unit][playback-succession]")
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

    succession.tryMovePrevious();
    CHECK(succession.state().currentTrackId == fixture.firstTrackId);
    CHECK_FALSE(succession.state().hasPrevious);
    CHECK(playbackTransport.elapsed() == std::chrono::milliseconds{0});
    CHECK(playbackTransport.state().transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSuccession - Next and Previous follow the cursor", "[runtime][unit][playback-succession][command]")
  {
    auto fixture = PlaybackSuccessionFixture{};
    fixture.buildThreeTrackManualView();
    auto& succession = *fixture.successionPtr;
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));

    succession.tryMoveNext();
    CHECK(succession.state().currentTrackId == fixture.secondTrackId);
    CHECK(succession.state().hasPrevious);

    succession.tryMovePrevious();
    CHECK(succession.state().currentTrackId == fixture.firstTrackId);
  }

  TEST_CASE("PlaybackSuccession - dedicated mode signals notify only on changes",
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

    succession.setRepeatMode(RepeatMode::One);
    CHECK(succession.state().repeat == RepeatMode::One);
    CHECK(succession.state().optResolvedSuccessor == fixture.firstTrackId);
    CHECK(repeatEvents == 1);
    succession.setRepeatMode(RepeatMode::One);
    CHECK(repeatEvents == 1);

    succession.setShuffleMode(ShuffleMode::On);
    CHECK(succession.state().shuffle == ShuffleMode::On);
    CHECK(shuffleEvents == 1);
  }

  TEST_CASE("PlaybackSuccession - clear deactivates succession without stopping transport",
            "[runtime][unit][playback-succession][command]")
  {
    auto fixture = PlaybackSuccessionFixture{};
    fixture.buildThreeTrackManualView();
    auto& succession = *fixture.successionPtr;
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));

    succession.clear();
    CHECK(succession.state().sourceState == PlaybackSuccessionSourceState::Inactive);
    CHECK(succession.state().currentTrackId == kInvalidTrackId);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Playing);
    CHECK(fixture.playbackTransport.state().nowPlaying.trackId == fixture.firstTrackId);
  }
} // namespace ao::rt::test
