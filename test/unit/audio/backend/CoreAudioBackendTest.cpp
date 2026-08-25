// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/CoreAudioBackend.h"

#include "lib/audio/backend/detail/BackendGraphRegistry.h"
#include "lib/audio/backend/detail/CoreAudioDeviceDiscovery.h"
#include "lib/audio/detail/DecoderOutput.h"
#include "test/unit/audio/BackendTestSupport.h"

#include <ao/audio/BackendIds.h>
#include <ao/audio/Property.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/flow/Graph.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <array>
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
        renderCalls.fetch_add(1U, std::memory_order_relaxed);
        return {.drained = true};
      }

      void handleUnderrun() noexcept override {}
      void handlePositionAdvanced(std::uint32_t /*frames*/) noexcept override {}
      void handleDrainComplete() noexcept override { drained.release(); }
      void handleRouteReady(std::string_view /*routeAnchor*/) noexcept override {}
      void handleFormatChanged(PcmFormat const& /*format*/) noexcept override {}
      void handlePropertyChanged(PropertySnapshot /*snapshot*/) noexcept override {}
      void handleBackendError(std::string_view /*message*/) noexcept override
      {
        errors.fetch_add(1U, std::memory_order_relaxed);
      }

      std::atomic<std::size_t> renderCalls{0U};
      std::atomic<std::size_t> errors{0U};
      std::binary_semaphore drained{0};
    };

    class BlockingDrainingRenderTarget final : public RenderTarget
    {
    public:
      RenderPcmResult renderPcm(std::span<std::byte> /*output*/) noexcept override
      {
        renderCalls.fetch_add(1U, std::memory_order_relaxed);
        renderEntered.release();
        releaseRender.acquire();
        return {.drained = true};
      }

      void handleUnderrun() noexcept override {}
      void handlePositionAdvanced(std::uint32_t /*frames*/) noexcept override {}
      void handleDrainComplete() noexcept override
      {
        drainCompletions.fetch_add(1U, std::memory_order_relaxed);
      }
      void handleRouteReady(std::string_view /*routeAnchor*/) noexcept override {}
      void handleFormatChanged(PcmFormat const& /*format*/) noexcept override {}
      void handlePropertyChanged(PropertySnapshot /*snapshot*/) noexcept override {}
      void handleBackendError(std::string_view /*message*/) noexcept override
      {
        errors.fetch_add(1U, std::memory_order_relaxed);
      }

      std::atomic<std::size_t> renderCalls{0U};
      std::atomic<std::size_t> drainCompletions{0U};
      std::atomic<std::size_t> errors{0U};
      std::binary_semaphore renderEntered{0};
      std::binary_semaphore releaseRender{0};
    };
  } // namespace

  TEST_CASE("CoreAudioBackend - caches software volume and mute before native open",
            "[audio][unit][coreaudio][backend]")
  {
    auto backend = CoreAudioBackend(
      Device{.id = DeviceId{"synthetic-uid"}, .displayName = "Synthetic Output", .backendId = kBackendCoreAudio},
      kProfileShared);
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

    auto backend = CoreAudioBackend(devices.front(), kProfileShared);
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

    constexpr auto sourceFormats = std::array{
      SignalFormat{.sampleRate = 48000,
                   .channels = 2,
                   .precisionBits = 24,
                   .sampleKind = SampleKind::Integer},
      SignalFormat{.sampleRate = 96000,
                   .channels = 2,
                   .precisionBits = 24,
                   .sampleKind = SampleKind::Integer}};
    auto backend = CoreAudioBackend(devices.front(), kProfileShared);
    auto target = ao::audio::test::NoopRenderTarget{};
    for (auto const& sourceFormat : sourceFormats)
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

    auto backend = CoreAudioBackend(devices.front(), kProfileShared);
    auto target = DrainingRenderTarget{};
    auto const openedRes = backend.open(
      {.sampleRate = 44100, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::Integer}, target);
    REQUIRE(openedRes);

    backend.start();
    REQUIRE(target.drained.try_acquire_for(std::chrono::seconds{5}));
    CHECK(target.renderCalls.load(std::memory_order_relaxed) == 1U);
    CHECK(target.errors.load(std::memory_order_relaxed) == 0U);

    backend.close();
    CHECK(target.renderCalls.load(std::memory_order_relaxed) == 1U);
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
    auto backend = CoreAudioBackend(devices.front(), kProfileShared, graphRegistryPtr);
    auto target = ao::audio::test::NoopRenderTarget{};
    REQUIRE(backend.open(
      {.sampleRate = 44100, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::Integer}, target));

    auto graphCalls = std::size_t{0U};
    auto sub = graphRegistryPtr->subscribe(
      devices.front().id.raw(),
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

    auto backend = CoreAudioBackend(devices.front(), kProfileShared);
    auto target = BlockingDrainingRenderTarget{};
    REQUIRE(backend.open(
      {.sampleRate = 44100, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::Integer}, target));
    backend.start();
    REQUIRE(target.renderEntered.try_acquire_for(std::chrono::seconds{5}));

    auto stopEntered = std::binary_semaphore{0};
    auto stopReturned = std::binary_semaphore{0};
    auto stopThread = std::jthread{
      [&]
      {
        stopEntered.release();
        backend.stop();
        stopReturned.release();
      }};
    stopEntered.acquire();
    CHECK_FALSE(stopReturned.try_acquire());

    target.releaseRender.release();
    REQUIRE(stopReturned.try_acquire_for(std::chrono::seconds{5}));
    stopThread.join();
    backend.close();

    CHECK(target.renderCalls.load(std::memory_order_relaxed) == 1U);
    CHECK(target.drainCompletions.load(std::memory_order_relaxed) == 0U);
    CHECK(target.errors.load(std::memory_order_relaxed) == 0U);
  }
} // namespace ao::audio::backend::test
