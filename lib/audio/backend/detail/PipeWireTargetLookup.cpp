// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "backend/detail/PipeWireTargetLookup.h"

#include "backend/detail/PipeWireMonitorParsing.h"
#include "backend/detail/PipeWireRuntime.h"
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/audio/PipeWireTargetIdentity.h>

extern "C"
{
#include <pipewire/pipewire.h>
#include <spa/utils/dict.h>
}

#include <cstdint>
#include <ctime>
#include <exception>
#include <expected>
#include <format>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace ao::audio::backend::detail
{
  namespace
  {
    struct TargetSnapshot final
    {
      ::pw_thread_loop& loop;
      std::string_view nodeName;
      // The registry id is only a handle for global_remove, never a link target.
      std::unordered_map<std::uint32_t, std::optional<std::uint64_t>> matches;
      std::optional<std::uint32_t> optRegistryProxyId = {};
      std::int32_t sequence = -1;
      bool done = false;
      std::optional<std::int32_t> optProtocolError;
    };

    void handleGlobal(void* data,
                      std::uint32_t id,
                      std::uint32_t /*permissions*/,
                      char const* type,
                      std::uint32_t /*version*/,
                      ::spa_dict const* properties) noexcept
    {
      if (properties == nullptr || std::string_view{type} != PW_TYPE_INTERFACE_Node)
      {
        return;
      }

      auto& snapshot = *static_cast<TargetSnapshot*>(data);
      auto const* name = ::spa_dict_lookup(properties, PW_KEY_NODE_NAME);
      auto const* mediaClass = ::spa_dict_lookup(properties, PW_KEY_MEDIA_CLASS);

      if (name == nullptr || snapshot.nodeName != name || mediaClass == nullptr || !isSinkMediaClass(mediaClass))
      {
        return;
      }

      // Count the name even when it has no usable serial: a second such node
      // makes the first node ambiguous, not uniquely targetable.
      try
      {
        snapshot.matches.insert_or_assign(id, pipeWireObjectSerial(properties));
      }
      catch (...)
      {
        AO_FATAL_EXCEPTION(std::current_exception(), "PipeWire target registry callback");
      }
    }

    ::pw_registry_events const kRegistryEvents = []
    {
      auto events = ::pw_registry_events{};
      events.version = PW_VERSION_REGISTRY_EVENTS;
      events.global = handleGlobal;
      events.global_remove = [](void* data, std::uint32_t id) noexcept
      { static_cast<TargetSnapshot*>(data)->matches.erase(id); };
      return events;
    }();

    ::pw_core_events const kCoreEvents = []
    {
      auto events = ::pw_core_events{};
      events.version = PW_VERSION_CORE_EVENTS;
      events.done = [](void* data, std::uint32_t id, std::int32_t sequence) noexcept
      {
        if (auto& snapshot = *static_cast<TargetSnapshot*>(data); id == PW_ID_CORE && sequence == snapshot.sequence)
        {
          snapshot.done = true;
          ::pw_thread_loop_signal(&snapshot.loop, false);
        }
      };
      events.error =
        [](void* data, std::uint32_t id, std::int32_t /*sequence*/, std::int32_t code, char const* /*message*/) noexcept
      {
        // Error sequences identify the failed request, not necessarily our sync.
        if (auto& snapshot = *static_cast<TargetSnapshot*>(data); id == PW_ID_CORE || id == snapshot.optRegistryProxyId)
        {
          snapshot.optProtocolError = code;
          ::pw_thread_loop_signal(&snapshot.loop, false);
        }
      };
      return events;
    }();

    Result<std::optional<std::uint64_t>> lookupTargetSerial(::pw_core& core,
                                                            ::pw_thread_loop& loop,
                                                            ::timespec const& deadline,
                                                            std::string_view nodeName)
    {
      // Only the timed wait releases the caller's lock. Both listeners are
      // removed with it held, before their borrowed callback state expires.
      auto snapshot = TargetSnapshot{.loop = loop, .nodeName = nodeName, .matches = {}, .optProtocolError = {}};
      auto coreListener = SpaHookGuard{};

      if (::pw_core_add_listener(&core, coreListener.get(), &kCoreEvents, &snapshot) < 0)
      {
        return makeError(Error::Code::InitFailed, "Failed to start PipeWire target lookup");
      }

      auto registryPtr = PwRegistryPtr{::pw_core_get_registry(&core, PW_VERSION_REGISTRY, 0)};
      auto registryListener = SpaHookGuard{};

      if (!registryPtr ||
          ::pw_registry_add_listener(registryPtr.get(), registryListener.get(), &kRegistryEvents, &snapshot) < 0)
      {
        return makeError(Error::Code::InitFailed, "Failed to observe PipeWire target registry");
      }

      // PipeWire's typed registry handle is an opaque proxy on this connection, not a registry global ID.
      auto* const proxy =
        reinterpret_cast<::pw_proxy*>(registryPtr.get()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
      snapshot.optRegistryProxyId = ::pw_proxy_get_id(proxy);
      snapshot.sequence = ::pw_core_sync(&core, PW_ID_CORE, 0);

      if (snapshot.sequence < 0)
      {
        return makeError(Error::Code::InitFailed, "Failed to synchronize PipeWire target registry");
      }

      while (!snapshot.done && !snapshot.optProtocolError)
      {
        if (::pw_thread_loop_timed_wait_full(&loop, &deadline) < 0)
        {
          return makeError(Error::Code::InitFailed, "Timed out waiting for PipeWire target registry");
        }
      }

      if (snapshot.optProtocolError)
      {
        return makeError(
          Error::Code::InitFailed, std::format("PipeWire target registry error: {}", *snapshot.optProtocolError));
      }

      if (snapshot.matches.size() != 1)
      {
        return std::nullopt;
      }

      return snapshot.matches.begin()->second;
    }
  } // namespace

  Result<> setPipeWireNamedTarget(::pw_core& core,
                                  ::pw_thread_loop& loop,
                                  ::timespec const& deadline,
                                  ::pw_properties& properties,
                                  std::string_view deviceId,
                                  OutputRoutingPolicy routingPolicy,
                                  bool exclusive)
  {
    auto const optNodeName = pipeWireTargetNodeName(deviceId);

    if (!optNodeName)
    {
      return makeError(Error::Code::InvalidInput, "Invalid PipeWire target identity");
    }

    auto serialRes = lookupTargetSerial(core, loop, deadline, *optNodeName);

    if (!serialRes)
    {
      return std::unexpected{std::move(serialRes.error())};
    }

    auto const& optSerial = *serialRes;

    if (optSerial && ::pw_properties_set(&properties, PW_KEY_TARGET_OBJECT, std::format("{}", *optSerial).c_str()) < 0)
    {
      return makeError(Error::Code::InitFailed, "Failed to set PipeWire target object");
    }

    if (routingPolicy == OutputRoutingPolicy::Fixed)
    {
      if (!optSerial)
      {
        return makeError(Error::Code::DeviceNotFound, "PipeWire target has no unique identifiable sink");
      }

      // Refuse native fallback if this object disappears after the snapshot;
      // also ignore target metadata that would move a pinned stream later.
      if (::pw_properties_set(&properties, "node.dont-fallback", "true") < 0 ||
          ::pw_properties_set(&properties, "node.dont-move", "true") < 0)
      {
        return makeError(Error::Code::InitFailed, "Failed to pin PipeWire stream target");
      }
    }

    if (exclusive && ::pw_properties_set(&properties, PW_KEY_NODE_EXCLUSIVE, "true") < 0)
    {
      return makeError(Error::Code::InitFailed, "Failed to set PipeWire exclusive mode");
    }

    return {};
  }
} // namespace ao::audio::backend::detail
