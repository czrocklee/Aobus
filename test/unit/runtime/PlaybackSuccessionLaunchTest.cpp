// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/playback/PlaybackSuccession.h"
#include "runtime/playback/PlaybackTransport.h"
#include "test/unit/audio/EngineTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/PlaybackSuccessionBaseTestSupport.h"
#include "test/unit/runtime/PlaybackSuccessionTransportTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/Transport.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewState.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <variant>

namespace ao::rt::test
{
  using playback_succession::PlaybackSuccessionFixture;
  using playback_succession::PlaybackSuccessionTransportFixture;
  using playback_succession::PlaybackSuccessionTransportFixtureConfig;
  using playback_succession::playFromViewAndWait;
  using playback_succession::PreparationReleaseGuard;

  TEST_CASE("PlaybackSuccession - strict launch commits only a validated captured spec",
            "[runtime][unit][playback-succession][launch]")
  {
    auto fixture = PlaybackSuccessionFixture{};
    auto const outsideTrackId = fixture.addPlayableTrack("Outside");
    fixture.buildThreeTrackManualView();
    auto& succession = *fixture.successionPtr;
    std::uint32_t changedCount = 0;
    auto const changedSubscription =
      succession.onChanged([&](PlaybackSuccessionState const&) noexcept { ++changedCount; });

    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    auto const accepted = succession.state();
    CHECK(accepted.sourceState == PlaybackSuccessionSourceState::Live);
    CHECK(accepted.currentTrackId == fixture.firstTrackId);
    CHECK(accepted.sourceListId == fixture.listId);
    CHECK(accepted.hasNext);
    CHECK_FALSE(accepted.hasPrevious);
    CHECK(accepted.optResolvedSuccessor == fixture.secondTrackId);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Playing);
    CHECK(changedCount == 1);

    SECTION("unknown view")
    {
      auto const rejectedRes = succession.playFromView(ViewId{999999}, fixture.secondTrackId);
      REQUIRE_FALSE(rejectedRes);
      CHECK(rejectedRes.error().code == Error::Code::NotFound);
    }

    SECTION("start absent from captured projection")
    {
      auto const rejectedRes = succession.playFromView(fixture.viewId, outsideTrackId);
      REQUIRE_FALSE(rejectedRes);
      CHECK(rejectedRes.error().code == Error::Code::NotFound);
    }

