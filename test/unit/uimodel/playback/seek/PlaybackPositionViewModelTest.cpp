// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "runtime/playback/PlaybackBootstrap.h"
#include "runtime/playback/PlaybackTransport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/runtime/ApplicationPlaybackTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/PlaybackSuccessionTransportTestSupport.h"
#include "test/unit/runtime/PlaybackTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/Transport.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/playback/PlaybackEvents.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/playback/seek/PlaybackPosition.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <tuple>
#include <vector>

namespace ao::uimodel::test
{
  using namespace ao::rt::test;
  using namespace ao::rt::test::playback_succession;
  using namespace ao::rt;

  namespace
  {
    struct PlaybackRelativeSeekFixture final
    {
      PlaybackRelativeSeekFixture()
        : bootstrap{application.transport.playbackTransport}, playback{createPlayback(application, bootstrap)}
      {
        auto const previousRevision = playback.snapshot().transport.positionRevision;
        REQUIRE(playback.commands().startFromView(application.viewId, application.firstTrackId));
        REQUIRE(tryWaitForPlaybackSettlement(application.transport.executor,
                                             previousRevision,
                                             [this] { return playback.snapshot().transport.positionRevision; }));
        application.transport.executor.drain();
        REQUIRE(playback.snapshot().transport.nowPlaying.trackId == application.firstTrackId);
        REQUIRE(playback.snapshot().transport.duration > std::chrono::seconds{1});
        REQUIRE(application.transport.renderTarget != nullptr);
      }

      std::chrono::milliseconds renderBlockElapsed() const
      {
        auto output = std::array<std::byte, 4096>{};
        auto const renderRes = application.transport.renderTarget->renderPcm(output);
        REQUIRE(renderRes.bytesWritten > 0);
        REQUIRE(renderRes.positionFrames > 0);
        application.transport.renderTarget->handlePositionAdvanced(renderRes.positionFrames);
        return application.transport.playbackTransport.elapsed();
      }

      static PlaybackService createPlayback(PlaybackSuccessionTransportFixture& application,
                                            PlaybackBootstrap& bootstrap)
      {
        application.buildTwoTrackManualView();
        return bootstrap.createPlaybackService(application.transport.executor,
                                               *application.successionPtr,
                                               application.transport.libraryFixture.library(),
                                               application.changes);
      }

      PlaybackSuccessionTransportFixture application;
      PlaybackBootstrap bootstrap;
      PlaybackService playback;
    };
  } // namespace

