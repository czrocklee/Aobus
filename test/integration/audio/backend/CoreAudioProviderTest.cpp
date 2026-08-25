// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/CoreAudioProvider.h"

#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/Property.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

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
    class SineRenderTarget final : public RenderTarget
    {
    public:
      SineRenderTarget(PcmFormat format, std::uint32_t const totalFrames)
        : _format{format}, _totalFrames{totalFrames}
      {
      }

      RenderPcmResult renderPcm(std::span<std::byte> output) noexcept override
      {
        auto const frameSize = frameBytes(_format);
        auto const requestFrames = static_cast<std::uint32_t>(output.size() / frameSize);
        auto const frames = std::min(requestFrames, _totalFrames - _renderedFrames);
        auto* const samples = reinterpret_cast<std::int16_t*>(output.data());

        for (auto frame = std::uint32_t{0U}; frame < frames; ++frame)
        {
          auto const phase = 2.0 * std::numbers::pi * 440.0 * (_renderedFrames + frame) /
                             static_cast<double>(_format.sampleRate);
          auto const value = static_cast<std::int16_t>(std::sin(phase) * 0.05 * 32767.0);
          for (auto channel = std::uint32_t{0U}; channel < _format.channels; ++channel)
          {
            samples[(static_cast<std::size_t>(frame) * _format.channels) + channel] = value;
          }
        }

        _renderedFrames += frames;
        return {.bytesWritten = static_cast<std::size_t>(frames) * frameSize,
                .positionFrames = frames,
                .drained = _renderedFrames == _totalFrames};
      }

      void handleUnderrun() noexcept override {}
      void handlePositionAdvanced(std::uint32_t const frames) noexcept override
      {
        _advancedFrames.fetch_add(frames, std::memory_order_relaxed);
      }
      void handleDrainComplete() noexcept override
      {
        {
          auto const lock = std::scoped_lock{_mutex};
          _drained = true;
        }
        _condition.notify_all();
      }
      void handleRouteReady(std::string_view const routeAnchor) noexcept override
      {
        auto const lock = std::scoped_lock{_mutex};
        _routeAnchor = routeAnchor;
      }
      void handleFormatChanged(PcmFormat const& /*format*/) noexcept override {}
      void handlePropertyChanged(PropertySnapshot /*snapshot*/) noexcept override {}
      void handleBackendError(std::string_view const message) noexcept override
      {
        {
          auto const lock = std::scoped_lock{_mutex};
          _error = message;
          _drained = true;
        }
        _condition.notify_all();
      }

      bool waitForDrain(std::chrono::seconds const timeout)
      {
        auto lock = std::unique_lock{_mutex};
        return _condition.wait_for(lock, timeout, [this] { return _drained; });
      }

      std::string error() const
      {
        auto const lock = std::scoped_lock{_mutex};
        return _error;
      }

      std::string routeAnchor() const
      {
        auto const lock = std::scoped_lock{_mutex};
        return _routeAnchor;
      }

      std::uint32_t advancedFrames() const noexcept
      {
        return _advancedFrames.load(std::memory_order_relaxed);
      }

    private:
      PcmFormat _format;
      std::uint32_t _totalFrames = 0U;
      std::uint32_t _renderedFrames = 0U;
      std::atomic<std::uint32_t> _advancedFrames{0U};
      mutable std::mutex _mutex{};
      std::condition_variable _condition{};
      bool _drained = false;
      std::string _error{};
      std::string _routeAnchor{};
    };
  } // namespace

  TEST_CASE("CoreAudioProvider - enumerates and renders through a real output",
            "[audio][integration][coreaudio][.manual]")
  {
    auto provider = CoreAudioProvider{};
    auto const status = provider.status();
    if (status.devices.empty())
    {
      SKIP("macOS host has no live Core Audio output device");
    }

    auto const defaultCount = std::ranges::count(status.devices, true, &Device::isDefault);
    CHECK(defaultCount <= 1);
    if (defaultCount == 1)
    {
      CHECK(status.devices.front().isDefault);
    }

    auto const sourceFormat =
      SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::Integer};
    auto const pcm = pcmFormat(sourceFormat, SampleEncoding::Signed16Le);
    auto target = SineRenderTarget{pcm, sourceFormat.sampleRate / 4U};
    auto backendPtr = provider.createBackend(status.devices.front(), kProfileShared);
    REQUIRE(backendPtr);
    REQUIRE(backendPtr->set(props::kVolume, 0.25F));

    auto const openedRes = backendPtr->open(sourceFormat, target);
    if (!openedRes)
    {
      FAIL("Core Audio open failed: " << openedRes.error().message);
    }
    REQUIRE(openedRes->clientFormat == pcm);
    CHECK(target.routeAnchor() == status.devices.front().id.raw());

    backendPtr->start();
    auto const drained = target.waitForDrain(std::chrono::seconds{10});

    INFO("Core Audio backend error: " << target.error());
    CHECK(target.error().empty());
    CHECK(drained);
    CHECK(target.advancedFrames() == sourceFormat.sampleRate / 4U);

    backendPtr->stop();
    backendPtr->close();
    provider.shutdown();
  }
} // namespace ao::audio::backend::test
