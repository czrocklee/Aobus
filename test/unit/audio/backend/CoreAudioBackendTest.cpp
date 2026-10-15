// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/CoreAudioBackend.h"

#include "lib/audio/backend/detail/BackendGraphRegistry.h"
#include "lib/audio/backend/detail/CoreAudioDeviceDiscovery.h"
#include "lib/audio/detail/DecoderOutput.h"
#include "test/unit/audio/BackendTestSupport.h"
#include "test/unit/audio/backend/CoreAudioTestSupport.h"
#include <ao/Error.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/Property.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/flow/Graph.h>
#include <ao/utility/ScopedRegistration.h>

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
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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
      void handleDrainComplete() noexcept override
      {
        _drainCompletions.fetch_add(1U, std::memory_order_relaxed);
        _drained.release();
      }
      void handleRouteReady(std::string_view /*routeAnchor*/) noexcept override {}
      void handleFormatChanged(PcmFormat const& /*format*/) noexcept override {}
      void handlePropertyChanged(PropertySnapshot /*snapshot*/) noexcept override {}
      void handleBackendError(std::string_view /*message*/) noexcept override
      {
        _errors.fetch_add(1U, std::memory_order_relaxed);
      }

      bool tryWaitForDrain(std::chrono::seconds const timeout) { return _drained.try_acquire_for(timeout); }
      std::size_t drainCompletions() const noexcept { return _drainCompletions.load(std::memory_order_relaxed); }
      std::size_t renderCalls() const noexcept { return _renderCalls.load(std::memory_order_relaxed); }
      std::size_t errors() const noexcept { return _errors.load(std::memory_order_relaxed); }

    private:
      std::atomic<std::size_t> _renderCalls{0U};
      std::atomic<std::size_t> _errors{0U};
      std::atomic<std::size_t> _drainCompletions{0U};
      std::counting_semaphore<> _drained{0};
    };

    class BlockingDrainingRenderTarget final : public RenderTarget
    {
    public:
      RenderPcmResult renderPcm(std::span<std::byte> /*output*/) noexcept override
      {
        _renderCalls.fetch_add(1U, std::memory_order_relaxed);
        _renderEntered.release();
        _releaseRender.wait(false);
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

      bool tryWaitForRender(std::chrono::seconds const timeout) { return _renderEntered.try_acquire_for(timeout); }
      void releaseRender()
      {
        _releaseRender.store(true);
        _releaseRender.notify_all();
      }
      std::size_t renderCalls() const noexcept { return _renderCalls.load(std::memory_order_relaxed); }
      std::size_t drainCompletions() const noexcept { return _drainCompletions.load(std::memory_order_relaxed); }
      std::size_t errors() const noexcept { return _errors.load(std::memory_order_relaxed); }

    private:
      std::atomic<std::size_t> _renderCalls{0U};
      std::atomic<std::size_t> _drainCompletions{0U};
      std::atomic<std::size_t> _errors{0U};
      std::counting_semaphore<> _renderEntered{0};
      std::atomic_bool _releaseRender{false};
    };

    Device requireNativeTestOutput()
    {
      auto const requestedRes = audio::test::requestedCoreAudioTestOutput();

      // A present but unresolvable UID is a configuration error, not a device skip.
      if (!requestedRes)
      {
        FAIL("AOBUS_TEST_COREAUDIO_DEVICE_UID: " << requestedRes.error().message);
      }

      if (auto const& optDevice = *requestedRes; optDevice)
      {
        return *optDevice;
      }

      auto const devices = detail::enumerateCoreAudioOutputDevices();

      if (devices.empty())
      {
        SKIP("macOS host has no live Core Audio output device");
      }

      return devices.front();
    }
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

  TEST_CASE("CoreAudioBackend - an unrepresentable source is refused as a track incompatibility before native access",
            "[audio][unit][coreaudio][backend]")
  {
    // The UID names no native device, so a regressed guard could only surface
    // a device lookup failure here, never an audio unit.
    auto backend = CoreAudioBackend{
      Device{.id = DeviceId{"synthetic-uid"}, .displayName = "Synthetic Output", .backendId = kBackendCoreAudio},
      kProfileShared};
    auto target = ao::audio::test::NoopRenderTarget{};

    SECTION("a 33-bit integer signal has no lossless encoding at all")
    {
      auto const openedRes = backend.open(
        {.sampleRate = 44100, .channels = 2, .precisionBits = 33, .sampleKind = SampleKind::Integer}, target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::FormatRejected);
      CHECK(openedRes.error().error.message == "No lossless Core Audio client PCM encoding is available");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
    }

    SECTION("a non-32-bit float signal has no lossless encoding at all")
    {
      auto const openedRes = backend.open(
        {.sampleRate = 44100, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::FloatingPoint}, target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::FormatRejected);
      CHECK(openedRes.error().error.message == "No lossless Core Audio client PCM encoding is available");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
    }
  }

  TEST_CASE("CoreAudioBackend - zero geometry is refused as invalid input before native access",
            "[audio][unit][coreaudio][backend]")
  {
    // The UID names no native device, so a regressed guard could only surface
    // a device lookup failure here, never an audio unit.
    auto backend = CoreAudioBackend{
      Device{.id = DeviceId{"synthetic-uid"}, .displayName = "Synthetic Output", .backendId = kBackendCoreAudio},
      kProfileShared};
    auto target = ao::audio::test::NoopRenderTarget{};

    SECTION("a zero sample rate is invalid geometry")
    {
      auto const sourceFormat =
        SignalFormat{.sampleRate = 0, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::Integer};
      auto const openedRes = backend.open(sourceFormat, target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::InvalidInput);
      CHECK(openedRes.error().error.message == "Core Audio PCM formats require a sample rate and channels");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
      CHECK_FALSE(backend.prewarmFormatHint(sourceFormat).has_value());
    }

    SECTION("a zero channel count is invalid geometry")
    {
      auto const sourceFormat =
        SignalFormat{.sampleRate = 44100, .channels = 0, .precisionBits = 16, .sampleKind = SampleKind::Integer};
      auto const openedRes = backend.open(sourceFormat, target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::InvalidInput);
      CHECK(openedRes.error().error.message == "Core Audio PCM formats require a sample rate and channels");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
      CHECK_FALSE(backend.prewarmFormatHint(sourceFormat).has_value());
    }
  }

  TEST_CASE("CoreAudioBackend - geometry refusal outranks precision refusal", "[audio][unit][coreaudio][backend]")
  {
    auto backend = CoreAudioBackend{
      Device{.id = DeviceId{"synthetic-uid"}, .displayName = "Synthetic Output", .backendId = kBackendCoreAudio},
      kProfileShared};
    auto target = ao::audio::test::NoopRenderTarget{};

    SECTION("an unrepresentable source with no channels still reports the geometry refusal")
    {
      auto const openedRes = backend.open(
        {.sampleRate = 44100, .channels = 0, .precisionBits = 33, .sampleKind = SampleKind::Integer}, target);

      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().error.code == Error::Code::InvalidInput);
      CHECK(openedRes.error().error.message == "Core Audio PCM formats require a sample rate and channels");
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
    }
  }

  TEST_CASE("CoreAudioBackend - prewarm predicts the lossless mode without native negotiation",
            "[audio][unit][coreaudio][backend]")
  {
    // prewarmFormatHint is non-blocking and performs no native open or
    // negotiation, so a synthetic device observes only local admission policy.
    auto backend = CoreAudioBackend{
      Device{.id = DeviceId{"synthetic-uid"}, .displayName = "Synthetic Output", .backendId = kBackendCoreAudio},
      kProfileShared};

    SECTION("an ordinary representable source retains its precision")
    {
      auto const optHint = backend.prewarmFormatHint(
        {.sampleRate = 44100, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::Integer});

      REQUIRE(optHint);
      CHECK(optHint->sampleRate == 44100U);
      CHECK(optHint->channels == 2U);
      CHECK(optHint->encoding == SampleEncoding::Signed16Le);
    }
  }

  TEST_CASE("CoreAudioBackend - native AUHAL open confirms the exact lossless client stream",
            "[audio][integration][coreaudio][backend]")
  {
    auto const device = requireNativeTestOutput();
    CAPTURE(device.id.raw());

    auto target = ao::audio::test::NoopRenderTarget{};
    auto backend = CoreAudioBackend{device, kProfileShared};
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
    auto const device = requireNativeTestOutput();
    CAPTURE(device.id.raw());

    constexpr auto kSourceFormats = std::array{
      SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 24, .sampleKind = SampleKind::Integer},
      SignalFormat{.sampleRate = 96000, .channels = 2, .precisionBits = 24, .sampleKind = SampleKind::Integer}};
    auto target = ao::audio::test::NoopRenderTarget{};
    auto backend = CoreAudioBackend{device, kProfileShared};

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
            "[audio][integration][coreaudio][backend][concurrency]")
  {
    auto const device = requireNativeTestOutput();
    CAPTURE(device.id.raw());

    auto target = DrainingRenderTarget{};
    auto backend = CoreAudioBackend{device, kProfileShared};
    auto const openedRes = backend.open(
      {.sampleRate = 44100, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::Integer}, target);
    REQUIRE(openedRes);

    backend.start();
    REQUIRE(target.tryWaitForDrain(std::chrono::seconds{5}));
    CHECK(target.renderCalls() == 1U);
    CHECK(target.errors() == 0U);

    backend.close();
    CHECK(target.renderCalls() == 1U);
    CHECK(target.drainCompletions() == 1U);
    CHECK(target.errors() == 0U);
  }

  TEST_CASE("CoreAudioBackend - graph observer may close backend without recursive clear",
            "[audio][integration][coreaudio][backend]")
  {
    auto const device = requireNativeTestOutput();
    CAPTURE(device.id.raw());

    std::size_t graphCalls = 0;
    auto graphs = std::vector<flow::Graph>{};
    auto graphRegistryPtr = std::make_shared<detail::BackendGraphRegistry>();
    auto target = ao::audio::test::NoopRenderTarget{};
    auto backend = CoreAudioBackend{device, kProfileShared, graphRegistryPtr};
    REQUIRE(backend.open(
      {.sampleRate = 44100, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::Integer}, target));

    auto sub = graphRegistryPtr->subscribe(device.id.raw(),
                                           [&](flow::Graph const& graph)
                                           {
                                             ++graphCalls;
                                             graphs.push_back(graph);

                                             if (!graph.nodes.empty())
                                             {
                                               backend.close();
                                             }
                                           });

    REQUIRE(sub);
    CHECK(graphCalls == 2U);
    REQUIRE(graphs.size() == 2U);
    REQUIRE(graphs[0].nodes.size() == 2U);
    CHECK(graphs[0].nodes[0].id == device.id.raw() + ":client");
    CHECK(graphs[0].nodes[1].id == device.id.raw());
    CHECK(graphs[1] == flow::Graph{});
    CHECK_FALSE(backend.queryProperty(PropertyId::Volume).isAvailable);

    backend.close();
    CHECK(graphCalls == 2U);
  }

  TEST_CASE("CoreAudioBackend - stop fences a blocked render and cancels its drain",
            "[audio][integration][coreaudio][backend][concurrency]")
  {
    auto const device = requireNativeTestOutput();
    CAPTURE(device.id.raw());

    auto target = BlockingDrainingRenderTarget{};
    auto backend = CoreAudioBackend{device, kProfileShared};
    auto stopEntered = std::binary_semaphore{0};
    auto stopReturned = std::binary_semaphore{0};
    auto stopThread = std::jthread{};
    auto cleanup = utility::ScopedRegistration{[&]
                                               {
                                                 target.releaseRender();

                                                 if (stopThread.joinable())
                                                 {
                                                   stopThread.join();
                                                 }
                                               }};
    REQUIRE(backend.open(
      {.sampleRate = 44100, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::Integer}, target));
    backend.start();
    REQUIRE(target.tryWaitForRender(std::chrono::seconds{5}));

    stopThread = std::jthread{[&]
                              {
                                stopEntered.release();
                                backend.stop();
                                stopReturned.release();
                              }};
    REQUIRE(stopEntered.try_acquire_for(std::chrono::seconds{5}));
    CHECK_FALSE(stopReturned.try_acquire());

    target.releaseRender();
    REQUIRE(stopReturned.try_acquire_for(std::chrono::seconds{5}));
    cleanup.reset();
    backend.close();

    CHECK(target.renderCalls() == 1U);
    CHECK(target.drainCompletions() == 0U);
    CHECK(target.errors() == 0U);
  }
} // namespace ao::audio::backend::test
