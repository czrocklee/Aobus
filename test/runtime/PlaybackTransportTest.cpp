// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "runtime/playback/PlaybackTransport.h"

#include "lib/audio/DecoderFactory.h"
#include "test/FilesystemTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/audio/AudioFixtureSupport.h"
#include "test/audio/BackendTestSupport.h" // NOLINT(misc-include-cleaner) -- provides SpyBackend to FakeIt.
#include "test/audio/ScriptedDecoderSession.h"
#include "test/library/TrackTestSupport.h"
#include "test/runtime/ExecutorTestSupport.h"
#include "test/runtime/PlaybackTestSupport.h"
#include "test/runtime/PlaybackTransportTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/DecoderSession.h>
#include <ao/audio/Device.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/Transport.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackState.h>

#include <catch2/catch_test_macros.hpp>
#include <fakeit.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("PlaybackTransport playback - playTrack fails when track does not exist")
  {
    auto fixture = PlaybackTransportFixture<InlineExecutor>{};

    auto const res = fixture.playbackTransport.playTrack(TrackId{99999}, ListId{7});

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::NotFound);
  }

  TEST_CASE("PlaybackTransport playback - playTrack resolves track metadata")
  {
    auto fixture = PlaybackTransportFixture<InlineExecutor>{};

    fixture.onDevicesChangedCb(fixture.status.devices);

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

  TEST_CASE("PlaybackTransport playback - playTrack rejects a URI escaping through a symlink")
  {
    auto fixture = PlaybackTransportFixture<InlineExecutor>{};
    auto const outside = ao::test::TempDir{};
    auto const outsideFile = outside.path() / "song.flac";
    std::filesystem::copy_file(audio::test::requireAudioFixture("basic_metadata.flac"), outsideFile);
    auto const symlink = ao::test::SymlinkFixture{
      outside.path(), fixture.libraryFixture.root() / "alias", ao::test::SymlinkType::Directory};
    auto const trackId = fixture.libraryFixture.addTrack({.title = "Outside", .uri = "alias/song.flac"});

    auto const res = fixture.playbackTransport.playTrack(trackId, ListId{7});

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::InvalidInput);
    CHECK(res.error().message.contains("outside the library root"));
    CHECK(fixture.playbackTransport.state().nowPlaying == NowPlayingInfo{});
  }

  TEST_CASE("PlaybackTransport playback - prepareNext does not replace current state")
  {
    auto fixture = PlaybackTransportFixture<InlineExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    auto const fixtureUri = fixture.installAudioFixture();

    auto const currentTrack = fixture.libraryFixture.addTrack({.title = "Current Track", .uri = fixtureUri});
    auto const nextTrack = fixture.libraryFixture.addTrack({.title = "Prepared Track", .uri = fixtureUri});

    REQUIRE(fixture.playbackTransport.playTrack(currentTrack, ListId{7}));
    auto const nextRequestRes = playbackRequestForTrack(fixture.libraryFixture.library(), nextTrack);
    REQUIRE(nextRequestRes);
    auto const preparedTokenRes = fixture.playbackTransport.prepareNext(*nextRequestRes, ListId{7});
    REQUIRE(preparedTokenRes);
    auto const preparedToken = *preparedTokenRes;

    CHECK(fixture.playbackTransport.state().nowPlaying.trackId == currentTrack);
    CHECK(fixture.playbackTransport.state().nowPlaying.title == "Current Track");
    CHECK_FALSE(playbackRequestForTrack(fixture.libraryFixture.library(), TrackId{99999}));
    CHECK(fixture.playbackTransport.state().nowPlaying.trackId == currentTrack);
    CHECK(fixture.playbackTransport.clearPreparedNext() == preparedToken);
  }

  TEST_CASE("PlaybackTransport playback - natural terminal completion clears guarded seek ownership", "[concurrency]")
  {
    auto const format =
      audio::PcmFormat{.sampleRate = 1000, .channels = 1, .encoding = audio::SampleEncoding::Signed16Le};
    auto const factory = [format](auto const&, std::optional<audio::SampleEncoding> optOutputEncoding)
    {
      auto const sourceFormat = audio::signalFormat(format);
      auto decoderPtr = std::make_unique<audio::test::ScriptedDecoderSession>(audio::DecodedStreamInfo{
        .sourceFormat = sourceFormat,
        .outputFormat = audio::pcmFormat(sourceFormat, optOutputEncoding.value_or(format.encoding)),
        .duration = std::chrono::milliseconds{10},
        .isLossy = false});
      // Nonempty PCM and EOF both fit in preroll, so render progress does not
      // depend on scheduling the background decoder before an iteration limit.
      decoderPtr->setReadScript({{.data = std::vector(20, std::byte{0})}, {.endOfStream = true}});
      return decoderPtr;
    };
    auto fixture = PlaybackTransportFixture<QueuedExecutor>{factory};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    auto const fixtureUri = fixture.installAudioFixture();
    auto const trackId = fixture.libraryFixture.addTrack(
      {.title = "Terminal Track", .uri = fixtureUri, .duration = std::chrono::milliseconds{10}});

    std::size_t idleCount = 0;
    auto idleSub = fixture.playbackTransport.onIdle([&] noexcept { ++idleCount; });

    REQUIRE(fixture.playbackTransport.playTrack(trackId, ListId{7}));
    REQUIRE(fixture.renderTarget != nullptr);
    auto const occurrenceId = fixture.playbackTransport.state().occurrenceId;
    REQUIRE(occurrenceId.value != 0);
    REQUIRE(fixture.playbackTransport.state().duration > std::chrono::milliseconds{1});
    fixture.executor.drain();

    auto buffer = std::array<std::byte, 4096>{};
    CHECK(fixture.renderTarget->renderPcm(buffer).bytesWritten > 0);
    CHECK(idleCount == 0);
    REQUIRE(fixture.renderTarget->renderPcm(buffer).drained);
    CHECK(fixture.playbackTransport.state().occurrenceId == occurrenceId);

    fixture.renderTarget->handleDrainComplete();
    CHECK(fixture.playbackTransport.state().occurrenceId == occurrenceId);

    SECTION("deliver the natural end on the executor")
    {
      REQUIRE(fixture.executor.tryDrainUntil([&idleCount] { return idleCount > 0; }));
    }

    SECTION("request a missing output after Engine retired the item but before delivering its end")
    {
      // Engine posts state, route, then end after retiring the item. Keep
      // those callbacks queued until the missing-route request is settled.
      REQUIRE(fixture.executor.tryWaitUntilQueuedCount(3, std::chrono::seconds{5}));
      REQUIRE_FALSE(fixture.playbackTransport.canAdvanceFrom(occurrenceId));
      REQUIRE(idleCount == 0);
      auto const missing = audio::OutputDeviceSelection{.backendId = fixture.status.descriptor.id,
                                                        .deviceId = audio::DeviceId{"missing"},
                                                        .profileId = audio::kProfileShared};
      fixture.playbackTransport.setOutputDevice(missing);
      fixture.executor.drain();
      CHECK(fixture.playbackTransport.state().output.optPendingOutputSelection == missing);
      CHECK_FALSE(fixture.playbackTransport.state().ready);
    }

    CHECK(idleCount == 1);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Idle);
    CHECK(fixture.playbackTransport.state().occurrenceId == PlaybackOccurrenceId{});
    CHECK_FALSE(fixture.playbackTransport.trySeek(
      occurrenceId, std::chrono::milliseconds{1}, PlaybackTransport::SeekMode::Preview));
    CHECK_FALSE(fixture.playbackTransport.trySeek(occurrenceId, std::chrono::milliseconds{1}));
  }

  TEST_CASE("PlaybackTransport playback - natural advance commits prepared track without idle", "[concurrency]")
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
      [&](PlaybackTransport::NowPlayingChanged const& ev) noexcept { nowPlaying.push_back(ev); });
    auto idleSub = fixture.playbackTransport.onIdle([&] noexcept { ++idleCount; });

    REQUIRE(fixture.playbackTransport.playTrack(currentTrack, ListId{7}));
    auto const currentOccurrenceId = fixture.playbackTransport.state().occurrenceId;
    REQUIRE(currentOccurrenceId.value != 0);
    auto const nextRequestRes = playbackRequestForTrack(fixture.libraryFixture.library(), nextTrack);
    REQUIRE(nextRequestRes);
    auto const preparedTokenRes = fixture.playbackTransport.prepareNext(*nextRequestRes, ListId{7});
    REQUIRE(preparedTokenRes);
    auto const preparedToken = *preparedTokenRes;
    REQUIRE(fixture.renderTarget != nullptr);

    // Only observe the natural advance below, not playTrack's own emission.
    nowPlaying.clear();

    // Drive the render side to the end of the current track. Both tracks are
    // the same lossless FLAC, so the engine splices into the prepared successor
    // and reports the advance with the caller-supplied item id; the player
    // marshals it onto the executor, which drain() runs on this thread.
    auto buffer = std::array<std::byte, 4096>{};

    REQUIRE(tryDriveRenderUntil(
      *fixture.renderTarget, fixture.executor, buffer, [&nowPlaying] { return !nowPlaying.empty(); }));

    // Item-id match: the prepared request is committed as now-playing, exactly
    // once, without an idle in between (idle would send playback down the
    // explicit-restart fallback).
    REQUIRE(nowPlaying.size() == 1);
    CHECK(nowPlaying[0].trackId == nextTrack);
    CHECK(nowPlaying[0].sourceListId == ListId{7});
    CHECK(nowPlaying[0].optPreparedNextToken == preparedToken);
    CHECK(fixture.playbackTransport.state().nowPlaying.trackId == nextTrack);
    CHECK(fixture.playbackTransport.state().nowPlaying.title == "Prepared Track");
    auto const successorOccurrenceId = fixture.playbackTransport.state().occurrenceId;
    REQUIRE(successorOccurrenceId.value != 0);
    CHECK(successorOccurrenceId != currentOccurrenceId);
    CHECK(fixture.playbackTransport.trySeek(
      successorOccurrenceId, std::chrono::milliseconds{1}, PlaybackTransport::SeekMode::Preview));
    CHECK(idleCount == 0);
  }

  TEST_CASE("PlaybackTransport playback - final seek before advanced callback keeps prepared metadata", "[concurrency]")
  {
    auto fixture = PlaybackTransportFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    auto const fixtureUri = fixture.installAudioFixture();
    auto const currentTrack = fixture.libraryFixture.addTrack({.title = "Current Track", .uri = fixtureUri});
    auto const nextTrack = fixture.libraryFixture.addTrack({.title = "Prepared Track", .uri = fixtureUri});

    auto nowPlaying = std::vector<PlaybackTransport::NowPlayingChanged>{};
    auto nowPlayingSub = fixture.playbackTransport.onNowPlayingChanged(
      [&](PlaybackTransport::NowPlayingChanged const& ev) noexcept { nowPlaying.push_back(ev); });

    REQUIRE(fixture.playbackTransport.playTrack(currentTrack, ListId{7}));
    auto const nextRequestRes = playbackRequestForTrack(fixture.libraryFixture.library(), nextTrack);
    REQUIRE(nextRequestRes);
    auto const preparedTokenRes = fixture.playbackTransport.prepareNext(*nextRequestRes, ListId{7});
    REQUIRE(preparedTokenRes);
    auto const preparedToken = *preparedTokenRes;
    REQUIRE(fixture.renderTarget != nullptr);
    fixture.executor.drain();
    nowPlaying.clear();

    auto buffer = std::array<std::byte, 4096>{};
    REQUIRE(tryDriveRenderUntilTaskQueued(*fixture.renderTarget, fixture.executor, buffer));

    fixture.playbackTransport.seek(std::chrono::milliseconds{0}, PlaybackTransport::SeekMode::Final);
    fixture.executor.drain();

    REQUIRE(nowPlaying.size() == 1);
    CHECK(nowPlaying[0].trackId == nextTrack);
    CHECK(nowPlaying[0].sourceListId == ListId{7});
    CHECK(nowPlaying[0].optPreparedNextToken == preparedToken);
    CHECK(fixture.playbackTransport.state().nowPlaying.trackId == nextTrack);
    CHECK(fixture.playbackTransport.state().nowPlaying.title == "Prepared Track");
  }

  TEST_CASE("PlaybackTransport playback - missing output pauses the gapless winner before its callback",
            "[concurrency]")
  {
    auto fixture = PlaybackTransportFixture<QueuedExecutor>{};
    auto& transport = fixture.playbackTransport;
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    auto const fixtureUri = fixture.installAudioFixture();
    auto const currentTrack = fixture.libraryFixture.addTrack({.title = "Current Track", .uri = fixtureUri});
    auto const nextTrack = fixture.libraryFixture.addTrack({.title = "Prepared Track", .uri = fixtureUri});
    auto nowPlaying = std::vector<PlaybackTransport::NowPlayingChanged>{};
    auto nowPlayingSub = transport.onNowPlayingChanged([&](PlaybackTransport::NowPlayingChanged const& ev) noexcept
                                                       { nowPlaying.push_back(ev); });

    REQUIRE(transport.playTrack(currentTrack, ListId{7}));
    auto const currentOccurrence = transport.state().occurrenceId;
    auto const nextRequestRes = playbackRequestForTrack(fixture.libraryFixture.library(), nextTrack);
    REQUIRE(nextRequestRes);
    auto const preparedTokenRes = transport.prepareNext(*nextRequestRes, ListId{8});
    REQUIRE(preparedTokenRes);
    REQUIRE(fixture.renderTarget != nullptr);
    fixture.executor.drain();
    nowPlaying.clear();
    REQUIRE(transport.canAdvanceFrom(currentOccurrence));

    // Render has crossed into the successor, but the executor still presents the old track.
    auto buffer = std::array<std::byte, 4096>{};
    REQUIRE(tryDriveRenderUntilTaskQueued(*fixture.renderTarget, fixture.executor, buffer));
    REQUIRE(nowPlaying.empty());
    REQUIRE(transport.state().nowPlaying.trackId == currentTrack);
    REQUIRE_FALSE(transport.canAdvanceFrom(currentOccurrence));

    auto const dac = audio::OutputDeviceSelection{.backendId = fixture.status.descriptor.id,
                                                  .deviceId = audio::DeviceId{"studio-dac"},
                                                  .profileId = audio::kProfileShared};
    transport.setOutputDevice(dac);
    fixture.executor.drain();

    CHECK(transport.state().output.optPendingOutputSelection == dac);
    CHECK(transport.state().transport == audio::Transport::Paused);
    CHECK(transport.state().nowPlaying.trackId == nextTrack);
    CHECK(transport.state().nowPlaying.title == "Prepared Track");
    REQUIRE(nowPlaying.size() == 1);
    CHECK(nowPlaying.front().trackId == nextTrack);
    CHECK(nowPlaying.front().sourceListId == ListId{8});
    CHECK(nowPlaying.front().optPreparedNextToken == *preparedTokenRes);
    auto& backend = fixture.spyBackendPtr->mock();
    fakeit::Verify(Method(backend, close)).Once();

    fixture.onDevicesChangedCb({audio::Device{.id = dac.deviceId, .backendId = dac.backendId}});
    fixture.executor.drain();

    REQUIRE(transport.state().ready);
    CHECK(transport.state().output.selectedDevice == dac);
    CHECK(transport.state().transport == audio::Transport::Paused);
    CHECK(transport.state().nowPlaying.trackId == nextTrack);
    CHECK(nowPlaying.size() == 1);
    fakeit::Verify(Method(backend, open)).Once();
    fakeit::Verify(Method(backend, start)).Once();

    transport.resume();
    fixture.executor.drain();

    CHECK(transport.state().transport == audio::Transport::Playing);
    CHECK(transport.state().nowPlaying.trackId == nextTrack);
    CHECK(transport.state().nowPlaying.sourceListId == ListId{8});
    auto const winnerAnnouncements = std::ranges::count(
      nowPlaying, std::optional{*preparedTokenRes}, &PlaybackTransport::NowPlayingChanged::optPreparedNextToken);
    CHECK(winnerAnnouncements == 1);
    fakeit::Verify(Method(backend, open)).Exactly(2);
    fakeit::Verify(Method(backend, start)).Exactly(2);
  }

  TEST_CASE("PlaybackTransport playback - a missing output does not hold a track lost during route lookup",
            "[concurrency]")
  {
    auto renderMutex = std::mutex{};
    auto fixture = PlaybackTransportFixture<QueuedExecutor>{};
    auto& transport = fixture.playbackTransport;
    auto replacement = audio::OutputDeviceSelection{.backendId = fixture.status.descriptor.id,
                                                    .deviceId = audio::DeviceId{"alternate"},
                                                    .profileId = audio::kProfileShared};
    auto devices = fixture.status.devices;
    devices.push_back(audio::Device{.id = replacement.deviceId, .backendId = replacement.backendId});
    fixture.onDevicesChangedCb(devices);
    fixture.executor.drain();
    auto const track = fixture.libraryFixture.addTrack({.title = "Track A", .uri = fixture.installAudioFixture()});
    REQUIRE(transport.playTrack(track, ListId{7}));
    fixture.executor.drain();
    auto const occurrence = transport.state().occurrenceId;
    REQUIRE(transport.canAdvanceFrom(occurrence));

    std::size_t idleCount = 0;
    std::size_t reportCount = 0;
    auto idleSub = transport.onIdle([&] { ++idleCount; });
    auto reportSub = fixture.notificationService.onFeedUpdated([&](NotificationFeedUpdate const&) { ++reportCount; });
    auto& backend = fixture.spyBackendPtr->mock();
    fakeit::When(Method(backend, close))
      .AlwaysDo(
        [&]
        {
          // Engine destroys its render target after close returns. Quiesce the callback first.
          auto const lock = std::scoped_lock{renderMutex};
          fixture.renderTarget = nullptr;
        });
    auto const queueFailure = [&fixture, &renderMutex, occurrence]
    {
      {
        auto const lock = std::scoped_lock{renderMutex};
        REQUIRE(fixture.renderTarget != nullptr);
        fixture.renderTarget->handleBackendError("device lost during playback");
      }
      // The event worker retires A and queues failure, then state. Don't deliver either yet.
      REQUIRE(fixture.executor.tryWaitUntilQueuedCount(2, std::chrono::seconds{5}));
      REQUIRE_FALSE(fixture.playbackTransport.canAdvanceFrom(occurrence));
    };
    auto const changeDuringLookup = [&]
    {
      // Provider lookup follows the cursor sample and precedes either stopping or replacing the backend.
      fakeit::When(Method(fixture.mockProvider, status))
        .Do(
          [queueFailure, status = fixture.status]
          {
            queueFailure();
            return status;
          })
        .AlwaysReturn(fixture.status);
      transport.setOutputDevice(replacement);
    };

    SECTION("the requested output is missing until discovery")
    {
      replacement.deviceId = audio::DeviceId{"missing"};
      changeDuringLookup();
      CHECK(transport.state().transport != audio::Transport::Paused);
      fixture.executor.drain();
      CHECK(reportCount == 1);
      CHECK_FALSE(transport.state().ready);
      CHECK(transport.state().output.optPendingOutputSelection == replacement);
      CHECK_FALSE(transport.canAdvanceFrom(occurrence));
      fixture.onDevicesChangedCb({audio::Device{.id = replacement.deviceId, .backendId = replacement.backendId}});
    }

    SECTION("the replacement is already listed when the device fails")
    {
      changeDuringLookup();
    }

    SECTION("the failure was already delivered before choosing the listed replacement")
    {
      queueFailure();
      fixture.executor.drain();
      REQUIRE(reportCount == 1);
      transport.setOutputDevice(replacement);
    }

    fixture.executor.drain();
    REQUIRE(transport.state().ready);
    CHECK(transport.state().output.selectedDevice == replacement);
    CHECK_FALSE(transport.state().output.optPendingOutputSelection);
    CHECK(reportCount == 1);
    transport.resume();
    fixture.executor.drain();

    fakeit::Verify(Method(backend, open)).Once();
    fakeit::Verify(Method(backend, start)).Once();
    CHECK(idleCount == 0);
    CHECK(reportCount == 1);
    auto const feed = fixture.notificationService.feed();
    REQUIRE(feed.entries.size() == 1);
    REQUIRE(std::holds_alternative<NotificationReport>(feed.entries.front().message));
    auto const& report = std::get<NotificationReport>(feed.entries.front().message);
    CHECK(report.templateId == NotificationReportTemplate::PlaybackDeviceLost);
    CHECK(report.trackId == track);
    CHECK(report.detail == "device lost during playback");
  }

  TEST_CASE("PlaybackTransport playback - a failed output reinspection does not become a natural end")
  {
    bool rejectInspection = false;
    auto fixture = PlaybackTransportFixture<QueuedExecutor>{
      [&](std::filesystem::path const& path,
          std::optional<audio::SampleEncoding> optEncoding) -> Result<std::unique_ptr<audio::DecoderSession>>
      {
        if (rejectInspection && !optEncoding)
        {
          return makeError(Error::Code::IoError, "cannot reopen current track");
        }

        return audio::openDecoderSession(path, optEncoding);
      }};
    auto devices = fixture.status.devices;
    auto const alternate = audio::Device{.id = audio::DeviceId{"alternate"}, .backendId = fixture.status.descriptor.id};
    devices.push_back(alternate);
    fixture.onDevicesChangedCb(devices);
    fixture.executor.drain();

    auto& transport = fixture.playbackTransport;
    std::size_t idleCount = 0;
    auto idleSub = transport.onIdle([&] { ++idleCount; });
    auto const track = fixture.libraryFixture.addTrack({.uri = fixture.installAudioFixture()});
    REQUIRE(transport.playTrack(track, ListId{7}));
    rejectInspection = true;
    transport.setOutputDevice(alternate.backendId, alternate.id, audio::kProfileShared);
    REQUIRE(fixture.executor.tryDrainUntil([&] { return !fixture.notificationService.feed().entries.empty(); }));
    // Reinspection failed before opening the replacement stream. The engine
    // has no item and is Idle, but its terminal outcome was not EOF.
    REQUIRE(transport.state().transport == audio::Transport::Idle);
    REQUIRE(idleCount == 0);

    transport.setOutputDevice(alternate.backendId, audio::DeviceId{"missing"}, audio::kProfileShared);
    fixture.executor.drain();
    CHECK_FALSE(transport.state().ready);
    CHECK(idleCount == 0);
    auto const feed = fixture.notificationService.feed();
    REQUIRE(feed.entries.size() == 1);
    REQUIRE(std::holds_alternative<NotificationReport>(feed.entries.front().message));
    CHECK(std::get<NotificationReport>(feed.entries.front().message).templateId ==
          NotificationReportTemplate::PlaybackTrackOpenFailed);
  }

  TEST_CASE("PlaybackTransport playback - rejected preflight reports synchronously")
  {
    auto fixture = PlaybackTransportFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    auto const trackId = fixture.libraryFixture.addTrack({.title = "Broken Track", .uri = "broken.txt"});

    auto const res = fixture.playbackTransport.playTrack(trackId, ListId{7});

    REQUIRE_FALSE(res);
    CHECK(res.error().message.contains("Unsupported audio file extension"));
    fixture.executor.drain();
    auto const feed = fixture.notificationService.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().lifetime == NotificationLifetime::pinned());
    REQUIRE(std::holds_alternative<NotificationReport>(feed.entries.front().message));
    CHECK(
      std::get<NotificationReport>(feed.entries.front().message).detail.contains("Unsupported audio file extension"));
  }

  TEST_CASE("PlaybackTransport playback - rejected preflight report suppresses identical updates")
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
    auto updateSub =
      fixture.notificationService.onFeedUpdated([&](NotificationFeedUpdate const&) noexcept { ++mutationCount; });

    REQUIRE_FALSE(fixture.playbackTransport.playTrack(trackId, ListId{7}));
    CHECK(mutationCount == 0);

    feed = fixture.notificationService.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().id == originalId);
  }

  TEST_CASE("PlaybackTransport playback - rejected preflight preserves accepted playback")
  {
    auto fixture = PlaybackTransportFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    auto const fixtureUri = fixture.installAudioFixture();
    auto const brokenTrack = fixture.libraryFixture.addTrack({.title = "Stale Broken Track", .uri = "broken.txt"});
    auto const replacementTrack = fixture.libraryFixture.addTrack({.title = "Replacement Track", .uri = fixtureUri});

    REQUIRE(fixture.playbackTransport.playTrack(replacementTrack, ListId{7}));
    REQUIRE_FALSE(fixture.playbackTransport.playTrack(brokenTrack, ListId{7}));

    fixture.executor.drain();

    CHECK(fixture.notificationService.feed().entries.size() == 1);
    CHECK(fixture.playbackTransport.state().nowPlaying.trackId == replacementTrack);
  }

  // A device held by another application stops playback and must not trigger a
  // succession skip, but it can be released at any moment with no event to
  // retract the report. Pinning it would leave a stale error on screen long
  // after the device became available again.
  TEST_CASE("PlaybackTransport playback - a busy device reports a clearable failure")
  {
    auto fixture = PlaybackTransportFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    fakeit::When(Method(fixture.spyBackendPtr->mock(), open))
      .AlwaysDo(
        [](audio::SignalFormat const&, audio::RenderTarget&) -> audio::BackendOpenResult
        {
          // A device held by another application is a genuine output fault, so
          // the refusal keeps the conservative output-failure cause default.
          return std::unexpected{audio::BackendOpenFailure{
            .error = makeError(Error::Code::ResourceBusy, "Device is in use by another application").error()}};
        });

    auto const fixtureUri = fixture.installAudioFixture();
    auto const trackId = fixture.libraryFixture.addTrack({.title = "Busy Device Track", .uri = fixtureUri});

    // The request itself is accepted; the route failure arrives on the engine's
    // own notification path rather than as a synchronous rejection.
    CHECK(fixture.playbackTransport.playTrack(trackId, ListId{7}));
    REQUIRE(fixture.executor.tryDrainUntil([&] { return !fixture.notificationService.feed().entries.empty(); }));

    auto const feed = fixture.notificationService.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().lifetime == NotificationLifetime::history());
    REQUIRE(std::holds_alternative<NotificationReport>(feed.entries.front().message));
    CHECK(std::get<NotificationReport>(feed.entries.front().message).templateId ==
          NotificationReportTemplate::PlaybackRouteActivationFailed);
    // Still a stop, not a skip: nothing advanced past the track.
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Error);
  }

  TEST_CASE("PlaybackTransport playback - route activation failures dedupe by kind")
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

  TEST_CASE("PlaybackTransport playback - backend error publishes pinned device failure")
  {
    auto fixture = PlaybackTransportFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();

    auto const fixtureUri = fixture.installAudioFixture();
    auto const trackId = fixture.libraryFixture.addTrack({.title = "Playing Track", .uri = fixtureUri});

    REQUIRE(fixture.playbackTransport.playTrack(trackId, ListId{7}));
    REQUIRE(fixture.renderTarget != nullptr);

    fixture.renderTarget->handleBackendError("device lost");
    REQUIRE(fixture.executor.tryDrainUntil([&] { return !fixture.notificationService.feed().entries.empty(); }));

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
