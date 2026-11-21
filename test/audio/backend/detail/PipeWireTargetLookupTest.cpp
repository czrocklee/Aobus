// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/PipeWireTargetLookup.h"

#include "lib/audio/backend/detail/PipeWireRuntime.h"
#include <ao/Error.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/utility/Raii.h>

#include <catch2/catch_test_macros.hpp>

extern "C"
{
#include <pipewire/client.h>
#include <pipewire/context.h>
#include <pipewire/core.h>
#include <pipewire/global.h>
#include <pipewire/impl-client.h>
#include <pipewire/keys.h>
#include <pipewire/properties.h>
#include <pipewire/resource.h>
#include <pipewire/thread-loop.h>
#include <spa/utils/dict.h>
}

#include <cerrno>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string_view>

namespace ao::audio::backend::detail::test
{
  namespace
  {
    constexpr std::int64_t kTestTimeoutNanoseconds = 5'000'000'000;

    Result<> lookupMissingReroutableTarget(::pw_core& core,
                                           ::pw_thread_loop& loop,
                                           ::pw_properties& properties,
                                           std::int64_t timeoutNanoseconds)
    {
      auto deadline = ::timespec{};
      REQUIRE(::pw_thread_loop_get_time(&loop, &deadline, timeoutNanoseconds) == 0);
      return setPipeWireNamedTarget(core,
                                    loop,
                                    deadline,
                                    properties,
                                    "node:ao-test-unadvertised-target",
                                    OutputRoutingPolicy::AllowSystemReroute,
                                    false);
    }

    struct TargetLookupErrorInjection final
    {
      SpaHookGuard clientListener;
      // An unset id means the newly created registry itself.
      std::optional<std::uint32_t> optProxyId = {};
      bool sent = false;
    };

    ::pw_impl_client_events const kClientEvents = []
    {
      auto events = ::pw_impl_client_events{};
      events.version = PW_VERSION_IMPL_CLIENT_EVENTS;
      events.resource_added = [](void* data, ::pw_resource* resource) noexcept
      {
        if (std::string_view{::pw_resource_get_type(resource, nullptr)} != PW_TYPE_INTERFACE_Registry)
        {
          return;
        }

        auto& injection = *static_cast<TargetLookupErrorInjection*>(data);
        auto* client = ::pw_resource_get_client(resource);
        auto* fault = injection.optProxyId ? ::pw_impl_client_find_resource(client, *injection.optProxyId) : resource;

        if (fault != nullptr)
        {
          // Respond to get_registry, before the server handles the lookup's sync.
          ::pw_resource_error(fault, -EIO, "Injected lookup-time proxy error");
          injection.sent = true;
        }
      };
      return events;
    }();

    ::pw_context_events const kContextEvents = []
    {
      auto events = ::pw_context_events{};
      events.version = PW_VERSION_CONTEXT_EVENTS;
      events.global_added = [](void* data, ::pw_global* global) noexcept
      {
        if (std::string_view{::pw_global_get_type(global)} == PW_TYPE_INTERFACE_Client)
        {
          auto& injection = *static_cast<TargetLookupErrorInjection*>(data);
          auto* client = static_cast<::pw_impl_client*>(::pw_global_get_object(global));
          ::pw_impl_client_add_listener(client, injection.clientListener.get(), &kClientEvents, &injection);
        }
      };
      return events;
    }();
  } // namespace

  TEST_CASE("PipeWireTargetLookup - only core and current registry errors prevent fallback")
  {
    auto const envGuard = PipeWireEnvironmentGuard{};
    auto const loopPtr = PwThreadLoopPtr{::pw_thread_loop_new("TestTargetLookup", nullptr)};
    REQUIRE(loopPtr);
    auto const contextPtr = PwContextPtr{::pw_context_new(::pw_thread_loop_get_loop(loopPtr.get()), nullptr, 0)};
    REQUIRE(contextPtr);
    REQUIRE(::pw_thread_loop_start(loopPtr.get()) == 0);
    auto const runningLoopPtr = utility::makeUniquePtr<::pw_thread_loop_stop>(loopPtr.get());

    auto const lock = PwThreadLoopGuard{loopPtr.get()};
    // Detach both server hooks before disconnecting, with the loop still locked.
    auto corePtr = PwCorePtr{};
    auto injection = TargetLookupErrorInjection{};
    auto contextListener = SpaHookGuard{};
    ::pw_context_add_listener(contextPtr.get(), contextListener.get(), &kContextEvents, &injection);
    corePtr.reset(::pw_context_connect_self(contextPtr.get(), nullptr, 0));
    REQUIRE(corePtr);
    auto const dict = ::spa_dict{};
    auto const propsPtr = utility::makeUniquePtr<::pw_properties_free>(::pw_properties_new_dict(&dict));
    REQUIRE(propsPtr);

    auto const lookup = [&](std::optional<std::uint32_t> optProxyId)
    {
      injection.optProxyId = optProxyId;
      auto res = lookupMissingReroutableTarget(*corePtr, *loopPtr, *propsPtr, kTestTimeoutNanoseconds);
      CHECK(injection.sent);
      CHECK(::pw_properties_get(propsPtr.get(), PW_KEY_TARGET_OBJECT) == nullptr);
      return res;
    };

    SECTION("an error on another proxy does not prevent fallback")
    {
      CHECK(lookup(PW_ID_CLIENT));
    }

    SECTION("an error on the core prevents fallback")
    {
      auto const res = lookup(PW_ID_CORE);
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::InitFailed);
    }

    SECTION("an error on the current registry prevents fallback")
    {
      auto const res = lookup(std::nullopt);
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::InitFailed);
    }
  }

  TEST_CASE("PipeWireTargetLookup - an incomplete registry cannot authorize fallback at the deadline")
  {
    auto const envGuard = PipeWireEnvironmentGuard{};
    auto const loopPtr = PwThreadLoopPtr{::pw_thread_loop_new("TestTargetDeadline", nullptr)};
    REQUIRE(loopPtr);
    auto const contextPtr = PwContextPtr{::pw_context_new(::pw_thread_loop_get_loop(loopPtr.get()), nullptr, 0)};
    REQUIRE(contextPtr);
    auto const corePtr = PwCorePtr{::pw_context_connect_self(contextPtr.get(), nullptr, 0)};
    REQUIRE(corePtr);
    auto const dict = ::spa_dict{};
    auto const propsPtr = utility::makeUniquePtr<::pw_properties_free>(::pw_properties_new_dict(&dict));
    REQUIRE(propsPtr);

    {
      // Without a running dispatch loop, the sync reply cannot race the already-due deadline.
      auto const lock = PwThreadLoopGuard{loopPtr.get()};
      auto const res = lookupMissingReroutableTarget(*corePtr, *loopPtr, *propsPtr, 0);
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::InitFailed);
    }

    // The same core and missing name permit native policy after a completed round trip.
    REQUIRE(::pw_thread_loop_start(loopPtr.get()) == 0);
    auto const runningLoopPtr = utility::makeUniquePtr<::pw_thread_loop_stop>(loopPtr.get());
    auto const lock = PwThreadLoopGuard{loopPtr.get()};
    CHECK(lookupMissingReroutableTarget(*corePtr, *loopPtr, *propsPtr, kTestTimeoutNanoseconds));
    CHECK(::pw_properties_get(propsPtr.get(), PW_KEY_TARGET_OBJECT) == nullptr);
  }
} // namespace ao::audio::backend::detail::test
