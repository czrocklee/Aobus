// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/audio/backend/PipeWireBackend.h"
#include "lib/audio/backend/PipeWireProvider.h"
#include "lib/audio/backend/detail/PipeWireRuntime.h"
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/Property.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/flow/Graph.h>
#include <ao/utility/Raii.h>
#include <ao/utility/ScopedRegistration.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

extern "C"
{
#include <pipewire/context.h>
#include <pipewire/core.h>
#include <pipewire/keys.h>
#include <pipewire/main-loop.h>
#include <pipewire/node.h>
#include <pipewire/properties.h>
#include <pipewire/proxy.h>
#include <pipewire/thread-loop.h>
#include <spa/utils/dict.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::audio::backend::test
{
  using namespace ao::audio::backend::detail;

  namespace
  {
    constexpr auto kRouteTimeout = std::chrono::seconds{2};
    constexpr auto kAlternateSinkName = "ao-test-fallback-sink";
    constexpr auto kAlternateSinkId = "node:ao-test-fallback-sink";
    constexpr auto kSelectedSinkName = "ao-test-selected-sink";
    constexpr auto kSelectedSinkId = "node:ao-test-selected-sink";
    constexpr auto kCommonSinkLabel = "Aobus test output";
    constexpr auto kStereoSource = SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16};

    class PlaybackObserver final : public RenderTarget
    {
    public:
      PlaybackObserver() = default;

      explicit PlaybackObserver(std::string routeAnchor)
        : _routeAnchor{std::move(routeAnchor)}
      {
      }

      RenderPcmResult renderPcm(std::span<std::byte> output) noexcept override
      {
        std::ranges::fill(output, std::byte{0});

        if (!_rendered.exchange(true, std::memory_order_relaxed))
        {
          _firstRender.release();
        }

        return {.bytesWritten = output.size()};
      }

      void handleUnderrun() noexcept override {}
      void handlePositionAdvanced(std::uint32_t /*frames*/) noexcept override {}
      void handleDrainComplete() noexcept override {}
      void handleFormatChanged(PcmFormat const& /*format*/) noexcept override {}
      void handlePropertyChanged(PropertySnapshot /*snapshot*/) noexcept override {}

      void handleRouteReady(std::string_view routeAnchor) noexcept override
      {
        auto const lock = std::scoped_lock{_mutex};
        _routeAnchor = routeAnchor;
      }

      void handleBackendError(std::string_view message) noexcept override
      {
        {
          auto const lock = std::scoped_lock{_mutex};
          _optError = message;
        }

        _cv.notify_all();
      }

      void updateGraph(flow::Graph const& graph)
      {
        {
          auto const lock = std::scoped_lock{_mutex};

          for (auto const& link : graph.connections)
          {
            if (link.isActive && link.sourceId == _routeAnchor)
            {
              auto const it = std::ranges::find(graph.nodes, link.destinationId, &flow::Node::id);

              if (it != graph.nodes.end() && it->type == flow::NodeType::Sink && !hasSeenLinkTo(it->id))
              {
                _linkedSinkIds.push_back(it->id);
              }
            }
          }
        }

        _cv.notify_all();
      }

      std::string routeAnchor() const
      {
        auto const lock = std::scoped_lock{_mutex};
        return _routeAnchor;
      }

      bool tryWaitForFirstRender() { return _firstRender.try_acquire_for(kRouteTimeout); }

      bool tryWaitForLinkTo(std::string_view sinkId)
      {
        auto lock = std::unique_lock{_mutex};
        return _cv.wait_for(lock, kRouteTimeout, [&] { return hasSeenLinkTo(sinkId); });
      }

      std::vector<std::string> observedSinkIds() const
      {
        auto const lock = std::scoped_lock{_mutex};
        return _linkedSinkIds;
      }

      std::optional<std::string> tryWaitForBackendError()
      {
        auto lock = std::unique_lock{_mutex};
        _cv.wait_for(lock, kRouteTimeout, [&] { return _optError.has_value(); });
        return _optError;
      }

      std::optional<std::string> backendError() const
      {
        auto const lock = std::scoped_lock{_mutex};
        return _optError;
      }

    private:
      bool hasSeenLinkTo(std::string_view sinkId) const { return std::ranges::contains(_linkedSinkIds, sinkId); }

      std::atomic_bool _rendered{false};
      std::binary_semaphore _firstRender{0};
      mutable std::mutex _mutex;
      std::condition_variable _cv;
      std::optional<std::string> _optError;
      std::string _routeAnchor;
      std::vector<std::string> _linkedSinkIds;
    };

    struct NativeSinkIdentity final
    {
      std::uint32_t globalId;
      std::string nodeName;
      std::string objectSerial;

      std::string graphNodeId() const { return std::to_string(globalId); }
    };

    class [[nodiscard]] NullAudioSinkGuard final
    {
    public:
      NullAudioSinkGuard(NullAudioSinkGuard const&) = delete;
      NullAudioSinkGuard& operator=(NullAudioSinkGuard const&) = delete;
      NullAudioSinkGuard(NullAudioSinkGuard&&) = delete;
      NullAudioSinkGuard& operator=(NullAudioSinkGuard&&) = delete;

      explicit NullAudioSinkGuard(char const* nodeName = "rs-test-null-sink",
                                  char const* mediaClass = "Audio/Sink",
                                  char const* priority = "1000",
                                  char const* nodeNick = nullptr)
      {
        _threadLoopPtr.reset(::pw_thread_loop_new("TestSinkLoop", nullptr));

        if (!_threadLoopPtr)
        {
          return;
        }

        _contextPtr.reset(::pw_context_new(::pw_thread_loop_get_loop(_threadLoopPtr.get()), nullptr, 0));

        if (!_contextPtr || ::pw_thread_loop_start(_threadLoopPtr.get()) < 0)
        {
          return;
        }

        auto const guard = PwThreadLoopGuard{_threadLoopPtr.get()};
        _corePtr.reset(::pw_context_connect(_contextPtr.get(), nullptr, 0));

        if (!_corePtr)
        {
          return;
        }

        auto propsPtr = utility::makeUniquePtr<::pw_properties_free>(::pw_properties_new(PW_KEY_FACTORY_NAME,
                                                                                         "support.null-audio-sink",
                                                                                         PW_KEY_NODE_NAME,
                                                                                         nodeName,
                                                                                         PW_KEY_MEDIA_CLASS,
                                                                                         mediaClass,
                                                                                         PW_KEY_OBJECT_LINGER,
                                                                                         "false",
                                                                                         PW_KEY_PRIORITY_SESSION,
                                                                                         priority,
                                                                                         nullptr));

        if (!propsPtr)
        {
          return;
        }

        if (nodeNick != nullptr)
        {
          ::pw_properties_set(propsPtr.get(), PW_KEY_NODE_NICK, nodeNick);
        }

        void* const proxy = ::pw_core_create_object(
          _corePtr.get(), "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, &propsPtr->dict, 0);
        _proxyPtr.reset(static_cast<::pw_proxy*>(proxy));

        if (_proxyPtr)
        {
          ::pw_proxy_add_listener(_proxyPtr.get(), _proxyListener.get(), &kProxyEvents, this);
        }
      }

      ~NullAudioSinkGuard()
      {
        if (_threadLoopPtr)
        {
          ::pw_thread_loop_stop(_threadLoopPtr.get());
          auto const guard = PwThreadLoopGuard{_threadLoopPtr.get()};
          _proxyListener.reset();
          _proxyPtr.reset();
          _corePtr.reset();
          _contextPtr.reset();
        }
      }

      bool isValid() const { return _proxyPtr != nullptr; }

      std::optional<NativeSinkIdentity> tryWaitForIdentity() const
      {
        auto const guard = PwThreadLoopGuard{_threadLoopPtr.get()};
        auto deadline = ::timespec{};
        ::pw_thread_loop_get_time(
          _threadLoopPtr.get(), &deadline, std::chrono::duration_cast<std::chrono::nanoseconds>(kRouteTimeout).count());

        while (!_optIdentity)
        {
          if (::pw_thread_loop_timed_wait_full(_threadLoopPtr.get(), &deadline) < 0)
          {
            return std::nullopt;
          }
        }

        return _optIdentity;
      }

    private:
      static ::pw_proxy_events const kProxyEvents;

      PipeWireEnvironmentGuard _envGuard;
      PwThreadLoopPtr _threadLoopPtr;
      PwContextPtr _contextPtr;
      PwCorePtr _corePtr;
      PwProxyPtr<::pw_proxy> _proxyPtr;
      SpaHookGuard _proxyListener;
      std::optional<NativeSinkIdentity> _optIdentity;
    };

    ::pw_proxy_events const NullAudioSinkGuard::kProxyEvents = []
    {
      auto events = ::pw_proxy_events{};
      events.version = PW_VERSION_PROXY_EVENTS;
      events.bound_props = [](void* data, std::uint32_t globalId, ::spa_dict const* props) noexcept
      {
        auto& self = *static_cast<NullAudioSinkGuard*>(data);
        auto const* name = props != nullptr ? ::spa_dict_lookup(props, PW_KEY_NODE_NAME) : nullptr;
        auto const* serial = props != nullptr ? ::spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL) : nullptr;
        self._optIdentity = NativeSinkIdentity{.globalId = globalId,
                                               .nodeName = name != nullptr ? name : "",
                                               .objectSerial = serial != nullptr ? serial : ""};
        ::pw_thread_loop_signal(self._threadLoopPtr.get(), false);
      };
      return events;
    }();

    NativeSinkIdentity requireSinkIdentity(NullAudioSinkGuard const& sink, std::string_view nodeName)
    {
      CAPTURE(nodeName);
      REQUIRE(sink.isValid());
      auto optIdentity = sink.tryWaitForIdentity();
      REQUIRE(optIdentity);
      REQUIRE(optIdentity->nodeName == nodeName);
      REQUIRE_FALSE(optIdentity->objectSerial.empty());
      return std::move(*optIdentity);
    }

    class DeviceSnapshotObserver final
    {
    public:
      void update(std::vector<Device> devices)
      {
        {
          auto lock = std::scoped_lock{_mutex};
          _devices = std::move(devices);
          ++_updateCount;
        }

        _cv.notify_all();
      }

      bool tryWaitUntilContains(std::string_view expectedNameOrId, std::chrono::milliseconds timeout)
      {
        auto lock = std::unique_lock{_mutex};
        return _cv.wait_for(lock, timeout, [this, expectedNameOrId] { return containsDeviceLocked(expectedNameOrId); });
      }

      std::optional<Device> tryWaitForDevice(DeviceId const& id)
      {
        auto lock = std::unique_lock{_mutex};

        if (!_cv.wait_for(lock, kRouteTimeout, [&] { return std::ranges::contains(_devices, id, &Device::id); }))
        {
          return std::nullopt;
        }

        return *std::ranges::find(_devices, id, &Device::id);
      }

      bool tryWaitUntilRemoved(DeviceId const& id)
      {
        auto lock = std::unique_lock{_mutex};
        return _cv.wait_for(lock, kRouteTimeout, [&] { return !std::ranges::contains(_devices, id, &Device::id); });
      }

      std::string describeSnapshot() const
      {
        auto lock = std::scoped_lock{_mutex};
        auto description = std::string{"["};

        for (auto const& device : _devices)
        {
          if (description.size() > 1)
          {
            description += ", ";
          }

          description += device.id.raw();
          description += "/";
          description += device.displayName;
        }

        description += "]";
        return description;
      }

      std::size_t updateCount() const
      {
        auto lock = std::scoped_lock{_mutex};
        return _updateCount;
      }

      bool hasDuplicateDeviceIds() const
      {
        auto lock = std::scoped_lock{_mutex};

        for (auto left = _devices.begin(); left != _devices.end(); ++left)
        {
          for (auto right = std::next(left); right != _devices.end(); ++right)
          {
            if (left->id == right->id)
            {
              return true;
            }
          }
        }

        return false;
      }

    private:
      bool containsDeviceLocked(std::string_view expectedNameOrId) const
      {
        return std::ranges::any_of(_devices,
                                   [&](auto const& device)
                                   { return device.displayName == expectedNameOrId || device.id == expectedNameOrId; });
      }

      mutable std::mutex _mutex;
      std::condition_variable _cv;
      std::vector<Device> _devices;
      std::size_t _updateCount = 0;
    };

    void requirePrivatePipeWire()
    {
      if (auto const* required = std::getenv("AOBUS_REQUIRE_PIPEWIRE");
          required == nullptr || std::string_view{required} != "1")
      {
        SKIP("Requires the private PipeWire policy fixture");
      }

      auto const* runtime = std::getenv("PIPEWIRE_RUNTIME_DIR");
      REQUIRE(runtime != nullptr);
      REQUIRE_FALSE(std::string_view{runtime}.empty());
    }

    class PrivatePipeWireFixture final
    {
    public:
      PipeWireProvider& provider() { return _provider; }

      Device requireDevice(char const* expectedId)
      {
        auto optDevice = _devicesPtr->tryWaitForDevice(DeviceId{expectedId});
        CAPTURE(expectedId, _devicesPtr->describeSnapshot());
        REQUIRE(optDevice);
        return std::move(*optDevice);
      }

      void requireDeviceRemoved(DeviceId const& id)
      {
        auto const removed = _devicesPtr->tryWaitUntilRemoved(id);
        CAPTURE(id.raw(), _devicesPtr->describeSnapshot());
        REQUIRE(removed);
      }

    private:
      PipeWireProvider _provider;
      std::shared_ptr<DeviceSnapshotObserver> _devicesPtr = std::make_shared<DeviceSnapshotObserver>();
      utility::ScopedRegistration _subscription =
        _provider.subscribeDevices([observerPtr = _devicesPtr](auto const& devices) { observerPtr->update(devices); });
    };

    utility::ScopedRegistration subscribePlaybackGraph(PipeWireProvider& provider, PlaybackObserver& observer)
    {
      auto const anchor = observer.routeAnchor();
      REQUIRE_FALSE(anchor.empty());
      return provider.subscribeGraph(anchor, [&observer](auto const& graph) { observer.updateGraph(graph); });
    }

    utility::ScopedRegistration openAndSubscribeGraph(PipeWireProvider& provider,
                                                      Backend& backend,
                                                      PlaybackObserver& observer)
    {
      auto const openedRes = backend.open(kStereoSource, observer);
      INFO((openedRes ? "PipeWire negotiated" : openedRes.error().error.message));
      REQUIRE(openedRes);
      auto registration = subscribePlaybackGraph(provider, observer);
      backend.start();
      return registration;
    }

    void requirePlaybackLink(PlaybackObserver& observer, NativeSinkIdentity const& expectedSink)
    {
      CAPTURE(expectedSink.globalId, expectedSink.nodeName, expectedSink.objectSerial);
      REQUIRE(observer.tryWaitForLinkTo(expectedSink.graphNodeId()));
      CHECK(observer.observedSinkIds() == std::vector{expectedSink.graphNodeId()});
    }

    utility::ScopedRegistration openAndObserve(PipeWireProvider& provider,
                                               Backend& backend,
                                               PlaybackObserver& observer,
                                               NativeSinkIdentity const& expectedSink)
    {
      auto registration = openAndSubscribeGraph(provider, backend, observer);
      REQUIRE(observer.tryWaitForFirstRender());
      requirePlaybackLink(observer, expectedSink);
      return registration;
    }

    void requireRetainedLink(PlaybackObserver const& incumbentObserver, NativeSinkIdentity const& expectedSink)
    {
      // A new monitor sees the current graph, not the incumbent's link history.
      auto provider = PipeWireProvider{};
      auto observer = PlaybackObserver{incumbentObserver.routeAnchor()};
      auto const graphRegistration = subscribePlaybackGraph(provider, observer);
      requirePlaybackLink(observer, expectedSink);
      CHECK_FALSE(incumbentObserver.backendError());
    }

    void checkOutputFailure(Backend& backend, PlaybackObserver& observer)
    {
      auto const openedRes = backend.open(kStereoSource, observer);
      INFO((openedRes ? "PipeWire negotiated" : openedRes.error().error.message));
      REQUIRE_FALSE(openedRes);
      CHECK(openedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
      CHECK(observer.routeAnchor().empty());
    }
  } // namespace

  TEST_CASE("PipeWireProvider - integrates with a real daemon through the API")
  {
    auto const envGuard = PipeWireEnvironmentGuard{};

    // Quick check if we can connect to a daemon
    {
      auto loopPtr = utility::makeUniquePtr<::pw_main_loop_destroy>(::pw_main_loop_new(nullptr));
      REQUIRE(loopPtr);
      auto contextPtr = PwContextPtr{::pw_context_new(::pw_main_loop_get_loop(loopPtr.get()), nullptr, 0)};
      REQUIRE(contextPtr);
      auto corePtr = PwCorePtr{::pw_context_connect(contextPtr.get(), nullptr, 0)};

      if (!corePtr)
      {
        if (auto const* required = std::getenv("AOBUS_REQUIRE_PIPEWIRE");
            required != nullptr && std::string_view{required} == "1")
        {
          FAIL("Required PipeWire daemon is unavailable (AOBUS_REQUIRE_PIPEWIRE=1)");
        }

        SKIP("Optional PipeWire daemon is unavailable");
      }
    }

    auto const sinkGuard = NullAudioSinkGuard{};

    INFO("A connected PipeWire daemon must support the test's null sink fixture");
    REQUIRE(sinkGuard.isValid());

    auto provider = PipeWireProvider{};
    auto devicesPtr = std::make_shared<DeviceSnapshotObserver>();
    auto const sub = provider.subscribeDevices([devicesPtr](std::vector<Device> const& nextDevices)
                                               { devicesPtr->update(nextDevices); });

    SECTION("Enumeration finds the dummy sink")
    {
      auto const found = devicesPtr->tryWaitUntilContains("rs-test-null-sink", std::chrono::seconds{1});

      INFO("Expected PipeWire device 'rs-test-null-sink' after 1s; observed "
           << devicesPtr->updateCount() << " device snapshots: " << devicesPtr->describeSnapshot());
      CHECK(found);
    }

    SECTION("Enumeration exposes one logical device per id")
    {
      auto const found = devicesPtr->tryWaitUntilContains("rs-test-null-sink", std::chrono::seconds{1});
      REQUIRE(found);

      INFO("Observed PipeWire device snapshot: " << devicesPtr->describeSnapshot());
      CHECK_FALSE(devicesPtr->hasDuplicateDeviceIds());
    }

    SECTION("Enumeration finds Audio/Duplex nodes")
    {
      auto const duplexGuard = NullAudioSinkGuard{"ao-test-duplex-sink", "Audio/Duplex"};
      REQUIRE(duplexGuard.isValid());
      auto const found = devicesPtr->tryWaitUntilContains("ao-test-duplex-sink", std::chrono::seconds{1});

      INFO("Expected PipeWire device 'ao-test-duplex-sink' after 1s; observed "
           << devicesPtr->updateCount() << " device snapshots: " << devicesPtr->describeSnapshot());
      CHECK(found);
    }

    SECTION("Shared backend negotiates the preferred source-native client format")
    {
      auto target = PlaybackObserver{};
      auto backend =
        PipeWireBackend{Device{.id = DeviceId{""}, .isDefault = true, .backendId = kBackendPipeWire}, kProfileShared};
      auto const openedRes = backend.open(kStereoSource, target);
      auto const diagnostic = openedRes.has_value() ? std::string{} : openedRes.error().error.message;

      INFO(diagnostic);
      REQUIRE(openedRes);
      CAPTURE(openedRes->clientFormat.sampleRate,
              openedRes->clientFormat.channels,
              static_cast<int>(openedRes->clientFormat.encoding));
      CHECK(openedRes->clientFormat.sampleRate == 48000);
      CHECK(openedRes->clientFormat.channels == 2);
      CHECK(openedRes->clientFormat.encoding == SampleEncoding::Signed16Le);
    }
  }

  TEST_CASE("PipeWireBackend - saved node-name selection opens on a replacement with a different serial")
  {
    requirePrivatePipeWire();

    auto fixture = PrivatePipeWireFixture{};
    auto savedDevice = Device{};
    auto oldSerial = std::string{};
    {
      auto initialSink = NullAudioSinkGuard{kSelectedSinkName, "Audio/Sink", "1000", kCommonSinkLabel};
      oldSerial = requireSinkIdentity(initialSink, kSelectedSinkName).objectSerial;
      savedDevice = fixture.requireDevice(kSelectedSinkId);
      REQUIRE(savedDevice.displayName == kCommonSinkLabel);
      CHECK_FALSE(std::ranges::contains(fixture.provider().status().devices, DeviceId{oldSerial}, &Device::id));
    }

    fixture.requireDeviceRemoved(savedDevice.id);
    auto alternateSink = NullAudioSinkGuard{kAlternateSinkName, "Audio/Sink", "2000", kCommonSinkLabel};
    auto const alternate = requireSinkIdentity(alternateSink, kAlternateSinkName);
    CHECK(fixture.requireDevice(kAlternateSinkId).displayName == savedDevice.displayName);

    auto replacementSink = NullAudioSinkGuard{kSelectedSinkName, "Audio/Sink", "1000", kCommonSinkLabel};
    auto const replacement = requireSinkIdentity(replacementSink, kSelectedSinkName);
    CAPTURE(oldSerial, replacement.objectSerial);
    REQUIRE(replacement.objectSerial != oldSerial);
    REQUIRE(fixture.requireDevice(kSelectedSinkId).id == savedDevice.id);

    auto observer = PlaybackObserver{};
    auto backendPtr = std::unique_ptr<Backend>{};
    auto const* expectedSink = &replacement;
    bool expectSharedControls = true;

    SECTION("Saved shared request selects the replacement")
    {
      backendPtr = fixture.provider().createBackend(savedDevice, kProfileShared);
    }

    SECTION("Saved exclusive request selects the replacement without shared controls")
    {
      backendPtr = fixture.provider().createBackend(savedDevice, kProfileExclusive);
      expectSharedControls = false;
    }

    SECTION("An empty ID still uses shared system routing even with the exclusive profile")
    {
      backendPtr =
        fixture.provider().createBackend(Device{.isDefault = true, .backendId = kBackendPipeWire}, kProfileExclusive);
      expectedSink = &alternate;
    }

    REQUIRE(backendPtr);
    auto const graphRegistration = openAndObserve(fixture.provider(), *backendPtr, observer, *expectedSink);
    CHECK(backendPtr->queryProperty(PropertyId::Volume).isAvailable == expectSharedControls);
  }

  TEST_CASE("PipeWireBackend - a stale name cannot select a different sink with the same display label")
  {
    requirePrivatePipeWire();

    auto alternateSink = NullAudioSinkGuard{kAlternateSinkName, "Audio/Sink", "1000", kCommonSinkLabel};
    auto const alternate = requireSinkIdentity(alternateSink, kAlternateSinkName);
    auto fixture = PrivatePipeWireFixture{};
    auto const alternateDevice = fixture.requireDevice(kAlternateSinkId);
    auto savedDevice = Device{};
    {
      auto selectedSink = NullAudioSinkGuard{kSelectedSinkName, "Audio/Sink", "1000", kCommonSinkLabel};
      requireSinkIdentity(selectedSink, kSelectedSinkName);
      savedDevice = fixture.requireDevice(kSelectedSinkId);
      REQUIRE(savedDevice.displayName == alternateDevice.displayName);
    }

    fixture.requireDeviceRemoved(savedDevice.id);
    auto observer = PlaybackObserver{};

    SECTION("Fixed selection fails to open")
    {
      auto backendPtr = fixture.provider().createBackend(savedDevice, kProfileShared);
      REQUIRE(backendPtr);
      checkOutputFailure(*backendPtr, observer);
    }

    SECTION("Opted-in named selection follows system policy to the alternate sink")
    {
      auto backendPtr = fixture.provider().createReroutableBackend(savedDevice, kProfileShared);
      REQUIRE(backendPtr);
      auto const graphRegistration = openAndObserve(fixture.provider(), *backendPtr, observer, alternate);
    }
  }

  TEST_CASE("PipeWireBackend - opted-in missing named selection preserves exclusive linking")
  {
    requirePrivatePipeWire();

    auto alternateSink = NullAudioSinkGuard{kAlternateSinkName};
    auto const alternate = requireSinkIdentity(alternateSink, kAlternateSinkName);
    auto fixture = PrivatePipeWireFixture{};
    auto const alternateDevice = fixture.requireDevice(kAlternateSinkId);
    auto savedDevice = Device{};
    {
      auto selectedSink = NullAudioSinkGuard{kSelectedSinkName};
      savedDevice = fixture.requireDevice(kSelectedSinkId);
    }

    fixture.requireDeviceRemoved(savedDevice.id);
    auto exclusiveObserver = PlaybackObserver{};
    auto sharedObserver = PlaybackObserver{};
    auto exclusivePtr = fixture.provider().createReroutableBackend(savedDevice, kProfileExclusive);
    auto sharedPtr = fixture.provider().createBackend(alternateDevice, kProfileShared);
    REQUIRE(exclusivePtr);
    REQUIRE(sharedPtr);
    CHECK(exclusivePtr->profileId() == kProfileExclusive);

    SECTION("An idle alternate accepts the exclusive fallback and rejects a later shared link")
    {
      auto const graphRegistration = openAndSubscribeGraph(fixture.provider(), *exclusivePtr, exclusiveObserver);
      requirePlaybackLink(exclusiveObserver, alternate);
      CHECK_FALSE(exclusivePtr->queryProperty(PropertyId::Volume).isAvailable);

      checkOutputFailure(*sharedPtr, sharedObserver);
      requireRetainedLink(exclusiveObserver, alternate);
    }

    SECTION("A shared link on the alternate is not evicted by the exclusive fallback")
    {
      auto const graphRegistration = openAndSubscribeGraph(fixture.provider(), *sharedPtr, sharedObserver);
      requirePlaybackLink(sharedObserver, alternate);

      checkOutputFailure(*exclusivePtr, exclusiveObserver);
      requireRetainedLink(sharedObserver, alternate);
    }
  }

  TEST_CASE("PipeWireBackend - qualified numeric node names work but legacy serial IDs do not")
  {
    requirePrivatePipeWire();

    // A live target with a numeric name exposes accidental use of PipeWire's serial syntax.
    auto sink = NullAudioSinkGuard{"999999999"};
    auto const identity = requireSinkIdentity(sink, "999999999");
    auto fixture = PrivatePipeWireFixture{};
    auto const device = fixture.requireDevice("node:999999999");
    auto observer = PlaybackObserver{};

    SECTION("Qualified numeric name selects the named sink")
    {
      auto backendPtr = fixture.provider().createBackend(device, kProfileShared);
      REQUIRE(backendPtr);
      auto const graphRegistration = openAndObserve(fixture.provider(), *backendPtr, observer, identity);
    }

    SECTION("Fixed selection rejects the live object's bare serial")
    {
      auto legacyDevice = device;
      legacyDevice.id = DeviceId{identity.objectSerial};
      auto backendPtr = fixture.provider().createBackend(legacyDevice, kProfileShared);
      REQUIRE(backendPtr);
      checkOutputFailure(*backendPtr, observer);
    }

    SECTION("Opted-in selection also rejects the live object's bare serial")
    {
      auto legacyDevice = device;
      legacyDevice.id = DeviceId{identity.objectSerial};
      auto backendPtr = fixture.provider().createReroutableBackend(legacyDevice, kProfileShared);
      REQUIRE(backendPtr);
      checkOutputFailure(*backendPtr, observer);
    }

    SECTION("A numeric node name without the prefix is invalid")
    {
      auto legacyDevice = device;
      legacyDevice.id = DeviceId{identity.nodeName};
      auto backendPtr = fixture.provider().createReroutableBackend(legacyDevice, kProfileShared);
      REQUIRE(backendPtr);
      checkOutputFailure(*backendPtr, observer);
    }
  }

  TEST_CASE("PipeWireBackend - simultaneous duplicate node names are not a fixed selection")
  {
    requirePrivatePipeWire();

    auto alternateSink = NullAudioSinkGuard{kAlternateSinkName, "Audio/Sink", "2000"};
    auto const alternate = requireSinkIdentity(alternateSink, kAlternateSinkName);
    auto selectedSink = NullAudioSinkGuard{kSelectedSinkName};
    auto const selected = requireSinkIdentity(selectedSink, kSelectedSinkName);
    auto fixture = PrivatePipeWireFixture{};
    fixture.requireDevice(kAlternateSinkId);
    auto const savedDevice = fixture.requireDevice(kSelectedSinkId);

    auto duplicateSink = NullAudioSinkGuard{kSelectedSinkName};
    auto const duplicate = requireSinkIdentity(duplicateSink, kSelectedSinkName);
    REQUIRE(duplicate.objectSerial != selected.objectSerial);
    REQUIRE(duplicate.globalId != selected.globalId);
    fixture.requireDeviceRemoved(savedDevice.id);

    auto observer = PlaybackObserver{};

    SECTION("Previously unique fixed selection fails to open")
    {
      auto backendPtr = fixture.provider().createBackend(savedDevice, kProfileShared);
      REQUIRE(backendPtr);
      checkOutputFailure(*backendPtr, observer);
    }

    SECTION("Opt-in follows system policy without pinning either duplicate")
    {
      auto backendPtr = fixture.provider().createReroutableBackend(savedDevice, kProfileShared);
      REQUIRE(backendPtr);
      auto const graphRegistration = openAndObserve(fixture.provider(), *backendPtr, observer, alternate);
    }
  }

  TEST_CASE("PipeWireBackend - live unplug follows the stream routing policy")
  {
    requirePrivatePipeWire();

    auto optFallbackSink = std::optional<NullAudioSinkGuard>{};
    auto optSelectedSink = std::optional<NullAudioSinkGuard>{std::in_place, kSelectedSinkName, "Audio/Sink", "2000"};
    auto const selected = requireSinkIdentity(*optSelectedSink, kSelectedSinkName);
    auto fixture = PrivatePipeWireFixture{};
    auto selectedDevice = fixture.requireDevice(kSelectedSinkId);
    // A concrete default-marked device is still a named selection.
    selectedDevice.isDefault = true;
    auto observer = PlaybackObserver{};
    auto backendPtr = std::unique_ptr<Backend>{};
    auto graphRegistration = utility::ScopedRegistration{};
    auto const openOnSelected = [&]
    {
      REQUIRE(backendPtr);
      return openAndObserve(fixture.provider(), *backendPtr, observer, selected);
    };
    bool expectReroute = true;

    SECTION("Fixed named selection reports a terminal output error")
    {
      backendPtr = fixture.provider().createBackend(selectedDevice, kProfileShared);
      expectReroute = false;
    }

    SECTION("Opted-in named selection links to another sink")
    {
      backendPtr = fixture.provider().createReroutableBackend(selectedDevice, kProfileShared);
    }

    SECTION("Empty ID follows the system default and links to another sink")
    {
      backendPtr =
        fixture.provider().createBackend(Device{.isDefault = true, .backendId = kBackendPipeWire}, kProfileShared);
      // Registry visibility does not mean WirePlumber has elected its default. Establish
      // the initial route with one sink before offering the lower-priority fallback.
      graphRegistration = openOnSelected();
    }

    auto const alternate = requireSinkIdentity(optFallbackSink.emplace(kAlternateSinkName), kAlternateSinkName);
    fixture.requireDevice(kAlternateSinkId);

    if (!graphRegistration)
    {
      // Named requests must select the requested sink with the alternative already present.
      graphRegistration = openOnSelected();
    }

    CHECK(observer.observedSinkIds() == std::vector{selected.graphNodeId()});
    optSelectedSink.reset();
    fixture.requireDeviceRemoved(selectedDevice.id);

    if (expectReroute)
    {
      REQUIRE(observer.tryWaitForLinkTo(alternate.graphNodeId()));
      requireRetainedLink(observer, alternate);
    }
    else
    {
      auto const optError = observer.tryWaitForBackendError();
      REQUIRE(optError);
      CHECK_FALSE(optError->empty());
      CHECK(observer.observedSinkIds() == std::vector{selected.graphNodeId()});
    }
  }
} // namespace ao::audio::backend::test
