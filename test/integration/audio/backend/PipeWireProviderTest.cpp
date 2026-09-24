// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/audio/backend/PipeWireProvider.h"

#include "lib/audio/backend/PipeWireBackend.h"
#include "lib/audio/backend/detail/PipeWireRuntime.h"
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/Property.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/utility/Raii.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

extern "C"
{
#include <pipewire/context.h>
#include <pipewire/core.h>
#include <pipewire/main-loop.h>
#include <pipewire/node.h>
#include <pipewire/properties.h>
#include <pipewire/proxy.h>
#include <pipewire/thread-loop.h>
}

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <mutex>
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
    class NoopRenderTarget final : public RenderTarget
    {
    public:
      RenderPcmResult renderPcm(std::span<std::byte> /*output*/) noexcept override { return {.drained = true}; }
      void handleUnderrun() noexcept override {}
      void handlePositionAdvanced(std::uint32_t /*frames*/) noexcept override {}
      void handleDrainComplete() noexcept override {}
      void handleRouteReady(std::string_view /*routeAnchor*/) noexcept override {}
      void handleFormatChanged(PcmFormat const& /*format*/) noexcept override {}
      void handlePropertyChanged(PropertySnapshot /*snapshot*/) noexcept override {}
      void handleBackendError(std::string_view /*message*/) noexcept override {}
    };

    struct [[nodiscard]] NullAudioSinkGuard final
    {
      PipeWireEnvironmentGuard envGuard;
      PwThreadLoopPtr threadLoopPtr;
      PwContextPtr contextPtr;
      PwCorePtr corePtr;
      PwProxyPtr<::pw_proxy> proxyPtr;

      NullAudioSinkGuard(NullAudioSinkGuard const&) = delete;
      NullAudioSinkGuard& operator=(NullAudioSinkGuard const&) = delete;
      NullAudioSinkGuard(NullAudioSinkGuard&&) = delete;
      NullAudioSinkGuard& operator=(NullAudioSinkGuard&&) = delete;

      explicit NullAudioSinkGuard(char const* nodeName = "rs-test-null-sink", char const* mediaClass = "Audio/Sink")
      {
        threadLoopPtr.reset(::pw_thread_loop_new("TestSinkLoop", nullptr));

        if (!threadLoopPtr)
        {
          return;
        }

        contextPtr.reset(::pw_context_new(::pw_thread_loop_get_loop(threadLoopPtr.get()), nullptr, 0));

        if (!contextPtr)
        {
          return;
        }

        if (::pw_thread_loop_start(threadLoopPtr.get()) < 0)
        {
          return;
        }

        {
          auto guard = PwThreadLoopGuard{threadLoopPtr.get()};
          corePtr.reset(::pw_context_connect(contextPtr.get(), nullptr, 0));

          if (corePtr)
          {
            auto propsPtr = utility::makeUniquePtr<::pw_properties_free>(::pw_properties_new("factory.name",
                                                                                             "support.null-audio-sink",
                                                                                             "node.name",
                                                                                             nodeName,
                                                                                             "media.class",
                                                                                             mediaClass,
                                                                                             "object.linger",
                                                                                             "false",
                                                                                             nullptr));

            if (!propsPtr)
            {
              return;
            }

            // Create node via adapter factory
            void* const p = ::pw_core_create_object(
              corePtr.get(), "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, &propsPtr->dict, 0);
            proxyPtr.reset(static_cast<::pw_proxy*>(p));

            // Sync to ensure it's processed
            ::pw_core_sync(corePtr.get(), PW_ID_CORE, 0);
          }
        }
      }

      ~NullAudioSinkGuard()
      {
        if (threadLoopPtr)
        {
          ::pw_thread_loop_stop(threadLoopPtr.get());
          {
            auto guard = PwThreadLoopGuard{threadLoopPtr.get()};
            proxyPtr.reset(); // Destroy proxy while lock is held
            corePtr.reset();
            contextPtr.reset();
          }
        }
      }

      bool isValid() const { return proxyPtr != nullptr; }
    };

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
  } // namespace

  TEST_CASE("PipeWireProvider - integrates with a real daemon through the API", "[audio][integration][pipewire]")
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
      auto target = NoopRenderTarget{};
      auto backend =
        PipeWireBackend{Device{.id = DeviceId{""}, .isDefault = true, .backendId = kBackendPipeWire}, kProfileShared};
      auto const sourceFormat = SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16};
      auto const openedRes = backend.open(sourceFormat, target);
      auto const diagnostic = openedRes.has_value() ? std::string{} : openedRes.error().message;

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
} // namespace ao::audio::backend::test
