#include <ao/audio/IBackend.h>
#include <ao/audio/backend/PipeWireBackend.h>
#include <ao/audio/backend/PipeWireProvider.h>
#include <ao/audio/backend/detail/PipeWireShared.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_all.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>

#include <memory>
#include <thread>
#include <vector>

using namespace ao::audio::backend;
using namespace ao::audio::backend::detail;
using namespace ao::audio;

namespace
{
  struct DummySinkGuard final
  {
    PwThreadLoopPtr threadLoop;
    PwContextPtr context;
    PwCorePtr core;
    PwProxyPtr<::pw_proxy> proxy;

    DummySinkGuard(DummySinkGuard const&) = delete;
    DummySinkGuard& operator=(DummySinkGuard const&) = delete;
    DummySinkGuard(DummySinkGuard&&) = delete;
    DummySinkGuard& operator=(DummySinkGuard&&) = delete;

    DummySinkGuard()
    {
      ensurePipeWireInit();
      threadLoop.reset(::pw_thread_loop_new("TestSinkLoop", nullptr));

      if (!threadLoop)
      {
        return;
      }

      context.reset(::pw_context_new(::pw_thread_loop_get_loop(threadLoop.get()), nullptr, 0));

      if (!context)
      {
        return;
      }

      if (::pw_thread_loop_start(threadLoop.get()) < 0)
      {
        return;
      }

      ::pw_thread_loop_lock(threadLoop.get());
      core.reset(::pw_context_connect(context.get(), nullptr, 0));

      if (core)
      {
        ::pw_properties* const props = ::pw_properties_new("factory.name",
                                                           "support.null-audio-sink",
                                                           "node.name",
                                                           "rs-test-dummy-sink",
                                                           "media.class",
                                                           "Audio/Sink",
                                                           "object.linger",
                                                           "false",
                                                           nullptr);

        // Create node via adapter factory
        void* const p =
          ::pw_core_create_object(core.get(), "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, &props->dict, 0);
        proxy.reset(static_cast<::pw_proxy*>(p));
        ::pw_properties_free(props);

        // Sync to ensure it's processed
        ::pw_core_sync(core.get(), PW_ID_CORE, 0);
      }

      ::pw_thread_loop_unlock(threadLoop.get());
    }

    ~DummySinkGuard()
    {
      if (threadLoop)
      {
        ::pw_thread_loop_lock(threadLoop.get());
        proxy.reset(); // Destroy proxy while lock is held
        ::pw_thread_loop_unlock(threadLoop.get());
        ::pw_thread_loop_stop(threadLoop.get());
      }
    }

    bool isValid() const { return proxy != nullptr; }
  };
}

TEST_CASE("PipeWireProvider - Integration with Real Daemon via API", "[integration][pipewire]")
{
  // Check if PipeWire is available
  ensurePipeWireInit();

  // Quick check if we can connect to a daemon
  {
    auto* loop = ::pw_main_loop_new(nullptr);
    auto* context = ::pw_context_new(::pw_main_loop_get_loop(loop), nullptr, 0);
    auto* core = ::pw_context_connect(context, nullptr, 0);

    if (core == nullptr)
    {
      ::pw_main_loop_destroy(loop);
      WARN("Skipping PipeWire integration test: Daemon not running");
      return;
    }

    ::pw_core_disconnect(core);
    ::pw_context_destroy(context);
    ::pw_main_loop_destroy(loop);
  }

  auto const sinkGuard = DummySinkGuard{};

  if (!sinkGuard.isValid())
  {
    WARN("Skipping PipeWire integration test: Failed to create dummy sink");
    return;
  }

  auto currentDevices = std::make_shared<std::vector<Device>>();
  auto provider = PipeWireProvider{};
  auto const sub =
    provider.subscribeDevices([currentDevices](std::vector<Device> const& devices) { *currentDevices = devices; });

  SECTION("Enumeration finds the dummy sink")
  {
    bool found = false;

    // Wait a bit for PipeWire to propagate the new node to the provider's registry
    for (int i = 0; i < 20; ++i)
    {
      for (auto const& d : *currentDevices)
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

      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    REQUIRE(found);
  }

  SECTION("Enumeration finds Audio/Duplex nodes")
  {
    ensurePipeWireInit();
    auto threadLoop = PwThreadLoopPtr{};
    auto context = PwContextPtr{};
    auto core = PwCorePtr{};
    auto proxy = PwProxyPtr<::pw_proxy>{};

    threadLoop.reset(::pw_thread_loop_new("DuplexTestLoop", nullptr));
    REQUIRE(threadLoop);
    context.reset(::pw_context_new(::pw_thread_loop_get_loop(threadLoop.get()), nullptr, 0));
    REQUIRE(context);
    REQUIRE(::pw_thread_loop_start(threadLoop.get()) >= 0);

    ::pw_thread_loop_lock(threadLoop.get());
    core.reset(::pw_context_connect(context.get(), nullptr, 0));

    if (core)
    {
      ::pw_properties* const props = ::pw_properties_new("factory.name",
                                                         "support.null-audio-sink",
                                                         "node.name",
                                                         "ao-test-duplex-sink",
                                                         "media.class",
                                                         "Audio/Duplex",
                                                         "object.linger",
                                                         "false",
                                                         nullptr);
      void* const p =
        ::pw_core_create_object(core.get(), "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, &props->dict, 0);
      proxy.reset(static_cast<::pw_proxy*>(p));
      ::pw_properties_free(props);
      ::pw_core_sync(core.get(), PW_ID_CORE, 0);
    }

    ::pw_thread_loop_unlock(threadLoop.get());

    if (proxy)
    {
      bool found = false;

      for (int i = 0; i < 20; ++i)
      {
        for (auto const& d : *currentDevices)
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

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      CHECK(found);
    }

    if (threadLoop)
    {
      ::pw_thread_loop_lock(threadLoop.get());
      proxy.reset();
      ::pw_thread_loop_unlock(threadLoop.get());
      ::pw_thread_loop_stop(threadLoop.get());
    }
  }
}
