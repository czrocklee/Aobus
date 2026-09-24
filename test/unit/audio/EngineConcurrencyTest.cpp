// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "EngineTestSupport.h"
#include "ScriptedDecoderSession.h"
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/Device.h>
#include <ao/audio/Engine.h>
#include <ao/audio/OpenedPcmMode.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/Property.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/Transport.h>

#include <catch2/catch_test_macros.hpp>
#include <gsl-lite/gsl-lite.hpp>

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
#include <optional>
#include <semaphore>
#include <stop_token>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    class FakeBlockingPropertyBackend final : public Backend
    {
    public:
      Result<OpenedPcmMode> open(SignalFormat const& sourceFormat, RenderTarget& /*target*/) override
      {
        return OpenedPcmMode{.clientFormat = pcmFormat(sourceFormat, SampleEncoding::Signed16Le)};
      }
      void start() override {}
      void pause() override {}
      void resume() override {}
      void flush() override {}
      void stop() override {}
      void close() override {}

      BackendId backendId() const override { return BackendId{"blocking"}; }
      ProfileId profileId() const override { return ProfileId{"test"}; }

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

      bool tryWaitForEnteredCalls(std::size_t count, std::chrono::milliseconds timeout) const
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

    class RenderingBackend final : public Backend
    {
    public:
      Result<OpenedPcmMode> open(SignalFormat const& sourceFormat, RenderTarget& target) override
      {
        _format = pcmFormat(sourceFormat, SampleEncoding::Signed16Le);
        _target.store(&target, std::memory_order_relaxed);
        return OpenedPcmMode{.clientFormat = _format};
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
                                     auto const result = t->renderPcm(buffer);

                                     if (!_hasRendered.exchange(true))
                                     {
                                       _firstRender.release();
                                     }

                                     // A drained backend run must not render again.
                                     if (result.drained)
                                     {
                                       break;
                                     }
                                   }
                                 }
                               }};
      }

      bool tryWaitForFirstRender(std::chrono::milliseconds timeout) { return _firstRender.try_acquire_for(timeout); }

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

      BackendId backendId() const override { return BackendId{"rendering"}; }
      ProfileId profileId() const override { return ProfileId{"test"}; }

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
      std::atomic<RenderTarget*> _target{nullptr};
      PcmFormat _format{};
      std::atomic_bool _hasRendered{false};
      std::binary_semaphore _firstRender{0};
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
    auto backendPtr = std::make_unique<FakeBlockingPropertyBackend>();
    auto* const backendRaw = backendPtr.get();
    auto engine = Engine{std::move(backendPtr), device};

    auto secondStartedPromise = std::promise<void>{};
    auto secondStarted = secondStartedPromise.get_future();
    auto first = std::future<Result<>>{};
    auto second = std::future<Result<>>{};
    auto cleanup = gsl_lite::finally(
      [&]
      {
        backendRaw->releaseCalls();

        if (first.valid())
        {
          first.wait();
        }

        if (second.valid())
        {
          second.wait();
        }
      });
    first = std::async(std::launch::async, [&engine] { return engine.setVolume(0.25F); });
    REQUIRE(backendRaw->tryWaitForEnteredCalls(1, std::chrono::seconds{5}));

    second = std::async(std::launch::async,
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
    CHECK(backendRaw->tryWaitForEnteredCalls(2, std::chrono::seconds{1}));
    CHECK(backendRaw->maxActiveCalls() == 1);
  }

  // Run under TSan (./ao test --tsan): the control thread loops play/seek/stop
  // while a contract-conforming backend joins its render thread before each
  // source reset and a poller reads the same queue through status(). PipeWire
  // closes its own render admission and drains both data- and main-loop work.
  TEST_CASE("Engine - concurrent source swap is race-free", "[audio][unit][engine][concurrency][stress]")
  {
    auto const device = Device{.id = DeviceId{"test-device"},
                               .displayName = "Test",
                               .description = "Test",
                               .isDefault = false,
                               .backendId = kBackendNone};

    auto const fmt = PcmFormat{.sampleRate = 44100, .channels = 2, .encoding = SampleEncoding::Signed16Le};
    auto const factory = [fmt](auto const&, std::optional<SampleEncoding> optOutputEncoding)
    {
      auto const sourceFormat = signalFormat(fmt);
      auto decPtr = std::make_unique<ScriptedDecoderSession>(
        DecodedStreamInfo{.sourceFormat = sourceFormat,
                          .outputFormat = pcmFormat(sourceFormat, optOutputEncoding.value_or(fmt.encoding)),
                          .duration = std::chrono::milliseconds{0},
                          .isLossy = false});
      auto data = std::vector(4096, std::byte{0});
      decPtr->setReadScript({{.data = data, .endOfStream = false},
                             {.data = data, .endOfStream = false},
                             {.data = data, .endOfStream = false},
                             {.endOfStream = true}});
      return decPtr;
    };

    auto backendPtr = std::make_unique<RenderingBackend>();
    auto* const backendRaw = backendPtr.get();
    auto engine = Engine{std::move(backendPtr), device, factory};
    auto const desc = PlaybackInput{.filePath = "song.flac"};
    auto pollerStarted = std::binary_semaphore{0};

    auto poller = std::jthread{[&](std::stop_token const& st)
                               {
                                 std::ignore = engine.status();
                                 pollerStarted.release();

                                 while (!st.stop_requested())
                                 {
                                   std::ignore = engine.status();
                                 }
                               }};

    REQUIRE(pollerStarted.try_acquire_for(std::chrono::seconds{5}));

    for (std::int32_t i = 0; i < 50; ++i)
    {
      engine.play(makePlaybackItem(desc));

      if (i == 0)
      {
        REQUIRE(backendRaw->tryWaitForFirstRender(std::chrono::seconds{5}));
      }

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
