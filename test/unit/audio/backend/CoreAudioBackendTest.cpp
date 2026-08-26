// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/CoreAudioBackend.h"

#include "lib/audio/backend/detail/BackendGraphRegistry.h"
#include "lib/audio/backend/detail/CoreAudioDeviceDiscovery.h"
#include "lib/audio/detail/DecoderOutput.h"
#include "test/unit/audio/BackendTestSupport.h"
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/Property.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/flow/Graph.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <semaphore>
#include <span>
#include <string_view>
#include <thread>

namespace ao::audio::backend::test
{
  namespace
  {
    class DrainingRenderTarget final : public RenderTarget
    {
    public:
      RenderPcmResult renderPcm(std::span<std::byte> /*output*/) noexcept override
      {
        _renderCalls.fetch_add(1U, std::memory_order_relaxed);
        return {.drained = true};
      }

      void handleUnderrun() noexcept override {}
      void handlePositionAdvanced(std::uint32_t /*frames*/) noexcept override {}
      void handleDrainComplete() noexcept override { _drained.release(); }
      void handleRouteReady(std::string_view /*routeAnchor*/) noexcept override {}
      void handleFormatChanged(PcmFormat const& /*format*/) noexcept override {}
      void handlePropertyChanged(PropertySnapshot /*snapshot*/) noexcept override {}
      void handleBackendError(std::string_view /*message*/) noexcept override
      {
        _errors.fetch_add(1U, std::memory_order_relaxed);
      }

      bool waitForDrain(std::chrono::seconds const timeout) { return _drained.try_acquire_for(timeout); }
      std::size_t renderCalls() const noexcept { return _renderCalls.load(std::memory_order_relaxed); }
      std::size_t errors() const noexcept { return _errors.load(std::memory_order_relaxed); }

    private:
      std::atomic<std::size_t> _renderCalls{0U};
      std::atomic<std::size_t> _errors{0U};
      std::binary_semaphore _drained{0};
    };

    class BlockingDrainingRenderTarget final : public RenderTarget
    {
    public:
      RenderPcmResult renderPcm(std::span<std::byte> /*output*/) noexcept override
      {
        _renderCalls.fetch_add(1U, std::memory_order_relaxed);
        _renderEntered.release();
        _releaseRender.acquire();
        return {.drained = true};
      }

      void handleUnderrun() noexcept override {}
      void handlePositionAdvanced(std::uint32_t /*frames*/) noexcept override {}
      void handleDrainComplete() noexcept override { _drainCompletions.fetch_add(1U, std::memory_order_relaxed); }
      void handleRouteReady(std::string_view /*routeAnchor*/) noexcept override {}
      void handleFormatChanged(PcmFormat const& /*format*/) noexcept override {}
      void handlePropertyChanged(PropertySnapshot /*snapshot*/) noexcept override {}
      void handleBackendError(std::string_view /*message*/) noexcept override
      {
        _errors.fetch_add(1U, std::memory_order_relaxed);
      }

      bool waitForRender(std::chrono::seconds const timeout) { return _renderEntered.try_acquire_for(timeout); }
      void releaseRender() { _releaseRender.release(); }
      std::size_t renderCalls() const noexcept { return _renderCalls.load(std::memory_order_relaxed); }
      std::size_t drainCompletions() const noexcept { return _drainCompletions.load(std::memory_order_relaxed); }
      std::size_t errors() const noexcept { return _errors.load(std::memory_order_relaxed); }

    private:
      std::atomic<std::size_t> _renderCalls{0U};
      std::atomic<std::size_t> _drainCompletions{0U};
      std::atomic<std::size_t> _errors{0U};
      std::binary_semaphore _renderEntered{0};
      std::binary_semaphore _releaseRender{0};
    };
  } // namespace

  TEST_CASE("CoreAudioBackend - caches software volume and mute before native open",
            "[audio][unit][coreaudio][backend]")
  {
    auto backend = CoreAudioBackend{
      Device{.id = DeviceId{"synthetic-uid"}, .displayName = "Synthetic Output", .backendId = kBackendCoreAudio},
      kProfileShared};
    CHECK(backend.backendId() == kBackendCoreAudio);
    CHECK(backend.profileId() == kProfileShared);
    CHECK_FALSE(backend.queryProperty(PropertyId::Volume).isAvailable);

    backend.start();
    backend.pause();
    backend.resume();
    backend.flush();
    backend.stop();
    backend.close();
    CHECK_FALSE(backend.queryProperty(PropertyId::Volume).isAvailable);

    REQUIRE(backend.set(props::kVolume, 0.4F));
    REQUIRE(backend.set(props::kMuted, true));
    auto const volumeRes = backend.get(props::kVolume);
    auto const mutedRes = backend.get(props::kMuted);
    REQUIRE(volumeRes);
    REQUIRE(mutedRes);
    CHECK(*volumeRes == 0.4F);
    CHECK(*mutedRes);
  }

