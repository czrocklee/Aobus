// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/backend/PipeWireMonitor.h>
#include <ao/audio/backend/detail/PipeWireShared.h>
#include <ao/utility/ByteView.h>
#include <ao/utility/Log.h>

extern "C"
{
#include <pipewire/core.h>
#include <pipewire/keys.h>
#include <pipewire/link.h>
#include <pipewire/node.h>
#include <pipewire/pipewire.h>
#include <pipewire/proxy.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/param/format-utils.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/support/loop.h>
#include <spa/utils/defs.h>
#include <spa/utils/dict.h>
#include <spa/utils/type.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <ranges>
#include <unordered_map>
#include <unordered_set>

#include <ao/audio/backend/detail/PipeWireMonitorHelpers.h>

namespace ao::audio::backend
{
  using namespace detail;

  enum class NodeBindingRole : std::uint8_t
  {
    Stream,
    Sink,
  };

  // --- Internal Helpers (Monitor only) ---

  bool isActiveLink(::pw_link_state state) noexcept
  {
    return state == PW_LINK_STATE_PAUSED || state == PW_LINK_STATE_ACTIVE;
  }

  std::string formatStreamFormat(ao::audio::Format const& format)
  {
    auto const* const sampleType = format.isFloat ? "float" : "pcm";

    return std::format("{}Hz/{}-bit/{}ch {}", format.sampleRate, format.bitDepth, format.channels, sampleType);
  }

  // --- PipeWireMonitor Impl ---

  struct PipeWireMonitor::Impl final
  {
    struct LinkBinding final
    {
      std::uint32_t id = PW_ID_ANY;
      detail::PwProxyPtr<::pw_link> proxy;
      detail::SpaHookGuard listener;
      void reset()
      {
        id = PW_ID_ANY;
        listener.reset();
        proxy.reset();
      }
    };

    struct NodeBinding final
    {
      std::uint32_t id = PW_ID_ANY;
      Impl* impl = nullptr;
      NodeBindingRole role = NodeBindingRole::Sink;
      detail::PwProxyPtr<::pw_node> proxy;
      detail::SpaHookGuard listener;
      void reset()
      {
        id = PW_ID_ANY;
        impl = nullptr;
        role = NodeBindingRole::Sink;
        listener.reset();
        proxy.reset();
      }
    };

    struct GraphSubscription final
    {
      std::uint64_t id = 0;
      std::string routeAnchor;
      std::function<void(ao::audio::flow::Graph const&)> callback;
    };

    struct DeviceSubscription final
    {
      std::uint64_t id = 0;
      PipeWireMonitor::DeviceCallback callback;
    };

    Impl()
    {
      detail::ensurePipeWireInit();
      threadLoop.reset(::pw_thread_loop_new("PipeWireMonitor", nullptr));

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
        AUDIO_LOG_ERROR("Failed to start PipeWire thread loop");
        return;
      }

      ::pw_thread_loop_lock(threadLoop.get());
      core.reset(::pw_context_connect(context.get(), nullptr, 0));
      ::pw_thread_loop_unlock(threadLoop.get());
    }

    ~Impl()
    {
      stopping.store(true, std::memory_order_release);
      {
        auto const lock = std::lock_guard<std::mutex>{mutex};
        deviceSubscriptions.clear();
        graphSubscriptions.clear();
      }

      if (threadLoop)
      {
        ::pw_thread_loop_stop(threadLoop.get());
      }

      refreshEvent.reset();
      linkBindings.clear();
      streamNodeBindings.clear();
      sinkNodeBindings.clear();
      registryListener.reset();
      registry.reset();
      coreListener.reset();
      nodeFormatMap.clear();
      sinkCapabilitiesMap.clear();
      sinkPropsMap.clear();
      core.reset();
      context.reset();
      threadLoop.reset();
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    detail::PwThreadLoopPtr threadLoop;
    detail::PwContextPtr context;
    detail::PwCorePtr core;

    mutable std::mutex mutex;
    detail::PwRegistryPtr registry;
    detail::SpaHookGuard registryListener;
    detail::SpaHookGuard coreListener;
    std::int32_t coreSyncSeq = -1;
    std::unordered_map<std::uint32_t, NodeRecord> nodes;
    std::unordered_map<std::uint32_t, LinkRecord> links;
    std::unordered_map<std::uint32_t, LinkBinding> linkBindings;
    std::unordered_map<std::uint32_t, std::unique_ptr<NodeBinding>> streamNodeBindings;
    std::unordered_map<std::uint32_t, std::unique_ptr<NodeBinding>> sinkNodeBindings;
    std::unordered_map<std::uint32_t, ao::audio::Format> nodeFormatMap;
    std::unordered_map<std::uint32_t, ao::audio::DeviceCapabilities> sinkCapabilitiesMap;
    std::unordered_map<std::uint32_t, SinkProps> sinkPropsMap;
    detail::SpaSourcePtr refreshEvent;

    std::atomic<bool> stopping{false};
    std::uint64_t nextSubscriptionId = 1;
    std::vector<GraphSubscription> graphSubscriptions;
    std::vector<DeviceSubscription> deviceSubscriptions;

    void triggerRefresh()
    {
      if (refreshEvent)
      {
        ::pw_loop_signal_event(::pw_thread_loop_get_loop(threadLoop.get()), refreshEvent.get());
      }
      else
      {
        refresh();
      }
    }

    void refresh();

    ao::audio::Subscription subscribeDevices(DeviceCallback callback);
    ao::audio::Subscription subscribeGraph(std::string_view routeAnchor,
                                           std::function<void(ao::audio::flow::Graph const&)> callback);

    std::vector<ao::audio::Device> enumerateSinks() const;

    void syncStreamBindings(std::unordered_set<std::uint32_t> const& subscribedStreamIds);
    void syncSinkBindings();

    struct ReachableContext
    {
      std::vector<std::uint32_t> reachableNodes;
      std::unordered_set<std::uint32_t> reachableSet;
      std::unordered_set<std::uint32_t> fullSet;
    };

    ReachableContext findReachableNodes(std::uint32_t streamId) const;
    ao::audio::flow::Node convertToAudioNode(std::uint32_t id,
                                             std::uint32_t streamId,
                                             std::unordered_set<std::uint32_t> const& reachableSet) const;
    void populateGraph(ao::audio::flow::Graph& graph, std::uint32_t streamId) const;

    static void onCoreDone(void* data, std::uint32_t /*id*/, int seq)
    {
      auto* const impl = static_cast<PipeWireMonitor::Impl*>(data);

      {
        auto const lock = std::lock_guard<std::mutex>{impl->mutex};

        if (seq == impl->coreSyncSeq)
        {
        } // Initial population done
      }

      impl->triggerRefresh();
    }

    static void onRegistryGlobal(void* data,
                                 std::uint32_t id,
                                 std::uint32_t /*permissions*/,
                                 char const* type,
                                 std::uint32_t version,
                                 ::spa_dict const* props)
    {
      auto* const impl = static_cast<PipeWireMonitor::Impl*>(data);
      auto const isNode = (::strcmp(type, PW_TYPE_INTERFACE_Node) == 0);
      auto const isLink = (::strcmp(type, PW_TYPE_INTERFACE_Link) == 0);

      if (!isNode && !isLink)
      {
        return;
      }

      static ::pw_link_events const linkEvents = {.version = PW_VERSION_LINK_EVENTS,
                                                  .info = [](void* data, ::pw_link_info const* info)
                                                  {
                                                    if (!info)
                                                    {
                                                      return;
                                                    }

                                                    auto* const impl = static_cast<PipeWireMonitor::Impl*>(data);

                                                    {
                                                      auto const lock = std::lock_guard<std::mutex>{impl->mutex};
                                                      auto& link = impl->links[info->id];

                                                      link.outputNodeId = info->output_node_id;
                                                      link.inputNodeId = info->input_node_id;

                                                      if (info->change_mask & PW_LINK_CHANGE_MASK_STATE)
                                                      {
                                                        link.state = info->state;
                                                      }
                                                    }

                                                    impl->triggerRefresh();
                                                  }};

      {
        auto const lock = std::lock_guard<std::mutex>{impl->mutex};

        if (isNode)
        {
          impl->nodes[id] = parseNodeRecord(version, props);
        }
        else if (isLink)
        {
          auto* const proxy = ::pw_registry_bind(impl->registry.get(), id, PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, 0);

          if (proxy != nullptr)
          {
            auto& binding = impl->linkBindings[id];
            binding.id = id;
            binding.proxy.reset(static_cast<::pw_link*>(proxy));
            ::pw_link_add_listener(binding.proxy.get(), binding.listener.get(), &linkEvents, impl);
          }
        }
      }

      impl->triggerRefresh();
    }

    static void onRegistryGlobalRemove(void* data, std::uint32_t id)
    {
      auto* const impl = static_cast<PipeWireMonitor::Impl*>(data);
      bool needsRefresh = false;

      {
        auto const lock = std::lock_guard<std::mutex>{impl->mutex};

        if (auto const it = impl->linkBindings.find(id); it != impl->linkBindings.end())
        {
          impl->linkBindings.erase(it);
        }

        if (auto const it = impl->streamNodeBindings.find(id); it != impl->streamNodeBindings.end())
        {
          impl->streamNodeBindings.erase(it);
          impl->nodeFormatMap.erase(id);
          needsRefresh = true;
        }

        if (auto const it = impl->sinkNodeBindings.find(id); it != impl->sinkNodeBindings.end())
        {
          impl->sinkNodeBindings.erase(it);
          impl->nodeFormatMap.erase(id);
          impl->sinkCapabilitiesMap.erase(id);
          impl->sinkPropsMap.erase(id);
          needsRefresh = true;
        }

        if (impl->nodes.contains(id))
        {
          impl->nodes.erase(id);
          needsRefresh = true;
        }

        if (impl->links.contains(id))
        {
          impl->links.erase(id);
          needsRefresh = true;
        }
      }

      if (needsRefresh)
      {
        impl->triggerRefresh();
      }
    }

    static void onNodeInfo(void* data, ::pw_node_info const* info)
    {
      if (info == nullptr)
      {
        return;
      }

      auto* const binding = static_cast<PipeWireMonitor::Impl::NodeBinding*>(data);
      auto* const impl = binding->impl;

      {
        auto const lock = std::lock_guard<std::mutex>{impl->mutex};

        if ((info->change_mask & PW_NODE_CHANGE_MASK_PROPS) != 0)
        {
          auto version = static_cast<std::uint32_t>(PW_VERSION_NODE);

          if (auto const it = impl->nodes.find(info->id); it != impl->nodes.end())
          {
            version = it->second.version;
          }

          impl->nodes[info->id] = parseNodeRecord(version, info->props);
        }
      }

      impl->triggerRefresh();
    }

    static void onNodeParam(void* data,
                            int /*seq*/,
                            std::uint32_t id,
                            std::uint32_t /*index*/,
                            std::uint32_t /*next*/,
                            ::spa_pod const* param)
    {
      auto* const binding = static_cast<PipeWireMonitor::Impl::NodeBinding*>(data);
      auto* const impl = binding->impl;

      {
        auto const lock = std::lock_guard<std::mutex>{impl->mutex};

        if (id == SPA_PARAM_Format)
        {
          if (auto const fmt = detail::parseRawStreamFormat(param))
          {
            impl->nodeFormatMap[binding->id] = *fmt;
          }
        }
        else if (binding->role == NodeBindingRole::Sink && id == SPA_PARAM_EnumFormat)
        {
          if (!impl->nodeFormatMap.contains(binding->id))
          {
            if (auto const fmt = detail::parseRawStreamFormat(param))
            {
              impl->nodeFormatMap[binding->id] = *fmt;
            }
          }

          auto& caps = impl->sinkCapabilitiesMap[binding->id];
          parseEnumFormat(param, caps);
        }
        else if (id == SPA_PARAM_Props)
        {
          mergeSinkProps(impl->sinkPropsMap[binding->id], param);
        }
      }

      impl->triggerRefresh();
    }

    static void onRefreshEvent(void* data, std::uint64_t /*expiry*/)
    {
      static_cast<PipeWireMonitor::Impl*>(data)->refresh();
    }

    static inline ::pw_core_events const coreEvents = []
    {
      ::pw_core_events ev = {};
      ev.version = PW_VERSION_CORE_EVENTS;
      ev.done = onCoreDone;
      return ev;
    }();

    static inline ::pw_registry_events const registryEvents = []
    {
      ::pw_registry_events ev = {};
      ev.version = PW_VERSION_REGISTRY_EVENTS;
      ev.global = onRegistryGlobal;
      ev.global_remove = onRegistryGlobalRemove;
      return ev;
    }();

    static inline ::pw_node_events const streamNodeEvents = []
    {
      ::pw_node_events ev = {};
      ev.version = PW_VERSION_NODE_EVENTS;
      ev.info = onNodeInfo;
      ev.param = onNodeParam;
      return ev;
    }();

    static inline ::pw_node_events const sinkNodeEvents = []
    {
      ::pw_node_events ev = {};
      ev.version = PW_VERSION_NODE_EVENTS;
      ev.info = onNodeInfo;
      ev.param = onNodeParam;
      return ev;
    }();
  };

  // --- PipeWireMonitor Implementation ---

  PipeWireMonitor::PipeWireMonitor()
    : _impl(std::make_unique<Impl>())
  {
  }

  PipeWireMonitor::~PipeWireMonitor() = default;

  void PipeWireMonitor::start()
  {
    if (!_impl->threadLoop)
    {
      return;
    }

    _impl->refreshEvent.get_deleter().loop = _impl->threadLoop.get();
    auto* const event =
      ::pw_loop_add_event(::pw_thread_loop_get_loop(_impl->threadLoop.get()), &Impl::onRefreshEvent, _impl.get());
    if (event == nullptr)
    {
      AUDIO_LOG_ERROR("Failed to add PipeWire refresh event - periodic refresh disabled");
      return;
    }
    _impl->refreshEvent.reset(event);

    ::pw_thread_loop_lock(_impl->threadLoop.get());

    if (_impl->core)
    {
      auto* const registry = ::pw_core_get_registry(_impl->core.get(), PW_VERSION_REGISTRY, 0);
      _impl->registry.reset(static_cast<::pw_registry*>(registry));

      if (_impl->registry)
      {
        ::pw_registry_add_listener(
          _impl->registry.get(), _impl->registryListener.get(), &Impl::registryEvents, _impl.get());
        ::pw_core_add_listener(_impl->core.get(), _impl->coreListener.get(), &Impl::coreEvents, _impl.get());
        _impl->coreSyncSeq = ::pw_core_sync(_impl->core.get(), PW_ID_CORE, 0);
      }
    }

    ::pw_thread_loop_unlock(_impl->threadLoop.get());
    _impl->triggerRefresh();
  }

  void PipeWireMonitor::stop()
  {
    auto const lock = std::lock_guard<std::mutex>{_impl->mutex};
    _impl->refreshEvent.reset();
    _impl->linkBindings.clear();
    _impl->streamNodeBindings.clear();
    _impl->sinkNodeBindings.clear();
    _impl->registryListener.reset();
    _impl->registry.reset();
    _impl->coreListener.reset();
    _impl->nodes.clear();
    _impl->links.clear();
    _impl->nodeFormatMap.clear();
    _impl->sinkCapabilitiesMap.clear();
    _impl->sinkPropsMap.clear();
  }

  ao::audio::Subscription PipeWireMonitor::subscribeDevices(DeviceCallback callback)
  {
    return _impl->subscribeDevices(std::move(callback));
  }

  std::vector<ao::audio::Device> PipeWireMonitor::enumerateSinks() const
  {
    return _impl->enumerateSinks();
  }

  std::optional<std::uint32_t> PipeWireMonitor::findSinkIdByName(std::string_view name) const
  {
    auto const lock = std::lock_guard<std::mutex>{_impl->mutex};

    for (auto const& [id, node] : _impl->nodes)
    {
      if (isSinkMediaClass(node.mediaClass) && node.nodeName == name)
      {
        return id;
      }
    }
    return std::nullopt;
  }

  ao::audio::Subscription PipeWireMonitor::subscribeGraph(std::string_view routeAnchor,
                                                          std::function<void(ao::audio::flow::Graph const&)> callback)
  {
    return _impl->subscribeGraph(routeAnchor, std::move(callback));
  }

  void PipeWireMonitor::refresh()
  {
    _impl->refresh();
  }

  // --- Impl Implementations ---

  ao::audio::Subscription PipeWireMonitor::Impl::subscribeDevices(DeviceCallback callback)
  {
    auto const id = nextSubscriptionId++;
    auto devices = std::vector<Device>{};
    auto cb = DeviceCallback{};

    {
      auto const lock = std::lock_guard<std::mutex>{mutex};
      deviceSubscriptions.push_back({id, callback});
      cb = std::move(callback);
      devices = enumerateSinks();
    }

    if (cb)
    {
      cb(devices);
    }

    return ao::audio::Subscription{[this, id]()
                                   {
                                     auto const lock = std::lock_guard<std::mutex>{mutex};
                                     auto const it =
                                       std::ranges::find(deviceSubscriptions, id, &DeviceSubscription::id);
                                     if (it != deviceSubscriptions.end())
                                     {
                                       deviceSubscriptions.erase(it);
                                     }
                                   }};
  }

  ao::audio::Subscription PipeWireMonitor::Impl::subscribeGraph(
    std::string_view routeAnchor,
    std::function<void(ao::audio::flow::Graph const&)> callback)
  {
    auto const id = nextSubscriptionId++;
    {
      auto const lock = std::lock_guard<std::mutex>{mutex};
      graphSubscriptions.push_back({id, std::string(routeAnchor), std::move(callback)});
    }
    triggerRefresh();

    return ao::audio::Subscription{[this, id]()
                                   {
                                     {
                                       auto const lock = std::lock_guard<std::mutex>{mutex};
                                       auto const it =
                                         std::ranges::find(graphSubscriptions, id, &GraphSubscription::id);
                                       if (it != graphSubscriptions.end())
                                       {
                                         graphSubscriptions.erase(it);
                                       }
                                     }
                                     triggerRefresh();
                                   }};
  }

  std::vector<ao::audio::Device> PipeWireMonitor::Impl::enumerateSinks() const
  {
    auto devices = std::vector<ao::audio::Device>{};
    for (auto const& [id, node] : nodes)
    {
      if (isSinkMediaClass(node.mediaClass))
      {
        auto const deviceId = node.objectSerial ? std::format("{}", *node.objectSerial) : std::format("{}", id);
        auto displayName = node.nodeNick;
        if (displayName.empty())
        {
          displayName = node.nodeName.empty() ? node.objectPath : node.nodeName;
        }
        auto const description = (node.nodeNick.empty() ? "" : node.nodeName);
        auto caps = ao::audio::DeviceCapabilities{};

        if (auto const it = sinkCapabilitiesMap.find(id); it != sinkCapabilitiesMap.end())
        {
          caps = it->second;
        }

        devices.push_back({.id = DeviceId{deviceId},
                           .displayName = displayName,
                           .description = description,
                           .isDefault = false,
                           .backendId = ao::audio::kBackendPipeWire,
                           .capabilities = {}});
        devices.push_back({.id = DeviceId{deviceId},
                           .displayName = std::format("{} (Exclusive)", displayName),
                           .description = description,
                           .isDefault = false,
                           .backendId = ao::audio::kBackendPipeWire,
                           .capabilities = caps});
      }
    }
    return devices;
  }

  void PipeWireMonitor::Impl::refresh()
  {
    ::pw_thread_loop_lock(threadLoop.get());

    // Phase 1: sync bindings under mutex
    {
      auto const lock = std::lock_guard<std::mutex>{mutex};
      auto subscribedStreamIds = std::unordered_set<std::uint32_t>{};

      for (auto const& sub : graphSubscriptions)
      {
        if (auto const parsedId = detail::parseUintProperty(sub.routeAnchor.c_str()))
        {
          subscribedStreamIds.insert(*parsedId);
        }
      }

      syncStreamBindings(subscribedStreamIds);
      syncSinkBindings();
    }

    // Phase 2: build graphs and collect callbacks under mutex
    auto pendingGraphCbs =
      std::vector<std::pair<std::function<void(ao::audio::flow::Graph const&)>, ao::audio::flow::Graph>>{};
    auto pendingDeviceCbs = std::vector<DeviceCallback>{};
    auto deviceSnapshot = std::vector<ao::audio::Device>{};

    {
      auto const lock = std::lock_guard<std::mutex>{mutex};

      for (auto const& sub : graphSubscriptions)
      {
        auto const parsedId = detail::parseUintProperty(sub.routeAnchor.c_str());
        if (parsedId && *parsedId != PW_ID_ANY && sub.callback)
        {
          auto graph = ao::audio::flow::Graph{};
          populateGraph(graph, *parsedId);
          pendingGraphCbs.emplace_back(sub.callback, std::move(graph));
        }
      }

      if (!deviceSubscriptions.empty())
      {
        deviceSnapshot = enumerateSinks();
        for (auto const& sub : deviceSubscriptions)
        {
          if (sub.callback)
          {
            pendingDeviceCbs.push_back(sub.callback);
          }
        }
      }
    }

    // Phase 3: invoke all callbacks outside ALL locks (unless stopping)
    if (!stopping.load(std::memory_order_acquire))
    {
      for (auto& [cb, graph] : pendingGraphCbs)
      {
        cb(graph);
      }

      for (auto& cb : pendingDeviceCbs)
      {
        cb(deviceSnapshot);
      }
    }

    ::pw_thread_loop_unlock(threadLoop.get());
  }

  void PipeWireMonitor::Impl::syncStreamBindings(std::unordered_set<std::uint32_t> const& subscribedStreamIds)
  {
    for (auto it = streamNodeBindings.begin(); it != streamNodeBindings.end();)
    {
      if (!subscribedStreamIds.contains(it->first))
      {
        nodeFormatMap.erase(it->first);
        it = streamNodeBindings.erase(it);
      }
      else
      {
        ++it;
      }
    }

    for (auto streamId : subscribedStreamIds)
    {
      if (streamNodeBindings.contains(streamId))
      {
        continue;
      }

      auto const it = nodes.find(streamId);

      if (!registry || it == nodes.end())
      {
        continue;
      }

      auto* node = ::pw_registry_bind(registry.get(),
                                      streamId,
                                      PW_TYPE_INTERFACE_Node,
                                      std::min(it->second.version, static_cast<std::uint32_t>(PW_VERSION_NODE)),
                                      0);

      if (node == nullptr)
      {
        continue;
      }

      auto binding = std::make_unique<NodeBinding>();
      binding->id = streamId;
      binding->impl = this;
      binding->role = NodeBindingRole::Stream;
      binding->proxy.reset(static_cast<::pw_node*>(node));
      auto const params = std::to_array<std::uint32_t>({SPA_PARAM_Format, SPA_PARAM_Props});
      ::pw_node_subscribe_params(
        binding->proxy.get(), ao::utility::layout::asLegacyPtr<std::uint32_t>(params.data()), params.size());
      ::pw_node_enum_params(binding->proxy.get(), 1, SPA_PARAM_Format, 0, -1, nullptr);
      ::pw_node_enum_params(binding->proxy.get(), 2, SPA_PARAM_Props, 0, -1, nullptr);
      auto* bindingPtr = binding.get();
      ::pw_node_add_listener(bindingPtr->proxy.get(), bindingPtr->listener.get(), &streamNodeEvents, bindingPtr);
      streamNodeBindings[streamId] = std::move(binding);
    }
  }

  void PipeWireMonitor::Impl::syncSinkBindings()
  {
    for (auto const& [id, node] : nodes)
    {
      if (isSinkMediaClass(node.mediaClass) && !sinkNodeBindings.contains(id))
      {
        auto* proxy = ::pw_registry_bind(
          registry.get(), id, PW_TYPE_INTERFACE_Node, std::min(node.version, (std::uint32_t)PW_VERSION_NODE), 0);

        if (proxy != nullptr)
        {
          auto binding = std::make_unique<NodeBinding>();
          binding->id = id;
          binding->impl = this;
          binding->role = NodeBindingRole::Sink;
          binding->proxy.reset(static_cast<::pw_node*>(proxy));

          auto const params = std::to_array<std::uint32_t>({SPA_PARAM_Format, SPA_PARAM_EnumFormat, SPA_PARAM_Props});
          ::pw_node_subscribe_params(
            binding->proxy.get(), ao::utility::layout::asLegacyPtr<std::uint32_t>(params.data()), params.size());
          ::pw_node_enum_params(binding->proxy.get(), 1, SPA_PARAM_Format, 0, -1, nullptr);
          ::pw_node_enum_params(binding->proxy.get(), 2, SPA_PARAM_EnumFormat, 0, -1, nullptr);
          ::pw_node_enum_params(
            binding->proxy.get(), 3, SPA_PARAM_Props, 0, -1, nullptr); // NOLINT(readability-magic-numbers)

          auto* bindingPtr = binding.get();
          ::pw_node_add_listener(bindingPtr->proxy.get(), bindingPtr->listener.get(), &sinkNodeEvents, bindingPtr);
          sinkNodeBindings[id] = std::move(binding);
        }
      }
    }
  }

  PipeWireMonitor::Impl::ReachableContext PipeWireMonitor::Impl::findReachableNodes(std::uint32_t streamId) const
  {
    ReachableContext ctx;

    if (streamId != PW_ID_ANY)
    {
      ctx.reachableNodes.push_back(streamId);
      ctx.reachableSet.insert(streamId);
    }

    for (std::size_t i = 0; i < ctx.reachableNodes.size(); ++i)
    {
      auto curr = ctx.reachableNodes[i];

      // NOLINTNEXTLINE(readability-identifier-length)
      for (auto const& [_, link] : links)
      {
        if (!isActiveLink(static_cast<::pw_link_state>(link.state)) || link.outputNodeId != curr ||
            link.inputNodeId == PW_ID_ANY)
        {
          continue;
        }

        if (ctx.reachableSet.insert(link.inputNodeId).second)
        {
          ctx.reachableNodes.push_back(link.inputNodeId);
        }
      }
    }

    ctx.fullSet = ctx.reachableSet;

    // NOLINTNEXTLINE(readability-identifier-length)
    for (auto const& [_, link] : links)
    {
      if (isActiveLink(static_cast<::pw_link_state>(link.state)) && ctx.reachableSet.contains(link.inputNodeId))
      {
        ctx.fullSet.insert(link.outputNodeId);
      }
    }

    return ctx;
  }

  ao::audio::flow::Node PipeWireMonitor::Impl::convertToAudioNode(
    std::uint32_t id,
    std::uint32_t streamId,
    std::unordered_set<std::uint32_t> const& reachableSet) const
  {
    auto const it = nodes.find(id);

    if (it == nodes.end())
    {
      if (id == streamId)
      {
        return ao::audio::flow::Node{.id = std::format("{}", id),
                                     .type = ao::audio::flow::NodeType::Stream,
                                     .name = "Aobus Playback",
                                     .optFormat = std::nullopt};
      }

      return ao::audio::flow::Node{};
    }

    bool const isSink = isSinkMediaClass(it->second.mediaClass);
    bool const isRs = (id == streamId);
    auto type = ao::audio::flow::NodeType::ExternalSource;

    if (isRs)
    {
      type = ao::audio::flow::NodeType::Stream;
    }
    else if (isSink)
    {
      type = ao::audio::flow::NodeType::Sink;
    }
    else if (reachableSet.contains(id))
    {
      type = ao::audio::flow::NodeType::Intermediary;
    }

    auto name = it->second.nodeNick;

    if (name.empty())
    {
      name = it->second.nodeName.empty() ? it->second.objectPath : it->second.nodeName;
    }

    ao::audio::flow::Node node{
      .id = std::format("{}", id), .type = type, .name = name, .objectPath = it->second.objectPath};

    if (auto const optFormatIt = nodeFormatMap.find(id); optFormatIt != nodeFormatMap.end())
    {
      node.optFormat = optFormatIt->second;
    }

    if (auto const propsIt = sinkPropsMap.find(id); propsIt != sinkPropsMap.end())
    {
      auto const& sinkProps = propsIt->second;
      node.volumeNotUnity = !sinkProps.isUnity();
      node.isMuted = sinkProps.isMuted || sinkProps.isSoftMuted;
    }

    return node;
  }

  void PipeWireMonitor::Impl::populateGraph(ao::audio::flow::Graph& graph, std::uint32_t streamId) const
  {
    auto const ctx = findReachableNodes(streamId);

    for (auto id : ctx.fullSet)
    {
      auto node = convertToAudioNode(id, streamId, ctx.reachableSet);

      if (!node.id.empty())
      {
        graph.nodes.push_back(std::move(node));
      }
    }

    // NOLINTNEXTLINE(readability-identifier-length)
    for (auto const& [_, link] : links)
    {
      if (isActiveLink(static_cast<::pw_link_state>(link.state)) && ctx.fullSet.contains(link.outputNodeId) &&
          ctx.fullSet.contains(link.inputNodeId))
      {
        graph.connections.push_back({.sourceId = std::format("{}", link.outputNodeId),
                                     .destId = std::format("{}", link.inputNodeId),
                                     .isActive = true});
      }
    }
  }
} // namespace ao::audio::backend
