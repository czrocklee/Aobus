// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/Format.h>
#include <ao/audio/Property.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/backend/WasapiProvider.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <numbers>
#include <span>
#include <string>
#include <string_view>

namespace ao::audio::backend::test
{
  namespace
  {
    /**
     * @brief Renders a quiet 440Hz sine burst, then reports drained.
     */
    class SineRenderTarget final : public RenderTarget
    {
    public:
      SineRenderTarget(Format const& format, std::uint32_t totalFrames)
        : _format{format}, _totalFrames{totalFrames}
      {
      }

      RenderPcmResult renderPcm(std::span<std::byte> output) noexcept override
      {
        auto const frameSize = frameBytes(_format);
        auto const requestFrames = static_cast<std::uint32_t>(output.size() / frameSize);
        auto const frames = std::min(requestFrames, _totalFrames - _renderedFrames);

        if (frames == 0)
        {
          return {.bytesWritten = 0, .drained = true};
        }

        auto* samples = reinterpret_cast<std::int16_t*>(output.data());

        for (std::uint32_t frame = 0; frame < frames; ++frame)
        {
          auto const phase = 2.0 * std::numbers::pi * 440.0 * (_renderedFrames + frame) / _format.sampleRate;
          auto const value = static_cast<std::int16_t>(std::sin(phase) * 0.1 * 32767.0);

          for (std::uint32_t channel = 0; channel < _format.channels; ++channel)
          {
            samples[(static_cast<std::size_t>(frame) * _format.channels) + channel] = value;
          }
        }

        _renderedFrames += frames;

        return {.bytesWritten = static_cast<std::size_t>(frames) * frameSize,
                .positionFrameOffset = 0,
                .positionFrames = frames,
                .drained = (_renderedFrames == _totalFrames)};
      }

      void handleUnderrun() noexcept override {}

      void handlePositionAdvanced(std::uint32_t frames) noexcept override
      {
        _advancedFrames.fetch_add(frames, std::memory_order_relaxed);
      }

      void handleDrainComplete() noexcept override
      {
        {
          auto const lock = std::scoped_lock{_mutex};
          _drainComplete = true;
        }

        _cv.notify_all();
      }

      void handleRouteReady(std::string_view routeAnchor) noexcept override
      {
        auto const lock = std::scoped_lock{_mutex};
        _routeAnchor = std::string{routeAnchor};
      }

      void handleFormatChanged(Format const& /*format*/) noexcept override {}
      void handlePropertyChanged(PropertySnapshot /*snapshot*/) noexcept override {}

      void handleBackendError(std::string_view message) noexcept override
      {
        {
          auto const lock = std::scoped_lock{_mutex};
          _errorMessage = std::string{message};
          _drainComplete = true; // unblock the waiter
        }

        _cv.notify_all();
      }

      bool waitForDrainComplete(std::chrono::milliseconds timeout)
      {
        auto lock = std::unique_lock{_mutex};
        return _cv.wait_for(lock, timeout, [this] { return _drainComplete; });
      }

      std::uint32_t advancedFrames() const { return _advancedFrames.load(std::memory_order_relaxed); }

      std::string errorMessage() const
      {
        auto const lock = std::scoped_lock{_mutex};
        return _errorMessage;
      }

      std::string routeAnchor() const
      {
        auto const lock = std::scoped_lock{_mutex};
        return _routeAnchor;
      }

    private:
      Format _format;
      std::uint32_t _totalFrames;
      std::uint32_t _renderedFrames = 0; // render-thread only

      std::atomic<std::uint32_t> _advancedFrames{0};

      mutable std::mutex _mutex;
      std::condition_variable _cv;
      bool _drainComplete = false;
      std::string _errorMessage;
      std::string _routeAnchor;
    };
  } // namespace

  TEST_CASE("WasapiProvider - enumerates and renders through real endpoints", "[audio][integration][wasapi][.manual]")
  {
    auto provider = WasapiProvider{};
    auto const status = provider.status();

    if (status.devices.empty())
    {
      SKIP("No active WASAPI render endpoints");
    }

    SECTION("Enumeration exposes one logical device per id with the default first")
    {
      for (auto const& device : status.devices)
      {
        INFO("Device: " << device.id.raw() << " / " << device.displayName);
        CHECK_FALSE(device.id.raw().empty());
        CHECK_FALSE(device.displayName.empty());
        CHECK(device.backendId == kBackendWasapi);
        CHECK_FALSE(device.capabilities.sampleFormats.empty());
      }

      auto const defaultCount = std::ranges::count(status.devices, true, &Device::isDefault);
      CHECK(defaultCount == 1);
      CHECK(status.devices.front().isDefault);
    }

    SECTION("Backend plays a short PCM burst to the default endpoint")
    {
      auto const format = Format{.sampleRate = 48000, .channels = 2, .bitDepth = 16, .validBits = 16};

      // 250ms of audio keeps the test quick while still crossing several
      // device periods.
      auto const totalFrames = static_cast<std::uint32_t>(format.sampleRate / 4);
      auto target = SineRenderTarget{format, totalFrames};

      auto const backendPtr = provider.createBackend(status.devices.front(), kProfileShared);
      REQUIRE(backendPtr != nullptr);
      CHECK(backendPtr->backendId() == kBackendWasapi);
      CHECK(backendPtr->profileId() == kProfileShared);

      if (auto const openResult = backendPtr->open(format, &target); !openResult)
      {
        FAIL("open failed: " << openResult.error().message);
      }

      CHECK_FALSE(target.routeAnchor().empty());

      backendPtr->start();

      auto const drained = target.waitForDrainComplete(std::chrono::seconds{10});

      INFO("Backend error: " << target.errorMessage());
      CHECK(target.errorMessage().empty());
      CHECK(drained);
      CHECK(target.advancedFrames() == totalFrames);

      backendPtr->stop();
      backendPtr->close();
    }
  }
} // namespace ao::audio::backend::test
