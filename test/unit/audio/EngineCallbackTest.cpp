// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "EngineTestSupport.h"
#include "FakeCapturingBackend.h"
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Engine.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/Property.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/Transport.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <expected>
#include <future>
#include <memory>
#include <mutex>
#include <semaphore>
#include <string_view>
#include <thread>
#include <utility>

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
        std::shared_ptr<BackendLifecycleCounts> lifecycleCountsPtr = std::make_shared<BackendLifecycleCounts>())
        : _lifecycleCountsPtr{std::move(lifecycleCountsPtr)}
      {
      }

      Result<> open(Format const& format, RenderTarget* target) override
      {
        auto const lock = std::scoped_lock{_mutex};
        _format = format;
        _target = target;
        return {};
      }

      void start() override {}
      void pause() override {}
      void resume() override {}
      void flush() override {}

      void stop() override
      {
        auto lock = std::unique_lock{_mutex};
        _lifecycleCountsPtr->stop.fetch_add(1, std::memory_order_relaxed);

        if (!_blockStop)
        {
          return;
        }

        _stopEntered = true;
        _cv.notify_all();
        _cv.wait(lock, [this] { return _releaseStop; });
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

      bool waitForStopEntered(std::chrono::milliseconds timeout)
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
      RenderTarget* _target = nullptr;
      Format _format{};
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

  TEST_CASE("Engine - backend error callback transitions to error", "[audio][unit][engine][callback]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto engine = Engine{std::move(backendPtr), device, makeScriptedEngineDecoderFactory()};

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "song.flac"}));

    auto* const target = backendRaw->target();
    auto stateChanged = CallbackLatch{};
    engine.setOnStateChanged([&] { stateChanged.notify(); });

    target->handleBackendError("Hardware failure");

    CHECK(stateChanged.waitForCount(1));
    auto const snap = engine.status();

    CHECK(snap.transport == Transport::Error);
    CHECK(snap.statusText == "Hardware failure");
  }

  TEST_CASE("Engine - route ready callback updates route snapshot", "[audio][unit][engine][callback]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto engine = Engine{std::move(backendPtr), device, makeScriptedEngineDecoderFactory()};

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "song.flac"}));

    auto* const target = backendRaw->target();
    auto callbackThreadPromise = std::promise<std::thread::id>{};
    auto callbackThread = callbackThreadPromise.get_future();
    auto const callerThread = std::this_thread::get_id();

    engine.setOnStateChanged([&callbackThreadPromise] { callbackThreadPromise.set_value(std::this_thread::get_id()); });

    target->handleRouteReady("anchor-123");

    REQUIRE(callbackThread.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
    CHECK(callbackThread.get() != callerThread);
    auto const route = engine.routeStatus();

    REQUIRE(route.optAnchor);
    CHECK(route.optAnchor->id == "anchor-123");
  }

  TEST_CASE("Engine - playback status callbacks update engine internals", "[audio][unit][engine][callback]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backendRaw = backendPtr.get();

    // The latch must outlive the engine: the event thread keeps invoking
    // callbacks until the engine is destroyed (which joins it), and this test
    // fires several state-changing events while only waiting for the first
    // notification, so later notifications are still in flight at scope exit.
    auto stateChanged = CallbackLatch{};
    auto engine = Engine{std::move(backendPtr), device, makeScriptedEngineDecoderFactory()};

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "song.flac"}));
    auto* const target = backendRaw->target();
    engine.setOnStateChanged([&] { stateChanged.notify(); });

    target->handleUnderrun();
    target->handlePositionAdvanced(100);
    target->handleFormatChanged(Format{.sampleRate = 48000, .channels = 2, .bitDepth = 24, .isInterleaved = true});
    target->handleFormatChanged(Format{.sampleRate = 48000, .channels = 2, .bitDepth = 24, .isInterleaved = true});
    backendRaw->emitPropertyChanged(PropertyId::Volume);

    CHECK(stateChanged.waitForCount(1));
    CHECK(engine.status().routeState.engineOutputFormat.sampleRate == 48000);
  }

  TEST_CASE("Engine - stop drops retired render session target", "[audio][unit][engine][callback]")
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

  TEST_CASE("Engine - user callbacks run outside backend callback stack and may reenter",
            "[audio][unit][engine][callback]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto engine = Engine{std::move(backendPtr), device, makeScriptedEngineDecoderFactory()};

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "song.flac"}));

    auto callbackThreadPromise = std::promise<std::thread::id>{};
    auto callbackThread = callbackThreadPromise.get_future();
    auto const backendCallbackThread = std::this_thread::get_id();

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

  TEST_CASE("Engine - event callback defers engine teardown", "[audio][regression][engine][concurrency]")
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
    auto enginePtr = std::make_unique<Engine>(std::move(backendPtr), device, makeScriptedEngineDecoderFactory());

    enginePtr->play(makePlaybackItem(PlaybackInput{.filePath = "song.flac"}));
    auto* const target = backendRaw->target();
    REQUIRE(target != nullptr);

    auto routeChanged = std::atomic{false};
    auto routeDelivered = CallbackLatch{};
    auto teardownRequested = std::atomic{false};
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

    REQUIRE(routeDelivered.waitForCount(1));
    CHECK(teardownRequested.load(std::memory_order_acquire));
    REQUIRE(enginePtr);
    enginePtr.reset();
    REQUIRE(callbackStorageDestroyed.waitForCount(1));
    CHECK(enginePtr == nullptr);
    CHECK(routeChanged.load(std::memory_order_acquire));
  }

  TEST_CASE("Engine - shutdown serializes controls and is idempotent", "[audio][unit][engine][concurrency]")
  {
    auto const device = makeEngineTestDevice();
    auto lifecycleCountsPtr = std::make_shared<BackendLifecycleCounts>();
    auto backendPtr = std::make_unique<FakeBlockingStopBackend>(lifecycleCountsPtr);
    auto* const backendRaw = backendPtr.get();
    auto engine = Engine{std::move(backendPtr), device, makeScriptedEngineDecoderFactory()};

    backendRaw->blockStop();
    auto shutdownFuture = std::async(std::launch::async, [&engine] { engine.shutdown(); });
    auto const stopWasEntered = backendRaw->waitForStopEntered(std::chrono::seconds{1});

    if (!stopWasEntered)
    {
      backendRaw->releaseStop();
    }

    REQUIRE(stopWasEntered);

    auto commandStarted = std::promise<void>{};
    auto commandStartedFuture = commandStarted.get_future();
    auto commandFuture = std::async(std::launch::async,
                                    [&engine, &commandStarted]
                                    {
                                      commandStarted.set_value();
                                      return engine.setVolume(0.5F);
                                    });
    auto const commandWasStarted = commandStartedFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready;

    backendRaw->releaseStop();
    REQUIRE(commandWasStarted);
    REQUIRE(shutdownFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
    REQUIRE(commandFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready);

    auto const commandResult = commandFuture.get();
    REQUIRE_FALSE(commandResult);
    CHECK(commandResult.error().code == Error::Code::InvalidState);

    engine.shutdown();
    CHECK(lifecycleCountsPtr->stop.load(std::memory_order_relaxed) == 1);
    CHECK(lifecycleCountsPtr->close.load(std::memory_order_relaxed) == 1);
    CHECK(lifecycleCountsPtr->setProperty.load(std::memory_order_relaxed) == 0);
  }

  TEST_CASE("Engine - queued render event from retired session is ignored", "[audio][unit][engine][callback]")
  {
    auto const device = makeEngineTestDevice();
    auto blockingBackendPtr = std::make_unique<FakeBlockingStopBackend>();
    auto* const blockingBackendRaw = blockingBackendPtr.get();
    auto blockingEngine = Engine{std::move(blockingBackendPtr), device, makeScriptedEngineDecoderFactory()};

    auto routeChanged = std::atomic{false};
    blockingEngine.setOnRouteChanged([&](Engine::RouteStatus const&)
                                     { routeChanged.store(true, std::memory_order_release); });

    blockingEngine.play(makePlaybackItem(PlaybackInput{.filePath = "song.flac"}));
    CHECK(blockingEngine.status().transport == Transport::Playing);

    blockingBackendRaw->blockStop();
    auto stopFuture = std::async(std::launch::async, [&] { blockingEngine.stop(); });
    CHECK(blockingBackendRaw->waitForStopEntered(std::chrono::seconds{1}));

    blockingBackendRaw->emitRouteReady("stale-anchor");
    CHECK_FALSE(routeChanged.load(std::memory_order_acquire));

    blockingBackendRaw->releaseStop();
    CHECK(stopFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready);

    CHECK_FALSE(routeChanged.load(std::memory_order_acquire));
    CHECK_FALSE(blockingEngine.routeStatus().optAnchor);
    CHECK(blockingEngine.status().transport == Transport::Idle);
  }

  TEST_CASE("Engine - play and stop barriers suppress a materialized old-generation route",
            "[audio][unit][engine][barrier]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto stateEntered = std::binary_semaphore{0};
    auto stateRelease = std::binary_semaphore{0};
    auto workerFlushed = std::binary_semaphore{0};
    auto engine = Engine{std::move(backendPtr), device, makeScriptedEngineDecoderFactory()};
    auto releaseGuard = SemaphoreReleaseGuard{stateRelease};
    auto routeCount = std::atomic{std::size_t{0}};

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
      auto preparedStart = engine.stagePlayback(makePlaybackItem(PlaybackInput{.filePath = "replacement.flac"}));
      REQUIRE(preparedStart);
      auto receipt = engine.commitPlayback(std::move(*preparedStart));
      REQUIRE(receipt);
      barrier = receipt->cancellationBarrier;
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

  TEST_CASE("Engine - play and stop barriers suppress a materialized old-generation track end",
            "[audio][unit][engine][barrier]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto stateEntered = std::binary_semaphore{0};
    auto stateRelease = std::binary_semaphore{0};
    auto workerFlushed = std::binary_semaphore{0};
    auto engine = Engine{std::move(backendPtr), device, makeScriptedEngineDecoderFactory()};
    auto releaseGuard = SemaphoreReleaseGuard{stateRelease};
    auto endedCount = std::atomic{std::size_t{0}};

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
      auto preparedStart = engine.stagePlayback(makePlaybackItem(PlaybackInput{.filePath = "replacement.flac"}));
      REQUIRE(preparedStart);
      auto receipt = engine.commitPlayback(std::move(*preparedStart));
      REQUIRE(receipt);
      barrier = receipt->cancellationBarrier;
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

  TEST_CASE("Engine - play and stop barriers suppress a materialized old-generation failure",
            "[audio][unit][engine][barrier]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto workerEntered = std::binary_semaphore{0};
    auto workerRelease = std::binary_semaphore{0};
    auto workerFlushed = std::binary_semaphore{0};
    auto engine = Engine{std::move(backendPtr), device, makeScriptedEngineDecoderFactory()};
    auto releaseGuard = SemaphoreReleaseGuard{workerRelease};
    auto failureCount = std::atomic{std::size_t{0}};

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
      auto preparedStart = engine.stagePlayback(makePlaybackItem(PlaybackInput{.filePath = "replacement.flac"}));
      REQUIRE(preparedStart);
      auto receipt = engine.commitPlayback(std::move(*preparedStart));
      REQUIRE(receipt);
      barrier = receipt->cancellationBarrier;
      CHECK(receipt->generation == engine.playbackGeneration());
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
} // namespace ao::audio::test
