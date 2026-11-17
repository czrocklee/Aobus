// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "EngineTestSupport.h"
#include "FakeCapturingBackend.h"
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Engine.h>
#include <ao/audio/OpenedPcmMode.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/Property.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/Transport.h>
#include <ao/utility/ScopedRegistration.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <expected>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    struct BackendLifecycleCounts final
    {
      std::atomic<int> stop{0};
      std::atomic<int> close{0};
      std::atomic<int> setProperty{0};
    };

    class FakeBlockingStopBackend final : public Backend
    {
    public:
      explicit FakeBlockingStopBackend(
        std::shared_ptr<BackendLifecycleCounts> lifecycleCountsPtr = std::make_shared<BackendLifecycleCounts>(),
        std::function<void()> beforeStopReturns = {})
        : _lifecycleCountsPtr{std::move(lifecycleCountsPtr)}, _beforeStopReturns{std::move(beforeStopReturns)}
      {
      }

      BackendOpenResult open(SignalFormat const& sourceFormat, RenderTarget& target) override
      {
        auto const lock = std::scoped_lock{_mutex};
        _format = pcmFormat(sourceFormat, SampleEncoding::Signed16Le);
        _target = &target;
        return OpenedPcmMode{.clientFormat = _format};
      }

      void start() override {}
      void pause() override {}
      void resume() override {}
      void flush() override {}

      void stop() override
      {
        {
          auto lock = std::unique_lock{_mutex};
          _lifecycleCountsPtr->stop.fetch_add(1, std::memory_order_relaxed);

          if (_blockStop)
          {
            _stopEntered = true;
            _cv.notify_all();
            _cv.wait(lock, [this] { return _releaseStop; });
          }
        }

        if (_beforeStopReturns)
        {
          _beforeStopReturns();
        }
      }

      void close() override
      {
        auto const lock = std::scoped_lock{_mutex};
        _lifecycleCountsPtr->close.fetch_add(1, std::memory_order_relaxed);
        _target = nullptr;
      }

      BackendId backendId() const override { return BackendId{"blocking-stop"}; }
      ProfileId profileId() const override { return ProfileId{"test"}; }

      Result<> setProperty(PropertyId /*id*/, PropertyValue const& /*value*/) override
      {
        auto const lock = std::scoped_lock{_mutex};
        _lifecycleCountsPtr->setProperty.fetch_add(1, std::memory_order_relaxed);
        return {};
      }

      Result<PropertyValue> property(PropertyId id) const override
      {
        if (id == PropertyId::Volume)
        {
          return 1.0F;
        }

        if (id == PropertyId::Muted)
        {
          return false;
        }

        return std::unexpected(Error{.code = Error::Code::NotSupported});
      }

      PropertyInfo queryProperty(PropertyId /*id*/) const noexcept override
      {
        return {.canRead = true, .canWrite = true, .isAvailable = true, .emitsChangeNotifications = false};
      }

      void blockStop()
      {
        auto const lock = std::scoped_lock{_mutex};
        _blockStop = true;
      }

      bool tryWaitForStopEntered(std::chrono::milliseconds timeout)
      {
        auto lock = std::unique_lock{_mutex};
        return _cv.wait_for(lock, timeout, [this] { return _stopEntered; });
      }

      void releaseStop()
      {
        auto const lock = std::scoped_lock{_mutex};
        _releaseStop = true;
        _cv.notify_all();
      }

      RenderTarget* target() const
      {
        auto const lock = std::scoped_lock{_mutex};
        return _target;
      }

      // The caller keeps stop() blocked until all admitted renders return.
      std::optional<RenderPcmResult> tryRenderBeforeBlockedStop(std::span<std::byte> output)
      {
        auto* target = static_cast<RenderTarget*>(nullptr);
        {
          auto const lock = std::scoped_lock{_mutex};

          if (!_blockStop || _stopEntered || _releaseStop || _target == nullptr)
          {
            return std::nullopt;
          }

          target = _target;
        }

        return target->renderPcm(output);
      }

      void emitRouteReady(std::string_view routeAnchor)
      {
        auto* target = static_cast<RenderTarget*>(nullptr);
        {
          auto const lock = std::scoped_lock{_mutex};
          target = _target;
        }

        if (target != nullptr)
        {
          target->handleRouteReady(routeAnchor);
        }
      }

    private:
      mutable std::mutex _mutex;
      std::condition_variable _cv;
      std::shared_ptr<BackendLifecycleCounts> _lifecycleCountsPtr;
      std::function<void()> _beforeStopReturns;
      RenderTarget* _target = nullptr;
      PcmFormat _format{};
      bool _blockStop = false;
      bool _stopEntered = false;
      bool _releaseStop = false;
    };

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
  } // namespace

  TEST_CASE("Engine - backend error callback transitions to error")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto stateChanged = CallbackLatch{};
    auto engine = Engine{std::move(backendPtr), device, makeScriptedEngineDecoderFactory()};

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "song.flac"}));

    auto* const target = backendRaw->target();
    REQUIRE(target != nullptr);
    engine.setOnStateChanged([&] { stateChanged.notify(); });

    target->handleBackendError("Hardware failure");

    CHECK(stateChanged.tryWaitForCount(1));
    auto const snap = engine.status();

    CHECK(snap.transport == Transport::Error);
    CHECK(snap.statusText == "Hardware failure");
  }

  TEST_CASE("Engine - route ready callback updates route snapshot", "[concurrency]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto callbackThreadPromise = std::promise<std::thread::id>{};
    auto callbackThread = callbackThreadPromise.get_future();
    auto const callerThread = std::this_thread::get_id();
    auto engine = Engine{std::move(backendPtr), device, makeScriptedEngineDecoderFactory()};

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "song.flac"}));

    auto* const target = backendRaw->target();
    REQUIRE(target != nullptr);

    engine.setOnStateChanged([&callbackThreadPromise] { callbackThreadPromise.set_value(std::this_thread::get_id()); });

    target->handleRouteReady("anchor-123");

    REQUIRE(callbackThread.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
    CHECK(callbackThread.get() != callerThread);
    auto const route = engine.routeStatus();

    REQUIRE(route.optAnchor);
    CHECK(route.optAnchor->id == "anchor-123");
  }

  TEST_CASE("Engine - format callback publishes the complete output format")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backendRaw = backendPtr.get();

    auto stateChanged = CallbackLatch{};
    auto engine = Engine{std::move(backendPtr), device, makeScriptedEngineDecoderFactory()};

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "song.flac"}));
    auto* const target = backendRaw->target();
    engine.setOnStateChanged([&] { stateChanged.notify(); });

    REQUIRE(target != nullptr);
    auto const changedFormat =
      PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed24PackedLe};
    target->handleFormatChanged(changedFormat);

    REQUIRE(stateChanged.tryWaitForCount(1));
    CHECK(engine.status().routeState.engineOutputFormat == changedFormat);
  }

  TEST_CASE("Engine - stop drops retired render session target", "[concurrency]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto engine = Engine{std::move(backendPtr), device, makeScriptedEngineDecoderFactory()};

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "song.flac"}));
    auto* const target = backendRaw->target();
    CHECK(target != nullptr);

    engine.stop();

    auto const snap = engine.status();
    CHECK(snap.transport == Transport::Idle);
    CHECK(snap.statusText.empty());
    CHECK(snap.underrunCount == 0);
    CHECK_FALSE(engine.routeStatus().optAnchor);
    CHECK(backendRaw->target() == nullptr);
  }

  TEST_CASE("Engine - user callbacks run outside backend callback stack and may reenter", "[concurrency]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto callbackThreadPromise = std::promise<std::thread::id>{};
    auto callbackThread = callbackThreadPromise.get_future();
    auto const backendCallbackThread = std::this_thread::get_id();
    auto engine = Engine{std::move(backendPtr), device, makeScriptedEngineDecoderFactory()};

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "song.flac"}));
    REQUIRE(backendRaw->target() != nullptr);

    engine.setOnRouteChanged(
      [&](auto const&)
      {
        auto const callbackThreadId = std::this_thread::get_id();
        engine.stop();
        callbackThreadPromise.set_value(callbackThreadId);
      });

    backendRaw->emitRouteReady("reentrant-anchor");

    REQUIRE(callbackThread.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
    CHECK(callbackThread.get() != backendCallbackThread);
    CHECK(engine.status().transport == Transport::Idle);
  }

  TEST_CASE("Engine - event callback defers engine teardown", "[concurrency]")
  {
    struct CallbackLifetime final
    {
      explicit CallbackLifetime(CallbackLatch& destroyedRef)
        : destroyed{destroyedRef}
      {
      }

      ~CallbackLifetime() { destroyed.notify(); }

      CallbackLifetime(CallbackLifetime const&) = delete;
      CallbackLifetime& operator=(CallbackLifetime const&) = delete;
      CallbackLifetime(CallbackLifetime&&) = delete;
      CallbackLifetime& operator=(CallbackLifetime&&) = delete;

      CallbackLatch& destroyed;
    };

    auto const device = makeEngineTestDevice();
    auto callbackStorageDestroyed = CallbackLatch{};
    auto callbackLifetimePtr = std::make_shared<CallbackLifetime>(callbackStorageDestroyed);
    auto backendPtr = std::make_unique<FakeBlockingStopBackend>();
    auto* const backendRaw = backendPtr.get();
    auto routeChanged = std::atomic<bool>{false};
    auto routeDelivered = CallbackLatch{};
    auto teardownRequested = std::atomic<bool>{false};
    auto enginePtr = std::make_unique<Engine>(std::move(backendPtr), device, makeScriptedEngineDecoderFactory());

    enginePtr->play(makePlaybackItem(PlaybackInput{.filePath = "song.flac"}));
    auto* const target = backendRaw->target();
    REQUIRE(target != nullptr);
    enginePtr->setOnStateChanged(
      [&teardownRequested, callbackLifetimePtr]
      {
        if (!callbackLifetimePtr)
        {
          return;
        }

        teardownRequested.store(true, std::memory_order_release);
      });
    enginePtr->setOnRouteChanged(
      [&routeChanged, &routeDelivered](Engine::RouteStatus const&)
      {
        routeChanged.store(true, std::memory_order_release);
        routeDelivered.notify();
      });
    callbackLifetimePtr.reset();

    target->handleRouteReady("destroy-anchor");

    REQUIRE(routeDelivered.tryWaitForCount(1));
    CHECK(teardownRequested.load(std::memory_order_acquire));
    REQUIRE(enginePtr);
    enginePtr.reset();
    REQUIRE(callbackStorageDestroyed.tryWaitForCount(1));
    CHECK(enginePtr == nullptr);
    CHECK(routeChanged.load(std::memory_order_acquire));
  }

  TEST_CASE("Engine - shutdown serializes controls and is idempotent", "[concurrency]")
  {
    auto const device = makeEngineTestDevice();
    auto lifecycleCountsPtr = std::make_shared<BackendLifecycleCounts>();
    auto backendPtr = std::make_unique<FakeBlockingStopBackend>(lifecycleCountsPtr);
    auto* const backendRaw = backendPtr.get();
    auto engine = Engine{std::move(backendPtr), device, makeScriptedEngineDecoderFactory()};

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "song.flac"}));
    REQUIRE(backendRaw->target() != nullptr);
    auto shutdownFuture = std::future<void>{};
    auto commandStarted = std::promise<void>{};
    auto commandStartedFuture = commandStarted.get_future();
    auto commandFuture = std::future<Result<>>{};
    auto releaseStop = utility::ScopedRegistration{[backendRaw] { backendRaw->releaseStop(); }};
    backendRaw->blockStop();
    shutdownFuture = std::async(std::launch::async, [&engine] { engine.shutdown(); });
    REQUIRE(backendRaw->tryWaitForStopEntered(std::chrono::seconds{1}));

    commandFuture = std::async(std::launch::async,
                               [&engine, &commandStarted]
                               {
                                 commandStarted.set_value();
                                 return engine.setVolume(0.5F);
                               });
    auto const commandWasStarted = commandStartedFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready;

    releaseStop.reset();
    REQUIRE(commandWasStarted);
    REQUIRE(shutdownFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
    REQUIRE(commandFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready);

    auto const commandRes = commandFuture.get();
    REQUIRE_FALSE(commandRes);
    CHECK(commandRes.error().code == Error::Code::InvalidState);

    engine.shutdown();
    CHECK(lifecycleCountsPtr->stop.load(std::memory_order_relaxed) == 1);
    CHECK(lifecycleCountsPtr->close.load(std::memory_order_relaxed) == 1);
    CHECK(lifecycleCountsPtr->setProperty.load(std::memory_order_relaxed) == 0);
  }

  TEST_CASE("Engine - shutdown retains both gapless decoders until backend stop", "[concurrency]")
  {
    constexpr std::size_t kTrackBytes = 100;
    auto const lifecycleCountsPtr = std::make_shared<BackendLifecycleCounts>();
    auto const decoderCountsPtr = std::make_shared<DecoderLifeCounters>();
    std::size_t liveAtStop = 0;
    bool closedAtStop = true;
    auto backendPtr = std::make_unique<FakeBlockingStopBackend>(
      lifecycleCountsPtr,
      [&]
      {
        liveAtStop = decoderCountsPtr->live();
        closedAtStop = lifecycleCountsPtr->close.load(std::memory_order_relaxed) != 0;
      });
    auto* const backendRaw = backendPtr.get();
    auto const format = makeEngineTestFormat();
    auto const info = makeScriptedStreamInfo(format);
    auto const data = std::vector<std::byte>(kTrackBytes, std::byte{0});
    auto workerEntered = std::binary_semaphore{0};
    auto workerRelease = std::binary_semaphore{0};
    auto engine = Engine{std::move(backendPtr),
                         makeEngineTestDevice(),
                         makeCountingDecoderFactory(
                           {
                             {.path = "current.flac", .info = info, .data = data},
                             {.path = "next.flac", .info = info, .data = data},
                           },
                           decoderCountsPtr)};

    engine.play(makePlaybackItem("current.flac"));
    auto const preparedRes = engine.setNext(makePlaybackItem("next.flac"));
    REQUIRE(preparedRes);
    REQUIRE(preparedRes->transition == Engine::PreparedTransitionMode::Gapless);
    REQUIRE(decoderCountsPtr->live() == 2);

    SECTION("armed successor")
    {
      engine.shutdown();
    }

    SECTION("claimed successor with a splice queued")
    {
      constexpr auto kTimeout = std::chrono::seconds{5};
      // Unwind both gates before the future or Engine can join the event worker.
      auto shutdownFuture = std::future<void>{};
      auto releaseWorker = SemaphoreReleaseGuard{workerRelease};
      auto releaseStop = utility::ScopedRegistration{[backendRaw] { backendRaw->releaseStop(); }};
      backendRaw->blockStop();
      engine.defer(
        [&]
        {
          workerEntered.release();
          workerRelease.acquire();
        });
      REQUIRE(workerEntered.try_acquire_for(kTimeout));

      auto output = std::array<std::byte, 2 * kTrackBytes>{};
      auto const optCrossing = backendRaw->tryRenderBeforeBlockedStop(output);
      REQUIRE(optCrossing);
      REQUIRE_FALSE(optCrossing->drained);
      REQUIRE(optCrossing->bytesWritten == output.size());
      REQUIRE(optCrossing->positionFrameOffset == kTrackBytes / frameBytes(format));

      shutdownFuture = std::async(std::launch::async, [&engine] { engine.shutdown(); });
      // An empty render reports drained only once shutdown retires the target.
      auto const deadline = std::chrono::steady_clock::now() + kTimeout;
      auto optProbe = backendRaw->tryRenderBeforeBlockedStop({});

      while (optProbe && !optProbe->drained && std::chrono::steady_clock::now() < deadline)
      {
        std::this_thread::yield();
        optProbe = backendRaw->tryRenderBeforeBlockedStop({});
      }

      REQUIRE(optProbe);
      REQUIRE(optProbe->drained);

      releaseStop.reset();
      releaseWorker.release();
      REQUIRE(shutdownFuture.wait_for(kTimeout) == std::future_status::ready);
      shutdownFuture.get();
    }

    CHECK(liveAtStop == 2);
    CHECK_FALSE(closedAtStop);
    CHECK(lifecycleCountsPtr->stop.load(std::memory_order_relaxed) == 1);
    CHECK(lifecycleCountsPtr->close.load(std::memory_order_relaxed) == 1);
    CHECK(decoderCountsPtr->live() == 0);
  }

  TEST_CASE("Engine - queued render event from retired session is ignored", "[concurrency]")
  {
    auto const device = makeEngineTestDevice();
    auto blockingBackendPtr = std::make_unique<FakeBlockingStopBackend>();
    auto* const blockingBackendRaw = blockingBackendPtr.get();
    auto routeChanged = std::atomic<bool>{false};
    auto settled = CallbackLatch{};
    auto blockingEngine = Engine{std::move(blockingBackendPtr), device, makeScriptedEngineDecoderFactory()};
    blockingEngine.setOnRouteChanged([&](Engine::RouteStatus const&)
                                     { routeChanged.store(true, std::memory_order_release); });

    blockingEngine.play(makePlaybackItem(PlaybackInput{.filePath = "song.flac"}));
    CHECK(blockingEngine.status().transport == Transport::Playing);

    auto stopFuture = std::future<void>{};
    auto releaseStop = utility::ScopedRegistration{[blockingBackendRaw] { blockingBackendRaw->releaseStop(); }};
    blockingBackendRaw->blockStop();
    stopFuture = std::async(std::launch::async, [&] { blockingEngine.stop(); });
    REQUIRE(blockingBackendRaw->tryWaitForStopEntered(std::chrono::seconds{1}));

    blockingBackendRaw->emitRouteReady("stale-anchor");
    CHECK_FALSE(routeChanged.load(std::memory_order_acquire));

    releaseStop.reset();
    REQUIRE(stopFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
    stopFuture.get();
    blockingEngine.defer([&] { settled.notify(); });
    REQUIRE(settled.tryWaitForCount(1));

    CHECK_FALSE(routeChanged.load(std::memory_order_acquire));
    CHECK_FALSE(blockingEngine.routeStatus().optAnchor);
    CHECK(blockingEngine.status().transport == Transport::Idle);
  }

  TEST_CASE("Engine - play and stop barriers suppress a materialized old-generation route", "[concurrency]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto stateEntered = std::binary_semaphore{0};
    auto stateRelease = std::binary_semaphore{0};
    auto workerFlushed = std::binary_semaphore{0};
    auto routeCount = std::atomic<std::size_t>{0};
    auto engine = Engine{std::move(backendPtr), device, makeScriptedEngineDecoderFactory()};
    auto releaseGuard = SemaphoreReleaseGuard{stateRelease};

    engine.setOnStateChanged(
      [&]
      {
        stateEntered.release();
        stateRelease.acquire();
      });
    engine.setOnRouteChanged([&](Engine::RouteStatus const&) { routeCount.fetch_add(1, std::memory_order_relaxed); });
    engine.play(makePlaybackItem(PlaybackInput{.filePath = "current.flac"}));
    auto const oldGeneration = engine.playbackGeneration();

    backendRaw->emitRouteReady("old-anchor");
    REQUIRE(stateEntered.try_acquire_for(std::chrono::seconds{5}));

    auto barrier = Engine::PreparedCancellationBarrier{};

    SECTION("successful explicit play")
    {
      auto preparedStartRes = engine.stagePlayback(makePlaybackItem(PlaybackInput{.filePath = "replacement.flac"}));
      REQUIRE(preparedStartRes);
      auto receiptRes = engine.commitPlayback(std::move(*preparedStartRes));
      REQUIRE(receiptRes);
      barrier = receiptRes->cancellationBarrier;
    }

    SECTION("completed stop")
    {
      barrier = engine.stopWithBarrier();
    }

    REQUIRE(barrier.covers(oldGeneration));
    releaseGuard.release();
    engine.defer([&] { workerFlushed.release(); });
    REQUIRE(workerFlushed.try_acquire_for(std::chrono::seconds{5}));
    CHECK(routeCount.load(std::memory_order_relaxed) == 0);
  }

  TEST_CASE("Engine - play and stop barriers suppress a materialized old-generation track end", "[concurrency]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto stateEntered = std::binary_semaphore{0};
    auto stateRelease = std::binary_semaphore{0};
    auto workerFlushed = std::binary_semaphore{0};
    auto endedCount = std::atomic<std::size_t>{0};
    auto engine = Engine{std::move(backendPtr), device, makeScriptedEngineDecoderFactory()};
    auto releaseGuard = SemaphoreReleaseGuard{stateRelease};

    engine.setOnStateChanged(
      [&]
      {
        stateEntered.release();
        stateRelease.acquire();
      });
    engine.setOnTrackEnded([&](Engine::TrackEnded const&) { endedCount.fetch_add(1, std::memory_order_relaxed); });
    engine.play(makePlaybackItem(PlaybackInput{.filePath = "current.flac"}));
    auto const oldGeneration = engine.playbackGeneration();
    auto* const target = backendRaw->target();
    REQUIRE(target != nullptr);
    auto output = std::array<std::byte, 200>{};
    std::ignore = target->renderPcm(output);
    REQUIRE(target->renderPcm(output).drained);

    backendRaw->emitDrainComplete();
    REQUIRE(stateEntered.try_acquire_for(std::chrono::seconds{5}));

    auto barrier = Engine::PreparedCancellationBarrier{};

    SECTION("successful explicit play")
    {
      auto preparedStartRes = engine.stagePlayback(makePlaybackItem(PlaybackInput{.filePath = "replacement.flac"}));
      REQUIRE(preparedStartRes);
      auto receiptRes = engine.commitPlayback(std::move(*preparedStartRes));
      REQUIRE(receiptRes);
      barrier = receiptRes->cancellationBarrier;
    }

    SECTION("completed stop")
    {
      barrier = engine.stopWithBarrier();
    }

    REQUIRE(barrier.covers(oldGeneration));
    releaseGuard.release();
    engine.defer([&] { workerFlushed.release(); });
    REQUIRE(workerFlushed.try_acquire_for(std::chrono::seconds{5}));
    CHECK(endedCount.load(std::memory_order_relaxed) == 0);
  }

  TEST_CASE("Engine - play and stop barriers suppress a materialized old-generation failure", "[concurrency]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto workerEntered = std::binary_semaphore{0};
    auto workerRelease = std::binary_semaphore{0};
    auto workerFlushed = std::binary_semaphore{0};
    auto failureCount = std::atomic<std::size_t>{0};
    auto engine = Engine{std::move(backendPtr), device, makeScriptedEngineDecoderFactory()};
    auto releaseGuard = SemaphoreReleaseGuard{workerRelease};

    engine.setOnPlaybackFailure([&](Engine::PlaybackFailure const&)
                                { failureCount.fetch_add(1, std::memory_order_relaxed); });
    engine.play(makePlaybackItem(PlaybackInput{.filePath = "current.flac"}));

    engine.defer(
      [&]
      {
        workerEntered.release();
        workerRelease.acquire();
      });
    REQUIRE(workerEntered.try_acquire_for(std::chrono::seconds{5}));

    backendRaw->setOpenResult(
      std::unexpected{Error{.code = Error::Code::IoError, .message = "queued old route failure"}});
    engine.play(makePlaybackItem(PlaybackInput{.filePath = "old-failure.flac"}));
    auto const failedGeneration = engine.playbackGeneration();
    backendRaw->setOpenResult({});

    auto barrier = Engine::PreparedCancellationBarrier{};

    SECTION("successful explicit play")
    {
      auto preparedStartRes = engine.stagePlayback(makePlaybackItem(PlaybackInput{.filePath = "replacement.flac"}));
      REQUIRE(preparedStartRes);
      auto receiptRes = engine.commitPlayback(std::move(*preparedStartRes));
      REQUIRE(receiptRes);
      barrier = receiptRes->cancellationBarrier;
      CHECK(receiptRes->generation == engine.playbackGeneration());
    }

    SECTION("completed stop")
    {
      barrier = engine.stopWithBarrier();
      CHECK(engine.transport() == Transport::Idle);
    }

    REQUIRE(barrier.covers(failedGeneration));
    releaseGuard.release();
    engine.defer([&] { workerFlushed.release(); });
    REQUIRE(workerFlushed.try_acquire_for(std::chrono::seconds{5}));
    CHECK(failureCount.load(std::memory_order_relaxed) == 0);
  }

  TEST_CASE("Engine - backend error permits a new playback route")
  {
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto stateChanged = CallbackLatch{};
    auto routeChanged = CallbackLatch{};
    auto engine = Engine{std::move(backendPtr), makeEngineTestDevice(), makeScriptedEngineDecoderFactory()};
    auto const item = makePlaybackItem("test.flac");
    engine.play(item);
    REQUIRE(backendRaw->target() != nullptr);
    engine.setOnStateChanged([&] { stateChanged.notify(); });

    backendRaw->emitBackendError("hardware failed");
    REQUIRE(stateChanged.tryWaitForCount(1));
    REQUIRE(engine.status().transport == Transport::Error);

    engine.play(item);
    REQUIRE(engine.status().transport == Transport::Playing);
    REQUIRE(backendRaw->target() != nullptr);
    engine.setOnRouteChanged([&](auto const&) { routeChanged.notify(); });
    backendRaw->emitRouteReady("test-anchor");
    REQUIRE(routeChanged.tryWaitForCount(1));
    auto const route = engine.routeStatus();
    REQUIRE(route.optAnchor);
    CHECK(route.optAnchor->id == "test-anchor");
  }
} // namespace ao::audio::test
