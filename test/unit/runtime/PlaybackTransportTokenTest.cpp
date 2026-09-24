// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/playback/PlaybackTransport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/audio/EngineTestSupport.h"
#include "test/unit/audio/ScriptedDecoderSession.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/PlaybackTransportTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/DecoderSession.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/Player.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/Transport.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PreparedPlayback.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    constexpr auto kSourceListId = ListId{7};

    PlaybackTransport::PlaybackRequest request(TrackId const trackId, std::string const& path, std::string title)
    {
      return playbackRequest(trackId, path, std::move(title), "Token Artist", std::chrono::minutes{3});
    }

    template<typename ExecutorT>
    void makeReady(PlaybackTransportFixture<ExecutorT>& fixture)
    {
      fixture.onDevicesChangedCb(fixture.status.devices);
    }
  } // namespace

  TEST_CASE("PlaybackTransport token - same track receives transport-lifetime unique tokens and exact disarm",
            "[runtime][unit][playback][token]")
  {
    auto fixture = PlaybackTransportFixture<InlineExecutor>{};
    makeReady(fixture);
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const current = request(TrackId{1}, fixturePath, "Current");
    auto const successor = request(TrackId{2}, fixturePath, "Same successor");

    REQUIRE(fixture.playbackTransport.play(current, kSourceListId));
    auto const beforePrepare = fixture.playbackTransport.state().nowPlaying;

    auto const firstRes = fixture.playbackTransport.prepareNext(successor, kSourceListId);
    REQUIRE(firstRes);
    auto const firstToken = *firstRes;
    CHECK(fixture.playbackTransport.state().nowPlaying == beforePrepare);

    // Explicit session replacement does not reset the transport-lifetime token counter.
    REQUIRE(fixture.playbackTransport.play(current, ListId{8}));
    auto const secondRes = fixture.playbackTransport.prepareNext(successor, kSourceListId);
    REQUIRE(secondRes);
    auto const secondToken = *secondRes;
    CHECK(secondToken != firstToken);
    CHECK(fixture.playbackTransport.clearPreparedNext() == secondToken);
  }

  TEST_CASE("PlaybackTransport token - missing replacement preserves the active token identity",
            "[runtime][unit][playback][token]")
  {
    auto fixture = PlaybackTransportFixture<InlineExecutor>{};
    makeReady(fixture);
    auto const fixtureUri = fixture.installAudioFixture();
    auto const currentTrackId = fixture.libraryFixture.addTrack({.title = "Current", .uri = fixtureUri});
    auto const nextTrackId = fixture.libraryFixture.addTrack({.title = "Next", .uri = fixtureUri});

    REQUIRE(fixture.playbackTransport.playTrack(currentTrackId, kSourceListId));
    auto const nextRequestRes = playbackRequestForTrack(fixture.libraryFixture.library(), nextTrackId);
    REQUIRE(nextRequestRes);
    auto const tokenRes = fixture.playbackTransport.prepareNext(*nextRequestRes, kSourceListId);
    REQUIRE(tokenRes);
    auto const token = *tokenRes;

    auto const missingRes = playbackRequestForTrack(fixture.libraryFixture.library(), TrackId{99999});
    REQUIRE_FALSE(missingRes);
    CHECK(missingRes.error().code == Error::Code::NotFound);
    // The rejected missing-track replacement preserves the active token identity.
    CHECK(fixture.playbackTransport.clearPreparedNext() == token);
  }

  TEST_CASE("PlaybackTransport token - rejected stage and stale commit preserve current and lookahead",
            "[runtime][unit][playback][token][concurrency]")
  {
    auto fixture = PlaybackTransportFixture<InlineExecutor>{};
    makeReady(fixture);
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const current = request(TrackId{1}, fixturePath, "Current");
    auto const stagedCandidate = request(TrackId{2}, fixturePath, "Staged candidate");
    auto const replacement = request(TrackId{3}, fixturePath, "Replacement");
    auto const successor = request(TrackId{4}, fixturePath, "Replacement successor");

    REQUIRE(fixture.playbackTransport.play(current, kSourceListId));
    auto const oldTokenRes = fixture.playbackTransport.prepareNext(successor, kSourceListId);
    REQUIRE(oldTokenRes);
    auto const oldToken = *oldTokenRes;

    auto const rejectedStageRes = fixture.playbackTransport.stagePlayback(
      request(TrackId{99}, "/missing/staged.flac", "Missing staged candidate"), kSourceListId);
    REQUIRE_FALSE(rejectedStageRes);
    CHECK(fixture.playbackTransport.state().nowPlaying.trackId == current.item.trackId);
    CHECK(fixture.playbackTransport.clearPreparedNext() == oldToken);

    auto stagedRes = fixture.playbackTransport.stagePlayback(stagedCandidate, kSourceListId);
    REQUIRE(stagedRes);
    REQUIRE(fixture.playbackTransport.play(replacement, kSourceListId));

    auto const liveTokenRes = fixture.playbackTransport.prepareNext(successor, kSourceListId);
    REQUIRE(liveTokenRes);
    auto const liveToken = *liveTokenRes;
    auto const staleCommitRes = fixture.playbackTransport.commitPlayback(std::move(*stagedRes));

    REQUIRE_FALSE(staleCommitRes);
    CHECK(staleCommitRes.error().code == Error::Code::Conflict);
    CHECK(fixture.playbackTransport.state().nowPlaying.trackId == replacement.item.trackId);
    CHECK(fixture.playbackTransport.clearPreparedNext() == liveToken);
  }

  TEST_CASE("PlaybackTransport token - final decoder setup failure settles the committed cancellation barrier",
            "[runtime][unit][playback][token][concurrency]")
  {
    auto const format = audio::test::makeEngineTestFormat();
    auto decoderFactory =
      [format](std::filesystem::path const& path,
               std::optional<audio::SampleEncoding> optOutputEncoding) -> Result<std::unique_ptr<audio::DecoderSession>>
    {
      auto const sourceFormat = signalFormat(format);
      auto decoderPtr = std::make_unique<audio::test::ScriptedDecoderSession>(audio::DecodedStreamInfo{
        .sourceFormat = sourceFormat,
        .outputFormat = pcmFormat(sourceFormat, optOutputEncoding.value_or(format.encoding)),
        .duration = std::chrono::minutes{3},
        .codec = AudioCodec::Flac,
      });
      decoderPtr->setReadScript(
        {{.data = std::vector<std::byte>(4096, std::byte{0}), .endOfStream = false}, {.endOfStream = true}});

      if (path == "candidate-failure.flac" && optOutputEncoding)
      {
        return makeError(Error::Code::IoError, "final decoder setup failed");
      }

      return std::unique_ptr<audio::DecoderSession>{std::move(decoderPtr)};
    };
    auto executor = QueuedExecutor{};
    auto libraryFixture = MusicLibraryFixture{};
    auto runtime = async::Runtime{executor, 1};
    auto notifications = NotificationService{runtime};
    auto playerPtr = std::make_unique<audio::Player>(executor, std::move(decoderFactory));
    auto transportPtr =
      std::make_unique<PlaybackTransport>(executor, libraryFixture.library(), notifications, std::move(playerPtr));
    addReadyAudioProvider(*transportPtr);
    executor.drain();

    auto const current = request(TrackId{31}, "current.flac", "Current");
    auto const next = request(TrackId{32}, "next.flac", "Next");
    auto const candidate = request(TrackId{33}, "candidate-failure.flac", "Failed candidate");
    REQUIRE(transportPtr->play(current, kSourceListId));
    executor.drain();
    auto const preparedNextRes = transportPtr->prepareNext(next, kSourceListId);
    REQUIRE(preparedNextRes);

    auto stagedRes = transportPtr->stagePlayback(candidate, ListId{8});
    REQUIRE(stagedRes);

    std::size_t startedCount = 0;
    auto nowPlaying = std::vector<PlaybackTransport::NowPlayingChanged>{};
    std::size_t notificationCount = 0;
    auto startedSub = transportPtr->onStarted([&] noexcept { ++startedCount; });
    auto nowPlayingSub = transportPtr->onNowPlayingChanged(
      [&](PlaybackTransport::NowPlayingChanged const& event) noexcept { nowPlaying.push_back(event); });
    auto notificationSub = notifications.onFeedUpdated(
      [&](NotificationFeedUpdate const& update) noexcept
      {
        if (update.mutationKind == NotificationFeedMutationKind::Posted)
        {
          ++notificationCount;
        }
      });

    auto const committedRes = transportPtr->commitPlayback(std::move(*stagedRes));
    REQUIRE(committedRes);
    CHECK(transportPtr->state().transport == audio::Transport::Error);
    CHECK(transportPtr->state().nowPlaying.trackId == candidate.item.trackId);
    CHECK(transportPtr->state().nowPlaying.sourceListId == ListId{8});
    CHECK_FALSE(transportPtr->clearPreparedNext());
    CHECK(startedCount == 0);
    CHECK(nowPlaying.empty());
    CHECK(notificationCount == 0);
    REQUIRE(executor.tryDrainUntil([&] { return notificationCount == 1; }, std::chrono::seconds{5}));
    auto const feed = notifications.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().severity == NotificationSeverity::Error);
    CHECK(feed.entries.front().lifetime == NotificationLifetime::history());
    REQUIRE(std::holds_alternative<NotificationReport>(feed.entries.front().message));
    auto const& report = std::get<NotificationReport>(feed.entries.front().message);
    CHECK(report.templateId == NotificationReportTemplate::PlaybackTrackOpenFailed);
    CHECK(report.trackId == candidate.item.trackId);
    CHECK(report.detail.contains("final decoder setup failed"));
  }

  TEST_CASE("PlaybackTransport token - accepted empty playback ends naturally without a start or failure",
            "[runtime][unit][playback][token]")
  {
    auto const factory = [](auto const&, std::optional<audio::SampleEncoding> optOutputEncoding)
    {
      auto const sourceFormat = audio::SignalFormat{.sampleRate = 44100, .channels = 2, .precisionBits = 16};
      return std::make_unique<audio::test::ScriptedDecoderSession>(audio::DecodedStreamInfo{
        .sourceFormat = sourceFormat,
        .outputFormat = audio::pcmFormat(sourceFormat, optOutputEncoding.value_or(audio::SampleEncoding::Signed16Le)),
        .codec = AudioCodec::Flac,
      });
    };
    auto fixture = PlaybackTransportFixture<QueuedExecutor>{factory};
    makeReady(fixture);
    fixture.executor.drain();
    std::size_t idleEvents = 0;
    std::size_t startedEvents = 0;
    std::size_t nowPlayingEvents = 0;
    auto const idleSubscription = fixture.playbackTransport.onIdle([&] { ++idleEvents; });
    auto const startedSubscription = fixture.playbackTransport.onStarted([&] { ++startedEvents; });
    auto const nowPlayingSubscription = fixture.playbackTransport.onNowPlayingChanged(
      [&](PlaybackTransport::NowPlayingChanged const&) { ++nowPlayingEvents; });
    auto const candidate = request(TrackId{34}, "empty.flac", "Empty track");
    auto stagedRes = fixture.playbackTransport.stagePlayback(candidate, kSourceListId);
    REQUIRE(stagedRes);

    auto const committedRes = fixture.playbackTransport.commitPlayback(std::move(*stagedRes));
    REQUIRE(committedRes);
    CHECK(committedRes->generation > 0);
    REQUIRE(fixture.executor.tryDrainUntil([&] { return idleEvents == 1; }));
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Idle);
    CHECK(startedEvents == 0);
    CHECK(nowPlayingEvents == 0);
    CHECK(fixture.notificationService.feed().entries.empty());
  }

  TEST_CASE("PlaybackTransport token - drain fallback returns exact disarm acknowledgement",
            "[runtime][unit][playback][token]")
  {
    auto fixture = PlaybackTransportFixture<InlineExecutor>{};
    makeReady(fixture);
    auto const flacPath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const mp3Path = audio::test::requireAudioFixture("basic_metadata.mp3").string();
    auto const current = request(TrackId{1}, flacPath, "Current");
    auto const fallbackSuccessor = request(TrackId{2}, mp3Path, "Drain fallback successor");

    REQUIRE(fixture.playbackTransport.play(current, kSourceListId));
    auto const tokenRes = fixture.playbackTransport.prepareNext(fallbackSuccessor, kSourceListId);
    REQUIRE(tokenRes);
    auto const token = *tokenRes;

    // The disarm acknowledgement returns the exact armed token, then nothing.
    CHECK(fixture.playbackTransport.clearPreparedNext() == token);
    CHECK_FALSE(fixture.playbackTransport.clearPreparedNext());
  }

  TEST_CASE("PlaybackTransport token - repeated drain fallback reprepare does not retain request metadata",
            "[runtime][unit][playback][token]")
  {
    auto fixture = PlaybackTransportFixture<InlineExecutor>{};
    makeReady(fixture);
    auto const flacPath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const mp3Path = audio::test::requireAudioFixture("basic_metadata.mp3").string();
    auto const current = request(TrackId{1}, flacPath, "Current");
    auto const fallbackSuccessor = request(TrackId{2}, mp3Path, "Drain fallback successor");
    auto tokens = std::vector<PreparedNextToken>{};
    constexpr std::size_t kReprepareCount = 32;

    REQUIRE(fixture.playbackTransport.play(current, kSourceListId));
    tokens.reserve(kReprepareCount);

    for (std::size_t index = 0; index < kReprepareCount; ++index)
    {
      auto const tokenRes = fixture.playbackTransport.prepareNext(fallbackSuccessor, kSourceListId);
      REQUIRE(tokenRes);
      tokens.push_back(*tokenRes);

      CHECK(fixture.playbackTransport.clearPreparedNext() == tokens.back());
    }

    CHECK_FALSE(fixture.playbackTransport.clearPreparedNext());
  }

  TEST_CASE("PlaybackTransport token - stop barrier covers and removes an armed commitment",
            "[runtime][unit][playback][token][concurrency]")
  {
    auto fixture = PlaybackTransportFixture<InlineExecutor>{};
    makeReady(fixture);
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const current = request(TrackId{1}, fixturePath, "Current");
    auto const successor = request(TrackId{2}, fixturePath, "Successor");

    REQUIRE(fixture.playbackTransport.play(current, kSourceListId));
    REQUIRE(fixture.playbackTransport.prepareNext(successor, kSourceListId));

    fixture.playbackTransport.stop();

    // stop() covers and removes the armed prepared-next commitment: nothing
    // remains to clear, and now-playing is cleared.
    CHECK_FALSE(fixture.playbackTransport.clearPreparedNext());
    CHECK(fixture.playbackTransport.state().nowPlaying.trackId == kInvalidTrackId);
  }

  TEST_CASE("PlaybackTransport token - prepared decode failure clears the active same-track commitment",
            "[runtime][unit][playback][token][concurrency]")
  {
    auto failureGate = audio::test::StagedFailureGate{};
    auto executor = QueuedExecutor{};
    auto libraryFixture = MusicLibraryFixture{};
    auto runtime = async::Runtime{executor, 1};
    auto notifications = NotificationService{runtime};
    auto decoderFactory = audio::test::makeStagedFailureDecoderFactory("prepared-fail.flac", failureGate);
    auto playerPtr = std::make_unique<audio::Player>(executor, std::move(decoderFactory));
    auto transportPtr =
      std::make_unique<PlaybackTransport>(executor, libraryFixture.library(), notifications, std::move(playerPtr));
    auto releaseGuard = audio::test::StagedFailureReleaseGuard{failureGate};
    addReadyAudioProvider(*transportPtr);
    executor.drain();

    auto const sharedTrackId = TrackId{22};
    auto const current = request(TrackId{1}, "current.flac", "Current");
    auto const firstCommitment = request(sharedTrackId, "prepared-good.flac", "Same track first");
    auto const failingCommitment = request(sharedTrackId, "prepared-fail.flac", "Same track second");

    REQUIRE(transportPtr->play(current, kSourceListId));
    auto const firstTokenRes = transportPtr->prepareNext(firstCommitment, kSourceListId);
    REQUIRE(firstTokenRes);
    auto const firstToken = *firstTokenRes;
    REQUIRE(transportPtr->clearPreparedNext() == firstToken);

    auto const failingTokenRes = transportPtr->prepareNext(failingCommitment, kSourceListId);
    REQUIRE(failingTokenRes);
    auto const failingToken = *failingTokenRes;
    REQUIRE(failingToken != firstToken);
    REQUIRE(failureGate.tryWaitForRead());

    releaseGuard.release();
    REQUIRE(executor.tryDrainUntil([&] { return !notifications.feed().entries.empty(); }, std::chrono::seconds{5}));

    auto const feed = notifications.feed();
    REQUIRE(feed.entries.size() == 1);
    REQUIRE(std::holds_alternative<NotificationReport>(feed.entries.front().message));
    CHECK(std::get<NotificationReport>(feed.entries.front().message).detail == "gated staged decode failure");
    CHECK(transportPtr->state().nowPlaying.trackId == current.item.trackId);
    CHECK_FALSE(transportPtr->clearPreparedNext());
  }

  TEST_CASE("PlaybackTransport token - accepted start publishes in connection order with committed state",
            "[runtime][unit][playback][token]")
  {
    auto fixture = PlaybackTransportFixture<InlineExecutor>{};
    makeReady(fixture);
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const current = request(TrackId{1}, fixturePath, "Current");
    auto const replacement = request(TrackId{2}, fixturePath, "Accepted replacement");
    REQUIRE(fixture.playbackTransport.play(current, kSourceListId));

    // Transport observers are noexcept, so what this proves is delivery order:
    // the started observer already sees the replacement installed, and the
    // now-playing observer still receives its event afterwards.
    bool callbackEntered = false;
    auto observedTrackId = kInvalidTrackId;
    auto const startedSubscription = fixture.playbackTransport.onStarted(
      [&] noexcept
      {
        callbackEntered = true;
        observedTrackId = fixture.playbackTransport.state().nowPlaying.trackId;
      });
    auto nowPlaying = std::vector<PlaybackTransport::NowPlayingChanged>{};
    auto const nowPlayingSubscription = fixture.playbackTransport.onNowPlayingChanged(
      [&](PlaybackTransport::NowPlayingChanged const& event) noexcept { nowPlaying.push_back(event); });

    auto const acceptedRes = fixture.playbackTransport.play(replacement, ListId{10});

    REQUIRE(acceptedRes);
    CHECK(callbackEntered);
    CHECK(observedTrackId == replacement.item.trackId);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Playing);
    CHECK(fixture.playbackTransport.state().nowPlaying.trackId == replacement.item.trackId);
    REQUIRE(nowPlaying.size() == 1);
    CHECK(nowPlaying.front().trackId == replacement.item.trackId);
    CHECK(nowPlaying.front().sourceListId == ListId{10});

    auto const postPublicationBarrier = fixture.playbackTransport.stop();

    CHECK(postPublicationBarrier.generation > 0);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Idle);
    CHECK(fixture.playbackTransport.state().nowPlaying.trackId == kInvalidTrackId);
  }
} // namespace ao::rt::test