  TEST_CASE("PlaybackPositionViewModel - reactive updates", "[uimodel][unit][playback]")
  {
    auto fixture = ApplicationPlaybackFixtureT<QueuedExecutor>{};
    auto& playback = fixture.playback;
    auto& playbackTransport = fixture.playbackTransport;
    fixture.addReadyProvider();
    REQUIRE(fixture.executor.tryDrainUntil([&] { return playbackTransport.state().ready; }));

    auto log = ao::test::RenderLog<PlaybackPositionViewState>{};
    auto viewModel = PlaybackPositionViewModel{playback, [&log](auto const& state) { log.render(state); }};

    SECTION("Initial state is insensitive when idle")
    {
      REQUIRE(!log.empty());
      CHECK(log.last().seekable == false);
      CHECK(log.last().duration == std::chrono::milliseconds{0});
      CHECK(log.last().elapsed == std::chrono::milliseconds{0});

      log.clear();
      fixture.commands().setShuffleMode(ShuffleMode::On);
      CHECK(fixture.playback.snapshot().succession.shuffle == ShuffleMode::On);
      CHECK(log.empty());
    }

    SECTION("seek commands")
    {
      auto const trackId =
        fixture.libraryFixture.addTrack({.title = "Seek Test", .artist = "Artist", .album = "Album"});
      auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
      auto const desc = PlaybackTransport::PlaybackRequest{
        .item = NowPlayingInfo{.trackId = trackId, .title = "Seek Test", .artist = "Artist"},
        .input = audio::PlaybackInput{.filePath = fixturePath, .duration = std::chrono::seconds{1}},
      };
      REQUIRE(playbackTransport.play(desc, kInvalidListId));
      REQUIRE(fixture.executor.tryDrainUntil(
        [&] { return playbackTransport.state().transport == audio::Transport::Playing; }));
      fixture.executor.drain();
      auto const expectedDuration = playbackTransport.state().duration;
      auto const occurrenceId = playback.snapshot().transport.occurrenceId;
      REQUIRE(expectedDuration > std::chrono::milliseconds{0});
      REQUIRE(occurrenceId.value != 0);

      log.clear();
      viewModel.seekPreview(occurrenceId, std::chrono::milliseconds{250});

      REQUIRE(!log.empty());
      CHECK(log.last().occurrenceId == occurrenceId);
      CHECK(log.last().duration == expectedDuration);
      CHECK(log.last().elapsed == std::chrono::milliseconds{250});
      CHECK(log.last().isPlaying == true);
      CHECK(log.last().seekable == true);
      CHECK(log.last().immediateUpdate == false);
      CHECK(playbackTransport.state().elapsed == std::chrono::milliseconds{0});

      viewModel.seekFinal(occurrenceId, std::chrono::milliseconds{500});
      fixture.executor.drain();

      CHECK(log.last().duration == expectedDuration);
      CHECK(log.last().elapsed == std::chrono::milliseconds{500});
      CHECK(log.last().isPlaying == true);
      CHECK(log.last().seekable == true);
      CHECK(log.last().immediateUpdate == true);
      CHECK(playbackTransport.state().duration == expectedDuration);

      auto seekEvents = std::vector<PlaybackTransport::SeekUpdate>{};
      auto seekSub = playbackTransport.onSeekUpdate([&seekEvents](PlaybackTransport::SeekUpdate const& event) noexcept
                                                    { seekEvents.push_back(event); });

      viewModel.seekBy(std::chrono::milliseconds{200});
      REQUIRE(seekEvents.size() == 1);
      CHECK(seekEvents.back().mode == PlaybackTransport::SeekMode::Final);
      CHECK(seekEvents.back().elapsed == std::chrono::milliseconds{700});

      viewModel.seekBy(-std::chrono::seconds{1});
      REQUIRE(seekEvents.size() == 2);
      CHECK(seekEvents.back().mode == PlaybackTransport::SeekMode::Final);
      CHECK(seekEvents.back().elapsed == std::chrono::milliseconds{0});

      viewModel.seekBy(expectedDuration + std::chrono::seconds{1});
      REQUIRE(seekEvents.size() == 3);
      CHECK(seekEvents.back().elapsed == expectedDuration);

      viewModel.seekBy(std::chrono::milliseconds::max());
      REQUIRE(seekEvents.size() == 4);
      CHECK(seekEvents.back().elapsed == expectedDuration);

      viewModel.seekBy(std::chrono::milliseconds::min());
      REQUIRE(seekEvents.size() == 5);
      CHECK(seekEvents.back().elapsed == std::chrono::milliseconds{0});

      log.clear();
      viewModel.seekPreview(occurrenceId, std::chrono::milliseconds{250});
      REQUIRE(!log.empty());
      CHECK(log.last().isPreviewing);

      bool requestedDuringPublication = false;
      auto const snapshotSub = playback.events().onSnapshot(
        [&](PlaybackSnapshot const&) noexcept
        {
          if (!requestedDuringPublication)
          {
            requestedDuringPublication = true;
            viewModel.seekFinal(occurrenceId, std::chrono::milliseconds{500});
          }
        });

      fixture.commands().setVolume(0.5F);
      REQUIRE(requestedDuringPublication);
      fixture.executor.drain();
      REQUIRE(!log.empty());
      CHECK(log.last().occurrenceId == occurrenceId);
      CHECK(log.last().elapsed == std::chrono::milliseconds{500});
      CHECK_FALSE(log.last().isPreviewing);
      CHECK(log.last().immediateUpdate);
    }

    SECTION("relative seek is unavailable without a known duration")
    {
      auto seekEvents = std::vector<PlaybackTransport::SeekUpdate>{};
      auto seekSub = playbackTransport.onSeekUpdate([&seekEvents](PlaybackTransport::SeekUpdate const& event) noexcept
                                                    { seekEvents.push_back(event); });

      viewModel.seekBy(std::chrono::seconds{5});

      CHECK(seekEvents.empty());
    }
  }

  TEST_CASE("PlaybackPositionViewModel - initial view state", "[uimodel][unit][playback]")
  {
    auto fixture = ApplicationPlaybackFixture{};

    auto log = ao::test::RenderLog<PlaybackPositionViewState>{};
    auto const viewModel = PlaybackPositionViewModel{fixture.playback, [&log](auto const& view) { log.render(view); }};

    REQUIRE(!log.empty());
    CHECK(log.last().occurrenceId == rt::PlaybackOccurrenceId{});
    CHECK(log.last().elapsed == std::chrono::milliseconds{0});
    CHECK(log.last().duration == std::chrono::milliseconds{0});

    log.clear();
    fixture.commands().setShuffleMode(ShuffleMode::On);
    CHECK(fixture.playback.snapshot().succession.shuffle == ShuffleMode::On);
    CHECK(log.empty());
  }

