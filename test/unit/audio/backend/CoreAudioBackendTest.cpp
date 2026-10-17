// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/CoreAudioBackend.h"

#include "lib/audio/backend/detail/BackendGraphRegistry.h"
#include "lib/audio/backend/detail/CoreAudioDeviceDiscovery.h"
#include "lib/audio/detail/DecoderOutput.h"
#include "test/unit/audio/BackendTestSupport.h"
#include "test/unit/audio/backend/CoreAudioTestSupport.h"
#include <ao/Error.h>
#include <ao/audio/Backend.h>
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

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
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
        _renderReturns.fetch_add(1U, std::memory_order_release);
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
      std::size_t renderReturns() const noexcept { return _renderReturns.load(std::memory_order_acquire); }
      std::size_t drainCompletions() const noexcept { return _drainCompletions.load(std::memory_order_relaxed); }
      std::size_t errors() const noexcept { return _errors.load(std::memory_order_relaxed); }

    private:
      std::atomic<std::size_t> _renderCalls{0U};
      std::atomic<std::size_t> _renderReturns{0U};
      std::atomic<std::size_t> _drainCompletions{0U};
      std::atomic<std::size_t> _errors{0U};
      std::counting_semaphore<> _renderEntered{0};
      std::atomic_bool _releaseRender{false};
    };

    class OpenNotificationRenderTarget final : public RenderTarget
    {
    public:
      OpenNotificationRenderTarget(std::function<void(std::string_view)> routeReady,
                                   std::function<void(PcmFormat const&)> formatChanged,
                                   RenderTarget* renderDelegate = nullptr)
        : _routeReady{std::move(routeReady)}, _formatChanged{std::move(formatChanged)}, _renderDelegate{renderDelegate}
      {
      }

      RenderPcmResult renderPcm(std::span<std::byte> output) noexcept override
      {
        return _renderDelegate != nullptr ? _renderDelegate->renderPcm(output) : RenderPcmResult{.drained = true};
      }
      void handleUnderrun() noexcept override
      {
        if (_renderDelegate != nullptr)
        {
          _renderDelegate->handleUnderrun();
        }
      }
      void handlePositionAdvanced(std::uint32_t const frames) noexcept override
      {
        if (_renderDelegate != nullptr)
        {
          _renderDelegate->handlePositionAdvanced(frames);
        }
      }
      void handleDrainComplete() noexcept override
      {
        if (_renderDelegate != nullptr)
        {
          _renderDelegate->handleDrainComplete();
        }
      }
      void handleRouteReady(std::string_view const routeAnchor) noexcept override { _routeReady(routeAnchor); }
      void handleFormatChanged(PcmFormat const& format) noexcept override { _formatChanged(format); }
      void handlePropertyChanged(PropertySnapshot snapshot) noexcept override
      {
        if (_renderDelegate != nullptr)
        {
          _renderDelegate->handlePropertyChanged(std::move(snapshot));
        }
      }
      void handleBackendError(std::string_view const message) noexcept override
      {
        if (_renderDelegate != nullptr)
        {
          _renderDelegate->handleBackendError(message);
        }
      }

    private:
      std::function<void(std::string_view)> _routeReady;
      std::function<void(PcmFormat const&)> _formatChanged;
      // Optional test-owned delegate remains alive through backend settlement.
      RenderTarget* _renderDelegate = nullptr;
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

  TEST_CASE("CoreAudioBackend - open publication may close the native owner without resurrecting its graph",
            "[audio][integration][coreaudio][backend]")
  {
    auto const device = requireNativeTestOutput();
    CAPTURE(device.id.raw());

    auto graphs = std::vector<flow::Graph>{};
    auto graphRegistryPtr = std::make_shared<detail::BackendGraphRegistry>();
    auto target = ao::audio::test::NoopRenderTarget{};
    auto backend = CoreAudioBackend{device, kProfileShared, graphRegistryPtr};
    auto sub = graphRegistryPtr->subscribe(device.id.raw(),
                                           [&](flow::Graph const& graph)
                                           {
                                             graphs.push_back(graph);

                                             if (!graph.nodes.empty())
                                             {
                                               backend.close();
                                             }
                                           });
    REQUIRE(sub);
    REQUIRE(backend.open(
      {.sampleRate = 44100, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::Integer}, target));

    REQUIRE(graphs.size() == 3U);
    CHECK(graphs[0] == flow::Graph{});
    REQUIRE(graphs[1].nodes.size() == 2U);
    CHECK(graphs[1].nodes[0].id == device.id.raw() + ":client");
    CHECK(graphs[1].nodes[1].id == device.id.raw());
    CHECK(graphs[2] == flow::Graph{});
    CHECK_FALSE(backend.queryProperty(PropertyId::Volume).isAvailable);

    auto latest = flow::Graph{};
    auto lateSub = graphRegistryPtr->subscribe(device.id.raw(), [&](flow::Graph const& graph) { latest = graph; });
    CHECK(latest == flow::Graph{});
    backend.close();
    CHECK(graphs.size() == 3U);
  }

  TEST_CASE("CoreAudioBackend - property graph publication is handed off before observer-driven close",
            "[audio][integration][coreaudio][backend][concurrency]")
  {
    auto const device = requireNativeTestOutput();
    CAPTURE(device.id.raw());

    auto const setterThread = std::this_thread::get_id();
    auto publicationThread = std::thread::id{};
    auto cleared = std::binary_semaphore{0};
    auto graphs = std::vector<flow::Graph>{};
    auto setRes = Result<>{};
    bool requested = false;
    bool closeRequested = false;
    auto graphRegistryPtr = std::make_shared<detail::BackendGraphRegistry>();
    auto target = ao::audio::test::NoopRenderTarget{};
    auto backend = CoreAudioBackend{device, kProfileShared, graphRegistryPtr};
    REQUIRE(backend.open(
      {.sampleRate = 44100, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::Integer}, target));

    // Initial subscription delivery is synchronous. Requesting a property here
    // must not wait for a second graph delivery under this callback's gate.
    auto sub =
      graphRegistryPtr->subscribe(device.id.raw(),
                                  [&](flow::Graph const& graph)
                                  {
                                    if (!requested)
                                    {
                                      requested = true;
                                      setRes = backend.set(props::kVolume, 0.25F);
                                      return;
                                    }

                                    if (graph.nodes.empty())
                                    {
                                      if (closeRequested)
                                      {
                                        graphs.push_back(graph);
                                        cleared.release();
                                      }

                                      return;
                                    }

                                    auto const sink = std::ranges::find(graph.nodes, device.id.raw(), &flow::Node::id);

                                    if (!closeRequested && sink != graph.nodes.end() && sink->maxSoftwareGain == 0.25F)
                                    {
                                      publicationThread = std::this_thread::get_id();
                                      graphs.push_back(graph);
                                      closeRequested = true;
                                      backend.close();
                                    }
                                  });

    REQUIRE(sub);
    REQUIRE(requested);
    REQUIRE(setRes);
    REQUIRE(cleared.try_acquire_for(std::chrono::seconds{5}));
    // Cancellation also fences the worker before inspecting its captured payload.
    sub.reset();
    CHECK(publicationThread != setterThread);
    REQUIRE(graphs.size() == 2U);
    auto const sink = std::ranges::find(graphs[0].nodes, device.id.raw(), &flow::Node::id);
    REQUIRE(sink != graphs[0].nodes.end());
    CHECK(sink->maxSoftwareGain == 0.25F);
    CHECK(graphs[1] == flow::Graph{});
    CHECK_FALSE(backend.queryProperty(PropertyId::Volume).isAvailable);
  }

  TEST_CASE("CoreAudioBackend - property observer close settles an active open notification before returning",
            "[audio][integration][coreaudio][backend][concurrency]")
  {
    auto const device = requireNativeTestOutput();
    CAPTURE(device.id.raw());

    auto events = std::vector<std::string>{};
    auto eventMutex = std::mutex{};
    auto recordEvent = [&](std::string event)
    {
      auto const lock = std::scoped_lock{eventMutex};
      events.push_back(std::move(event));
    };
    auto routeActive = std::atomic_bool{false};
    auto releaseRoute = std::atomic_bool{false};
    auto closeAttempted = std::binary_semaphore{0};
    auto closeReturned = std::binary_semaphore{0};
    bool closeEnteredWithActiveRoute = false;
    bool closeReturnedWithActiveRoute = false;
    bool closeRequested = false;
    auto routeAnchor = std::string{};
    auto setRes = Result<>{};
    auto optOpenedRes = std::optional<BackendOpenResult>{};
    auto graphRegistryPtr = std::make_shared<detail::BackendGraphRegistry>();
    auto backend = CoreAudioBackend{device, kProfileShared, graphRegistryPtr};
    auto target = OpenNotificationRenderTarget{[&](std::string_view const anchor)
                                               {
                                                 routeAnchor = anchor;
                                                 routeActive.store(true, std::memory_order_release);
                                                 recordEvent("route entered");
                                                 setRes = backend.set(props::kVolume, 0.25F);
                                                 releaseRoute.wait(false);
                                                 recordEvent("route returning");
                                                 routeActive.store(false, std::memory_order_release);
                                               },
                                               [&](PcmFormat const&) { recordEvent("format changed"); }};
    auto sub =
      graphRegistryPtr->subscribe(device.id.raw(),
                                  [&](flow::Graph const& graph)
                                  {
                                    auto const sink = std::ranges::find(graph.nodes, device.id.raw(), &flow::Node::id);

                                    if (!closeRequested && sink != graph.nodes.end() && sink->maxSoftwareGain == 0.25F)
                                    {
                                      closeRequested = true;
                                      closeEnteredWithActiveRoute = routeActive.load(std::memory_order_acquire);
                                      recordEvent("close requested");
                                      closeAttempted.release();
                                      backend.close();
                                      closeReturnedWithActiveRoute = routeActive.load(std::memory_order_acquire);
                                      recordEvent("close returned");
                                      closeReturned.release();
                                    }
                                  });
    REQUIRE(sub);
    auto opener = std::jthread{};
    // Keep the target and every callback capture alive through both settlements,
    // including assertion failure. No target is released just to expose a UAF.
    auto cleanup = utility::ScopedRegistration{[&]
                                               {
                                                 releaseRoute.store(true);
                                                 releaseRoute.notify_all();

                                                 if (opener.joinable())
                                                 {
                                                   opener.join();
                                                 }

                                                 backend.close();
                                               }};
    opener = std::jthread{
      [&]
      {
        optOpenedRes = backend.open(
          {.sampleRate = 44100, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::Integer}, target);
      }};
    REQUIRE(closeAttempted.try_acquire_for(std::chrono::seconds{5}));
    releaseRoute.store(true);
    releaseRoute.notify_all();
    opener.join();
    REQUIRE(closeReturned.try_acquire_for(std::chrono::seconds{5}));
    sub.reset();
    cleanup.reset();

    REQUIRE(optOpenedRes);
    REQUIRE(*optOpenedRes);
    REQUIRE(setRes);
    CHECK(routeAnchor == device.id.raw());
    CHECK(closeEnteredWithActiveRoute);
    CHECK_FALSE(closeReturnedWithActiveRoute);
    auto const routeReturn = std::ranges::find(events, "route returning");
    auto const closeReturn = std::ranges::find(events, "close returned");
    REQUIRE(routeReturn != events.end());
    REQUIRE(closeReturn != events.end());
    CHECK(routeReturn < closeReturn);
    auto const formatChanged = std::ranges::find(events, "format changed");

    // A notification admitted before close's final retirement may finish before
    // close; none may begin after it returns. The target remains alive throughout.
    if (formatChanged != events.end())
    {
      CHECK(formatChanged < closeReturn);
    }

    CHECK_FALSE(backend.queryProperty(PropertyId::Volume).isAvailable);
    auto latest = flow::Graph{};
    auto lateSub = graphRegistryPtr->subscribe(device.id.raw(), [&](flow::Graph const& graph) { latest = graph; });
    CHECK(latest == flow::Graph{});
  }

  TEST_CASE("CoreAudioBackend - observer close quiesces rendering restarted by an admitted open notification",
            "[audio][integration][coreaudio][backend][concurrency]")
  {
    auto const device = requireNativeTestOutput();
    CAPTURE(device.id.raw());

    auto renderTarget = BlockingDrainingRenderTarget{};
    auto closeIntent = std::atomic_bool{false};
    auto closeCompleted = std::atomic_bool{false};
    auto releaseRoute = std::atomic_bool{false};
    auto routeCompleted = std::atomic_bool{false};
    auto callbackAfterClose = std::atomic_bool{false};
    auto closeReturned = std::binary_semaphore{0};
    bool closeRequested = false;
    bool routeCompletedAtClose = false;
    std::size_t renderReturnsAtClose = 0U;
    auto routeAnchor = std::string{};
    auto formats = std::vector<PcmFormat>{};
    auto setRes = Result<>{};
    auto optOpenedRes = std::optional<BackendOpenResult>{};
    auto graphRegistryPtr = std::make_shared<detail::BackendGraphRegistry>();
    auto backendPtr = std::make_unique<CoreAudioBackend>(device, kProfileShared, graphRegistryPtr);
    auto target = OpenNotificationRenderTarget{[&](std::string_view const anchor)
                                               {
                                                 if (closeCompleted.load(std::memory_order_acquire))
                                                 {
                                                   callbackAfterClose.store(true, std::memory_order_release);
                                                 }

                                                 routeAnchor = anchor;
                                                 setRes = backendPtr->set(props::kVolume, 0.25F);
                                                 closeIntent.wait(false);
                                                 // This is non-render callback reentry, not a competing application
                                                 // command. Start may itself block in the SDK; the driver can release
                                                 // both gates.
                                                 backendPtr->start();
                                                 releaseRoute.wait(false);
                                                 routeCompleted.store(true, std::memory_order_release);
                                               },
                                               [&](PcmFormat const& format)
                                               {
                                                 if (closeCompleted.load(std::memory_order_acquire))
                                                 {
                                                   callbackAfterClose.store(true, std::memory_order_release);
                                                 }

                                                 formats.push_back(format);
                                               },
                                               &renderTarget};
    auto sub =
      graphRegistryPtr->subscribe(device.id.raw(),
                                  [&](flow::Graph const& graph)
                                  {
                                    auto const sink = std::ranges::find(graph.nodes, device.id.raw(), &flow::Node::id);

                                    if (!closeRequested && sink != graph.nodes.end() && sink->maxSoftwareGain == 0.25F)
                                    {
                                      closeRequested = true;
                                      // Observe close intent, not an internal fence/mutex admission point.
                                      closeIntent.store(true);
                                      closeIntent.notify_all();
                                      backendPtr->close();
                                      routeCompletedAtClose = routeCompleted.load(std::memory_order_acquire);
                                      renderReturnsAtClose = renderTarget.renderReturns();
                                      closeCompleted.store(true, std::memory_order_release);
                                      closeReturned.release();
                                    }
                                  });
    auto opener = std::jthread{};
    auto cleanup = utility::ScopedRegistration{[&]
                                               {
                                                 closeIntent.store(true);
                                                 closeIntent.notify_all();
                                                 releaseRoute.store(true);
                                                 releaseRoute.notify_all();
                                                 renderTarget.releaseRender();

                                                 if (opener.joinable())
                                                 {
                                                   opener.join();
                                                 }

                                                 sub.reset();

                                                 if (backendPtr)
                                                 {
                                                   backendPtr->close();
                                                   backendPtr.reset();
                                                 }
                                               }};
    REQUIRE(sub);
    opener = std::jthread{
      [&]
      {
        optOpenedRes = backendPtr->open(
          {.sampleRate = 44100, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::Integer}, target);
      }};
    REQUIRE(renderTarget.tryWaitForRender(std::chrono::seconds{5}));
    // Release route-ready before the native renderer. If SDK start is still
    // awaiting its first callback, the route gate is already open on its return.
    releaseRoute.store(true);
    releaseRoute.notify_all();
    renderTarget.releaseRender();
    opener.join();
    REQUIRE(closeReturned.try_acquire_for(std::chrono::seconds{5}));
    sub.reset();
    REQUIRE_FALSE(backendPtr->queryProperty(PropertyId::Volume).isAvailable);
    cleanup.reset();

    REQUIRE(optOpenedRes);
    REQUIRE(*optOpenedRes);
    REQUIRE(setRes);
    CHECK(routeAnchor == device.id.raw());
    CHECK(closeRequested);
    CHECK(routeCompletedAtClose);
    CHECK(renderReturnsAtClose == 1U);
    CHECK(renderTarget.renderCalls() == 1U);
    CHECK(renderTarget.renderReturns() == 1U);
    CHECK(renderTarget.drainCompletions() == 0U);
    CHECK(renderTarget.errors() == 0U);
    CHECK(formats.empty());
    CHECK_FALSE(callbackAfterClose.load(std::memory_order_acquire));
    auto latest = flow::Graph{};
    auto lateSub = graphRegistryPtr->subscribe(device.id.raw(), [&](flow::Graph const& graph) { latest = graph; });
    CHECK(latest == flow::Graph{});
  }

  TEST_CASE("CoreAudioBackend - retiring the open generation in route ready suppresses its followup notification",
            "[audio][integration][coreaudio][backend]")
  {
    auto const device = requireNativeTestOutput();
    CAPTURE(device.id.raw());

    auto routeAnchors = std::vector<std::string>{};
    auto formats = std::vector<PcmFormat>{};
    auto backend = CoreAudioBackend{device, kProfileShared};
    auto target = OpenNotificationRenderTarget{[&](std::string_view const anchor)
                                               {
                                                 routeAnchors.emplace_back(anchor);
                                                 // stop retires the generation but does not revoke this same target or
                                                 // wait for non-render notifications. This is not callback-origin
                                                 // close.
                                                 backend.stop();
                                               },
                                               [&](PcmFormat const& format) { formats.push_back(format); }};
    auto const openedRes = backend.open(
      {.sampleRate = 44100, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::Integer}, target);
    backend.close();

    REQUIRE(openedRes);
    CHECK(routeAnchors == std::vector<std::string>{device.id.raw()});
    CHECK(formats.empty());
    CHECK_FALSE(backend.queryProperty(PropertyId::Volume).isAvailable);
  }

  TEST_CASE("CoreAudioBackend - registry retirement on native publication is synchronous and leaves close inert",
            "[audio][integration][coreaudio][backend][concurrency]")
  {
    auto const device = requireNativeTestOutput();
    CAPTURE(device.id.raw());

    auto graphs = std::vector<flow::Graph>{};
    std::size_t callsAtRetirement = 0U;
    auto graphRegistryPtr = std::make_shared<detail::BackendGraphRegistry>();
    auto target = ao::audio::test::NoopRenderTarget{};
    auto backendPtr = std::make_unique<CoreAudioBackend>(device, kProfileShared, graphRegistryPtr);
    auto sub = graphRegistryPtr->subscribe(device.id.raw(),
                                           [&](flow::Graph const& graph)
                                           {
                                             graphs.push_back(graph);

                                             if (!graph.nodes.empty())
                                             {
                                               graphRegistryPtr->shutdown();
                                               callsAtRetirement = graphs.size();
                                               backendPtr->close();
                                             }
                                           });
    REQUIRE(sub);
    REQUIRE(backendPtr->open(
      {.sampleRate = 44100, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::Integer}, target));

    CHECK(callsAtRetirement == 3U);
    REQUIRE(graphs.size() == 3U);
    CHECK(graphs[0] == flow::Graph{});
    REQUIRE(graphs[1].nodes.size() == 2U);
    CHECK(graphs[1].nodes[1].id == device.id.raw());
    CHECK(graphs[2] == flow::Graph{});
    CHECK_FALSE(backendPtr->queryProperty(PropertyId::Volume).isAvailable);
    REQUIRE(backendPtr->set(props::kMuted, true));
    backendPtr->close();
    // Destruction joins the existing worker, including any late property request.
    backendPtr.reset();
    CHECK(graphs.size() == callsAtRetirement);
    CHECK_FALSE(graphRegistryPtr->subscribe(device.id.raw(), [](flow::Graph const&) {}));
    sub.reset();
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
