// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CapturingBackend.h"
#include "EngineTestSupport.h"
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Engine.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/Property.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/Transport.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <expected>
#include <future>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>

namespace ao::audio::test
{
  namespace
  {
    class BlockingStopBackend final : public Backend
    {
    public:
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
        _target = nullptr;
      }

      BackendId backendId() const noexcept override { return BackendId{"blocking-stop"}; }
      ProfileId profileId() const noexcept override { return ProfileId{"test"}; }

      Result<> setProperty(PropertyId /*id*/, PropertyValue const& /*value*/) override { return {}; }

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

      void emitRouteReady(std::string_view routeAnchor)
      {
        auto* target = static_cast<RenderTarget*>(nullptr);
        {
          auto const lock = std::scoped_lock{_mutex};
          target = _target;
        }

        if (target != nullptr)
        {
          target->onRouteReady(routeAnchor);
        }
      }

    private:
      mutable std::mutex _mutex;
      std::condition_variable _cv;
      RenderTarget* _target = nullptr;
      Format _format{};
      bool _blockStop = false;
      bool _stopEntered = false;
      bool _releaseStop = false;
    };
  } // namespace

  TEST_CASE("Engine - backend error callback transitions to error", "[audio][unit][engine][callback]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<CapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto engine = Engine{std::move(backendPtr), device, makeScriptedEngineDecoderFactory()};

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "song.flac"}));

    auto* const target = backendRaw->target();
    auto stateChanged = CallbackLatch{};
    engine.setOnStateChanged([&] { stateChanged.notify(); });

    target->onBackendError("Hardware failure");

    CHECK(stateChanged.waitForCount(1));
    auto const snap = engine.status();

    CHECK(snap.transport == Transport::Error);
    CHECK(snap.statusText == "Hardware failure");
  }

  TEST_CASE("Engine - route ready callback updates route snapshot", "[audio][unit][engine][callback]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<CapturingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto engine = Engine{std::move(backendPtr), device, makeScriptedEngineDecoderFactory()};

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "song.flac"}));

    auto* const target = backendRaw->target();
    auto callbackThreadPromise = std::promise<std::thread::id>{};
    auto callbackThread = callbackThreadPromise.get_future();
    auto const callerThread = std::this_thread::get_id();

    engine.setOnStateChanged([&callbackThreadPromise] { callbackThreadPromise.set_value(std::this_thread::get_id()); });

    target->onRouteReady("anchor-123");

    REQUIRE(callbackThread.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
    CHECK(callbackThread.get() != callerThread);
    auto const route = engine.routeStatus();

    REQUIRE(route.optAnchor);
    CHECK(route.optAnchor->id == "anchor-123");
  }

  TEST_CASE("Engine - playback status callbacks update engine internals", "[audio][unit][engine][callback]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<CapturingBackend>();
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

    target->onUnderrun();
    target->onPositionAdvanced(100);
    target->onFormatChanged(Format{.sampleRate = 48000, .channels = 2, .bitDepth = 24, .isInterleaved = true});
    target->onFormatChanged(Format{.sampleRate = 48000, .channels = 2, .bitDepth = 24, .isInterleaved = true});
    target->onPropertyChanged(PropertyId::Volume);

    CHECK(stateChanged.waitForCount(1));
    CHECK(engine.status().routeState.engineOutputFormat.sampleRate == 48000);
  }

  TEST_CASE("Engine - stop drops retired render session target", "[audio][unit][engine][callback]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<CapturingBackend>();
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
    auto backendPtr = std::make_unique<CapturingBackend>();
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

  TEST_CASE("Engine - queued render event from retired session is ignored", "[audio][unit][engine][callback]")
  {
    auto const device = makeEngineTestDevice();
    auto blockingBackendPtr = std::make_unique<BlockingStopBackend>();
    auto* const blockingBackendRaw = blockingBackendPtr.get();
    auto blockingEngine = Engine{std::move(blockingBackendPtr), device, makeScriptedEngineDecoderFactory()};

    auto routeChanged = std::atomic<bool>{false};
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
} // namespace ao::audio::test
