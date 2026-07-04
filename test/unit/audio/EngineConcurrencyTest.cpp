// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "EngineTestSupport.h"
#include "ScriptedDecoderSession.h"
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/DecoderTypes.h>
#include <ao/audio/Engine.h>
#include <ao/audio/IBackend.h>
#include <ao/audio/IRenderTarget.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/Property.h>
#include <ao/audio/Transport.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <future>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    class BlockingPropertyBackend final : public IBackend
    {
    public:
      Result<> open(Format const& /*format*/, IRenderTarget* /*target*/) override { return {}; }
      void start() override {}
      void pause() override {}
      void resume() override {}
      void flush() override {}
      void stop() override {}
      void close() override {}

      BackendId backendId() const noexcept override { return BackendId{"blocking"}; }
      ProfileId profileId() const noexcept override { return ProfileId{"test"}; }

      Result<> setProperty(PropertyId /*id*/, PropertyValue const& /*value*/) override
      {
        auto lock = std::unique_lock{_mutex};
        ++_enteredCalls;
        ++_activeCalls;
        _maxActiveCalls = std::max(_maxActiveCalls, _activeCalls);
        _cv.notify_all();

        _cv.wait(lock, [this] { return _releaseCalls; });
        --_activeCalls;
        _cv.notify_all();
        return {};
      }

      Result<PropertyValue> property(PropertyId id) const override
      {
        if (id == PropertyId::Volume)
        {
          return PropertyValue{1.0F};
        }

        if (id == PropertyId::Muted)
        {
          return PropertyValue{false};
        }

        return std::unexpected(Error{.code = Error::Code::NotSupported});
      }

      PropertyInfo queryProperty(PropertyId /*id*/) const noexcept override
      {
        return {.canRead = true, .canWrite = true, .isAvailable = true, .emitsChangeNotifications = false};
      }

      bool waitForEnteredCalls(std::size_t count, std::chrono::milliseconds timeout) const
      {
        auto lock = std::unique_lock{_mutex};
        return _cv.wait_for(lock, timeout, [this, count] { return _enteredCalls >= count; });
      }

      void releaseCalls()
      {
        auto const lock = std::scoped_lock{_mutex};
        _releaseCalls = true;
        _cv.notify_all();
      }

      std::size_t maxActiveCalls() const
      {
        auto const lock = std::scoped_lock{_mutex};
        return _maxActiveCalls;
      }

    private:
      mutable std::mutex _mutex;
      mutable std::condition_variable _cv;
      std::size_t _enteredCalls = 0;
      std::size_t _activeCalls = 0;
      std::size_t _maxActiveCalls = 0;
      bool _releaseCalls = false;
    };

    class RenderingBackend final : public IBackend
    {
    public:
      Result<> open(Format const& format, IRenderTarget* target) override
      {
        _format = format;
        _target.store(target, std::memory_order_relaxed);
        return {};
      }

      void start() override
      {
        if (_thread.joinable())
        {
          return;
        }

        _thread = std::jthread{[this](std::stop_token const& st)
                               {
                                 auto buffer = std::array<std::byte, 1024>{};

                                 while (!st.stop_requested())
                                 {
                                   if (auto* const t = _target.load(std::memory_order_relaxed); t != nullptr)
                                   {
                                     std::ignore = t->renderPcm(buffer);
                                   }
                                 }
                               }};
      }

      void pause() override {}
      void resume() override {}
      void flush() override {}

      void stop() override
      {
        _thread.request_stop();

        if (_thread.joinable())
        {
          _thread.join();
        }
      }

      void close() override {}

      BackendId backendId() const noexcept override { return BackendId{"rendering"}; }
      ProfileId profileId() const noexcept override { return ProfileId{"test"}; }

      Result<> setProperty(PropertyId /*id*/, PropertyValue const& /*value*/) override { return {}; }

      Result<PropertyValue> property(PropertyId id) const override
      {
        if (id == PropertyId::Volume)
        {
          return PropertyValue{1.0F};
        }

        if (id == PropertyId::Muted)
        {
          return PropertyValue{false};
        }

        return std::unexpected(Error{.code = Error::Code::NotSupported});
      }

      PropertyInfo queryProperty(PropertyId /*id*/) const noexcept override
      {
        return {.canRead = true, .canWrite = true, .isAvailable = true, .emitsChangeNotifications = false};
      }

    private:
      std::atomic<IRenderTarget*> _target{nullptr};
      Format _format{};
      std::jthread _thread;
    };
  } // namespace

  TEST_CASE("Engine - concurrent control commands are serialized", "[audio][unit][engine][concurrency]")
  {
    auto const device = Device{.id = DeviceId{"test-device"},
                               .displayName = "Test",
                               .description = "Test",
                               .isDefault = false,
                               .backendId = kBackendNone};
    auto backendPtr = std::make_unique<BlockingPropertyBackend>();
    auto* const backendRaw = backendPtr.get();
    auto engine = Engine{std::move(backendPtr), device};

    auto first = std::async(std::launch::async, [&engine] { return engine.setVolume(0.25F); });
    auto const firstEntered = backendRaw->waitForEnteredCalls(1, std::chrono::seconds{1});

    if (!firstEntered)
    {
      backendRaw->releaseCalls();
    }

    CHECK(firstEntered);

    auto secondStartedPromise = std::promise<void>{};
    auto secondStarted = secondStartedPromise.get_future();
    auto second = std::async(std::launch::async,
                             [&]
                             {
                               secondStartedPromise.set_value();
                               return engine.setMuted(true);
                             });

    auto const secondStartedStatus = secondStarted.wait_for(std::chrono::seconds{1});

    backendRaw->releaseCalls();

    CHECK(secondStartedStatus == std::future_status::ready);
    REQUIRE(first.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
    REQUIRE(second.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
    CHECK(first.get());
    CHECK(second.get());
    CHECK(backendRaw->maxActiveCalls() == 1);
  }

  // Run under TSan (./ao test --tsan): the control thread loops play/seek/stop
  // while a render thread reads the lock-free source pointer and a poller reads
  // status() through the source slot owner.
  TEST_CASE("Engine - concurrent source swap is race-free", "[audio][unit][engine][concurrency]")
  {
    auto const device = Device{.id = DeviceId{"test-device"},
                               .displayName = "Test",
                               .description = "Test",
                               .isDefault = false,
                               .backendId = kBackendNone};

    auto const fmt = Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isInterleaved = true};
    auto const factory = [fmt](auto const&, auto const&)
    {
      auto decPtr = std::make_unique<ScriptedDecoderSession>(DecodedStreamInfo{
        .sourceFormat = fmt, .outputFormat = fmt, .duration = std::chrono::milliseconds{0}, .isLossy = false});
      auto data = std::vector(4096, std::byte{0});
      decPtr->setReadScript({{data, false}, {data, false}, {data, false}, {{}, true}});
      return decPtr;
    };

    auto engine = Engine{std::make_unique<RenderingBackend>(), device, factory};
    auto const desc = PlaybackInput{.filePath = "song.flac"};

    auto poller = std::jthread{[&](std::stop_token const& st)
                               {
                                 while (!st.stop_requested())
                                 {
                                   std::ignore = engine.status();
                                 }
                               }};

    for (std::int32_t i = 0; i < 50; ++i)
    {
      engine.play(makePlaybackItem(desc));
      engine.seek(std::chrono::milliseconds{10});
      engine.stop();
    }

    poller.request_stop();

    if (poller.joinable())
    {
      poller.join();
    }

    CHECK(engine.status().transport == Transport::Idle);
  }
} // namespace ao::audio::test