  TEST_CASE("PlaybackPositionViewModel - relative seek samples the live clock after headless construction",
            "[uimodel][regression][playback][seek]")
  {
    auto fixture = PlaybackRelativeSeekFixture{};
    auto const cachedElapsed = fixture.playback.snapshot().transport.elapsed;
    auto const liveElapsed = fixture.renderBlockElapsed();
    REQUIRE(liveElapsed > cachedElapsed);

    auto seekEvents = std::vector<PlaybackTransport::SeekUpdate>{};
    auto const seekSubscription = fixture.application.transport.playbackTransport.onSeekUpdate(
      [&seekEvents](PlaybackTransport::SeekUpdate const& event) noexcept { seekEvents.push_back(event); });
    auto viewModel =
      PlaybackPositionViewModel{fixture.playback, std::function<void(PlaybackPositionViewState const&)>{}};
    auto const delta = std::chrono::milliseconds{100};

    viewModel.seekBy(delta);

    REQUIRE(seekEvents.size() == 1);
    CHECK(seekEvents.front().mode == PlaybackTransport::SeekMode::Final);
    CHECK(seekEvents.front().elapsed == liveElapsed + delta);
    auto const actualElapsed = fixture.application.transport.playbackTransport.elapsed();
    CHECK(actualElapsed >= liveElapsed + delta - std::chrono::milliseconds{1});
    CHECK(actualElapsed <= liveElapsed + delta);
  }

  TEST_CASE("PlaybackPositionViewModel - orthogonal publication does not freeze the relative seek clock",
            "[uimodel][regression][playback][seek]")
  {
    auto fixture = PlaybackRelativeSeekFixture{};
    auto viewModel =
      PlaybackPositionViewModel{fixture.playback, std::function<void(PlaybackPositionViewState const&)>{}};
    std::ignore = fixture.renderBlockElapsed();
    fixture.playback.commands().setShuffleMode(ShuffleMode::On);
    auto const publishedElapsed = fixture.playback.snapshot().transport.elapsed;
    auto const liveElapsed = fixture.renderBlockElapsed();
    REQUIRE(liveElapsed > publishedElapsed);

    auto seekEvents = std::vector<PlaybackTransport::SeekUpdate>{};
    auto const seekSubscription = fixture.application.transport.playbackTransport.onSeekUpdate(
      [&seekEvents](PlaybackTransport::SeekUpdate const& event) noexcept { seekEvents.push_back(event); });
    auto const delta = std::chrono::milliseconds{100};

    viewModel.seekBy(delta);

    REQUIRE(seekEvents.size() == 1);
    CHECK(seekEvents.front().mode == PlaybackTransport::SeekMode::Final);
    CHECK(seekEvents.front().elapsed == liveElapsed + delta);
  }

  TEST_CASE("PlaybackPositionViewModel - reentrant relative seeks accumulate from each execution-live target",
            "[uimodel][regression][playback][seek]")
  {
    auto fixture = PlaybackRelativeSeekFixture{};
    fixture.playback.commands().pause();
    auto const occurrenceId = fixture.playback.snapshot().transport.occurrenceId;
    fixture.playback.commands().seek(occurrenceId, std::chrono::milliseconds{400});
    auto const before = fixture.playback.snapshot().transport;
    REQUIRE(before.elapsed == std::chrono::milliseconds{400});

    auto seekEvents = std::vector<PlaybackTransport::SeekUpdate>{};
    auto const seekSubscription = fixture.application.transport.playbackTransport.onSeekUpdate(
      [&seekEvents](PlaybackTransport::SeekUpdate const& event) noexcept { seekEvents.push_back(event); });
    auto viewModel =
      PlaybackPositionViewModel{fixture.playback, std::function<void(PlaybackPositionViewState const&)>{}};
    bool queuedRelativeSeeks = false;
    auto const snapshotSubscription = fixture.playback.events().onSnapshot(
      [&](PlaybackSnapshot const&) noexcept
      {
        if (!queuedRelativeSeeks)
        {
          queuedRelativeSeeks = true;
          viewModel.seekBy(std::chrono::milliseconds{100});
          viewModel.seekBy(std::chrono::milliseconds{100});
        }
      });

    fixture.playback.commands().setVolume(0.5F);
    REQUIRE(queuedRelativeSeeks);
    CHECK(seekEvents.empty());
    fixture.application.transport.executor.drain();

    REQUIRE(seekEvents.size() == 2);
    CHECK(seekEvents[0].mode == PlaybackTransport::SeekMode::Final);
    CHECK(seekEvents[0].elapsed == std::chrono::milliseconds{500});
    CHECK(seekEvents[1].mode == PlaybackTransport::SeekMode::Final);
    CHECK(seekEvents[1].elapsed == std::chrono::milliseconds{600});
    auto const after = fixture.playback.snapshot().transport;
    CHECK(after.elapsed == std::chrono::milliseconds{600});
    CHECK(after.finalSeekRevision.value == before.finalSeekRevision.value + 2);
  }

