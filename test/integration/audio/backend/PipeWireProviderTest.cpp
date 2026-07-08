// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Device.h>
#include <ao/audio/backend/PipeWireProvider.h>
#include <ao/audio/backend/detail/PipeWireShared.h>
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
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::audio::backend::test
{
  using namespace ao::audio::backend::detail;

  namespace
  {
    struct [[nodiscard]] DummySinkGuard final
    {
      PipeWireEnvironmentGuard envGuard;
      PwThreadLoopPtr threadLoopPtr;
      PwContextPtr contextPtr;
      PwCorePtr corePtr;
      PwProxyPtr<::pw_proxy> proxyPtr;

      DummySinkGuard(DummySinkGuard const&) = delete;
      DummySinkGuard& operator=(DummySinkGuard const&) = delete;
      DummySinkGuard(DummySinkGuard&&) = delete;
      DummySinkGuard& operator=(DummySinkGuard&&) = delete;

      DummySinkGuard()
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
                                                                                             "rs-test-dummy-sink",
                                                                                             "media.class",
                                                                                             "Audio/Sink",
                                                                                             "object.linger",
                                                                                             "false",
                                                                                             nullptr));

            // Create node via adapter factory
            void* const p = ::pw_core_create_object(
              corePtr.get(), "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, &propsPtr->dict, 0);
            proxyPtr.reset(static_cast<::pw_proxy*>(p));

            // Sync to ensure it's processed
            ::pw_core_sync(corePtr.get(), PW_ID_CORE, 0);
          }
        }
      }

      ~DummySinkGuard()
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

      bool waitUntilContains(std::string_view expectedNameOrId, std::chrono::milliseconds timeout)
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
      auto contextPtr = PwContextPtr{::pw_context_new(::pw_main_loop_get_loop(loopPtr.get()), nullptr, 0)};
      auto corePtr = PwCorePtr{::pw_context_connect(contextPtr.get(), nullptr, 0)};

      if (!corePtr)
      {
        WARN("Skipping PipeWire integration test: Daemon not running");
        return;
      }
    }

    auto const sinkGuard = DummySinkGuard{};

    if (!sinkGuard.isValid())
    {
      WARN("Skipping PipeWire integration test: Failed to create dummy sink");
      return;
    }

    REQUIRE(sinkGuard.isValid());

    auto provider = PipeWireProvider{};
    auto devicesPtr = std::make_shared<DeviceSnapshotObserver>();
    auto const sub = provider.subscribeDevices([devicesPtr](std::vector<Device> const& nextDevices)
                                               { devicesPtr->update(nextDevices); });

    SECTION("Enumeration finds the dummy sink")
    {
      auto const found = devicesPtr->waitUntilContains("rs-test-dummy-sink", std::chrono::seconds{1});

      INFO("Expected PipeWire device 'rs-test-dummy-sink' after 1s; observed "
           << devicesPtr->updateCount() << " device snapshots: " << devicesPtr->describeSnapshot());
      CHECK(found);
    }

    SECTION("Enumeration exposes one logical device per id")
    {
      auto const found = devicesPtr->waitUntilContains("rs-test-dummy-sink", std::chrono::seconds{1});
      REQUIRE(found);

      INFO("Observed PipeWire device snapshot: " << devicesPtr->describeSnapshot());
      CHECK_FALSE(devicesPtr->hasDuplicateDeviceIds());
    }

    SECTION("Enumeration finds Audio/Duplex nodes")
    {
      auto threadLoopPtr = PwThreadLoopPtr{};
      auto contextPtr = PwContextPtr{};
      auto corePtr = PwCorePtr{};
      auto proxyPtr = PwProxyPtr<::pw_proxy>{};

      threadLoopPtr.reset(::pw_thread_loop_new("DuplexTestLoop", nullptr));
      REQUIRE(threadLoopPtr);
      contextPtr.reset(::pw_context_new(::pw_thread_loop_get_loop(threadLoopPtr.get()), nullptr, 0));
      REQUIRE(contextPtr);
      REQUIRE(::pw_thread_loop_start(threadLoopPtr.get()) >= 0);

      {
        auto guard = PwThreadLoopGuard{threadLoopPtr.get()};
        corePtr.reset(::pw_context_connect(contextPtr.get(), nullptr, 0));

        if (corePtr)
        {
          auto propsPtr = utility::makeUniquePtr<::pw_properties_free>(::pw_properties_new("factory.name",
                                                                                           "support.null-audio-sink",
                                                                                           "node.name",
                                                                                           "ao-test-duplex-sink",
                                                                                           "media.class",
                                                                                           "Audio/Duplex",
                                                                                           "object.linger",
                                                                                           "false",
                                                                                           nullptr));
          void* const p = ::pw_core_create_object(
            corePtr.get(), "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, &propsPtr->dict, 0);
          proxyPtr.reset(static_cast<::pw_proxy*>(p));
          ::pw_core_sync(corePtr.get(), PW_ID_CORE, 0);
        }
      }

      if (proxyPtr)
      {
        auto const found = devicesPtr->waitUntilContains("ao-test-duplex-sink", std::chrono::seconds{1});

        INFO("Expected PipeWire device 'ao-test-duplex-sink' after 1s; observed "
             << devicesPtr->updateCount() << " device snapshots: " << devicesPtr->describeSnapshot());
        CHECK(found);
      }

      if (threadLoopPtr)
      {
        ::pw_thread_loop_stop(threadLoopPtr.get());
        {
          auto guard = PwThreadLoopGuard{threadLoopPtr.get()};
          proxyPtr.reset();
          corePtr.reset();
          contextPtr.reset();
        }
      }
    }
  }
} // namespace ao::audio::backend::test
