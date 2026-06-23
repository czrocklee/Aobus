// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/backend/PipeWireMonitor.h>
#include <ao/audio/backend/detail/PipeWireShared.h>
#include <ao/utility/ByteView.h>
#include <ao/utility/Log.h>

extern "C"
{
#include <pipewire/core.h>
#include <pipewire/link.h>
#include <pipewire/node.h>
#include <pipewire/pipewire.h>
#include <spa/param/param.h>
#include <spa/pod/pod.h>
#include <spa/utils/dict.h>
}

#include <ao/audio/Backend.h>
#include <ao/audio/Format.h>
#include <ao/audio/Subscription.h>
#include <ao/audio/backend/detail/PipeWireMonitorHelpers.h>
#include <ao/audio/flow/Graph.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ao::audio::backend
{
  using namespace detail;

  enum class NodeBindingRole : std::uint8_t
  {
    Stream,
    Sink,
  };

  // --- Internal Helpers (Monitor only) ---

  namespace
  {
    bool isActiveLink(::pw_link_state state) noexcept
    {
      return state == PW_LINK_STATE_PAUSED || state == PW_LINK_STATE_ACTIVE;
    }
  }

  // --- PipeWireMonitor Impl ---

  struct PipeWireMonitor::Impl final
  {
    struct LinkBinding final
    {
      std::uint32_t id = PW_ID_ANY;
      detail::PwProxyPtr<::pw_link> proxyPtr;
      detail::SpaHookGuard listener;

      void reset()
      {
        id = PW_ID_ANY;
        listener.reset();
        proxyPtr.reset();
      }
    };

    struct NodeBinding final
    {
      std::uint32_t id = PW_ID_ANY;
      Impl* impl = nullptr;
      NodeBindingRole role = NodeBindingRole::Sink;
      detail::PwProxyPtr<::pw_node> proxyPtr;
      detail::SpaHookGuard listener;

      void reset()
      {
        id = PW_ID_ANY;
        impl = nullptr;
        role = NodeBindingRole::Sink;
        listener.reset();
        proxyPtr.reset();
      }
    };

    struct GraphSubscription final
    {
      std::uint64_t id = 0;
      std::string routeAnchor;
      std::function<void(flow::Graph const&)> callback;
    };

    struct DeviceSubscription final
    {
      std::uint64_t id = 0;
      PipeWireMonitor::DeviceCallback callback;
    };

    Impl()
    {
      threadLoopPtr.reset(::pw_thread_loop_new("PipeWireMonitor", nullptr));

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
        AUDIO_LOG_ERROR("Failed to start PipeWire thread loop");
        return;
      }

      {
        auto guard = PwThreadLoopGuard{threadLoopPtr.get()};
        corePtr.reset(::pw_context_connect(contextPtr.get(), nullptr, 0));
      }
    }

    ~Impl()
    {
      stopping.store(true, std::memory_order_release);
      {
        auto const lock = std::scoped_lock{mutex};
        deviceSubscriptions.clear();
        graphSubscriptions.clear();
      }

      if (threadLoopPtr)
      {
        ::pw_thread_loop_stop(threadLoopPtr.get());
      }

      {
        auto guard = PwThreadLoopGuard{threadLoopPtr.get()};
        refreshEventPtr.reset();
        linkBindings.clear();
        streamNodeBindings.clear();
        sinkNodeBindings.clear();
        registryListener.reset();
        registryPtr.reset();
        coreListener.reset();
        nodeFormatMap.clear();
        sinkCapabilitiesMap.clear();
        sinkPropsMap.clear();
        corePtr.reset();
        contextPtr.reset();
      }

      threadLoopPtr.reset();
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    detail::PipeWireEnvironmentGuard envGuard;
    detail::PwThreadLoopPtr threadLoopPtr;
    detail::PwContextPtr contextPtr;
    detail::PwCorePtr corePtr;

    mutable std::mutex mutex;
    detail::PwRegistryPtr registryPtr;
    detail::SpaHookGuard registryListener;
    detail::SpaHookGuard coreListener;
    std::int32_t coreSyncSeq = -1;
    std::unordered_map<std::uint32_t, NodeRecord> nodes;
    std::unordered_map<std::uint32_t, LinkRecord> links;
    std::unordered_map<std::uint32_t, LinkBinding> linkBindings;
    std::unordered_map<std::uint32_t, std::unique_ptr<NodeBinding>> streamNodeBindings;
    std::unordered_map<std::uint32_t, std::unique_ptr<NodeBinding>> sinkNodeBindings;
    std::unordered_map<std::uint32_t, Format> nodeFormatMap;
    std::unordered_map<std::uint32_t, DeviceCapabilities> sinkCapabilitiesMap;
    std::unordered_map<std::uint32_t, SinkProps> sinkPropsMap;
    detail::SpaSourcePtr refreshEventPtr;

    std::atomic<bool> stopping{false};
    std::uint64_t nextSubscriptionId = 1;
    std::vector<GraphSubscription> graphSubscriptions;
    std::vector<DeviceSubscription> deviceSubscriptions;

    void triggerRefresh()
    {
      if (refreshEventPtr)
      {
        ::pw_loop_signal_event(::pw_thread_loop_get_loop(threadLoopPtr.get()), refreshEventPtr.get());
      }
      else
      {
        refresh();
      }
    }

    void refresh();

    Subscription subscribeDevices(DeviceCallback callback);
    Subscription subscribeGraph(std::string_view routeAnchor, std::function<void(flow::Graph const&)> callback);

    std::vector<Device> enumerateSinks() const;

    void syncStreamBindings(std::unordered_set<std::uint32_t> const& subscribedStreamIds);
    void syncSinkBindings();

    struct ReachableContext
    {
      std::vector<std::uint32_t> reachableNodes;
      std::unordered_set<std::uint32_t> reachableSet;
      std::unordered_set<std::uint32_t> fullSet;
    };

    ReachableContext findReachableNodes(std::uint32_t streamId) const;
    flow::Node convertToAudioNode(std::uint32_t id,
                                  std::uint32_t streamId,
                                  std::unordered_set<std::uint32_t> const& reachableSet) const;
    void populateGraph(flow::Graph& graph, std::uint32_t streamId) const;

    static void onCoreDone(void* data, std::uint32_t /*id*/, std::int32_t seq)
    {
      auto* const impl = static_cast<PipeWireMonitor::Impl*>(data);

      {
        auto const lock = std::scoped_lock{impl->mutex};

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
      auto const isNode = (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0);
      auto const isLink = (std::strcmp(type, PW_TYPE_INTERFACE_Link) == 0);

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
                                                      auto const lock = std::scoped_lock{impl->mutex};
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
        auto const lock = std::scoped_lock{impl->mutex};

        if (isNode)
        {
          impl->nodes[id] = parseNodeRecord(version, props);
        }
        else if (isLink)
        {
          auto* const proxy =
            ::pw_registry_bind(impl->registryPtr.get(), id, PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, 0);

          if (proxy != nullptr)
          {
            auto& binding = impl->linkBindings[id];
            binding.id = id;
            binding.proxyPtr.reset(static_cast<::pw_link*>(proxy));
            ::pw_link_add_listener(binding.proxyPtr.get(), binding.listener.get(), &linkEvents, impl);
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
        auto const lock = std::scoped_lock{impl->mutex};

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
        auto const lock = std::scoped_lock{impl->mutex};

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
                            std::int32_t /*seq*/,
                            std::uint32_t id,
                            std::uint32_t /*index*/,
                            std::uint32_t /*next*/,
                            ::spa_pod const* param)
    {
      auto* const binding = static_cast<PipeWireMonitor::Impl::NodeBinding*>(data);
      auto* const impl = binding->impl;

      {
        auto const lock = std::scoped_lock{impl->mutex};

        if (id == SPA_PARAM_Format)
        {
          if (auto const optFmt = detail::parseRawStreamFormat(param); optFmt)
          {
            impl->nodeFormatMap[binding->id] = *optFmt;
          }
        }
        else if (binding->role == NodeBindingRole::Sink && id == SPA_PARAM_EnumFormat)
        {
          if (!impl->nodeFormatMap.contains(binding->id))
          {
            if (auto const optFmt = detail::parseRawStreamFormat(param); optFmt)
            {
              impl->nodeFormatMap[binding->id] = *optFmt;
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
      auto ev = ::pw_core_events{};
      ev.version = PW_VERSION_CORE_EVENTS;
      ev.done = onCoreDone;
      return ev;
    }();

    static inline ::pw_registry_events const registryEvents = []
    {
      auto ev = ::pw_registry_events{};
      ev.version = PW_VERSION_REGISTRY_EVENTS;
      ev.global = onRegistryGlobal;
      ev.global_remove = onRegistryGlobalRemove;
      return ev;
    }();

    static inline ::pw_node_events const streamNodeEvents = []
    {
      auto ev = ::pw_node_events{};
      ev.version = PW_VERSION_NODE_EVENTS;
      ev.info = onNodeInfo;
      ev.param = onNodeParam;
      return ev;
    }();

    static inline ::pw_node_events const sinkNodeEvents = []
    {
      auto ev = ::pw_node_events{};
      ev.version = PW_VERSION_NODE_EVENTS;
      ev.info = onNodeInfo;
      ev.param = onNodeParam;
      return ev;
    }();
  };

  // --- PipeWireMonitor Implementation ---

  PipeWireMonitor::PipeWireMonitor()
    : _implPtr{std::make_unique<Impl>()}
  {
  }

  PipeWireMonitor::~PipeWireMonitor() = default;

  void PipeWireMonitor::start()
  {
    if (!_implPtr->threadLoopPtr)
    {
      return;
    }

    _implPtr->stopping.store(false, std::memory_order_release);

    _implPtr->refreshEventPtr.get_deleter().loop = _implPtr->threadLoopPtr.get();
    auto* const event = ::pw_loop_add_event(
      ::pw_thread_loop_get_loop(_implPtr->threadLoopPtr.get()), &Impl::onRefreshEvent, _implPtr.get());

    if (event == nullptr)
    {
      AUDIO_LOG_ERROR("Failed to add PipeWire refresh event - periodic refresh disabled");
      return;
    }

    _implPtr->refreshEventPtr.reset(event);

    {
      auto guard = PwThreadLoopGuard{_implPtr->threadLoopPtr.get()};

      if (_implPtr->corePtr)
      {
        auto* const registry = ::pw_core_get_registry(_implPtr->corePtr.get(), PW_VERSION_REGISTRY, 0);
        _implPtr->registryPtr.reset(static_cast<::pw_registry*>(registry));

        if (_implPtr->registryPtr)
        {
          ::pw_registry_add_listener(
            _implPtr->registryPtr.get(), _implPtr->registryListener.get(), &Impl::registryEvents, _implPtr.get());
          ::pw_core_add_listener(
            _implPtr->corePtr.get(), _implPtr->coreListener.get(), &Impl::coreEvents, _implPtr.get());
          _implPtr->coreSyncSeq = ::pw_core_sync(_implPtr->corePtr.get(), PW_ID_CORE, 0);
        }
      }
    }

    _implPtr->triggerRefresh();
  }

  void PipeWireMonitor::stop()
  {
    _implPtr->stopping.store(true, std::memory_order_release);

    auto guard = PwThreadLoopGuard{_implPtr->threadLoopPtr.get()};
    auto const lock = std::scoped_lock{_implPtr->mutex};
    _implPtr->deviceSubscriptions.clear();
    _implPtr->graphSubscriptions.clear();
    _implPtr->refreshEventPtr.reset();
    _implPtr->linkBindings.clear();
    _implPtr->streamNodeBindings.clear();
    _implPtr->sinkNodeBindings.clear();
    _implPtr->registryListener.reset();
    _implPtr->registryPtr.reset();
    _implPtr->coreListener.reset();
    _implPtr->nodes.clear();
    _implPtr->links.clear();
    _implPtr->nodeFormatMap.clear();
    _implPtr->sinkCapabilitiesMap.clear();
    _implPtr->sinkPropsMap.clear();
  }

  Subscription PipeWireMonitor::subscribeDevices(DeviceCallback callback)
  {
    return _implPtr->subscribeDevices(std::move(callback));
  }

  std::vector<Device> PipeWireMonitor::enumerateSinks() const
  {
    return _implPtr->enumerateSinks();
  }

  Subscription PipeWireMonitor::subscribeGraph(std::string_view routeAnchor,
                                               std::function<void(flow::Graph const&)> callback)
  {
    return _implPtr->subscribeGraph(routeAnchor, std::move(callback));
  }

  void PipeWireMonitor::refresh()
  {
    _implPtr->refresh();
  }

  // --- Impl Implementations ---

  Subscription PipeWireMonitor::Impl::subscribeDevices(DeviceCallback callback)
  {
    auto const id = nextSubscriptionId++;
    auto devices = std::vector<Device>{};
    auto cb = DeviceCallback{};

    {
      auto const lock = std::scoped_lock{mutex};
      deviceSubscriptions.push_back({id, callback});
      cb = std::move(callback);
      devices = enumerateSinks();
    }

    if (cb)
    {
      cb(devices);
    }

    return Subscription{[this, id]
                        {
                          auto const lock = std::scoped_lock{mutex};
                          auto const it = std::ranges::find(deviceSubscriptions, id, &DeviceSubscription::id);

                          if (it != deviceSubscriptions.end())
                          {
                            deviceSubscriptions.erase(it);
                          }
                        }};
  }

  Subscription PipeWireMonitor::Impl::subscribeGraph(std::string_view routeAnchor,
                                                     std::function<void(flow::Graph const&)> callback)
  {
    auto const id = nextSubscriptionId++;
    {
      auto const lock = std::scoped_lock{mutex};
      graphSubscriptions.push_back({id, std::string{routeAnchor}, std::move(callback)});
    }

    triggerRefresh();

    return Subscription{[this, id]
                        {
                          {
                            auto const lock = std::scoped_lock{mutex};
                            auto const it = std::ranges::find(graphSubscriptions, id, &GraphSubscription::id);

                            if (it != graphSubscriptions.end())
                            {
                              graphSubscriptions.erase(it);
                            }
                          }

                          triggerRefresh();
                        }};
  }

  std::vector<Device> PipeWireMonitor::Impl::enumerateSinks() const
  {
    auto devices = std::vector<Device>{};

    for (auto const& [id, node] : nodes)
    {
      if (isSinkMediaClass(node.mediaClass))
      {
        auto const deviceId = node.optObjectSerial ? std::format("{}", *node.optObjectSerial) : std::format("{}", id);
        auto displayName = node.nodeNick;

        if (displayName.empty())
        {
          displayName = node.nodeName.empty() ? node.objectPath : node.nodeName;
        }

        auto const description = (node.nodeNick.empty() ? "" : node.nodeName);
        auto caps = DeviceCapabilities{};

        if (auto const it = sinkCapabilitiesMap.find(id); it != sinkCapabilitiesMap.end())
        {
          caps = it->second;
        }

        devices.push_back({.id = DeviceId{deviceId},
                           .displayName = displayName,
                           .description = description,
                           .isDefault = false,
                           .backendId = kBackendPipeWire,
                           .capabilities = {}});
        devices.push_back({.id = DeviceId{deviceId},
                           .displayName = std::format("{} (Exclusive)", displayName),
                           .description = description,
                           .isDefault = false,
                           .backendId = kBackendPipeWire,
                           .capabilities = caps});
      }
    }

    return devices;
  }

  void PipeWireMonitor::Impl::refresh()
  {
    auto guard = PwThreadLoopGuard{threadLoopPtr.get()};

    // Phase 1: sync bindings under mutex
    {
      auto const lock = std::scoped_lock{mutex};
      auto subscribedStreamIds = std::unordered_set<std::uint32_t>{};

      for (auto const& sub : graphSubscriptions)
      {
        if (auto const optParsedId = detail::parseUintProperty(sub.routeAnchor.c_str()); optParsedId)
        {
          subscribedStreamIds.insert(*optParsedId);
        }
      }

      syncStreamBindings(subscribedStreamIds);
      syncSinkBindings();
    }

    // Phase 2: build graphs and collect callbacks under mutex
    auto pendingGraphCbs = std::vector<std::pair<std::function<void(flow::Graph const&)>, flow::Graph>>{};
    auto pendingDeviceCbs = std::vector<DeviceCallback>{};
    auto deviceSnapshot = std::vector<Device>{};

    {
      auto const lock = std::scoped_lock{mutex};

      for (auto const& sub : graphSubscriptions)
      {
        auto const optParsedId = detail::parseUintProperty(sub.routeAnchor.c_str());

        if (optParsedId && *optParsedId != PW_ID_ANY && sub.callback)
        {
          auto graph = flow::Graph{};
          populateGraph(graph, *optParsedId);
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

      if (!registryPtr || it == nodes.end())
      {
        continue;
      }

      auto* node = ::pw_registry_bind(registryPtr.get(),
                                      streamId,
                                      PW_TYPE_INTERFACE_Node,
                                      std::min(it->second.version, static_cast<std::uint32_t>(PW_VERSION_NODE)),
                                      0);

      if (node == nullptr)
      {
        continue;
      }

      auto bindingPtr = std::make_unique<NodeBinding>();
      bindingPtr->id = streamId;
      bindingPtr->impl = this;
      bindingPtr->role = NodeBindingRole::Stream;
      bindingPtr->proxyPtr.reset(static_cast<::pw_node*>(node));
      auto const params = std::to_array<std::uint32_t>({SPA_PARAM_Format, SPA_PARAM_Props});
      ::pw_node_subscribe_params(
        bindingPtr->proxyPtr.get(), utility::layout::asLegacyPtr<std::uint32_t>(params.data()), params.size());
      ::pw_node_enum_params(bindingPtr->proxyPtr.get(), 1, SPA_PARAM_Format, 0, UINT32_MAX, nullptr);
      ::pw_node_enum_params(bindingPtr->proxyPtr.get(), 2, SPA_PARAM_Props, 0, UINT32_MAX, nullptr);
      auto* binding = bindingPtr.get();
      ::pw_node_add_listener(binding->proxyPtr.get(), binding->listener.get(), &streamNodeEvents, binding);
      streamNodeBindings[streamId] = std::move(bindingPtr);
    }
  }

  void PipeWireMonitor::Impl::syncSinkBindings()
  {
    for (auto const& [id, node] : nodes)
    {
      if (isSinkMediaClass(node.mediaClass) && !sinkNodeBindings.contains(id))
      {
        auto* proxy = ::pw_registry_bind(
          registryPtr.get(), id, PW_TYPE_INTERFACE_Node, std::min(node.version, (std::uint32_t)PW_VERSION_NODE), 0);

        if (proxy != nullptr)
        {
          auto bindingPtr = std::make_unique<NodeBinding>();
          bindingPtr->id = id;
          bindingPtr->impl = this;
          bindingPtr->role = NodeBindingRole::Sink;
          bindingPtr->proxyPtr.reset(static_cast<::pw_node*>(proxy));

          auto const params = std::to_array<std::uint32_t>({SPA_PARAM_Format, SPA_PARAM_EnumFormat, SPA_PARAM_Props});
          ::pw_node_subscribe_params(
            bindingPtr->proxyPtr.get(), utility::layout::asLegacyPtr<std::uint32_t>(params.data()), params.size());
          constexpr std::uint32_t kFormatSequence = 1;
          constexpr std::uint32_t kEnumFormatSequence = 2;
          constexpr std::uint32_t kPropsSequence = 3;
          ::pw_node_enum_params(bindingPtr->proxyPtr.get(), kFormatSequence, SPA_PARAM_Format, 0, UINT32_MAX, nullptr);
          ::pw_node_enum_params(
            bindingPtr->proxyPtr.get(), kEnumFormatSequence, SPA_PARAM_EnumFormat, 0, UINT32_MAX, nullptr);
          ::pw_node_enum_params(bindingPtr->proxyPtr.get(), kPropsSequence, SPA_PARAM_Props, 0, UINT32_MAX, nullptr);

          auto* sinkBinding = bindingPtr.get();
          ::pw_node_add_listener(
            sinkBinding->proxyPtr.get(), sinkBinding->listener.get(), &sinkNodeEvents, sinkBinding);
          sinkNodeBindings[id] = std::move(bindingPtr);
        }
      }
    }
  }

  PipeWireMonitor::Impl::ReachableContext PipeWireMonitor::Impl::findReachableNodes(std::uint32_t streamId) const
  {
    auto ctx = ReachableContext{};

    if (streamId != PW_ID_ANY)
    {
      ctx.reachableNodes.push_back(streamId);
      ctx.reachableSet.insert(streamId);
    }

    for (std::size_t i = 0; i < ctx.reachableNodes.size(); ++i)
    {
      auto curr = ctx.reachableNodes[i];

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

    for (auto const& [_, link] : links)
    {
      if (isActiveLink(static_cast<::pw_link_state>(link.state)) && ctx.reachableSet.contains(link.inputNodeId))
      {
        ctx.fullSet.insert(link.outputNodeId);
      }
    }

    return ctx;
  }

  flow::Node PipeWireMonitor::Impl::convertToAudioNode(std::uint32_t id,
                                                       std::uint32_t streamId,
                                                       std::unordered_set<std::uint32_t> const& reachableSet) const
  {
    auto const it = nodes.find(id);

    if (it == nodes.end())
    {
      if (id == streamId)
      {
        return flow::Node{.id = std::format("{}", id),
                          .type = flow::NodeType::Stream,
                          .name = "Aobus Playback",
                          .optFormat = std::nullopt};
      }

      return flow::Node{};
    }

    bool const isSink = isSinkMediaClass(it->second.mediaClass);
    bool const isRs = (id == streamId);
    auto type = flow::NodeType::ExternalSource;

    if (isRs)
    {
      type = flow::NodeType::Stream;
    }
    else if (isSink)
    {
      type = flow::NodeType::Sink;
    }
    else if (reachableSet.contains(id))
    {
      type = flow::NodeType::Intermediary;
    }

    auto name = it->second.nodeNick;

    if (name.empty())
    {
      name = it->second.nodeName.empty() ? it->second.objectPath : it->second.nodeName;
    }

    auto node =
      flow::Node{.id = std::format("{}", id), .type = type, .name = name, .objectPath = it->second.objectPath};

    if (auto const optFormatIt = nodeFormatMap.find(id); optFormatIt != nodeFormatMap.end())
    {
      node.optFormat = optFormatIt->second;
    }

    if (auto const propsIt = sinkPropsMap.find(id); propsIt != sinkPropsMap.end())
    {
      auto const& sinkProps = propsIt->second;
      auto const volumeCls = sinkProps.classifyVolume();
      node.hardwareVolumeNotUnity = volumeCls.hardwareNotUnity;
      node.softwareVolumeNotUnity = volumeCls.softwareNotUnity;
      node.unclassifiedVolumeNotUnity = volumeCls.unclassifiedNotUnity;
      node.isMuted = sinkProps.isMuted || sinkProps.isSoftMuted;
    }

    return node;
  }

  void PipeWireMonitor::Impl::populateGraph(flow::Graph& graph, std::uint32_t streamId) const
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