  TEST_CASE("CoreAudioBackend - native AUHAL open confirms the exact lossless client stream",
            "[audio][integration][coreaudio][backend]")
  {
    auto const devices = detail::enumerateCoreAudioOutputDevices();

    if (devices.empty())
    {
      SKIP("macOS host has no live Core Audio output device");
    }

    auto backend = CoreAudioBackend{devices.front(), kProfileShared};
    auto target = ao::audio::test::NoopRenderTarget{};
    auto const openedRes = backend.open(
      {.sampleRate = 44100, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::Integer}, target);
    REQUIRE(openedRes);
    CHECK(openedRes->clientFormat.sampleRate == 44100U);
    CHECK(openedRes->clientFormat.channels == 2U);
    CHECK(openedRes->clientFormat.encoding == SampleEncoding::Signed16Le);
    CHECK_FALSE(openedRes->optEndpoint);
    CHECK(backend.queryProperty(PropertyId::Volume).isAvailable);
    CHECK_FALSE(backend.queryProperty(PropertyId::Volume).isHardwareAssisted);
    REQUIRE(backend.set(props::kVolume, 0.25F));
    REQUIRE(backend.set(props::kMuted, true));
    REQUIRE(backend.set(props::kMuted, false));
    auto const volumeRes = backend.get(props::kVolume);
    REQUIRE(volumeRes);
    CHECK(*volumeRes == 0.25F);

    backend.close();
    CHECK_FALSE(backend.queryProperty(PropertyId::Volume).isAvailable);
  }

  TEST_CASE("CoreAudioBackend - native AUHAL keeps common high-resolution clients lossless",
            "[audio][integration][coreaudio][backend]")
  {
    auto const devices = detail::enumerateCoreAudioOutputDevices();

    if (devices.empty())
    {
      SKIP("macOS host has no live Core Audio output device");
    }

    constexpr auto kSourceFormats = std::array{
      SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 24, .sampleKind = SampleKind::Integer},
      SignalFormat{.sampleRate = 96000, .channels = 2, .precisionBits = 24, .sampleKind = SampleKind::Integer}};
    auto backend = CoreAudioBackend{devices.front(), kProfileShared};
    auto target = ao::audio::test::NoopRenderTarget{};

    for (auto const& sourceFormat : kSourceFormats)
    {
      CAPTURE(sourceFormat.sampleRate);
      auto const openedRes = backend.open(sourceFormat, target);
      REQUIRE(openedRes);
      CHECK(::ao::audio::detail::isLosslessPcmEncoding(sourceFormat, openedRes->clientFormat.encoding));
      CHECK_FALSE(openedRes->optEndpoint);
      backend.close();
    }
  }

  TEST_CASE("CoreAudioBackend - native drain renders once and completes after the presentation tail",
            "[audio][integration][coreaudio][concurrency]")
  {
    auto const devices = detail::enumerateCoreAudioOutputDevices();

    if (devices.empty())
    {
      SKIP("macOS host has no live Core Audio output device");
    }

    auto backend = CoreAudioBackend{devices.front(), kProfileShared};
    auto target = DrainingRenderTarget{};
    auto const openedRes = backend.open(
      {.sampleRate = 44100, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::Integer}, target);
    REQUIRE(openedRes);

    backend.start();
    REQUIRE(target.waitForDrain(std::chrono::seconds{5}));
    CHECK(target.renderCalls() == 1U);
    CHECK(target.errors() == 0U);

    backend.close();
    CHECK(target.renderCalls() == 1U);
  }

  TEST_CASE("CoreAudioBackend - graph observer may close backend without recursive clear",
            "[audio][integration][coreaudio][concurrency]")
  {
    auto const devices = detail::enumerateCoreAudioOutputDevices();

    if (devices.empty())
    {
      SKIP("macOS host has no live Core Audio output device");
    }

    auto graphRegistryPtr = std::make_shared<detail::BackendGraphRegistry>();
    auto backend = CoreAudioBackend{devices.front(), kProfileShared, graphRegistryPtr};
    auto target = ao::audio::test::NoopRenderTarget{};
    REQUIRE(backend.open(
      {.sampleRate = 44100, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::Integer}, target));

    std::size_t graphCalls = 0;
    auto sub = graphRegistryPtr->subscribe(devices.front().id.raw(),
                                           [&](flow::Graph const& graph)
                                           {
                                             ++graphCalls;

                                             if (!graph.nodes.empty())
                                             {
                                               backend.close();
                                             }
                                           });

    REQUIRE(sub);
    CHECK(graphCalls == 2U);
    CHECK_FALSE(backend.queryProperty(PropertyId::Volume).isAvailable);

    backend.close();
    CHECK(graphCalls == 2U);
  }

  TEST_CASE("CoreAudioBackend - stop fences a blocked render and cancels its drain",
            "[audio][integration][coreaudio][concurrency]")
  {
    auto const devices = detail::enumerateCoreAudioOutputDevices();

    if (devices.empty())
    {
      SKIP("macOS host has no live Core Audio output device");
    }

    auto backend = CoreAudioBackend{devices.front(), kProfileShared};
    auto target = BlockingDrainingRenderTarget{};
    REQUIRE(backend.open(
      {.sampleRate = 44100, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::Integer}, target));
    backend.start();
    REQUIRE(target.waitForRender(std::chrono::seconds{5}));

    auto stopEntered = std::binary_semaphore{0};
    auto stopReturned = std::binary_semaphore{0};
    auto stopThread = std::jthread{[&]
                                   {
                                     stopEntered.release();
                                     backend.stop();
                                     stopReturned.release();
                                   }};
    stopEntered.acquire();
    CHECK_FALSE(stopReturned.try_acquire());

    target.releaseRender();
    REQUIRE(stopReturned.try_acquire_for(std::chrono::seconds{5}));
    stopThread.join();
    backend.close();

    CHECK(target.renderCalls() == 1U);
    CHECK(target.drainCompletions() == 0U);
    CHECK(target.errors() == 0U);
  }
} // namespace ao::audio::backend::test
