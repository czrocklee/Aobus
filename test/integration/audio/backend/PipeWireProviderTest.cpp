// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Backend.h>
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

#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
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
  }

  TEST_CASE("PipeWireProvider - Integration with Real Daemon via API", "[integration][pipewire]")
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

    auto currentDevicesPtr = std::make_shared<std::vector<Device>>();
    auto provider = PipeWireProvider{};
    auto const sub = provider.subscribeDevices([currentDevicesPtr](std::vector<Device> const& devices)
                                               { *currentDevicesPtr = devices; });

    SECTION("Enumeration finds the dummy sink")
    {
      bool found = false;

      // Wait a bit for PipeWire to propagate the new node to the provider's registry
      for (std::int32_t i = 0; i < 20; ++i)
      {
        for (auto const& d : *currentDevicesPtr)
        {
          if (d.displayName == "rs-test-dummy-sink" || d.id == "rs-test-dummy-sink")
          {
            found = true;
            break;
          }
        }

        if (found)
        {
          break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{50});
      }

      REQUIRE(found);
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
        bool found = false;

        for (std::int32_t i = 0; i < 20; ++i)
        {
          for (auto const& d : *currentDevicesPtr)
          {
            if (d.displayName == "ao-test-duplex-sink" || d.id == "ao-test-duplex-sink")
            {
              found = true;
              break;
            }
          }

          if (found)
          {
            break;
          }

          std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }

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