    CHECK(succession.state() == accepted);
    CHECK(fixture.playbackTransport.state().nowPlaying.trackId == fixture.firstTrackId);
    CHECK(changedCount == 1);
  }

  TEST_CASE("PlaybackSuccession - replaying the current track waits for a distinct explicit settlement",
            "[runtime][regression][playback-succession][concurrency]")
  {
    auto fixture = PlaybackSuccessionTransportFixture{};
    fixture.buildThreeTrackManualView();
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    std::uint32_t settlementCount = 0;
    auto const settlementSubscription =
      fixture.successionPtr->onExplicitStartSettled([&] noexcept { ++settlementCount; });

    REQUIRE(
      playFromViewAndWait(*fixture.successionPtr, fixture.transport.executor, fixture.viewId, fixture.firstTrackId));

    CHECK(settlementCount == 1);
    CHECK(fixture.successionPtr->state().currentTrackId == fixture.firstTrackId);
    CHECK(fixture.transport.playbackTransport.state().nowPlaying.trackId == fixture.firstTrackId);
    CHECK(fixture.transport.playbackTransport.state().transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSuccession - asynchronous preparation failure preserves the accepted session and transport",
            "[runtime][regression][playback-succession][launch]")
  {
    auto fixture = PlaybackSuccessionFixture{};
    auto const current = fixture.addPlayableTrack("Current");
    auto const successor = fixture.addPlayableTrack("Successor");
    auto const broken = fixture.addPlayableTrack("Broken");
    fixture.removePlayableFile(broken);
    fixture.openManualView(std::array{current, successor, broken});
    auto& succession = *fixture.successionPtr;

    REQUIRE(fixture.playAndWait(current));
    auto const sequenceBeforeRejection = succession.state();
    auto const transportBeforeRejection = fixture.playbackTransport.state();
    REQUIRE(sequenceBeforeRejection.optResolvedSuccessor == successor);
    std::uint32_t changedCount = 0;
    auto const changedSubscription =
      succession.onChanged([&](PlaybackSuccessionState const&) noexcept { ++changedCount; });

    auto const admittedRes = succession.playFromView(fixture.viewId, broken);

    REQUIRE(admittedRes);
    REQUIRE(fixture.executor.drainUntil([&fixture] { return !fixture.notifications.feed().entries.empty(); }));
    auto const rejectionFeed = fixture.notifications.feed();
    REQUIRE(rejectionFeed.entries.size() == 1);
    CHECK(rejectionFeed.entries.front().severity == NotificationSeverity::Error);
    CHECK(rejectionFeed.entries.front().lifetime == NotificationLifetime::pinned());
    REQUIRE(std::holds_alternative<NotificationReport>(rejectionFeed.entries.front().message));
    auto const& report = std::get<NotificationReport>(rejectionFeed.entries.front().message);
    CHECK(report.templateId == NotificationReportTemplate::PlaybackTrackOpenFailed);
    CHECK(report.trackId == broken);
    CHECK(report.subject == "Broken");
    CHECK_FALSE(report.detail.empty());
    CHECK(succession.state() == sequenceBeforeRejection);
    CHECK(changedCount == 0);

    auto const transportAfterRejection = fixture.playbackTransport.state();
    CHECK(transportAfterRejection.transport == transportBeforeRejection.transport);
    CHECK(transportAfterRejection.elapsed == transportBeforeRejection.elapsed);
    CHECK(transportAfterRejection.duration == transportBeforeRejection.duration);
    CHECK(transportAfterRejection.ready == transportBeforeRejection.ready);
    CHECK(transportAfterRejection.nowPlaying == transportBeforeRejection.nowPlaying);
    CHECK(transportAfterRejection.volume == transportBeforeRejection.volume);
    CHECK(transportAfterRejection.output == transportBeforeRejection.output);
    CHECK(transportAfterRejection.quality == transportBeforeRejection.quality);
    CHECK(transportAfterRejection.revision == transportBeforeRejection.revision);

    succession.next();

    CHECK(succession.state().currentTrackId == successor);
    CHECK(fixture.playbackTransport.state().nowPlaying.trackId == successor);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSuccession - pending start adopts the latest repeat and shuffle modes",
            "[runtime][regression][playback-succession][concurrency]")
  {
    auto gatePtr = std::make_shared<audio::test::BlockingPreparationGate>();
    auto fixture = PlaybackSuccessionTransportFixture{PlaybackSuccessionTransportFixtureConfig{
      .blockingGatePtr = gatePtr,
      .blockedFileName = "transport-playable-1.flac",
    }};
    fixture.buildThreeTrackManualView();
    auto& succession = *fixture.successionPtr;
    REQUIRE(succession.playFromView(fixture.viewId, fixture.secondTrackId));
    REQUIRE(gatePtr->waitForEntry());
    auto releaseGuard = PreparationReleaseGuard{gatePtr};

    SECTION("repeat")
    {
      succession.setRepeatMode(RepeatMode::One);
      releaseGuard.release();
      REQUIRE(fixture.transport.executor.drainUntil(
        [&] { return succession.state().currentTrackId == fixture.secondTrackId; }));

      CHECK(succession.state().repeat == RepeatMode::One);
      CHECK(succession.state().optResolvedSuccessor == fixture.secondTrackId);
    }

    SECTION("shuffle")
    {
      succession.setShuffleMode(ShuffleMode::On);
      releaseGuard.release();
      REQUIRE(fixture.transport.executor.drainUntil(
        [&] { return succession.state().currentTrackId == fixture.secondTrackId; }));
      REQUIRE(succession.state().shuffle == ShuffleMode::On);

      succession.previous();

      CHECK(succession.state().currentTrackId == fixture.secondTrackId);
      CHECK(fixture.transport.playbackTransport.state().nowPlaying.trackId == fixture.secondTrackId);
    }
  }

  TEST_CASE("PlaybackSuccession - newer pending start silently replaces blocked preparation",
            "[runtime][regression][playback-succession][concurrency]")
  {
    auto gatePtr = std::make_shared<audio::test::BlockingPreparationGate>();
    auto fixture = PlaybackSuccessionTransportFixture{PlaybackSuccessionTransportFixtureConfig{
      .blockingGatePtr = gatePtr,
      .blockedFileName = "transport-playable-1.flac",
    }};
    fixture.buildThreeTrackManualView();
    auto& succession = *fixture.successionPtr;
    REQUIRE(succession.playFromView(fixture.viewId, fixture.secondTrackId));
    REQUIRE(gatePtr->waitForEntry());
    auto releaseGuard = PreparationReleaseGuard{gatePtr};

    REQUIRE(succession.playFromView(fixture.viewId, fixture.thirdTrackId));
    releaseGuard.release();
    REQUIRE(fixture.transport.executor.drainUntil(
      [&] { return succession.state().currentTrackId == fixture.thirdTrackId; }, std::chrono::seconds{5}));

    CHECK(fixture.transport.playbackTransport.state().nowPlaying.trackId == fixture.thirdTrackId);
    CHECK(fixture.transport.notificationService.feed().entries.empty());
    CHECK(gatePtr->createdPtr->load(std::memory_order_relaxed) > 0);
    CHECK(gatePtr->destroyedPtr->load(std::memory_order_relaxed) ==
          gatePtr->createdPtr->load(std::memory_order_relaxed));
  }

  TEST_CASE("PlaybackSuccession - final seek silently cancels blocked pending start",
            "[runtime][regression][playback-succession][concurrency]")
  {
    auto gatePtr = std::make_shared<audio::test::BlockingPreparationGate>();
    auto fixture = PlaybackSuccessionTransportFixture{PlaybackSuccessionTransportFixtureConfig{
      .blockingGatePtr = gatePtr,
      .blockedFileName = "transport-playable-2.flac",
    }};
    fixture.buildThreeTrackManualView();
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    auto& succession = *fixture.successionPtr;
    REQUIRE(succession.playFromView(fixture.viewId, fixture.thirdTrackId));
    REQUIRE(gatePtr->waitForEntry());
    auto releaseGuard = PreparationReleaseGuard{gatePtr};

    fixture.transport.playbackTransport.seek(std::chrono::milliseconds{0}, PlaybackTransport::SeekMode::Final);
    releaseGuard.release();
    REQUIRE(fixture.transport.executor.drainUntil(
      [&]
      {
        return gatePtr->createdPtr->load(std::memory_order_relaxed) > 0 &&
               gatePtr->destroyedPtr->load(std::memory_order_relaxed) ==
                 gatePtr->createdPtr->load(std::memory_order_relaxed);
      },
      std::chrono::seconds{5}));

    CHECK(succession.state().currentTrackId == fixture.firstTrackId);
    CHECK(fixture.transport.playbackTransport.state().nowPlaying.trackId == fixture.firstTrackId);
    CHECK(fixture.transport.notificationService.feed().entries.empty());
  }

  TEST_CASE("PlaybackSuccession - output route change silently cancels blocked pending start",
            "[runtime][regression][playback-succession][concurrency]")
  {
    auto gatePtr = std::make_shared<audio::test::BlockingPreparationGate>();
    auto fixture = PlaybackSuccessionTransportFixture{PlaybackSuccessionTransportFixtureConfig{
      .blockingGatePtr = gatePtr,
      .blockedFileName = "transport-playable-2.flac",
    }};
    fixture.buildThreeTrackManualView();
    fixture.transport.status.devices.push_back(audio::Device{.id = audio::DeviceId{"alternate-device"},
                                                             .displayName = "Alternate",
                                                             .description = "Alternate output",
                                                             .isDefault = false,
                                                             .backendId = audio::BackendId{"mock_backend"}});
    fixture.transport.onDevicesChangedCb(fixture.transport.status.devices);
    fixture.transport.executor.drain();
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    auto& succession = *fixture.successionPtr;
    REQUIRE(succession.playFromView(fixture.viewId, fixture.thirdTrackId));
    REQUIRE(gatePtr->waitForEntry());
    auto releaseGuard = PreparationReleaseGuard{gatePtr};

    fixture.transport.playbackTransport.setOutputDevice(
      audio::BackendId{"mock_backend"}, audio::DeviceId{"alternate-device"}, audio::ProfileId{audio::kProfileShared});
    releaseGuard.release();
    REQUIRE(fixture.transport.executor.drainUntil(
      [&]
      {
        return gatePtr->createdPtr->load(std::memory_order_relaxed) > 0 &&
               gatePtr->destroyedPtr->load(std::memory_order_relaxed) ==
                 gatePtr->createdPtr->load(std::memory_order_relaxed);
      },
      std::chrono::seconds{5}));

    CHECK(succession.state().currentTrackId == fixture.firstTrackId);
    CHECK(fixture.transport.playbackTransport.state().nowPlaying.trackId == fixture.firstTrackId);
    CHECK(fixture.transport.notificationService.feed().entries.empty());
  }

  TEST_CASE("PlaybackSuccession - lookahead open failure rerolls the sticky shuffle successor",
            "[runtime][regression][playback-succession][concurrency]")
  {
    auto gatePtr = std::make_shared<audio::test::BlockingPreparationGate>();
    auto fixture = PlaybackSuccessionTransportFixture{PlaybackSuccessionTransportFixtureConfig{
      .blockingGatePtr = gatePtr,
      .failBlockedPreparation = true,
      .blockEveryLookahead = true,
    }};
    fixture.buildThreeTrackManualView();
    auto& succession = *fixture.successionPtr;
    succession.setShuffleMode(ShuffleMode::On);
    REQUIRE(playFromViewAndWait(succession, fixture.transport.executor, fixture.viewId, fixture.firstTrackId));
    REQUIRE(gatePtr->waitForEntry());
    auto releaseGuard = PreparationReleaseGuard{gatePtr};
    fixture.transport.executor.drain();
    auto const optInitialSuccessor = succession.state().optResolvedSuccessor;
    REQUIRE(optInitialSuccessor);

    releaseGuard.release();
    auto const settled = fixture.transport.executor.drainUntil(
      [&]
      {
        auto const& state = succession.state();
        auto const created = gatePtr->createdPtr->load(std::memory_order_relaxed);
        // A queued observation of the committed start may replace the first
        // blocked lookahead. Decoder construction count is therefore
        // scheduling-dependent; the contract is reroll plus full reclamation.
        return created >= 2 && gatePtr->destroyedPtr->load(std::memory_order_relaxed) == created &&
               state.optResolvedSuccessor && state.optResolvedSuccessor != optInitialSuccessor;
      },
      std::chrono::seconds{15});
    INFO("created=" << gatePtr->createdPtr->load(std::memory_order_relaxed)
                    << " destroyed=" << gatePtr->destroyedPtr->load(std::memory_order_relaxed)
                    << " initial=" << optInitialSuccessor->raw()
                    << " resolved=" << succession.state().optResolvedSuccessor.value_or(kInvalidTrackId).raw());
    REQUIRE(settled);

    CHECK(succession.state().currentTrackId == fixture.firstTrackId);
    CHECK(succession.state().shuffle == ShuffleMode::On);
    CHECK(succession.state().hasNext);
    REQUIRE(succession.state().optResolvedSuccessor);
    CHECK(succession.state().optResolvedSuccessor != optInitialSuccessor);
    CHECK(fixture.transport.playbackTransport.state().nowPlaying.trackId == fixture.firstTrackId);
    CHECK(fixture.transport.notificationService.feed().entries.empty());
  }

  TEST_CASE("PlaybackSuccession - device evidence refresh silently rejects a stale pending start",
            "[runtime][regression][playback-succession][concurrency]")
  {
    auto gatePtr = std::make_shared<audio::test::BlockingPreparationGate>();
    auto fixture = PlaybackSuccessionTransportFixture{PlaybackSuccessionTransportFixtureConfig{
      .blockingGatePtr = gatePtr,
      .blockedFileName = "transport-playable-2.flac",
    }};
    fixture.buildThreeTrackManualView();
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    fixture.transport.executor.drain();
    auto& succession = *fixture.successionPtr;
    REQUIRE(succession.playFromView(fixture.viewId, fixture.thirdTrackId));
    REQUIRE(gatePtr->waitForEntry());
    auto releaseGuard = PreparationReleaseGuard{gatePtr};

    fixture.transport.status.devices.front().description = "Refreshed active device evidence";
    fixture.transport.onDevicesChangedCb(fixture.transport.status.devices);
    fixture.transport.executor.drain();
    releaseGuard.release();
    REQUIRE(fixture.transport.executor.waitUntilQueuedCount(1, std::chrono::seconds{5}));
    fixture.transport.executor.drain();

    CHECK(succession.state().currentTrackId == fixture.firstTrackId);
    CHECK(fixture.transport.playbackTransport.state().nowPlaying.trackId == fixture.firstTrackId);
    CHECK(fixture.transport.playbackTransport.state().transport == audio::Transport::Playing);
    CHECK(fixture.transport.notificationService.feed().entries.empty());
  }

  TEST_CASE("PlaybackSuccession - changed playback input supersedes a blocked failing start",
            "[runtime][regression][playback-succession][concurrency]")
  {
    auto gatePtr = std::make_shared<audio::test::BlockingPreparationGate>();
    auto fixture = PlaybackSuccessionTransportFixture{PlaybackSuccessionTransportFixtureConfig{
      .blockingGatePtr = gatePtr,
      .blockedFileName = "transport-playable-2.flac",
      .failBlockedPreparation = true,
    }};
    fixture.buildThreeTrackManualView();
    fixture.commandsFixture.releaseLibrary();
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    auto const successionBefore = fixture.successionPtr->state();
    auto const transportBefore = fixture.transport.playbackTransport.state();
    REQUIRE(fixture.successionPtr->playFromView(fixture.viewId, fixture.thirdTrackId));
    REQUIRE(gatePtr->waitForEntry());
    auto releaseGuard = PreparationReleaseGuard{gatePtr};

    fixture.transport.libraryFixture.updateTrack(
      fixture.thirdTrackId, [](library::test::TrackSpec& spec) { spec.uri = "replacement-after-admission.flac"; });
    releaseGuard.release();
    REQUIRE(fixture.transport.executor.drainUntil(
      [&] { return gatePtr->destroyedPtr->load(std::memory_order_relaxed) > 0; }, std::chrono::seconds{5}));

    CHECK(fixture.successionPtr->state() == successionBefore);
    CHECK(fixture.transport.playbackTransport.state().transport == transportBefore.transport);
    CHECK(fixture.transport.playbackTransport.state().nowPlaying == transportBefore.nowPlaying);
    CHECK(fixture.transport.playbackTransport.state().revision == transportBefore.revision);
    CHECK(fixture.transport.notificationService.feed().entries.empty());
  }

  TEST_CASE("PlaybackSuccession - an accepted launch publishes consistent state to connected observers",
            "[runtime][regression][playback-succession][launch]")
  {
    auto fixture = PlaybackSuccessionFixture{};
    fixture.buildThreeTrackManualView();
    auto& succession = *fixture.successionPtr;
    auto& playbackTransport = fixture.playbackTransport;
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));

    // These contract-fulfilling observers see the launch in connection order,
    // and publication finishes with state consistent across the two owners.
    bool changedObserverEntered = false;
    auto trailingObserverTrackId = kInvalidTrackId;
    auto changedSubscription =
      succession.onChanged([&](PlaybackSuccessionState const&) noexcept { changedObserverEntered = true; });
    auto trailingSubscription = succession.onChanged([&](PlaybackSuccessionState const& state) noexcept
                                                     { trailingObserverTrackId = state.currentTrackId; });

    auto const launchedRes = succession.playFromView(fixture.viewId, fixture.thirdTrackId);

    REQUIRE(launchedRes);
    REQUIRE(fixture.executor.drainUntil(
      [&] { return playbackTransport.state().nowPlaying.trackId == fixture.thirdTrackId; }));
    CHECK(changedObserverEntered);
    CHECK(trailingObserverTrackId == fixture.thirdTrackId);
    CHECK(succession.state().sourceState == PlaybackSuccessionSourceState::Live);
    CHECK(succession.state().currentTrackId == fixture.thirdTrackId);
    CHECK(succession.state().shuffle == ShuffleMode::Off);
    CHECK(succession.state().repeat == RepeatMode::Off);
    CHECK(playbackTransport.state().transport == audio::Transport::Playing);
    CHECK(playbackTransport.state().nowPlaying.trackId == succession.state().currentTrackId);

    changedSubscription.reset();
    trailingSubscription.reset();
    succession.previous();

    CHECK(succession.state().currentTrackId == fixture.secondTrackId);
    CHECK(playbackTransport.state().nowPlaying.trackId == fixture.secondTrackId);
    CHECK(playbackTransport.state().transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSuccession - live membership governs succession without interrupting current audio",
            "[runtime][unit][playback-succession][projection]")
  {
    auto fixture = PlaybackSuccessionFixture{};
    fixture.buildThreeTrackManualView();
    auto& succession = *fixture.successionPtr;
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    auto const beforeRemoval = succession.state();

    fixture.removeFromList(std::array{fixture.firstTrackId});

    auto const afterRemoval = succession.state();
    CHECK(afterRemoval.currentTrackId == fixture.firstTrackId);
    CHECK(afterRemoval.sourceState == PlaybackSuccessionSourceState::Live);
    CHECK(afterRemoval.optResolvedSuccessor == fixture.secondTrackId);
    CHECK(afterRemoval == beforeRemoval);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Playing);
    CHECK(fixture.playbackTransport.state().nowPlaying.trackId == fixture.firstTrackId);

    REQUIRE(fixture.commandsFixture.runTask(fixture.commands().deleteList(fixture.listId)));
    auto const invalidated = succession.state();
    CHECK(invalidated.sourceState == PlaybackSuccessionSourceState::Invalidated);
    CHECK(invalidated.currentTrackId == fixture.firstTrackId);
    CHECK_FALSE(invalidated.hasNext);
    CHECK_FALSE(invalidated.optResolvedSuccessor);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Playing);

    auto const rejectedRelaunchRes = succession.playFromView(fixture.viewId, fixture.firstTrackId);
    REQUIRE_FALSE(rejectedRelaunchRes);
    CHECK(rejectedRelaunchRes.error().code == Error::Code::NotFound);
    CHECK(succession.state() == invalidated);

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

  TEST_CASE("PlaybackSuccession - launch spec remains detached from later view edits and destruction",
            "[runtime][unit][playback-succession][launch]")
  {
    auto fixture = PlaybackSuccessionFixture{};
    fixture.buildThreeTrackManualView(TrackListViewConfig{.filterExpression = "$year >= 2000"});
    auto& succession = *fixture.successionPtr;
    REQUIRE(fixture.playAndWait(fixture.secondTrackId));
    auto const captured = succession.state();
    REQUIRE(captured.optResolvedSuccessor == fixture.thirdTrackId);

    REQUIRE(fixture.views.setFilter(fixture.viewId, "$year < 2000"));
    REQUIRE(fixture.views.setPresentation(
      fixture.viewId,
      TrackPresentationSpec{.id = "reverse-title", .sortBy = {{.field = TrackSortField::Title, .ascending = false}}}));
    REQUIRE(fixture.workspace.closeView(fixture.viewId));

    CHECK(succession.state() == captured);
    CHECK(fixture.playbackTransport.state().nowPlaying.trackId == fixture.secondTrackId);

    succession.next();
    CHECK(succession.state().currentTrackId == fixture.thirdTrackId);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSuccession - strict launch rejects invalid quick filters and missing sources atomically",
            "[runtime][unit][playback-succession][launch]")
  {
    SECTION("invalid captured quick filter")
    {
      auto fixture = PlaybackSuccessionFixture{};
      fixture.buildThreeTrackManualView(TrackListViewConfig{.filterExpression = "("});

      auto const result = fixture.successionPtr->playFromView(fixture.viewId, fixture.firstTrackId);

      REQUIRE_FALSE(result);
      CHECK(fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Inactive);
      CHECK(fixture.playbackTransport.state().transport == audio::Transport::Idle);
    }

    SECTION("source deleted after view capture")
    {
      auto fixture = PlaybackSuccessionFixture{};
      fixture.buildThreeTrackManualView();
      REQUIRE(fixture.commandsFixture.runTask(fixture.commands().deleteList(fixture.listId)));

      auto const result = fixture.successionPtr->playFromView(fixture.viewId, fixture.firstTrackId);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::NotFound);
      CHECK(fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Inactive);
      CHECK(fixture.playbackTransport.state().transport == audio::Transport::Idle);
    }
  }
} // namespace ao::rt::test