  TEST_CASE("PlaybackPositionViewModel - queued navigation retires a stale relative seek occurrence",
            "[uimodel][regression][playback][seek]")
  {
    auto fixture = PlaybackRelativeSeekFixture{};
    auto viewModel =
      PlaybackPositionViewModel{fixture.playback, std::function<void(PlaybackPositionViewState const&)>{}};
    auto const before = fixture.playback.snapshot().transport;
    auto seekEvents = std::vector<PlaybackTransport::SeekUpdate>{};
    auto const seekSubscription = fixture.application.transport.playbackTransport.onSeekUpdate(
      [&seekEvents](PlaybackTransport::SeekUpdate const& event) noexcept { seekEvents.push_back(event); });
    bool queuedCommands = false;
    auto const snapshotSubscription = fixture.playback.events().onSnapshot(
      [&](PlaybackSnapshot const&) noexcept
      {
        if (!queuedCommands)
        {
          queuedCommands = true;
          fixture.playback.commands().next();
          viewModel.seekBy(std::chrono::milliseconds{100});
        }
      });

    fixture.playback.commands().setVolume(0.5F);
    REQUIRE(queuedCommands);
    fixture.application.transport.executor.drain();
    REQUIRE(fixture.application.transport.executor.tryDrainUntil(
      [&] { return fixture.playback.snapshot().transport.nowPlaying.trackId == fixture.application.secondTrackId; }));

    auto const after = fixture.playback.snapshot().transport;
    CHECK(after.occurrenceId != before.occurrenceId);
    CHECK(after.finalSeekRevision == before.finalSeekRevision);
    CHECK(seekEvents.empty());
  }

  TEST_CASE("PlaybackPositionViewModel - transport seeks render preview and final modes", "[uimodel][unit][playback]")
  {
    auto fixture = ApplicationPlaybackFixtureT<QueuedExecutor>{};
    auto& playback = fixture.playback;
    auto& playbackTransport = fixture.playbackTransport;
    fixture.addReadyProvider();
    REQUIRE(fixture.executor.tryDrainUntil([&] { return playbackTransport.state().ready; }));

    auto log = ao::test::RenderLog<PlaybackPositionViewState>{};
    auto const viewModel = PlaybackPositionViewModel{playback, [&log](auto const& view) { log.render(view); }};

    auto const trackId = fixture.libraryFixture.addTrack({.title = "Seek Test", .artist = "Artist", .album = "Album"});
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto desc = PlaybackTransport::PlaybackRequest{
      .item = NowPlayingInfo{.trackId = trackId, .title = "Seek Test", .artist = "Artist"},
      .input = audio::PlaybackInput{.filePath = fixturePath, .duration = std::chrono::seconds{30}},
    };
    log.clear();
    REQUIRE(playbackTransport.play(desc, kInvalidListId));
    REQUIRE(
      fixture.executor.tryDrainUntil([&] { return playbackTransport.state().transport == audio::Transport::Playing; }));
    fixture.executor.drain();
    auto const expectedDuration = playbackTransport.state().duration;
    REQUIRE(expectedDuration > std::chrono::milliseconds{0});
    // Playback coalesces the preparing and started transitions into one
    // coherent snapshot, so the view model settles on the playing state rather
    // than observing each intermediate transport signal.
    REQUIRE(!log.empty());
    CHECK(log.last().isPlaying);

    log.clear();
    playbackTransport.seek(std::chrono::milliseconds{500}, PlaybackTransport::SeekMode::Final);
    fixture.executor.drain();
    REQUIRE(!log.empty());
    CHECK(log.last().occurrenceId == playback.snapshot().transport.occurrenceId);
    CHECK(log.last().duration == expectedDuration);
    CHECK(log.last().elapsed == std::chrono::milliseconds{500});
    CHECK(log.last().isPlaying == true);
    CHECK(log.last().isPreviewing == false);
    CHECK(log.last().immediateUpdate == true);
    CHECK(playbackTransport.state().elapsed == std::chrono::milliseconds{500});

    log.clear();
    playbackTransport.seek(std::chrono::milliseconds{250}, PlaybackTransport::SeekMode::Preview);
    fixture.executor.drain();
    REQUIRE(!log.empty());
    CHECK(log.last().duration == expectedDuration);
    CHECK(log.last().elapsed == std::chrono::milliseconds{250});
    CHECK(log.last().isPlaying == true);
    CHECK(log.last().isPreviewing == true);
    CHECK(log.last().immediateUpdate == false);
    CHECK(playbackTransport.state().elapsed == std::chrono::milliseconds{500});
  }
} // namespace ao::uimodel::test
