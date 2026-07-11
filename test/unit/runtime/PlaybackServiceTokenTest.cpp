// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/audio/EngineTestSupport.h"
#include "test/unit/runtime/PlaybackServiceTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/DecoderSession.h>
#include <ao/audio/Device.h>
#include <ao/audio/Engine.h>
#include <ao/audio/Format.h>
#include <ao/audio/NullBackend.h>
#include <ao/audio/PcmBlock.h>
#include <ao/audio/Player.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/Subscription.h>
#include <ao/audio/Transport.h>
#include <ao/rt/NotificationIds.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackFailure.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PreparedPlayback.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    constexpr auto kSourceListId = ListId{7};

    PlaybackService::PlaybackRequest request(TrackId const trackId, std::string const& path, std::string title)
    {
      return playbackRequest(trackId, path, std::move(title), "Token Artist", std::chrono::minutes{3});
    }

    template<typename ExecutorT>
    void makeReady(PlaybackFixture<ExecutorT>& fixture)
    {
      fixture.onDevicesChangedCb(fixture.status.devices);
    }

    class [[nodiscard]] SemaphoreReleaseGuard final
    {
    public:
      explicit SemaphoreReleaseGuard(std::binary_semaphore& semaphore)
        : _semaphore{semaphore}
      {
      }

      ~SemaphoreReleaseGuard()
      {
        if (_armed)
        {
          _semaphore.release();
        }
      }

      void release()
      {
        _semaphore.release();
        _armed = false;
      }

      SemaphoreReleaseGuard(SemaphoreReleaseGuard const&) = delete;
      SemaphoreReleaseGuard& operator=(SemaphoreReleaseGuard const&) = delete;
      SemaphoreReleaseGuard(SemaphoreReleaseGuard&&) = delete;
      SemaphoreReleaseGuard& operator=(SemaphoreReleaseGuard&&) = delete;

    private:
      std::binary_semaphore& _semaphore;
      bool _armed = true;
    };

    class GatedDecoderSession final : public audio::DecoderSession
    {
    public:
      explicit GatedDecoderSession(std::binary_semaphore* failureRelease)
        : _failureRelease{failureRelease}
      {
      }

      Result<> open(std::filesystem::path const& /*path*/) noexcept override { return {}; }
      void close() noexcept override {}
      void flush() noexcept override {}
      Result<> seek(std::chrono::milliseconds /*offset*/) noexcept override { return {}; }

      // The block aliases decoder-owned preallocated storage, matching PcmBlock's lifetime contract.
      Result<audio::PcmBlock> readNextBlock() noexcept override
      {
        if (!_prerollReturned)
        {
          _prerollReturned = true;
          return audio::PcmBlock{
            .bytes = _prerollBytes,
            .bitDepth = 16,
            .frames = kPrerollFrames,
            .firstFrameIndex = 0,
            .endOfStream = false,
          };
        }

        if (_failureRelease != nullptr && !_failureReturned)
        {
          _failureRelease->acquire();
          _failureReturned = true;
          return std::unexpected{Error{.code = Error::Code::IoError, .message = "gated prepared decode failure"}};
        }

        return audio::PcmBlock{.bytes = {}, .bitDepth = 16, .endOfStream = true};
      }

      audio::DecodedStreamInfo streamInfo() const noexcept override
      {
        auto const format = audio::Format{
          .sampleRate = 44100,
          .channels = 2,
          .bitDepth = 16,
          .isInterleaved = true,
        };
        return audio::DecodedStreamInfo{
          .sourceFormat = format,
          .outputFormat = format,
          .duration = std::chrono::seconds{3},
          .isLossy = false,
          .codec = AudioCodec::Flac,
        };
      }

      GatedDecoderSession(GatedDecoderSession const&) = delete;
      GatedDecoderSession& operator=(GatedDecoderSession const&) = delete;
      GatedDecoderSession(GatedDecoderSession&&) = delete;
      GatedDecoderSession& operator=(GatedDecoderSession&&) = delete;
      ~GatedDecoderSession() override = default;

    private:
      static constexpr std::uint32_t kPrerollFrames = 25000;

      std::binary_semaphore* _failureRelease = nullptr;
      std::vector<std::byte> _prerollBytes =
        std::vector<std::byte>(static_cast<std::size_t>(kPrerollFrames) * 4U, std::byte{0});
      bool _prerollReturned = false;
      bool _failureReturned = false;
    };

    inline auto const kProbeBackendId = audio::BackendId{"playback-service-token-probe"};

    class RenderTargetProbe final
    {
    public:
      void publish(audio::RenderTarget* target)
      {
        auto const lock = std::scoped_lock{_mutex};
        _target = target;
      }

      audio::RenderTarget* target() const
      {
        auto const lock = std::scoped_lock{_mutex};
        return _target;
      }

    private:
      mutable std::mutex _mutex;
      audio::RenderTarget* _target = nullptr;
    };

    class ProbeBackend final : public audio::NullBackend
    {
    public:
      explicit ProbeBackend(std::shared_ptr<RenderTargetProbe> probePtr)
        : _probePtr{std::move(probePtr)}
      {
      }

      Result<> open(audio::Format const& /*format*/, audio::RenderTarget* target) override
      {
        _probePtr->publish(target);
        return {};
      }

      void close() override { _probePtr->publish(nullptr); }
      audio::BackendId backendId() const override { return kProbeBackendId; }
      audio::ProfileId profileId() const override { return audio::ProfileId{audio::kProfileShared}; }

    private:
      std::shared_ptr<RenderTargetProbe> _probePtr;
    };

    class ProbeProvider final : public audio::BackendProvider
    {
    public:
      explicit ProbeProvider(std::shared_ptr<RenderTargetProbe> probePtr)
        : _probePtr{std::move(probePtr)}
      {
      }

      void shutdown() noexcept override {}

      audio::Subscription subscribeDevices(OnDevicesChangedCallback callback) override
      {
        callback(devices());
        return {};
      }

      Status status() const override
      {
        return {
          .descriptor =
            {
              .id = kProbeBackendId,
              .name = "Playback service token probe",
              .supportedProfiles = {{.id = audio::kProfileShared, .name = "Shared"}},
            },
          .devices = devices(),
        };
      }

      std::unique_ptr<audio::Backend> createBackend(audio::Device const& /*device*/,
                                                    audio::ProfileId const& /*profile*/) override
      {
        return std::make_unique<ProbeBackend>(_probePtr);
      }

      audio::Subscription subscribeGraph(std::string_view /*routeAnchor*/, OnGraphChangedCallback /*callback*/) override
      {
        return {};
      }

    private:
      static std::vector<audio::Device> devices()
      {
        return {{
          .id = audio::DeviceId{"playback-service-token-device"},
          .displayName = "Token probe device",
          .isDefault = true,
          .backendId = kProbeBackendId,
        }};
      }

      std::shared_ptr<RenderTargetProbe> _probePtr;
    };
  } // namespace

  TEST_CASE("PlaybackService token - same track receives service-lifetime unique tokens and exact disarm",
            "[runtime][unit][playback][token]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};
    makeReady(fixture);
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const current = request(TrackId{1}, fixturePath, "Current");
    auto const successor = request(TrackId{2}, fixturePath, "Same successor");

    REQUIRE(fixture.playbackService.play(current, kSourceListId));
    auto const beforePrepare = fixture.playbackService.state().nowPlaying;

    auto const firstResult = fixture.playbackService.prepareNext(successor, kSourceListId);
    REQUIRE(firstResult);
    auto const firstToken = *firstResult;
    REQUIRE(PlaybackServiceTestAccess::preparedNextIssuedGeneration(fixture.playbackService, firstToken));
    CHECK(fixture.playbackService.state().nowPlaying == beforePrepare);

    auto const implicitReplacement = fixture.playbackService.prepareNext(successor, kSourceListId);
    REQUIRE_FALSE(implicitReplacement);
    CHECK(implicitReplacement.error().code == Error::Code::InvalidState);
    CHECK(PlaybackServiceTestAccess::preparedNextIssuedGeneration(fixture.playbackService, firstToken));

    CHECK(fixture.playbackService.clearPreparedNext() == firstToken);
    CHECK_FALSE(PlaybackServiceTestAccess::preparedNextIssuedGeneration(fixture.playbackService, firstToken));
    CHECK_FALSE(fixture.playbackService.clearPreparedNext());

    // Explicit session replacement does not reset the service-lifetime token counter.
    REQUIRE(fixture.playbackService.play(current, ListId{8}));
    auto const secondResult = fixture.playbackService.prepareNext(successor, kSourceListId);
    REQUIRE(secondResult);
    auto const secondToken = *secondResult;
    CHECK(secondToken != firstToken);
    CHECK(fixture.playbackService.clearPreparedNext() == secondToken);
  }

  TEST_CASE("PlaybackService token - missing replacement preserves the active token identity",
            "[runtime][unit][playback][token]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};
    makeReady(fixture);
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const currentTrackId = fixture.libraryFixture.addTrack({.title = "Current", .uri = fixturePath});
    auto const nextTrackId = fixture.libraryFixture.addTrack({.title = "Next", .uri = fixturePath});

    REQUIRE(fixture.playbackService.playTrack(currentTrackId, kSourceListId));
    auto const tokenResult = fixture.playbackService.prepareNext(nextTrackId, kSourceListId);
    REQUIRE(tokenResult);
    auto const token = *tokenResult;

    auto const missingResult = fixture.playbackService.prepareNext(TrackId{99999}, kSourceListId);
    REQUIRE_FALSE(missingResult);
    CHECK(missingResult.error().code == Error::Code::NotFound);
    CHECK(PlaybackServiceTestAccess::preparedNextIssuedGeneration(fixture.playbackService, token));
    CHECK(fixture.playbackService.clearPreparedNext() == token);
  }

  TEST_CASE("PlaybackService token - rejected stage and stale commit preserve current and lookahead",
            "[runtime][unit][playback][token]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};
    makeReady(fixture);
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const current = request(TrackId{1}, fixturePath, "Current");
    auto const stagedCandidate = request(TrackId{2}, fixturePath, "Staged candidate");
    auto const replacement = request(TrackId{3}, fixturePath, "Replacement");
    auto const successor = request(TrackId{4}, fixturePath, "Replacement successor");

    REQUIRE(fixture.playbackService.play(current, kSourceListId));
    auto const oldTokenResult = fixture.playbackService.prepareNext(successor, kSourceListId);
    REQUIRE(oldTokenResult);
    auto const oldToken = *oldTokenResult;

    auto const rejectedStage = fixture.playbackService.stagePlayback(
      request(TrackId{99}, "/missing/staged.flac", "Missing staged candidate"), kSourceListId);
    REQUIRE_FALSE(rejectedStage);
    CHECK(fixture.playbackService.state().nowPlaying.trackId == current.item.trackId);
    CHECK(PlaybackServiceTestAccess::preparedNextIssuedGeneration(fixture.playbackService, oldToken));
    CHECK(fixture.playbackService.clearPreparedNext() == oldToken);

    auto staged = fixture.playbackService.stagePlayback(stagedCandidate, kSourceListId);
    REQUIRE(staged);
    REQUIRE(fixture.playbackService.play(replacement, kSourceListId));

    auto const liveTokenResult = fixture.playbackService.prepareNext(successor, kSourceListId);
    REQUIRE(liveTokenResult);
    auto const liveToken = *liveTokenResult;
    auto const staleCommit = fixture.playbackService.commitPlayback(std::move(*staged));

    REQUIRE_FALSE(staleCommit);
    CHECK(staleCommit.error().code == Error::Code::InvalidState);
    CHECK(fixture.playbackService.state().nowPlaying.trackId == replacement.item.trackId);
    CHECK(PlaybackServiceTestAccess::preparedNextIssuedGeneration(fixture.playbackService, liveToken));
    CHECK(fixture.playbackService.clearPreparedNext() == liveToken);
  }

  TEST_CASE("PlaybackService token - staged decode error before commit reports once without publishing start",
            "[runtime][unit][playback][token]")
  {
    auto failureGate = audio::test::StagedFailureGate{};
    auto executor = QueuedExecutor{};
    auto libraryFixture = MusicLibraryFixture{};
    auto notifications = NotificationService{};
    auto playerPtr = std::make_unique<audio::Player>(
      executor, audio::test::makeStagedFailureDecoderFactory("candidate-failure.flac", failureGate));
    auto servicePtr = PlaybackServiceTestAccess::createWithPlayer(
      executor, libraryFixture.library(), notifications, std::move(playerPtr));
    addReadyAudioProvider(*servicePtr);
    executor.drain();

    auto const current = request(TrackId{31}, "current.flac", "Current");
    auto const next = request(TrackId{32}, "next.flac", "Next");
    auto const candidate = request(TrackId{33}, "candidate-failure.flac", "Failed candidate");
    REQUIRE(servicePtr->play(current, kSourceListId));
    executor.drain();
    auto const preparedNext = servicePtr->prepareNext(next, kSourceListId);
    REQUIRE(preparedNext);

    auto staged = servicePtr->stagePlayback(candidate, ListId{8});
    REQUIRE(staged);
    auto releaseGuard = audio::test::StagedFailureReleaseGuard{failureGate};
    REQUIRE(failureGate.waitForRead());

    std::size_t startedCount = 0;
    auto nowPlaying = std::vector<PlaybackService::NowPlayingChanged>{};
    auto failures = std::vector<PlaybackFailure>{};
    std::size_t notificationCount = 0;
    auto startedSub = servicePtr->onStarted([&] { ++startedCount; });
    auto nowPlayingSub = servicePtr->onNowPlayingChanged([&](PlaybackService::NowPlayingChanged const& event)
                                                         { nowPlaying.push_back(event); });
    auto failureSub =
      servicePtr->onPlaybackFailure([&](PlaybackFailure const& failure) { failures.push_back(failure); });
    auto notificationSub = notifications.onPosted([&](NotificationId) { ++notificationCount; });

    releaseGuard.release();
    executor.checkQueued(std::chrono::seconds{5});
    executor.drain();

    auto const committed = servicePtr->commitPlayback(std::move(*staged));
    REQUIRE_FALSE(committed);
    CHECK(committed.error().code == Error::Code::IoError);
    CHECK(committed.error().message == "gated staged decode failure");
    CHECK(servicePtr->state().transport == audio::Transport::Playing);
    CHECK(servicePtr->state().nowPlaying.trackId == current.item.trackId);
    CHECK(servicePtr->state().nowPlaying.sourceListId == kSourceListId);
    CHECK(servicePtr->clearPreparedNext() == *preparedNext);
    CHECK(startedCount == 0);
    CHECK(nowPlaying.empty());
    CHECK(failures.empty());
    CHECK(notificationCount == 1);
    auto const feed = notifications.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().severity == NotificationSeverity::Error);
    CHECK(feed.entries.front().message.contains("gated staged decode failure"));
  }

  TEST_CASE("PlaybackService token - drain fallback returns exact disarm acknowledgement",
            "[runtime][unit][playback][token]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};
    makeReady(fixture);
    auto const flacPath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const mp3Path = audio::test::requireAudioFixture("basic_metadata.mp3").string();
    auto const current = request(TrackId{1}, flacPath, "Current");
    auto const fallbackSuccessor = request(TrackId{2}, mp3Path, "Drain fallback successor");

    REQUIRE(fixture.playbackService.play(current, kSourceListId));
    auto const tokenResult = fixture.playbackService.prepareNext(fallbackSuccessor, kSourceListId);
    REQUIRE(tokenResult);
    auto const token = *tokenResult;
    auto const optIssuedGeneration =
      PlaybackServiceTestAccess::preparedNextIssuedGeneration(fixture.playbackService, token);
    REQUIRE(optIssuedGeneration);

    CHECK(fixture.playbackService.clearPreparedNext() == token);
    CHECK_FALSE(PlaybackServiceTestAccess::preparedNextIssuedGeneration(fixture.playbackService, token));
    CHECK_FALSE(fixture.playbackService.clearPreparedNext());
  }

  TEST_CASE("PlaybackService token - repeated drain fallback reprepare does not retain request metadata",
            "[runtime][regression][playback][token]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};
    makeReady(fixture);
    auto const flacPath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const mp3Path = audio::test::requireAudioFixture("basic_metadata.mp3").string();
    auto const current = request(TrackId{1}, flacPath, "Current");
    auto const fallbackSuccessor = request(TrackId{2}, mp3Path, "Drain fallback successor");
    auto tokens = std::vector<PreparedNextToken>{};
    constexpr std::size_t kReprepareCount = 32;

    REQUIRE(fixture.playbackService.play(current, kSourceListId));
    tokens.reserve(kReprepareCount);

    for (std::size_t index = 0; index < kReprepareCount; ++index)
    {
      auto const tokenResult = fixture.playbackService.prepareNext(fallbackSuccessor, kSourceListId);
      REQUIRE(tokenResult);
      tokens.push_back(*tokenResult);

      CHECK(fixture.playbackService.clearPreparedNext() == tokens.back());
      CHECK_FALSE(PlaybackServiceTestAccess::preparedNextIssuedGeneration(fixture.playbackService, tokens.back()));
    }

    for (auto const token : tokens)
    {
      CHECK_FALSE(PlaybackServiceTestAccess::preparedNextIssuedGeneration(fixture.playbackService, token));
    }

    CHECK_FALSE(fixture.playbackService.clearPreparedNext());
  }

  TEST_CASE("PlaybackService token - stop barrier covers and removes an armed commitment",
            "[runtime][unit][playback][token]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};
    makeReady(fixture);
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const current = request(TrackId{1}, fixturePath, "Current");
    auto const successor = request(TrackId{2}, fixturePath, "Successor");

    REQUIRE(fixture.playbackService.play(current, kSourceListId));
    auto const tokenResult = fixture.playbackService.prepareNext(successor, kSourceListId);
    REQUIRE(tokenResult);
    auto const token = *tokenResult;
    auto const optIssuedGeneration =
      PlaybackServiceTestAccess::preparedNextIssuedGeneration(fixture.playbackService, token);
    REQUIRE(optIssuedGeneration);

    auto const barrier = fixture.playbackService.stop();

    CHECK(barrier.covers(*optIssuedGeneration));
    CHECK_FALSE(PlaybackServiceTestAccess::preparedNextIssuedGeneration(fixture.playbackService, token));
    CHECK_FALSE(fixture.playbackService.clearPreparedNext());
    CHECK(fixture.playbackService.state().nowPlaying.trackId == kInvalidTrackId);
  }

  TEST_CASE("PlaybackService token - unknown prepared transition is rejected instead of becoming explicit play",
            "[runtime][unit][playback][token]")
  {
    auto executor = QueuedExecutor{};
    auto libraryFixture = MusicLibraryFixture{};
    auto notifications = NotificationService{};
    auto playerPtr = std::make_unique<audio::Player>(executor);
    auto* const playerRaw = playerPtr.get();
    auto servicePtr = PlaybackServiceTestAccess::createWithPlayer(
      executor, libraryFixture.library(), notifications, std::move(playerPtr));
    auto probePtr = std::make_shared<RenderTargetProbe>();
    servicePtr->addProvider(std::make_unique<ProbeProvider>(probePtr));
    executor.drain();

    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const current = request(TrackId{1}, fixturePath, "Current");
    auto nowPlaying = std::vector<PlaybackService::NowPlayingChanged>{};
    auto sub = servicePtr->onNowPlayingChanged([&](PlaybackService::NowPlayingChanged const& event)
                                               { nowPlaying.push_back(event); });

    REQUIRE(servicePtr->play(current, kSourceListId));
    executor.drain();
    nowPlaying.clear();

    auto const unknownItem = audio::Engine::PlaybackItem{
      .id = audio::Engine::PlaybackItemId{.value = 9999},
      .input = audio::PlaybackInput{.filePath = fixturePath, .duration = std::chrono::minutes{3}},
    };
    REQUIRE(playerRaw->prepareNext(unknownItem));
    auto* const target = probePtr->target();
    REQUIRE(target != nullptr);

    auto output = std::vector<std::byte>(4100);
    bool crossedSpliceBoundary = false;

    for (std::int32_t attempt = 0; attempt < 100000 && !crossedSpliceBoundary; ++attempt)
    {
      crossedSpliceBoundary = target->renderPcm(output).positionFrameOffset > 0;
    }

    REQUIRE(crossedSpliceBoundary);
    executor.checkQueued(std::chrono::seconds{5});
    executor.drain();

    CHECK(nowPlaying.empty());
    CHECK(servicePtr->state().nowPlaying.trackId == current.item.trackId);
  }

  TEST_CASE("PlaybackService token - prepared decode failure reports the exact same-track token",
            "[runtime][unit][playback][token]")
  {
    auto failureRelease = std::binary_semaphore{0};
    auto executor = QueuedExecutor{};
    auto libraryFixture = MusicLibraryFixture{};
    auto notifications = NotificationService{};
    auto playerPtr = std::make_unique<audio::Player>(
      executor,
      [&](std::filesystem::path const& path, audio::Format const&)
      {
        return std::make_unique<GatedDecoderSession>(
          path == std::filesystem::path{"prepared-fail.flac"} ? &failureRelease : nullptr);
      });
    auto servicePtr = PlaybackServiceTestAccess::createWithPlayer(
      executor, libraryFixture.library(), notifications, std::move(playerPtr));
    auto releaseGuard = SemaphoreReleaseGuard{failureRelease};
    addReadyAudioProvider(*servicePtr);
    executor.drain();

    auto const sharedTrackId = TrackId{22};
    auto const current = request(TrackId{1}, "current.flac", "Current");
    auto const firstCommitment = request(sharedTrackId, "prepared-good.flac", "Same track first");
    auto const failingCommitment = request(sharedTrackId, "prepared-fail.flac", "Same track second");

    REQUIRE(servicePtr->play(current, kSourceListId));
    auto const firstTokenResult = servicePtr->prepareNext(firstCommitment, kSourceListId);
    REQUIRE(firstTokenResult);
    auto const firstToken = *firstTokenResult;
    REQUIRE(servicePtr->clearPreparedNext() == firstToken);

    auto failures = std::vector<PlaybackFailure>{};
    auto sub = servicePtr->onPlaybackFailure([&](PlaybackFailure const& failure) { failures.push_back(failure); });
    auto const failingTokenResult = servicePtr->prepareNext(failingCommitment, kSourceListId);
    REQUIRE(failingTokenResult);
    auto const failingToken = *failingTokenResult;
    REQUIRE(failingToken != firstToken);

    releaseGuard.release();
    REQUIRE(executor.drainUntil([&] { return !failures.empty(); }, std::chrono::seconds{5}));

    REQUIRE(failures.size() == 1);
    CHECK(failures.front().kind == PlaybackFailureKind::Decode);
    CHECK(failures.front().trackId == sharedTrackId);
    CHECK(failures.front().sourceListId == kSourceListId);
    CHECK(failures.front().optPreparedNextToken == failingToken);
    CHECK(failures.front().error.message == "gated prepared decode failure");
    CHECK(failures.front().recoverable);
    CHECK(servicePtr->state().nowPlaying.trackId == current.item.trackId);
    CHECK_FALSE(PlaybackServiceTestAccess::preparedNextIssuedGeneration(*servicePtr, failingToken));
    CHECK_FALSE(servicePtr->clearPreparedNext());
  }

  TEST_CASE("PlaybackService token - accepted start contains reentrant transport mutation and observer throws",
            "[runtime][regression][playback][token]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};
    makeReady(fixture);
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const current = request(TrackId{1}, fixturePath, "Current");
    auto const replacement = request(TrackId{2}, fixturePath, "Accepted replacement");
    auto const reentrant = request(TrackId{3}, fixturePath, "Rejected reentrant");
    REQUIRE(fixture.playbackService.play(current, kSourceListId));
    auto staged = fixture.playbackService.stagePlayback(reentrant, ListId{8});
    REQUIRE(staged);

    auto optStageError = std::optional<Error::Code>{};
    auto optPlayError = std::optional<Error::Code>{};
    auto optCommitError = std::optional<Error::Code>{};
    auto optPrepareError = std::optional<Error::Code>{};
    auto optClearResult = std::optional<PreparedNextToken>{};
    auto stopBarrier = PreparedCancellationBarrier{};
    bool callbackEntered = false;
    auto const baselineVolume = fixture.playbackService.state().volume.level;
    auto const baselineMuted = fixture.playbackService.state().volume.muted;
    auto const baselineOutput = fixture.playbackService.state().output.selectedDevice;
    auto const startedSubscription = fixture.playbackService.onStarted(
      [&]
      {
        callbackEntered = true;

        if (auto const result = fixture.playbackService.stagePlayback(reentrant, ListId{9}); !result)
        {
          optStageError = result.error().code;
        }

        if (auto const result = fixture.playbackService.play(reentrant, ListId{9}); !result)
        {
          optPlayError = result.error().code;
        }

        if (auto const result = fixture.playbackService.commitPlayback(std::move(*staged)); !result)
        {
          optCommitError = result.error().code;
        }

        if (auto const result = fixture.playbackService.prepareNext(reentrant, ListId{9}); !result)
        {
          optPrepareError = result.error().code;
        }

        optClearResult = fixture.playbackService.clearPreparedNext();
        fixture.playbackService.pause();
        fixture.playbackService.resume();
        fixture.playbackService.seek(std::chrono::seconds{20});
        fixture.playbackService.setOutputDevice(
          audio::BackendId{"reentrant"}, audio::DeviceId{"reentrant"}, audio::ProfileId{"reentrant"});
        fixture.playbackService.setVolume(0.25F);
        fixture.playbackService.setMuted(!baselineMuted);
        stopBarrier = fixture.playbackService.stop();
        throwException<Exception>("scripted accepted-start observer failure");
      });
    auto nowPlaying = std::vector<PlaybackService::NowPlayingChanged>{};
    auto const nowPlayingSubscription = fixture.playbackService.onNowPlayingChanged(
      [&](PlaybackService::NowPlayingChanged const& event) { nowPlaying.push_back(event); });

    auto const accepted = fixture.playbackService.play(replacement, ListId{10});

    REQUIRE(accepted);
    CHECK(callbackEntered);
    CHECK(optStageError == Error::Code::InvalidState);
    CHECK(optPlayError == Error::Code::InvalidState);
    CHECK(optCommitError == Error::Code::InvalidState);
    CHECK(optPrepareError == Error::Code::InvalidState);
    CHECK_FALSE(optClearResult);
    CHECK(stopBarrier.generation == 0);
    CHECK_FALSE(stopBarrier.covers(0));
    CHECK_FALSE(stopBarrier.covers(accepted->cancellationBarrier.generation));
    CHECK(fixture.playbackService.state().transport == audio::Transport::Playing);
    CHECK(fixture.playbackService.state().nowPlaying.trackId == replacement.item.trackId);
    CHECK(fixture.playbackService.state().volume.level == baselineVolume);
    CHECK(fixture.playbackService.state().volume.muted == baselineMuted);
    CHECK(fixture.playbackService.state().output.selectedDevice == baselineOutput);
    REQUIRE(nowPlaying.size() == 1);
    CHECK(nowPlaying.front().trackId == replacement.item.trackId);
    CHECK(nowPlaying.front().sourceListId == ListId{10});

    auto const postPublicationBarrier = fixture.playbackService.stop();

    CHECK(postPublicationBarrier.generation > 0);
    CHECK(fixture.playbackService.state().transport == audio::Transport::Idle);
    CHECK(fixture.playbackService.state().nowPlaying.trackId == kInvalidTrackId);
  }
} // namespace ao::rt::test
