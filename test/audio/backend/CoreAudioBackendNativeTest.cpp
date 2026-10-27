// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/CoreAudioBackend.h"
#include "lib/audio/backend/detail/BackendGraphRegistry.h"
#include "lib/audio/detail/DecoderOutput.h"
#include "test/audio/BackendTestSupport.h"
#include "test/audio/backend/CoreAudioTestSupport.h"
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
#include <variant>
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
      auto const selectedRes = audio::test::selectCoreAudioTestOutput();

      // A present but unresolvable UID is a configuration error, not a device skip.
      if (!selectedRes)
      {
        FAIL("AOBUS_TEST_COREAUDIO_DEVICE_UID: " << selectedRes.error().message);
      }

      // An unset UID on a host with no live Core Audio output is the only
      // genuine absence boundary; there is never a substitute device.
      if (!*selectedRes)
      {
        SKIP("macOS host has no live Core Audio output device");
      }

      return **selectedRes;
    }
  } // namespace

  TEST_CASE("CoreAudioBackend - native AUHAL open retains canonical identities in the exact lossless client stream")
  {
    auto const device = requireNativeTestOutput();
    CAPTURE(device.id.raw());
    auto target = ao::audio::test::NoopRenderTarget{};
    auto graphRegistryPtr = std::make_shared<detail::BackendGraphRegistry>();
    auto backend = CoreAudioBackend{device, kProfileShared, graphRegistryPtr};
    auto const identified =
      PcmFormat{.sampleRate = 44100, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x3};
    auto const openedRes = backend.open(
      {.sampleRate = 44100, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::Integer, .speakerMask = 0x3},
      target);
    REQUIRE(openedRes);
    CHECK(openedRes->clientFormat.sampleRate == 44100U);
    CHECK(openedRes->clientFormat.speakerMask == 0x3U);
    CHECK(openedRes->clientFormat.channels == 2U);
    CHECK(openedRes->clientFormat.encoding == SampleEncoding::Signed16Le);
    CHECK_FALSE(openedRes->optEndpoint);
    CHECK_FALSE(openedRes->channelDelivery.isMapped);
    CHECK((openedRes->channelDelivery.nativeSpeakerMask == 0U || openedRes->channelDelivery.nativeSpeakerMask == 0x3U));
    auto captureNativeGraph = [&]
    {
      auto graph = flow::Graph{};
      auto sub = graphRegistryPtr->subscribe(device.id.raw(), [&](flow::Graph const& updated) { graph = updated; });
      REQUIRE(sub);
      sub.reset(); // Fence native worker delivery before inspecting its copy.
      return graph;
    };
    auto const nativeGraph = captureNativeGraph();
    REQUIRE(nativeGraph.nodes.size() == 2U);
    auto const& nativeNode = nativeGraph.nodes.front();
    REQUIRE(nativeNode.optFormat);
    REQUIRE(std::holds_alternative<PcmFormat>(*nativeNode.optFormat));
    auto const nativeFormat = std::get<PcmFormat>(*nativeNode.optFormat);
    CHECK(nativeFormat.sampleRate == identified.sampleRate);
    CHECK(nativeFormat.channels == identified.channels);
    CHECK(nativeFormat.encoding == identified.encoding);
    CHECK(openedRes->clientFormat == identified);
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

  TEST_CASE("CoreAudioBackend - native AUHAL open confirms the exact lossless client stream")
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

  TEST_CASE("CoreAudioBackend - native AUHAL keeps common high-resolution clients lossless")
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

  TEST_CASE("CoreAudioBackend - native drain renders once and completes after the presentation tail", "[concurrency]")
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

  TEST_CASE("CoreAudioBackend - start after a completed close delivers no callbacks and resurrects no graph",
            "[concurrency]")
  {
    auto const device = requireNativeTestOutput();
    CAPTURE(device.id.raw());

    auto target = DrainingRenderTarget{};
    auto graphs = std::vector<flow::Graph>{};
    auto graphRegistryPtr = std::make_shared<detail::BackendGraphRegistry>();
    auto backend = CoreAudioBackend{device, kProfileShared, graphRegistryPtr};
    auto sub = graphRegistryPtr->subscribe(device.id.raw(), [&](flow::Graph const& graph) { graphs.push_back(graph); });

    REQUIRE(backend.open(
      {.sampleRate = 44100, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::Integer}, target));
    backend.start();
    REQUIRE(target.tryWaitForDrain(std::chrono::seconds{5}));
    backend.close();

    // The target, its captures, and the backend stay alive through the inert
    // start; a resurrected callback would have to land on this target.
    auto const renderCallsAtClose = target.renderCalls();
    auto const drainCompletionsAtClose = target.drainCompletions();
    auto const errorsAtClose = target.errors();
    auto const graphCallsAtClose = graphs.size();
    REQUIRE(renderCallsAtClose == 1U);
    CHECK(drainCompletionsAtClose == 1U);
    CHECK(errorsAtClose == 0U);
    CHECK_FALSE(backend.queryProperty(PropertyId::Volume).isAvailable);

    backend.start();

    CHECK(target.renderCalls() == renderCallsAtClose);
    CHECK(target.drainCompletions() == drainCompletionsAtClose);
    CHECK(target.errors() == errorsAtClose);
    CHECK(graphs.size() == graphCallsAtClose);
    CHECK_FALSE(backend.queryProperty(PropertyId::Volume).isAvailable);

    auto latest = flow::Graph{};
    auto lateSub = graphRegistryPtr->subscribe(device.id.raw(), [&](flow::Graph const& graph) { latest = graph; });
    CHECK(latest == flow::Graph{});
  }

  TEST_CASE("CoreAudioBackend - graph observer may close backend without recursive clear")
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

  TEST_CASE("CoreAudioBackend - open publication may close the native owner without resurrecting its graph")
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

  TEST_CASE("CoreAudioBackend - a route-ready notification may operate another backend's synthetic route")
  {
    auto const device = requireNativeTestOutput();
    CAPTURE(device.id.raw());

    auto graphRegistryPtr = std::make_shared<detail::BackendGraphRegistry>();
    // Backend B's synthetic UID names no native device, so its refused open can
    // never reach an audio unit; only the notifying backend touches native hardware.
    // Both operations avoid graph delivery. A different backend identity alone
    // would not permit waiting for this shared registry's gate from the callback.
    auto const refusedSource =
      SignalFormat{.sampleRate = 44100, .channels = 2, .precisionBits = 33, .sampleKind = SampleKind::Integer};
    auto targetB = ao::audio::test::NoopRenderTarget{};
    auto backendB = CoreAudioBackend{
      Device{.id = DeviceId{"synthetic-uid-b"}, .displayName = "Synthetic Output B", .backendId = kBackendCoreAudio},
      kProfileShared,
      graphRegistryPtr};
    auto routeAnchor = std::string{};
    auto formats = std::vector<PcmFormat>{};
    auto optBackendBOpenRes = std::optional<BackendOpenResult>{};
    bool backendBClosed = false;
    auto backend = CoreAudioBackend{device, kProfileShared, graphRegistryPtr};
    auto target = OpenNotificationRenderTarget{[&](std::string_view const anchor)
                                               {
                                                 routeAnchor = anchor;
                                                 optBackendBOpenRes = backendB.open(refusedSource, targetB);
                                                 backendB.close();
                                                 backendBClosed = true;
                                               },
                                               [&](PcmFormat const& format) { formats.push_back(format); }};
    // An assertion failure must not unwind the borrowed targets before the
    // owning backends settle: this cleanup closes both backends while every
    // target and capture is still alive.
    auto cleanup = utility::ScopedRegistration{[&]
                                               {
                                                 backendB.close();
                                                 backend.close();
                                               }};
    auto const openedRes = backend.open(
      {.sampleRate = 44100, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::Integer}, target);

    REQUIRE(openedRes);
    CHECK(openedRes->clientFormat.sampleRate == 44100U);
    CHECK(openedRes->clientFormat.channels == 2U);
    CHECK(openedRes->clientFormat.encoding == SampleEncoding::Signed16Le);
    CHECK(routeAnchor == device.id.raw());
    REQUIRE(formats.size() == 1U);
    CHECK(formats[0].sampleRate == 44100U);
    CHECK(formats[0].channels == 2U);
    CHECK(formats[0].encoding == SampleEncoding::Signed16Le);
    REQUIRE(backendBClosed);
    REQUIRE(optBackendBOpenRes);
    REQUIRE_FALSE(*optBackendBOpenRes);
    CHECK(optBackendBOpenRes->error().error.code == Error::Code::FormatRejected);
    CHECK(optBackendBOpenRes->error().error.message == "No lossless Core Audio client PCM encoding is available");
    CHECK(optBackendBOpenRes->error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
    // The operated backend's synthetic route stayed inert; the notifying
    // backend kept its own native route.
    CHECK_FALSE(backendB.queryProperty(PropertyId::Volume).isAvailable);
    CHECK(backend.queryProperty(PropertyId::Volume).isAvailable);

    // Both backends settle here while the targets and captures remain alive;
    // an assertion failure before this point unwinds through the same cleanup.
    cleanup.reset();
    CHECK_FALSE(backendB.queryProperty(PropertyId::Volume).isAvailable);
    CHECK_FALSE(backend.queryProperty(PropertyId::Volume).isAvailable);
    auto latest = flow::Graph{};
    auto lateSub = graphRegistryPtr->subscribe(device.id.raw(), [&](flow::Graph const& graph) { latest = graph; });
    CHECK(latest == flow::Graph{});
  }

  TEST_CASE("CoreAudioBackend - a target defers graph subscription while an observer requests stop", "[concurrency]")
  {
    auto const device = requireNativeTestOutput();
    CAPTURE(device.id.raw());

    auto const ownerThread = std::this_thread::get_id();
    auto observerThread = std::thread::id{};
    auto targetThread = std::thread::id{};
    auto deferredDeliveryThread = std::thread::id{};
    auto observerEntered = std::binary_semaphore{0};
    auto handoffReady = std::atomic_bool{false};
    auto stopRequestIssued = std::atomic_bool{false};
    auto cancelObserver = std::atomic_bool{false};
    auto observerInitialGraph = flow::Graph{};
    std::size_t stopCalls = 0U;
    auto routeAnchors = std::vector<std::string>{};
    auto formats = std::vector<PcmFormat>{};
    auto graphs = std::vector<flow::Graph>{};
    auto subscribeAfterNotification = std::function<void()>{};
    auto optOpenedRes = std::optional<BackendOpenResult>{};
    auto graphRegistryPtr = std::make_shared<detail::BackendGraphRegistry>();
    auto gateSub = utility::ScopedRegistration{};
    auto deferredSub = utility::ScopedRegistration{};
    auto backend = CoreAudioBackend{device, kProfileShared, graphRegistryPtr};
    auto target = OpenNotificationRenderTarget{[&](std::string_view const anchor)
                                               {
                                                 targetThread = std::this_thread::get_id();
                                                 routeAnchors.emplace_back(anchor);
                                                 // Record the operation without entering the shared registry's
                                                 // delivery gate from this serialized target callback.
                                                 subscribeAfterNotification = [&]
                                                 {
                                                   deferredSub = graphRegistryPtr->subscribe(
                                                     device.id.raw(),
                                                     [&](flow::Graph const& graph)
                                                     {
                                                       if (graphs.empty())
                                                       {
                                                         deferredDeliveryThread = std::this_thread::get_id();
                                                       }

                                                       graphs.push_back(graph);
                                                     });
                                                 };
                                                 handoffReady.store(true);
                                                 handoffReady.notify_all();
                                                 // Test handshake only: the observer issues this marker BEFORE
                                                 // stop(), so this waits for neither stop completion nor its
                                                 // private mutex admission. The callback must then return.
                                                 stopRequestIssued.wait(false);
                                               },
                                               [&](PcmFormat const& format) { formats.push_back(format); }};
    auto observer = std::jthread{};
    auto opener = std::jthread{};
    // Release both test waits before joining, including an early native-open
    // failure. Targets and every capture outlive all callbacks and both joins.
    auto cleanup = utility::ScopedRegistration{[&]
                                               {
                                                 cancelObserver.store(true);
                                                 handoffReady.store(true);
                                                 handoffReady.notify_all();
                                                 stopRequestIssued.store(true);
                                                 stopRequestIssued.notify_all();

                                                 if (opener.joinable())
                                                 {
                                                   opener.join();
                                                 }

                                                 if (observer.joinable())
                                                 {
                                                   observer.join();
                                                 }

                                                 gateSub.reset();
                                                 deferredSub.reset();
                                                 backend.close();
                                               }};
    observer = std::jthread{[&]
                            {
                              // A separate route avoids native publications retriggering this observer;
                              // all routes in this actual registry still share the same delivery gate.
                              gateSub = graphRegistryPtr->subscribe("coreaudio-gate-control",
                                                                    [&](flow::Graph const& graph)
                                                                    {
                                                                      observerThread = std::this_thread::get_id();
                                                                      observerInitialGraph = graph;
                                                                      observerEntered.release();
                                                                      handoffReady.wait(false);

                                                                      if (cancelObserver.load())
                                                                      {
                                                                        return;
                                                                      }

                                                                      stopRequestIssued.store(true);
                                                                      stopRequestIssued.notify_all();
                                                                      backend.stop();
                                                                      ++stopCalls;
                                                                    });
                            }};
    REQUIRE(observerEntered.try_acquire_for(std::chrono::seconds{5}));
    opener = std::jthread{
      [&]
      {
        optOpenedRes = backend.open(
          {.sampleRate = 44100, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::Integer}, target);
      }};
    opener.join();
    // On early open failure cleanup must release the observer's handoff wait
    // before joining it; do not unconditionally join that wait first.
    REQUIRE(optOpenedRes);
    REQUIRE(*optOpenedRes);
    observer.join();
    gateSub.reset();

    CHECK(optOpenedRes->value().clientFormat.sampleRate == 44100U);
    CHECK(optOpenedRes->value().clientFormat.channels == 2U);
    CHECK(optOpenedRes->value().clientFormat.encoding == SampleEncoding::Signed16Le);
    CHECK(routeAnchors == std::vector<std::string>{device.id.raw()});
    REQUIRE(formats.size() == 1U);
    CHECK(formats[0].sampleRate == 44100U);
    CHECK(formats[0].channels == 2U);
    CHECK(formats[0].encoding == SampleEncoding::Signed16Le);
    CHECK(observerInitialGraph == flow::Graph{});
    CHECK(stopCalls == 1U);
    CHECK(observerThread != targetThread);
    CHECK(backend.queryProperty(PropertyId::Volume).isAvailable);
    CHECK_FALSE(deferredSub);
    CHECK(graphs.empty());
    REQUIRE(subscribeAfterNotification);

    // The complete open-call join, not the marker inside route-ready, proves
    // that its target callbacks have returned before the owner drains the work.
    subscribeAfterNotification();
    REQUIRE(deferredSub);
    backend.close();
    // Cancellation fences every possible worker delivery before capture reads.
    deferredSub.reset();
    CHECK(deferredDeliveryThread == ownerThread);
    REQUIRE(graphs.size() == 2U);
    REQUIRE(graphs[0].nodes.size() == 2U);
    CHECK(graphs[0].nodes[0].id == device.id.raw() + ":client");
    CHECK(graphs[0].nodes[0].type == flow::NodeType::Stream);
    CHECK(graphs[0].nodes[1].id == device.id.raw());
    CHECK(graphs[0].nodes[1].type == flow::NodeType::Sink);
    REQUIRE(graphs[0].connections.size() == 1U);
    CHECK(graphs[0].connections[0].sourceId == device.id.raw() + ":client");
    CHECK(graphs[0].connections[0].destinationId == device.id.raw());
    CHECK(graphs[1] == flow::Graph{});
    CHECK_FALSE(backend.queryProperty(PropertyId::Volume).isAvailable);
    cleanup.reset();
  }

  TEST_CASE("CoreAudioBackend - property graph publication is handed off before observer-driven close", "[concurrency]")
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
            "[concurrency]")
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
    auto closeRequestedSignal = std::binary_semaphore{0};
    auto closeReturned = std::binary_semaphore{0};
    bool closeRequestedWithActiveRoute = false;
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
                                      closeRequestedWithActiveRoute = routeActive.load(std::memory_order_acquire);
                                      recordEvent("close requested");
                                      // The signal precedes the close() call: it proves the close request,
                                      // not close's admission into its serialized section.
                                      closeRequestedSignal.release();
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
    REQUIRE(closeRequestedSignal.try_acquire_for(std::chrono::seconds{5}));
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
    CHECK(closeRequestedWithActiveRoute);
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
            "[concurrency]")
  {
    auto const device = requireNativeTestOutput();
    CAPTURE(device.id.raw());

    auto renderTarget = BlockingDrainingRenderTarget{};
    auto closeRequestIssued = std::atomic_bool{false};
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
                                                 closeRequestIssued.wait(false);
                                                 // This gate is the observer's close request, not close's actual
                                                 // admission: start below may run before or while close is admitted,
                                                 // and close must quiesce the run either way. This is non-render
                                                 // callback reentry, not a competing application command. Start may
                                                 // itself block in the SDK; the driver can release both gates.
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
                                      // Signal only that the close request was issued, not close's actual
                                      // admission: the store precedes the close() call, and nothing here
                                      // observes when the backend admits it.
                                      closeRequestIssued.store(true);
                                      closeRequestIssued.notify_all();
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
                                                 closeRequestIssued.store(true);
                                                 closeRequestIssued.notify_all();
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
    // Close can retire the attachment before the next notification or settle
    // exactly one current format notification. A run-only change does not
    // invalidate that format; duplicate or different payloads are never valid.
    auto const noAdmittedFormat = std::vector<PcmFormat>{};
    auto const currentNegotiatedFormat =
      std::vector{PcmFormat{.sampleRate = 44100U, .channels = 2U, .encoding = SampleEncoding::Signed16Le}};
    CHECK((formats == noAdmittedFormat || formats == currentNegotiatedFormat));

    CHECK_FALSE(callbackAfterClose.load(std::memory_order_acquire));
    auto latest = flow::Graph{};
    auto lateSub = graphRegistryPtr->subscribe(device.id.raw(), [&](flow::Graph const& graph) { latest = graph; });
    CHECK(latest == flow::Graph{});
  }

  TEST_CASE("CoreAudioBackend - observer close quiesces rendering started by a raw graph observer", "[concurrency]")
  {
    auto const device = requireNativeTestOutput();
    CAPTURE(device.id.raw());

    auto renderTarget = BlockingDrainingRenderTarget{};
    auto graphRegistryPtr = std::make_shared<detail::BackendGraphRegistry>();
    auto backendPtr = std::make_unique<CoreAudioBackend>(device, kProfileShared, graphRegistryPtr);
    auto observerMutex = std::mutex{};
    auto observerGraphs = std::vector<flow::Graph>{};
    std::size_t startCalls = 0U;
    auto closeIntentGiven = std::binary_semaphore{0};
    std::size_t renderCallsAtClose = 0U;
    std::size_t renderReturnsAtClose = 0U;
    std::size_t drainCompletionsAtClose = 0U;
    auto sub = utility::ScopedRegistration{};
    auto subscriberThread = std::jthread{};
    auto closeThread = std::jthread{};
    // The blocked first render pass must be released before either joined
    // operation can settle; the target, backend, and every capture stay alive
    // through settlement, including assertion unwinds.
    auto cleanup = utility::ScopedRegistration{[&]
                                               {
                                                 renderTarget.releaseRender();

                                                 if (subscriberThread.joinable())
                                                 {
                                                   subscriberThread.join();
                                                 }

                                                 if (closeThread.joinable())
                                                 {
                                                   closeThread.join();
                                                 }

                                                 sub.reset();

                                                 if (backendPtr)
                                                 {
                                                   backendPtr->close();
                                                   backendPtr.reset();
                                                 }
                                               }};
    auto const openedRes = backendPtr->open(
      {.sampleRate = 44100, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::Integer}, renderTarget);
    REQUIRE(openedRes);
    CHECK(openedRes->clientFormat.sampleRate == 44100U);
    CHECK(openedRes->clientFormat.channels == 2U);
    CHECK(openedRes->clientFormat.encoding == SampleEncoding::Signed16Le);

    // The raw observer subscribes after open, so its synchronous initial
    // delivery is the published nonempty graph on the subscribing thread.
    subscriberThread =
      std::jthread{[&]
                   {
                     sub = graphRegistryPtr->subscribe(device.id.raw(),
                                                       [&](flow::Graph const& graph)
                                                       {
                                                         bool startFromObserver = false;
                                                         {
                                                           auto const lock = std::scoped_lock{observerMutex};
                                                           observerGraphs.push_back(graph);
                                                           startFromObserver = !graph.nodes.empty() && startCalls == 0U;

                                                           if (startFromObserver)
                                                           {
                                                             ++startCalls;
                                                           }
                                                         }

                                                         if (startFromObserver)
                                                         {
                                                           // Raw graph-observer reentry: this callback holds the
                                                           // registry delivery gate, so it must not wait for the
                                                           // external close. SDK start may await the first render pass,
                                                           // which the driver releases.
                                                           backendPtr->start();
                                                         }
                                                       });
                   }};
    // The captured render entry, not elapsed time, proves the run was admitted.
    REQUIRE(renderTarget.tryWaitForRender(std::chrono::seconds{5}));
    closeThread = std::jthread{[&]
                               {
                                 // Close intent only: nothing here observes or fabricates close's
                                 // internal admission point.
                                 closeIntentGiven.release();
                                 backendPtr->close();
                                 renderCallsAtClose = renderTarget.renderCalls();
                                 renderReturnsAtClose = renderTarget.renderReturns();
                                 drainCompletionsAtClose = renderTarget.drainCompletions();
                               }};
    REQUIRE(closeIntentGiven.try_acquire_for(std::chrono::seconds{5}));
    // Release without waiting for start to return: SDK start may still be
    // blocked awaiting this first render pass.
    renderTarget.releaseRender();
    subscriberThread.join();
    closeThread.join();
    sub.reset();
    CHECK_FALSE(backendPtr->queryProperty(PropertyId::Volume).isAvailable);
    backendPtr.reset();

    REQUIRE(observerGraphs.size() == 2U);
    REQUIRE(observerGraphs[0].nodes.size() == 2U);
    CHECK(observerGraphs[0].nodes[0].id == device.id.raw() + ":client");
    CHECK(observerGraphs[0].nodes[1].id == device.id.raw());
    CHECK(observerGraphs[1] == flow::Graph{});
    CHECK(startCalls == 1U);
    CHECK(renderCallsAtClose == 1U);
    CHECK(renderReturnsAtClose == 1U);
    // The control worker may legally deliver the drain of the admitted run
    // before close wins serialization, but never after close returns.
    CHECK(drainCompletionsAtClose <= 1U);
    CHECK(renderTarget.renderCalls() == 1U);
    CHECK(renderTarget.renderReturns() == 1U);
    CHECK(renderTarget.drainCompletions() == drainCompletionsAtClose);
    CHECK(renderTarget.errors() == 0U);
    auto latest = flow::Graph{};
    auto lateSub = graphRegistryPtr->subscribe(device.id.raw(), [&](flow::Graph const& graph) { latest = graph; });
    CHECK(latest == flow::Graph{});
  }

  TEST_CASE("CoreAudioBackend - run-only start and stop inside an open notification keep the same open target",
            "[concurrency]")
  {
    auto const device = requireNativeTestOutput();
    CAPTURE(device.id.raw());

    enum class ReentryControl : std::uint8_t
    {
      None,
      RouteReadyStart,
      RouteReadyStop,
      FormatChangedStart,
      FormatChangedStop
    };
    auto reentryControl = ReentryControl::None;
    auto routeAnchors = std::vector<std::string>{};
    auto formats = std::vector<PcmFormat>{};
    auto graphs = std::vector<flow::Graph>{};
    auto graphRegistryPtr = std::make_shared<detail::BackendGraphRegistry>();
    auto backend = CoreAudioBackend{device, kProfileShared, graphRegistryPtr};
    // The render delegate owns the render and error counters; both targets and
    // every capture stay alive through cleanup.
    auto renderTarget = DrainingRenderTarget{};
    auto target = OpenNotificationRenderTarget{[&](std::string_view const anchor)
                                               {
                                                 routeAnchors.emplace_back(anchor);

                                                 if (reentryControl == ReentryControl::RouteReadyStart)
                                                 {
                                                   // Run-only reentry from a notification callback: no target
                                                   // revocation and no second open. The trivial render delegate
                                                   // lets a start that blocks inside the SDK complete its first
                                                   // render pass without an owned background gate.
                                                   backend.start();
                                                 }

                                                 if (reentryControl == ReentryControl::RouteReadyStop)
                                                 {
                                                   backend.stop();
                                                 }
                                               },
                                               [&](PcmFormat const& format)
                                               {
                                                 formats.push_back(format);

                                                 if (reentryControl == ReentryControl::FormatChangedStart)
                                                 {
                                                   backend.start();
                                                 }

                                                 if (reentryControl == ReentryControl::FormatChangedStop)
                                                 {
                                                   backend.stop();
                                                 }
                                               },
                                               &renderTarget};
    auto sub = graphRegistryPtr->subscribe(device.id.raw(), [&](flow::Graph const& graph) { graphs.push_back(graph); });
    REQUIRE(sub);
    auto cleanup = utility::ScopedRegistration{[&]
                                               {
                                                 sub.reset();
                                                 backend.close();
                                               }};

    SECTION("a route-ready reentrant start keeps the followup format notification")
    {
      reentryControl = ReentryControl::RouteReadyStart;
    }

    SECTION("a route-ready reentrant stop keeps the followup format notification")
    {
      reentryControl = ReentryControl::RouteReadyStop;
    }

    SECTION("a format-changed reentrant start keeps the initial graph publication")
    {
      reentryControl = ReentryControl::FormatChangedStart;
    }

    SECTION("a format-changed reentrant stop keeps the initial graph publication")
    {
      reentryControl = ReentryControl::FormatChangedStop;
    }

    auto const openedRes = backend.open(
      {.sampleRate = 44100, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::Integer}, target);
    sub.reset();

    REQUIRE(openedRes);
    CHECK(openedRes->clientFormat.sampleRate == 44100U);
    CHECK(openedRes->clientFormat.channels == 2U);
    CHECK(openedRes->clientFormat.encoding == SampleEncoding::Signed16Le);
    CHECK(routeAnchors == std::vector<std::string>{device.id.raw()});
    REQUIRE(formats.size() == 1U);
    CHECK(formats[0].sampleRate == 44100U);
    CHECK(formats[0].channels == 2U);
    CHECK(formats[0].encoding == SampleEncoding::Signed16Le);
    // The run never revoked the open target: the backend is still live after open.
    CHECK(backend.queryProperty(PropertyId::Volume).isAvailable);
    REQUIRE(graphs.size() == 2U);
    CHECK(graphs[0] == flow::Graph{});
    REQUIRE(graphs[1].nodes.size() == 2U);
    CHECK(graphs[1].nodes[0].id == device.id.raw() + ":client");
    CHECK(graphs[1].nodes[1].id == device.id.raw());
    CHECK(renderTarget.errors() == 0U);

    cleanup.reset();
    CHECK_FALSE(backend.queryProperty(PropertyId::Volume).isAvailable);
    auto latest = flow::Graph{};
    auto lateSub = graphRegistryPtr->subscribe(device.id.raw(), [&](flow::Graph const& graph) { latest = graph; });
    CHECK(latest == flow::Graph{});
    CHECK(renderTarget.errors() == 0U);
  }

  TEST_CASE("CoreAudioBackend - registry retirement on native publication is synchronous and leaves close inert",
            "[concurrency]")
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

  TEST_CASE("CoreAudioBackend - stop fences a blocked render and cancels its drain", "[concurrency]")
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
