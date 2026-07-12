// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/backend/PipeWireMonitor.h>
#include <ao/audio/backend/detail/PipeWireRuntime.h>
#include <ao/utility/ByteView.h>

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

#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/Format.h>
#include <ao/audio/Subscription.h>
#include <ao/audio/backend/detail/PipeWireMonitorParsing.h>
#include <ao/audio/flow/Graph.h>
#include <ao/utility/ThreadName.h>

#include <gsl-lite/gsl-lite.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ao::audio::backend
{
  using namespace detail;

  // --- Internal monitor operations ---

  namespace
  {
    enum class NodeBindingRole : std::uint8_t
    {
      Stream,
      Sink,
    };

    bool isActiveLink(::pw_link_state state) noexcept
    {
      return state == PW_LINK_STATE_PAUSED || state == PW_LINK_STATE_ACTIVE;
    }
  } // namespace

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

    struct RefreshSignal final
    {
      std::mutex mutex;
      std::condition_variable_any cv;
      bool requested = false;
    };

    struct PendingGraphCallback final
    {
      std::uint64_t id = 0;
      std::function<void(flow::Graph const&)> callback;
      flow::Graph graph;
    };

    struct PendingDeviceCallback final
    {
      std::uint64_t id = 0;
      PipeWireMonitor::DeviceCallback callback;
    };

    struct [[nodiscard]] CallbackInvocationScope final
    {
      explicit CallbackInvocationScope(Impl& impl)
        : impl{impl}
      {
        ++impl.callbackDepth;
      }

      ~CallbackInvocationScope() { --impl.callbackDepth; }

      CallbackInvocationScope(CallbackInvocationScope const&) = delete;
      CallbackInvocationScope& operator=(CallbackInvocationScope const&) = delete;
      CallbackInvocationScope(CallbackInvocationScope&&) = delete;
      CallbackInvocationScope& operator=(CallbackInvocationScope&&) = delete;

      Impl& impl;
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
        // Match the two init failures above: leave the monitor a no-op dud (null
        // core -> empty enumeration) and let the provider degrade to ALSA. Like a
        // missing daemon, this is reported by the absence of devices, not a log.
        return;
      }

      nativeLoopRunning = true;

      {
        auto guard = PwThreadLoopGuard{threadLoopPtr.get()};
        corePtr.reset(::pw_context_connect(contextPtr.get(), nullptr, 0));
      }
    }

    ~Impl()
    {
      shutdown();
      auto const callbackLock = std::scoped_lock{callbackMutex};
      gsl_Expects(callbackDepth == 0);
      gsl_Expects(activeSubscriptionCount == 0);
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    detail::PipeWireEnvironmentGuard envGuard;
    detail::PwThreadLoopPtr threadLoopPtr;
    detail::PwContextPtr contextPtr;
    detail::PwCorePtr corePtr;
    bool nativeLoopRunning = false;

    mutable std::mutex mutex;
    std::recursive_mutex callbackMutex;
    std::uint32_t callbackDepth = 0;
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
    std::unordered_map<std::uint32_t, DeviceFormatCapabilities> sinkCapabilitiesMap;
    std::unordered_map<std::uint32_t, SinkProps> sinkPropsMap;

    std::atomic<bool> stopping{false};
    std::atomic<bool> startCalled{false};
    std::atomic<bool> shutdownStarted{false};
    std::uint64_t nextSubscriptionId = 1;
    std::size_t activeSubscriptionCount = 0;
    std::vector<GraphSubscription> graphSubscriptions;
    std::vector<DeviceSubscription> deviceSubscriptions;
    std::shared_ptr<RefreshSignal> refreshSignalPtr = std::make_shared<RefreshSignal>();
    std::jthread refreshThread;

    void startRefreshThread()
    {
      auto const signalPtr = refreshSignalPtr;
      refreshThread =
        std::jthread{[this, signalPtr](std::stop_token const& stopToken)
                     {
                       setCurrentThreadName("PipeWireRefresh");

                       while (!stopToken.stop_requested())
                       {
                         auto lock = std::unique_lock{signalPtr->mutex};
                         signalPtr->cv.wait(lock, stopToken, [&signalPtr] { return signalPtr->requested; });

                         if (stopToken.stop_requested())
                         {
                           return;
                         }

                         signalPtr->requested = false;
                         lock.unlock();

                         refresh();
                       }
                     }};
    }

    void triggerRefresh()
    {
      if (stopping.load(std::memory_order_acquire))
      {
        return;
      }

      {
        auto const lock = std::scoped_lock{refreshSignalPtr->mutex};
        refreshSignalPtr->requested = true;
      }

      refreshSignalPtr->cv.notify_one();
    }

    void start();
    void shutdown() noexcept;
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

    static void handleCoreDone(void* data, std::uint32_t /*id*/, std::int32_t seq)
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

    static void handleRegistryGlobal(void* data,
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

    static void handleRegistryGlobalRemove(void* data, std::uint32_t id)
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

    static void handleNodeInfo(void* data, ::pw_node_info const* info)
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

    static void handleNodeParam(void* data,
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
          detail::updateCurrentFormatFromNodeParam(impl->nodeFormatMap, binding->id, id, param);
        }
        else if (binding->role == NodeBindingRole::Sink && id == SPA_PARAM_EnumFormat)
        {
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

    static inline ::pw_core_events const coreEvents = []
    {
      auto ev = ::pw_core_events{};
      ev.version = PW_VERSION_CORE_EVENTS;
      ev.done = handleCoreDone;
      return ev;
    }();

    static inline ::pw_registry_events const registryEvents = []
    {
      auto ev = ::pw_registry_events{};
      ev.version = PW_VERSION_REGISTRY_EVENTS;
      ev.global = handleRegistryGlobal;
      ev.global_remove = handleRegistryGlobalRemove;
      return ev;
    }();

    static inline ::pw_node_events const streamNodeEvents = []
    {
      auto ev = ::pw_node_events{};
      ev.version = PW_VERSION_NODE_EVENTS;
      ev.info = handleNodeInfo;
      ev.param = handleNodeParam;
      return ev;
    }();

    static inline ::pw_node_events const sinkNodeEvents = []
    {
      auto ev = ::pw_node_events{};
      ev.version = PW_VERSION_NODE_EVENTS;
      ev.info = handleNodeInfo;
      ev.param = handleNodeParam;
      return ev;
    }();
  };

  // --- PipeWireMonitor Implementation ---

  PipeWireMonitor::PipeWireMonitor()
    : _implPtr{std::make_unique<Impl>()}
  {
    _implPtr->startRefreshThread();
  }

  PipeWireMonitor::~PipeWireMonitor()
  {
    gsl_Expects(_implPtr != nullptr);
    _implPtr->shutdown();
  }

  void PipeWireMonitor::start()
  {
    _implPtr->start();
  }

  void PipeWireMonitor::stop()
  {
    _implPtr->shutdown();
  }

  Subscription PipeWireMonitor::subscribeDevices(DeviceCallback callback)
  {
    return _implPtr->subscribeDevices(std::move(callback));
  }

  std::vector<Device> PipeWireMonitor::enumerateSinks() const
  {
    auto const lock = std::scoped_lock{_implPtr->mutex};
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

  void PipeWireMonitor::Impl::start()
  {
    if (startCalled.exchange(true, std::memory_order_acq_rel) || !threadLoopPtr ||
        stopping.load(std::memory_order_acquire))
    {
      return;
    }

    {
      auto guard = PwThreadLoopGuard{threadLoopPtr.get()};

      if (corePtr)
      {
        auto* const registry = ::pw_core_get_registry(corePtr.get(), PW_VERSION_REGISTRY, 0);
        registryPtr.reset(static_cast<::pw_registry*>(registry));

        if (registryPtr)
        {
          ::pw_registry_add_listener(registryPtr.get(), registryListener.get(), &Impl::registryEvents, this);
          ::pw_core_add_listener(corePtr.get(), coreListener.get(), &Impl::coreEvents, this);
          coreSyncSeq = ::pw_core_sync(corePtr.get(), PW_ID_CORE, 0);
        }
      }
    }

    triggerRefresh();
  }

  void PipeWireMonitor::Impl::shutdown() noexcept
  {
    gsl_Expects(!refreshThread.joinable() || std::this_thread::get_id() != refreshThread.get_id());

    {
      auto const callbackLock = std::scoped_lock{callbackMutex};
      gsl_Expects(callbackDepth == 0);
    }

    if (shutdownStarted.exchange(true, std::memory_order_acq_rel))
    {
      return;
    }

    stopping.store(true, std::memory_order_release);

    refreshThread.request_stop();
    refreshSignalPtr->cv.notify_all();

    if (refreshThread.joinable())
    {
      refreshThread.join();
    }

    if (threadLoopPtr && nativeLoopRunning)
    {
      ::pw_thread_loop_stop(threadLoopPtr.get());
      nativeLoopRunning = false;
    }

    auto const callbackLock = std::scoped_lock{callbackMutex};
    auto guard = PwThreadLoopGuard{threadLoopPtr.get()};
    auto const lock = std::scoped_lock{mutex};
    deviceSubscriptions.clear();
    graphSubscriptions.clear();
    linkBindings.clear();
    streamNodeBindings.clear();
    sinkNodeBindings.clear();
    registryListener.reset();
    registryPtr.reset();
    coreListener.reset();
    nodes.clear();
    links.clear();
    nodeFormatMap.clear();
    sinkCapabilitiesMap.clear();
    sinkPropsMap.clear();
    corePtr.reset();
    contextPtr.reset();
  }

  Subscription PipeWireMonitor::Impl::subscribeDevices(DeviceCallback callback)
  {
    if (!callback)
    {
      return {};
    }

    std::uint64_t id = 0;
    auto devices = std::vector<Device>{};
    auto const callbackLock = std::scoped_lock{callbackMutex};

    {
      auto const lock = std::scoped_lock{mutex};

      if (stopping.load(std::memory_order_relaxed))
      {
        return {};
      }

      id = nextSubscriptionId++;
      deviceSubscriptions.push_back({.id = id, .callback = callback});
      devices = enumerateSinks();
    }

    try
    {
      auto const callbackScope = CallbackInvocationScope{*this};
      callback(devices);
    }
    catch (...)
    {
      auto const lock = std::scoped_lock{mutex};
      auto const it = std::ranges::find(deviceSubscriptions, id, &DeviceSubscription::id);

      if (it != deviceSubscriptions.end())
      {
        deviceSubscriptions.erase(it);
      }

      throw;
    }

    {
      auto const lock = std::scoped_lock{mutex};

      if (stopping.load(std::memory_order_relaxed) ||
          std::ranges::find(deviceSubscriptions, id, &DeviceSubscription::id) == deviceSubscriptions.end())
      {
        return {};
      }
    }

    ++activeSubscriptionCount;
    return Subscription{[this, id]
                        {
                          auto const callbackLock = std::scoped_lock{callbackMutex};
                          auto const lock = std::scoped_lock{mutex};
                          auto const it = std::ranges::find(deviceSubscriptions, id, &DeviceSubscription::id);

                          if (it != deviceSubscriptions.end())
                          {
                            deviceSubscriptions.erase(it);
                          }

                          gsl_Expects(activeSubscriptionCount != 0);
                          --activeSubscriptionCount;
                        }};
  }

  Subscription PipeWireMonitor::Impl::subscribeGraph(std::string_view routeAnchor,
                                                     std::function<void(flow::Graph const&)> callback)
  {
    if (!callback)
    {
      return {};
    }

    std::uint64_t id = 0;
    auto const callbackLock = std::scoped_lock{callbackMutex};

    {
      auto const lock = std::scoped_lock{mutex};

      if (stopping.load(std::memory_order_relaxed))
      {
        return {};
      }

      id = nextSubscriptionId++;
      graphSubscriptions.push_back(
        {.id = id, .routeAnchor = std::string{routeAnchor}, .callback = std::move(callback)});
    }

    ++activeSubscriptionCount;
    triggerRefresh();

    return Subscription{[this, id]
                        {
                          auto const callbackLock = std::scoped_lock{callbackMutex};
                          {
                            auto const lock = std::scoped_lock{mutex};
                            auto const it = std::ranges::find(graphSubscriptions, id, &GraphSubscription::id);

                            if (it != graphSubscriptions.end())
                            {
                              graphSubscriptions.erase(it);
                            }
                          }

                          gsl_Expects(activeSubscriptionCount != 0);
                          --activeSubscriptionCount;
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
        auto caps = DeviceFormatCapabilities{};

        if (auto const it = sinkCapabilitiesMap.find(id); it != sinkCapabilitiesMap.end())
        {
          caps = it->second;
        }

        devices.push_back({.id = DeviceId{deviceId},
                           .displayName = displayName,
                           .description = description,
                           .isDefault = false,
                           .backendId = kBackendPipeWire,
                           .capabilities = std::move(caps)});
      }
    }

    return devices;
  }

  void PipeWireMonitor::Impl::refresh()
  {
    if (stopping.load(std::memory_order_acquire))
    {
      return;
    }

    auto pendingGraphCbs = std::vector<PendingGraphCallback>{};
    auto pendingDeviceCbs = std::vector<PendingDeviceCallback>{};
    auto deviceSnapshot = std::vector<Device>{};

    // PipeWire objects and their derived state are touched only while the native
    // loop is locked. The guard ends before any user callback is considered.
    {
      auto guard = PwThreadLoopGuard{threadLoopPtr.get()};

      {
        auto const lock = std::scoped_lock{mutex};

        if (stopping.load(std::memory_order_relaxed))
        {
          return;
        }

        auto subscribedStreamIds = std::unordered_set<std::uint32_t>{};

        for (auto const& sub : graphSubscriptions)
        {
          if (auto const optParsedId = detail::parsePipeWireUint32(sub.routeAnchor.c_str()); optParsedId)
          {
            subscribedStreamIds.insert(*optParsedId);
          }
        }

        syncStreamBindings(subscribedStreamIds);
        syncSinkBindings();

        for (auto const& sub : graphSubscriptions)
        {
          auto const optParsedId = detail::parsePipeWireUint32(sub.routeAnchor.c_str());

          if (optParsedId && *optParsedId != PW_ID_ANY && sub.callback)
          {
            auto graph = flow::Graph{};
            populateGraph(graph, *optParsedId);
            pendingGraphCbs.push_back({.id = sub.id, .callback = sub.callback, .graph = std::move(graph)});
          }
        }

        if (!deviceSubscriptions.empty())
        {
          deviceSnapshot = enumerateSinks();

          for (auto const& sub : deviceSubscriptions)
          {
            if (sub.callback)
            {
              pendingDeviceCbs.push_back({.id = sub.id, .callback = sub.callback});
            }
          }
        }
      }
    }

    // Serialize user delivery outside the PipeWire loop lock and state mutex.
    // Rechecking the id after copying lets cancellation suppress stale delivery.
    for (auto& pending : pendingGraphCbs)
    {
      if (stopping.load(std::memory_order_acquire))
      {
        return;
      }

      {
        auto const callbackLock = std::scoped_lock{callbackMutex};

        {
          auto const lock = std::scoped_lock{mutex};

          if (stopping.load(std::memory_order_relaxed))
          {
            return;
          }

          if (std::ranges::find(graphSubscriptions, pending.id, &GraphSubscription::id) == graphSubscriptions.end())
          {
            continue;
          }
        }

        auto const callbackScope = CallbackInvocationScope{*this};
        pending.callback(pending.graph);
      }
    }

    for (auto& pending : pendingDeviceCbs)
    {
      if (stopping.load(std::memory_order_acquire))
      {
        return;
      }

      auto const callbackLock = std::scoped_lock{callbackMutex};

      {
        auto const lock = std::scoped_lock{mutex};

        if (stopping.load(std::memory_order_relaxed))
        {
          return;
        }

        if (std::ranges::find(deviceSubscriptions, pending.id, &DeviceSubscription::id) == deviceSubscriptions.end())
        {
          continue;
        }
      }

      auto const callbackScope = CallbackInvocationScope{*this};
      pending.callback(deviceSnapshot);
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
        auto* proxy = ::pw_registry_bind(registryPtr.get(),
                                         id,
                                         PW_TYPE_INTERFACE_Node,
                                         std::min(node.version, static_cast<std::uint32_t>(PW_VERSION_NODE)),
                                         0);

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
      auto const currentNodeId = ctx.reachableNodes[i];

      for (auto const& [_, link] : links)
      {
        if (!isActiveLink(static_cast<::pw_link_state>(link.state)) || link.outputNodeId != currentNodeId ||
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
      auto const volumeCls = sinkProps.classifyVolume(isRs ? SinkProps::VolumeClassificationContext::Stream
                                                           : SinkProps::VolumeClassificationContext::Sink);
      node.hardwareVolumeNotUnity = volumeCls.hardwareNotUnity;
      node.softwareVolumeNotUnity = volumeCls.softwareNotUnity;
      node.maxSoftwareGain = volumeCls.maxSoftwareGain;
      node.minSoftwareGain = volumeCls.minSoftwareGain;
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
                                     .destinationId = std::format("{}", link.inputNodeId),
                                     .isActive = true});
      }
    }
  }
} // namespace ao::audio::backend
