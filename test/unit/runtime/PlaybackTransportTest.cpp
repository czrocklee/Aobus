// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/FilesystemTestSupport.h"
#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/PlaybackTransportTestSupport.h"
#include <ao/audio/RenderTarget.h>
#include <ao/audio/Transport.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackFailure.h>
#include <ao/rt/PlaybackState.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("PlaybackTransport playback - playTrack fails when track does not exist", "[runtime][unit][playback][play]")
  {
    auto fixture = PlaybackTransportFixture<InlineExecutor>{};

    auto const result = fixture.playbackTransport.playTrack(TrackId{99999}, ListId{7});

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::NotFound);
  }

  TEST_CASE("PlaybackTransport playback - playTrack resolves track metadata", "[runtime][unit][playback][play]")
  {
    auto fixture = PlaybackTransportFixture<InlineExecutor>{};

    // Prime the device list. The first notification auto-selects the default
    // output; the duplicate exercises the "already selected" early return, and the
    // empty list exercises the no-devices guard.
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.onDevicesChangedCb(fixture.status.devices);
    auto emptyStatus = fixture.status;
    emptyStatus.devices.clear();
    fixture.onDevicesChangedCb(emptyStatus.devices);

    auto spec = library::test::TrackSpec{};
    spec.title = "Playable Track";
    spec.artist = "Queue Artist";
    spec.album = "Queue Album";
    spec.uri = fixture.installAudioFixture();
    spec.duration = std::chrono::minutes{3};
    auto const trackId = fixture.libraryFixture.addTrack(spec);

    CHECK(fixture.playbackTransport.playTrack(trackId, ListId{7}));
    CHECK(fixture.playbackTransport.state().nowPlaying == NowPlayingInfo{.trackId = trackId,
                                                                         .sourceListId = ListId{7},
                                                                         .title = "Playable Track",
                                                                         .artist = "Queue Artist",
                                                                         .album = "Queue Album"});
    CHECK(fixture.playbackTransport.state().duration > std::chrono::milliseconds{0});

    fixture.playbackTransport.stop();
    CHECK(fixture.playbackTransport.state().nowPlaying == NowPlayingInfo{});
  }

  TEST_CASE("PlaybackTransport playback - playTrack rejects a URI escaping through a symlink",
            "[runtime][regression][playback][uri]")
  {
    auto fixture = PlaybackTransportFixture<InlineExecutor>{};
    auto const outside = ao::test::TempDir{};
    auto const outsideFile = outside.path() / "song.flac";
    std::filesystem::copy_file(audio::test::requireAudioFixture("basic_metadata.flac"), outsideFile);
    auto const symlink = ao::test::SymlinkFixture{
      outside.path(), fixture.libraryFixture.root() / "alias", ao::test::SymlinkType::Directory};
    auto const trackId = fixture.libraryFixture.addTrack({.title = "Outside", .uri = "alias/song.flac"});

    auto const result = fixture.playbackTransport.playTrack(trackId, ListId{7});

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::InvalidInput);
    CHECK(result.error().message.contains("outside the library root"));
    CHECK(fixture.playbackTransport.state().nowPlaying == NowPlayingInfo{});
  }

  TEST_CASE("PlaybackTransport playback - prepareNext does not replace current state", "[runtime][unit][playback]")
  {
    auto fixture = PlaybackTransportFixture<InlineExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    auto const fixtureUri = fixture.installAudioFixture();

    auto const currentTrack = fixture.libraryFixture.addTrack({.title = "Current Track", .uri = fixtureUri});
    auto const nextTrack = fixture.libraryFixture.addTrack({.title = "Prepared Track", .uri = fixtureUri});

    REQUIRE(fixture.playbackTransport.playTrack(currentTrack, ListId{7}));
    auto const preparedTokenResult = fixture.playbackTransport.prepareNext(nextTrack, ListId{7});
    REQUIRE(preparedTokenResult);
    auto const preparedToken = *preparedTokenResult;

    CHECK(fixture.playbackTransport.state().nowPlaying.trackId == currentTrack);
    CHECK(fixture.playbackTransport.state().nowPlaying.title == "Current Track");
    CHECK_FALSE(fixture.playbackTransport.prepareNext(TrackId{99999}, ListId{7}));
    CHECK(fixture.playbackTransport.state().nowPlaying.trackId == currentTrack);
    CHECK(fixture.playbackTransport.clearPreparedNext() == preparedToken);
  }

  TEST_CASE("PlaybackTransport playback - drain emits idle when playback is actually idle",
            "[runtime][unit][playback][drain]")
  {
    auto fixture = PlaybackTransportFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    auto const fixtureUri = fixture.installAudioFixture();
    auto const trackId = fixture.libraryFixture.addTrack({.title = "Terminal Track", .uri = fixtureUri});

    std::size_t idleCount = 0;
    auto idleSub = fixture.playbackTransport.onIdle([&] { ++idleCount; });

    REQUIRE(fixture.playbackTransport.playTrack(trackId, ListId{7}));
    REQUIRE(fixture.renderTarget != nullptr);

    auto buffer = std::array<std::byte, 4096>{};
    bool isDrained = false;

    for (std::int32_t i = 0; i < 100000 && !isDrained; ++i)
    {
      isDrained = fixture.renderTarget->renderPcm(buffer).drained;
    }

    REQUIRE(isDrained);
    fixture.renderTarget->handleDrainComplete();

    for (std::int32_t i = 0; i < 100000 && idleCount == 0; ++i)
    {
      fixture.executor.drain();
    }

    REQUIRE(idleCount > 0);
    CHECK(idleCount == 1);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Idle);
  }

  TEST_CASE("PlaybackTransport playback - natural advance commits prepared track without idle",
            "[runtime][unit][playback][gapless]")
  {
    auto fixture = PlaybackTransportFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    auto const fixtureUri = fixture.installAudioFixture();
    auto const currentTrack = fixture.libraryFixture.addTrack({.title = "Current Track", .uri = fixtureUri});
    auto const nextTrack = fixture.libraryFixture.addTrack({.title = "Prepared Track", .uri = fixtureUri});

    auto nowPlaying = std::vector<PlaybackTransport::NowPlayingChanged>{};
    std::size_t idleCount = 0;
    auto nowPlayingSub = fixture.playbackTransport.onNowPlayingChanged(
      [&](PlaybackTransport::NowPlayingChanged const& ev) { nowPlaying.push_back(ev); });
    auto idleSub = fixture.playbackTransport.onIdle([&] { ++idleCount; });

    REQUIRE(fixture.playbackTransport.playTrack(currentTrack, ListId{7}));
    auto const preparedTokenResult = fixture.playbackTransport.prepareNext(nextTrack, ListId{7});
    REQUIRE(preparedTokenResult);
    auto const preparedToken = *preparedTokenResult;
    REQUIRE(fixture.renderTarget != nullptr);

    // Only observe the natural advance below, not playTrack's own emission.
    nowPlaying.clear();

    // Drive the render side to the end of the current track. Both tracks are
    // the same lossless FLAC, so the engine splices into the prepared successor
    // and reports the advance with the caller-supplied item id; the player
    // marshals it onto the executor, which drain() runs on this thread.
    auto buffer = std::array<std::byte, 4096>{};

    for (std::int32_t i = 0; i < 100000 && nowPlaying.empty(); ++i)
    {
      fixture.renderTarget->renderPcm(buffer);
      fixture.executor.drain();
    }

    REQUIRE_FALSE(nowPlaying.empty());

    // Item-id match: the prepared request is committed as now-playing, exactly
    // once, without an idle in between (idle would send playback down the
    // explicit-restart fallback).
    REQUIRE(nowPlaying.size() == 1);
    CHECK(nowPlaying[0].trackId == nextTrack);
    CHECK(nowPlaying[0].sourceListId == ListId{7});
    CHECK(nowPlaying[0].optPreparedNextToken == preparedToken);
    CHECK(fixture.playbackTransport.state().nowPlaying.trackId == nextTrack);
    CHECK(fixture.playbackTransport.state().nowPlaying.title == "Prepared Track");
    CHECK(idleCount == 0);
  }

  TEST_CASE("PlaybackTransport playback - final seek before advanced callback keeps prepared metadata",
            "[runtime][unit][playback][gapless]")
  {
    auto fixture = PlaybackTransportFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    auto const fixtureUri = fixture.installAudioFixture();
    auto const currentTrack = fixture.libraryFixture.addTrack({.title = "Current Track", .uri = fixtureUri});
    auto const nextTrack = fixture.libraryFixture.addTrack({.title = "Prepared Track", .uri = fixtureUri});

    auto nowPlaying = std::vector<PlaybackTransport::NowPlayingChanged>{};
    auto nowPlayingSub = fixture.playbackTransport.onNowPlayingChanged(
      [&](PlaybackTransport::NowPlayingChanged const& ev) { nowPlaying.push_back(ev); });

    REQUIRE(fixture.playbackTransport.playTrack(currentTrack, ListId{7}));
    auto const preparedTokenResult = fixture.playbackTransport.prepareNext(nextTrack, ListId{7});
    REQUIRE(preparedTokenResult);
    auto const preparedToken = *preparedTokenResult;
    REQUIRE(fixture.renderTarget != nullptr);
    fixture.executor.drain();
    nowPlaying.clear();

    auto buffer = std::array<std::byte, 4096>{};
    REQUIRE(driveRenderUntilTaskQueued(*fixture.renderTarget, fixture.executor, buffer));

    fixture.playbackTransport.seek(std::chrono::milliseconds{0}, PlaybackTransport::SeekMode::Final);
    fixture.executor.drain();

    REQUIRE(nowPlaying.size() == 1);
    CHECK(nowPlaying[0].trackId == nextTrack);
    CHECK(nowPlaying[0].sourceListId == ListId{7});
    CHECK(nowPlaying[0].optPreparedNextToken == preparedToken);
    CHECK(fixture.playbackTransport.state().nowPlaying.trackId == nextTrack);
    CHECK(fixture.playbackTransport.state().nowPlaying.title == "Prepared Track");
  }

  TEST_CASE("PlaybackTransport playback - rejected preflight reports synchronously without an engine failure event",
            "[runtime][unit][playback][error]")
  {
    auto fixture = PlaybackTransportFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    auto const trackId = fixture.libraryFixture.addTrack({.title = "Broken Track", .uri = "broken.txt"});

    auto failures = std::vector<PlaybackFailure>{};
    auto sub =
      fixture.playbackTransport.onPlaybackFailure([&](PlaybackFailure const& failure) { failures.push_back(failure); });

    auto const result = fixture.playbackTransport.playTrack(trackId, ListId{7});

    REQUIRE_FALSE(result);
    CHECK(result.error().message.contains("Unsupported audio file extension"));
    fixture.executor.drain();
    CHECK(failures.empty());
    auto const feed = fixture.notificationService.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().lifetime == NotificationLifetime::pinned());
    REQUIRE(std::holds_alternative<NotificationReport>(feed.entries.front().message));
    CHECK(
      std::get<NotificationReport>(feed.entries.front().message).detail.contains("Unsupported audio file extension"));
  }

  TEST_CASE("PlaybackTransport playback - rejected preflight bypasses asynchronous failure observers",
            "[runtime][unit][playback][error]")
  {
    auto fixture = PlaybackTransportFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    auto const trackId = fixture.libraryFixture.addTrack({.title = "Broken Track", .uri = "broken.txt"});
    auto firstFailures = std::vector<PlaybackFailure>{};
    auto secondFailures = std::vector<PlaybackFailure>{};
    auto firstSub = fixture.playbackTransport.onPlaybackFailure([&](PlaybackFailure const& failure)
                                                                { firstFailures.push_back(failure); });
    auto secondSub = fixture.playbackTransport.onPlaybackFailure([&](PlaybackFailure const& failure)
                                                                 { secondFailures.push_back(failure); });

    REQUIRE_FALSE(fixture.playbackTransport.playTrack(trackId, ListId{7}));
    fixture.executor.drain();

    CHECK(firstFailures.empty());
    CHECK(secondFailures.empty());
    REQUIRE(fixture.notificationService.feed().entries.size() == 1);
  }

  TEST_CASE("PlaybackTransport playback - rejected preflight report suppresses identical updates",
            "[runtime][unit][playback][error]")
  {
    auto fixture = PlaybackTransportFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    auto const trackId = fixture.libraryFixture.addTrack({.title = "Broken Track", .uri = "broken.txt"});

    REQUIRE_FALSE(fixture.playbackTransport.playTrack(trackId, ListId{7}));

    auto feed = fixture.notificationService.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().severity == NotificationSeverity::Error);
    CHECK(feed.entries.front().lifetime == NotificationLifetime::pinned());
    REQUIRE(std::holds_alternative<NotificationReport>(feed.entries.front().message));
    CHECK(
      std::get<NotificationReport>(feed.entries.front().message).detail.contains("Unsupported audio file extension"));

    auto const originalId = feed.entries.front().id;
    std::int32_t mutationCount = 0;
    auto updateSub = fixture.notificationService.onFeedUpdated([&](NotificationFeedUpdate const&) { ++mutationCount; });

    REQUIRE_FALSE(fixture.playbackTransport.playTrack(trackId, ListId{7}));
    CHECK(mutationCount == 0);

    feed = fixture.notificationService.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().id == originalId);
  }

  TEST_CASE("PlaybackTransport playback - rejected preflight preserves accepted playback",
            "[runtime][unit][playback][error]")
  {
    auto fixture = PlaybackTransportFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    auto const fixtureUri = fixture.installAudioFixture();
    auto const brokenTrack = fixture.libraryFixture.addTrack({.title = "Stale Broken Track", .uri = "broken.txt"});
    auto const replacementTrack = fixture.libraryFixture.addTrack({.title = "Replacement Track", .uri = fixtureUri});

    auto failures = std::vector<PlaybackFailure>{};
    auto sub =
      fixture.playbackTransport.onPlaybackFailure([&](PlaybackFailure const& failure) { failures.push_back(failure); });

    REQUIRE(fixture.playbackTransport.playTrack(replacementTrack, ListId{7}));
    REQUIRE_FALSE(fixture.playbackTransport.playTrack(brokenTrack, ListId{7}));

    fixture.executor.drain();

    CHECK(failures.empty());
    CHECK(fixture.notificationService.feed().entries.size() == 1);
    CHECK(fixture.playbackTransport.state().nowPlaying.trackId == replacementTrack);
  }

  TEST_CASE("PlaybackTransport playback - route activation failures dedupe by kind", "[runtime][unit][playback][error]")
  {
    auto fixture = PlaybackTransportFixture<InlineExecutor>{};

    auto const fixtureUri = fixture.installAudioFixture();
    auto const track1 = fixture.libraryFixture.addTrack({.title = "Track 1", .uri = fixtureUri});
    auto const track2 = fixture.libraryFixture.addTrack({.title = "Track 2", .uri = fixtureUri});

    CHECK_FALSE(fixture.playbackTransport.playTrack(track1, ListId{7}));
    CHECK_FALSE(fixture.playbackTransport.playTrack(track2, ListId{7}));

    auto const feed = fixture.notificationService.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().lifetime == NotificationLifetime::pinned());
    REQUIRE(std::holds_alternative<NotificationReport>(feed.entries.front().message));
    CHECK(std::get<NotificationReport>(feed.entries.front().message).templateId ==
          NotificationReportTemplate::PlaybackRouteActivationFailed);
  }

  TEST_CASE("PlaybackTransport playback - backend error publishes pinned device failure",
            "[runtime][unit][playback][error]")
  {
    auto fixture = PlaybackTransportFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    auto const fixtureUri = fixture.installAudioFixture();
    auto const trackId = fixture.libraryFixture.addTrack({.title = "Playing Track", .uri = fixtureUri});

    auto failures = std::vector<PlaybackFailure>{};
    auto sub =
      fixture.playbackTransport.onPlaybackFailure([&](PlaybackFailure const& failure) { failures.push_back(failure); });

    REQUIRE(fixture.playbackTransport.playTrack(trackId, ListId{7}));
    REQUIRE(fixture.renderTarget != nullptr);

    fixture.renderTarget->handleBackendError("device lost");
    REQUIRE(fixture.executor.drainUntil([&] { return !failures.empty(); }));

    REQUIRE(failures.size() == 1);
    CHECK(failures.front().kind == PlaybackFailureKind::DeviceLost);
    CHECK(failures.front().trackId == trackId);
    CHECK(failures.front().sourceListId == ListId{7});
    CHECK(failures.front().title == "Playing Track");
    CHECK_FALSE(failures.front().recoverable);
    CHECK(failures.front().error.message == "device lost");

    auto const feed = fixture.notificationService.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().severity == NotificationSeverity::Error);
    CHECK(feed.entries.front().lifetime == NotificationLifetime::pinned());
    REQUIRE(std::holds_alternative<NotificationReport>(feed.entries.front().message));
    auto const& report = std::get<NotificationReport>(feed.entries.front().message);
    CHECK(report.templateId == NotificationReportTemplate::PlaybackDeviceLost);
    CHECK(report.detail == "device lost");
  }
} // namespace ao::rt::test
