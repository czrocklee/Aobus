// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "backend/PipeWireMonitor.h"

#include "backend/detail/BackendObserverDispatcher.h"
#include "backend/detail/BackendObserverSlot.h"
#include "backend/detail/PipeWireMonitorHooks.h"
#include "backend/detail/PipeWireRuntime.h"
#include <ao/Contract.h>
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

#include "backend/detail/PipeWireFormatParsing.h"
#include "backend/detail/PipeWireMonitorParsing.h"
#include <ao/audio/BackendIds.h>
#include <ao/audio/ChannelDeliveryEvidence.h>
#include <ao/audio/Device.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/PipeWireTargetIdentity.h>
#include <ao/audio/flow/Graph.h>
#include <ao/utility/CallbackStackScope.h>
#include <ao/utility/ScopedRegistration.h>
#include <ao/utility/ThreadName.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <source_location>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
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
    template<typename Callback>
    void invokePipeWireCallback(std::string_view const context,
                                Callback&& callback,
                                std::source_location const location = std::source_location::current()) noexcept
    {
      try
      {
        std::forward<Callback>(callback)();
      }
      catch (...)
      {
        fatalFromException(std::current_exception(), context, location);
      }
    }

    bool isActiveLink(::pw_link_state state) noexcept
    {
      return state == PW_LINK_STATE_PAUSED || state == PW_LINK_STATE_ACTIVE;
    }

    void invokeHook(std::function<void()> const& hook, char const* const context) noexcept
    {
      if (!hook)
      {
        return;
      }

      try
      {
        hook();
      }
      catch (...)
      {
        AO_FATAL_EXCEPTION(std::current_exception(), context);
      }
    }
  } // namespace

  // --- Provider-independent monitor state ---

  struct PipeWireMonitor::State final : std::enable_shared_from_this<State>
  {
    struct LinkBinding final
    {
      detail::PwProxyPtr<::pw_link> proxyPtr;
      detail::SpaHookGuard listener;
    };

    struct NodeBinding final
    {
      std::uint32_t id = PW_ID_ANY;
      State* state = nullptr;
      detail::PwProxyPtr<::pw_node> proxyPtr;
      detail::SpaHookGuard listener;
    };

    using GraphSlot = BackendObserverSlot<std::function<void(flow::Graph const&)>>;
    using DeviceSlot = BackendObserverSlot<DeviceCallback>;

    struct GraphSubscription final
    {
      std::string routeAnchor;
      std::shared_ptr<GraphSlot> slotPtr;
    };

    struct RefreshSignal final
    {
      std::mutex mutex;
      std::condition_variable_any cv;
      bool requested = false;
    };

    struct GraphUpdate final
    {
      std::weak_ptr<GraphSlot> slotPtr;
      flow::Graph graph;
    };

    struct RefreshBatch final
    {
      std::vector<GraphUpdate> graphUpdates;
      std::vector<std::weak_ptr<DeviceSlot>> deviceSlots;
      std::vector<Device> devices;
    };

    enum class Lifecycle : std::uint8_t
    {
      Running,
      Stopping,
      Stopped,
    };

    explicit State(std::shared_ptr<detail::PipeWireMonitorHooks> hooksPtr)
      : monitorHooksPtr{std::move(hooksPtr)}
    {
      if (monitorHooksPtr)
      {
        return;
      }

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

    ~State()
    {
      tryRequestShutdown();
      completeShutdown();

      if (monitorHooksPtr)
      {
        invokeHook(monitorHooksPtr->onMonitorStateDestroyed, "PipeWire monitor-state destruction observer");
      }
    }

    State(State const&) = delete;
    State& operator=(State const&) = delete;
    State(State&&) = delete;
    State& operator=(State&&) = delete;

    detail::PipeWireEnvironmentGuard envGuard;
    std::shared_ptr<detail::PipeWireMonitorHooks> monitorHooksPtr;
    detail::PwThreadLoopPtr threadLoopPtr;
    detail::PwContextPtr contextPtr;
    detail::PwCorePtr corePtr;
    bool nativeLoopRunning = false;

    mutable std::mutex mutex;
    BackendObserverDispatcher dispatcher{};
    BackendObserverCalls observerCalls{};
    std::stop_source shutdownSource{};
    mutable std::mutex lifecycleMutex;
    std::condition_variable lifecycleChanged;
    Lifecycle lifecycle = Lifecycle::Running;
    bool startCalled = false;
    detail::PwRegistryPtr registryPtr;
    detail::SpaHookGuard registryListener;
    detail::SpaHookGuard coreListener;
    std::unordered_map<std::uint32_t, NodeRecord> nodes;
    std::unordered_map<std::uint32_t, LinkRecord> links;
    std::unordered_map<std::uint32_t, LinkBinding> linkBindings;
    std::unordered_map<std::uint32_t, std::unique_ptr<NodeBinding>> streamNodeBindings;
    std::unordered_map<std::uint32_t, std::unique_ptr<NodeBinding>> sinkNodeBindings;
    std::unordered_map<std::uint32_t, RawStreamFormat> nodeFormatMap;
    std::unordered_map<std::uint32_t, SinkProps> sinkPropsMap;

    std::atomic<bool> stopping{false};
    std::vector<GraphSubscription> graphSubscriptions;
    std::vector<std::shared_ptr<DeviceSlot>> deviceSubscriptions;
    std::shared_ptr<RefreshSignal> refreshSignalPtr = std::make_shared<RefreshSignal>();

    void installTestHooks()
    {
      if (!monitorHooksPtr)
      {
        return;
      }

      auto const weakStatePtr = weak_from_this();
      monitorHooksPtr->requestRefresh = [weakStatePtr]
      {
        if (auto const statePtr = weakStatePtr.lock(); statePtr)
        {
          statePtr->triggerRefresh();
        }
      };
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
    bool isRunning() const noexcept;
    bool tryRequestShutdown() noexcept;
    void completeShutdown() noexcept;
    void waitForShutdown() noexcept;
    void refresh();
    std::optional<RefreshBatch> prepareRefreshBatch();
    // Shutdown drains all admitted calls, including slots already unlinked by
    // self-reset. A single registration only has to drain its own borrow.
    template<typename Slot, typename Snapshot>
    void notifySlot(std::weak_ptr<Slot> const& weakSlotPtr, Snapshot const& snapshot, std::string_view context)
    {
      observerCalls.invoke(
        [&]
        {
          if (stopping.load(std::memory_order_acquire))
          {
            return;
          }

          if (auto const slotPtr = weakSlotPtr.lock(); slotPtr)
          {
            invokePipeWireCallback(
              context, [&] { slotPtr->invoke([&](auto const& callback) { callback(snapshot); }); });
          }
        });
    }

    void removeSubscription(std::shared_ptr<DeviceSlot> const& slotPtr);
    void removeSubscription(std::shared_ptr<GraphSlot> const& slotPtr);

    template<typename Slot, typename Enroll>
    utility::ScopedRegistration subscribeSlot(std::shared_ptr<Slot> const& slotPtr, Enroll&& enroll)
    {
      // Construct the unregister action before making the observer reachable.
      auto registration =
        utility::ScopedRegistration{[weakStatePtr = weak_from_this(), weakSlotPtr = std::weak_ptr{slotPtr}]
                                    {
                                      if (auto const slotPtr = weakSlotPtr.lock(); slotPtr)
                                      {
                                        slotPtr->retire();

                                        if (auto const statePtr = weakStatePtr.lock(); statePtr)
                                        {
                                          statePtr->removeSubscription(slotPtr);
                                        }
                                      }
                                    }};
      dispatcher.dispatch(std::forward<Enroll>(enroll), shutdownSource.get_token());

      if (stopping.load(std::memory_order_acquire))
      {
        return {};
      }

      return registration;
    }

    utility::ScopedRegistration subscribeDevices(DeviceCallback callback);
    utility::ScopedRegistration subscribeGraph(std::string_view routeAnchor,
                                               std::function<void(flow::Graph const&)> callback);

    std::vector<Device> enumerateSinks() const;

    void syncStreamBindings(std::unordered_set<std::uint32_t> const& subscribedStreamIds);
    void syncSinkBindings();

    struct StreamGraphSelection
    {
      std::vector<std::uint32_t> downstreamNodeIds;
      std::unordered_set<std::uint32_t> downstreamNodeIdSet;
      std::unordered_set<std::uint32_t> includedNodeIds;
    };

    StreamGraphSelection selectStreamGraphNodes(std::uint32_t streamId) const;
    flow::Node convertToAudioNode(std::uint32_t id,
                                  std::uint32_t streamId,
                                  std::unordered_set<std::uint32_t> const& downstreamNodeIdSet) const;
    void populateGraph(flow::Graph& graph, std::uint32_t streamId) const;

    static void handleRegistryGlobal(void* data,
                                     std::uint32_t id,
                                     std::uint32_t /*permissions*/,
                                     char const* type,
                                     std::uint32_t version,
                                     ::spa_dict const* props)
    {
      auto* const impl = static_cast<PipeWireMonitor::State*>(data);
      auto const isNode = (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0);
      auto const isLink = (std::strcmp(type, PW_TYPE_INTERFACE_Link) == 0);

      if (!isNode && !isLink)
      {
        return;
      }

      static ::pw_link_events const kLinkEvents = {.version = PW_VERSION_LINK_EVENTS,
                                                   .info = [](void* data, ::pw_link_info const* info) noexcept
                                                   {
                                                     invokePipeWireCallback(
                                                       "PipeWire link-info callback",
                                                       [data, info]
                                                       {
                                                         if (!info)
                                                         {
                                                           return;
                                                         }

                                                         auto* const impl = static_cast<PipeWireMonitor::State*>(data);

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
                                                       });
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
            binding.proxyPtr.reset(static_cast<::pw_link*>(proxy));
            ::pw_link_add_listener(binding.proxyPtr.get(), binding.listener.get(), &kLinkEvents, impl);
          }
        }
      }

      impl->triggerRefresh();
    }

    static void handleRegistryGlobalRemove(void* data, std::uint32_t id)
    {
      auto* const impl = static_cast<PipeWireMonitor::State*>(data);
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

      auto* const binding = static_cast<PipeWireMonitor::State::NodeBinding*>(data);
      auto* const impl = binding->state;

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
      auto* const binding = static_cast<PipeWireMonitor::State::NodeBinding*>(data);
      auto* const impl = binding->state;

      {
        auto const lock = std::scoped_lock{impl->mutex};

        if (id == SPA_PARAM_Format)
        {
          detail::updateCurrentFormatFromNodeParam(impl->nodeFormatMap, binding->id, id, param);
        }
        else if (id == SPA_PARAM_Props)
        {
          mergeSinkProps(impl->sinkPropsMap[binding->id], param);
        }
      }

      impl->triggerRefresh();
    }

    static void handleCoreDone(void* data, std::uint32_t /*id*/, std::int32_t /*seq*/) noexcept
    {
      invokePipeWireCallback("PipeWire core-done callback", [data] { static_cast<State*>(data)->triggerRefresh(); });
    }

    static inline ::pw_core_events const kCoreEvents = []
    {
      auto ev = ::pw_core_events{};
      ev.version = PW_VERSION_CORE_EVENTS;
      ev.done = handleCoreDone;
      return ev;
    }();

    static inline ::pw_registry_events const kRegistryEvents = []
    {
      auto ev = ::pw_registry_events{};
      ev.version = PW_VERSION_REGISTRY_EVENTS;
      ev.global = [](void* data,
                     std::uint32_t id,
                     std::uint32_t permissions,
                     char const* type,
                     std::uint32_t version,
                     ::spa_dict const* props) noexcept
      {
        invokePipeWireCallback("PipeWire registry-global callback",
                               [=] { handleRegistryGlobal(data, id, permissions, type, version, props); });
      };
      ev.global_remove = [](void* data, std::uint32_t id) noexcept
      { invokePipeWireCallback("PipeWire registry-remove callback", [=] { handleRegistryGlobalRemove(data, id); }); };
      return ev;
    }();

    static inline ::pw_node_events const kStreamNodeEvents = []
    {
      auto ev = ::pw_node_events{};
      ev.version = PW_VERSION_NODE_EVENTS;
      ev.info = [](void* data, ::pw_node_info const* info) noexcept
      { invokePipeWireCallback("PipeWire stream-node info callback", [=] { handleNodeInfo(data, info); }); };
      ev.param = [](void* data,
                    std::int32_t seq,
                    std::uint32_t id,
                    std::uint32_t index,
                    std::uint32_t next,
                    ::spa_pod const* param) noexcept
      {
        invokePipeWireCallback(
          "PipeWire stream-node parameter callback", [=] { handleNodeParam(data, seq, id, index, next, param); });
      };
      return ev;
    }();

    static inline ::pw_node_events const kSinkNodeEvents = []
    {
      auto ev = ::pw_node_events{};
      ev.version = PW_VERSION_NODE_EVENTS;
      ev.info = [](void* data, ::pw_node_info const* info) noexcept
      { invokePipeWireCallback("PipeWire sink-node info callback", [=] { handleNodeInfo(data, info); }); };
      ev.param = [](void* data,
                    std::int32_t seq,
                    std::uint32_t id,
                    std::uint32_t index,
                    std::uint32_t next,
                    ::spa_pod const* param) noexcept
      {
        invokePipeWireCallback(
          "PipeWire sink-node parameter callback", [=] { handleNodeParam(data, seq, id, index, next, param); });
      };
      return ev;
    }();
  };

  struct PipeWireMonitor::Worker final
  {
    explicit Worker(std::shared_ptr<State> statePtr)
      : _statePtr{std::move(statePtr)}
    {
      auto const retainedStatePtr = _statePtr;
      auto const signalPtr = retainedStatePtr->refreshSignalPtr;
      _refreshThread =
        std::jthread{[retainedStatePtr, signalPtr]
                     {
                       auto const monitorScope = utility::CallbackStackScope{retainedStatePtr.get()};
                       auto const stopToken = retainedStatePtr->shutdownSource.get_token();

                       try
                       {
                         setCurrentThreadName("PipeWireRefresh");

                         while (!stopToken.stop_requested())
                         {
                           auto lock = std::unique_lock{signalPtr->mutex};
                           signalPtr->cv.wait(lock, stopToken, [&signalPtr] { return signalPtr->requested; });

                           if (stopToken.stop_requested())
                           {
                             break;
                           }

                           signalPtr->requested = false;
                           lock.unlock();
                           retainedStatePtr->refresh();

                           if (retainedStatePtr->monitorHooksPtr)
                           {
                             invokeHook(retainedStatePtr->monitorHooksPtr->onRefreshComplete,
                                        "PipeWire refresh-completion observer");
                           }
                         }
                       }
                       catch (...)
                       {
                         AO_FATAL_EXCEPTION(std::current_exception(), "PipeWire refresh thread");
                       }

                       if (retainedStatePtr->monitorHooksPtr)
                       {
                         invokeHook(retainedStatePtr->monitorHooksPtr->onMonitorExit, "PipeWire monitor-exit observer");
                       }

                       retainedStatePtr->completeShutdown();
                     }};
    }

    ~Worker() { shutdown(); }

    Worker(Worker const&) = delete;
    Worker& operator=(Worker const&) = delete;
    Worker(Worker&&) = delete;
    Worker& operator=(Worker&&) = delete;

    void shutdown() noexcept
    {
      auto const statePtr = _statePtr;
      auto const returnWithoutWaiting = statePtr->dispatcher.isDispatchingOnThisThread() ||
                                        utility::CallbackStackScope::containsIdentity(statePtr.get());

      // Stop can release a subscriber that destroys the facade. Take the thread
      // handle before signalling the worker; use only retained state afterwards.
      auto threadToJoin = std::jthread{};
      {
        auto const lock = std::scoped_lock{_mutex};

        if (_refreshThread.joinable())
        {
          if (returnWithoutWaiting)
          {
            _refreshThread.detach();
          }
          else
          {
            threadToJoin = std::move(_refreshThread);
          }
        }
      }

      if (auto const startedShutdown = statePtr->tryRequestShutdown(); statePtr->monitorHooksPtr)
      {
        if (startedShutdown)
        {
          invokeHook(statePtr->monitorHooksPtr->onShutdownStarted, "PipeWire shutdown-start observer");
        }
        else if (!returnWithoutWaiting)
        {
          invokeHook(statePtr->monitorHooksPtr->onShutdownWait, "PipeWire shutdown-wait observer");
        }
      }

      if (returnWithoutWaiting)
      {
        return;
      }

      if (threadToJoin.joinable())
      {
        threadToJoin.join();
      }

      statePtr->waitForShutdown();
    }

  private:
    std::shared_ptr<State> _statePtr;
    std::mutex _mutex;
    std::jthread _refreshThread;
  };

  // --- PipeWireMonitor Implementation ---

  PipeWireMonitor::PipeWireMonitor()
    : PipeWireMonitor{nullptr}
  {
  }

  PipeWireMonitor::PipeWireMonitor(std::shared_ptr<detail::PipeWireMonitorHooks> monitorHooksPtr)
    : _statePtr{std::make_shared<State>(std::move(monitorHooksPtr))}, _workerPtr{std::make_shared<Worker>(_statePtr)}
  {
    _statePtr->installTestHooks();
  }

  PipeWireMonitor::~PipeWireMonitor()
  {
    AO_INVARIANT(_statePtr != nullptr);
    _workerPtr->shutdown();
  }

  void PipeWireMonitor::start()
  {
    auto const statePtr = _statePtr;
    statePtr->start();
  }

  void PipeWireMonitor::stop()
  {
    auto const workerPtr = _workerPtr;
    workerPtr->shutdown();
  }

  utility::ScopedRegistration PipeWireMonitor::subscribeDevices(DeviceCallback callback)
  {
    auto const statePtr = _statePtr;
    return statePtr->subscribeDevices(std::move(callback));
  }

  std::vector<Device> PipeWireMonitor::enumerateSinks() const
  {
    auto const statePtr = _statePtr;
    auto const lock = std::scoped_lock{statePtr->mutex};
    return statePtr->enumerateSinks();
  }

  bool PipeWireMonitor::isRunning() const noexcept
  {
    auto const statePtr = _statePtr;
    return statePtr->isRunning();
  }

  utility::ScopedRegistration PipeWireMonitor::subscribeGraph(std::string_view routeAnchor,
                                                              std::function<void(flow::Graph const&)> callback)
  {
    auto const statePtr = _statePtr;
    return statePtr->subscribeGraph(routeAnchor, std::move(callback));
  }

  void PipeWireMonitor::refresh()
  {
    auto const statePtr = _statePtr;
    statePtr->refresh();
  }

  // --- State implementation ---

  void PipeWireMonitor::State::start()
  {
    auto const lock = std::scoped_lock{lifecycleMutex};

    if (startCalled || lifecycle != Lifecycle::Running)
    {
      return;
    }

    startCalled = true;

    if (monitorHooksPtr)
    {
      triggerRefresh();
      return;
    }

    if (!threadLoopPtr)
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
          ::pw_registry_add_listener(registryPtr.get(), registryListener.get(), &State::kRegistryEvents, this);
          ::pw_core_add_listener(corePtr.get(), coreListener.get(), &State::kCoreEvents, this);
          // Refresh again after the initial registry events have been dispatched.
          std::ignore = ::pw_core_sync(corePtr.get(), PW_ID_CORE, 0);
        }
      }
    }

    triggerRefresh();
  }

  bool PipeWireMonitor::State::isRunning() const noexcept
  {
    auto const lock = std::scoped_lock{lifecycleMutex};
    return lifecycle == Lifecycle::Running;
  }

  bool PipeWireMonitor::State::tryRequestShutdown() noexcept
  {
    {
      auto const lock = std::scoped_lock{lifecycleMutex};

      if (lifecycle != Lifecycle::Running)
      {
        return false;
      }

      lifecycle = Lifecycle::Stopping;
      stopping.store(true, std::memory_order_release);
    }

    shutdownSource.request_stop();
    return true;
  }

  void PipeWireMonitor::State::completeShutdown() noexcept
  {
    {
      auto const lock = std::scoped_lock{lifecycleMutex};

      if (lifecycle == Lifecycle::Stopped)
      {
        return;
      }
    }

    observerCalls.retire();

    if (threadLoopPtr && nativeLoopRunning)
    {
      ::pw_thread_loop_stop(threadLoopPtr.get());
      nativeLoopRunning = false;
    }

    auto retiredDevices = decltype(deviceSubscriptions){};
    auto retiredGraphs = decltype(graphSubscriptions){};
    {
      auto guard = PwThreadLoopGuard{threadLoopPtr.get()};
      auto const lock = std::scoped_lock{mutex};
      retiredDevices.swap(deviceSubscriptions);
      retiredGraphs.swap(graphSubscriptions);
      linkBindings.clear();
      streamNodeBindings.clear();
      sinkNodeBindings.clear();
      registryListener.reset();
      registryPtr.reset();
      coreListener.reset();
      nodes.clear();
      links.clear();
      nodeFormatMap.clear();
      sinkPropsMap.clear();
      corePtr.reset();
      contextPtr.reset();
    }

    // Destroy captures without locks and before releasing external shutdown waiters.
    retiredDevices.clear();
    retiredGraphs.clear();

    auto const lock = std::scoped_lock{lifecycleMutex};
    lifecycle = Lifecycle::Stopped;
    lifecycleChanged.notify_all();
  }

  void PipeWireMonitor::State::waitForShutdown() noexcept
  {
    auto lock = std::unique_lock{lifecycleMutex};
    lifecycleChanged.wait(lock, [this] { return lifecycle == Lifecycle::Stopped; });
  }

  utility::ScopedRegistration PipeWireMonitor::State::subscribeDevices(DeviceCallback callback)
  {
    if (!callback)
    {
      return {};
    }

    auto const slotPtr = std::make_shared<DeviceSlot>(std::move(callback));
    return subscribeSlot(slotPtr,
                         [&]
                         {
                           auto devices = std::vector<Device>{};
                           {
                             auto const lock = std::scoped_lock{mutex};

                             if (stopping.load(std::memory_order_acquire))
                             {
                               return;
                             }

                             devices = enumerateSinks();
                             deviceSubscriptions.push_back(slotPtr);
                           }

                           notifySlot(std::weak_ptr{slotPtr}, devices, "PipeWire device observer");
                         });
  }

  utility::ScopedRegistration PipeWireMonitor::State::subscribeGraph(std::string_view routeAnchor,
                                                                     std::function<void(flow::Graph const&)> callback)
  {
    if (!callback)
    {
      return {};
    }

    auto const slotPtr = std::make_shared<GraphSlot>(std::move(callback));
    return subscribeSlot(
      slotPtr,
      [&]
      {
        {
          auto const lock = std::scoped_lock{mutex};

          if (stopping.load(std::memory_order_acquire))
          {
            return;
          }

          graphSubscriptions.push_back({.routeAnchor = std::string{routeAnchor}, .slotPtr = slotPtr});
        }

        if (monitorHooksPtr)
        {
          invokeHook(monitorHooksPtr->onGraphSubscriptionInserted, "PipeWire graph-subscription insertion observer");
        }

        triggerRefresh();
      });
  }

  void PipeWireMonitor::State::removeSubscription(std::shared_ptr<DeviceSlot> const& slotPtr)
  {
    auto const lock = std::scoped_lock{mutex};
    std::erase(deviceSubscriptions, slotPtr);
  }

  void PipeWireMonitor::State::removeSubscription(std::shared_ptr<GraphSlot> const& slotPtr)
  {
    {
      auto const lock = std::scoped_lock{mutex};
      std::erase_if(graphSubscriptions, [&](auto const& subscription) { return subscription.slotPtr == slotPtr; });
    }
    triggerRefresh();
  }

  std::vector<Device> PipeWireMonitor::State::enumerateSinks() const
  {
    if (stopping.load(std::memory_order_acquire))
    {
      return {};
    }

    if (monitorHooksPtr && monitorHooksPtr->enumerateSinks)
    {
      return monitorHooksPtr->enumerateSinks();
    }

    auto devices = std::vector<Device>{};

    // A display label or a recycled registry number cannot identify a saved output.
    // Do not offer a name shared by two sinks, even if just one has a usable serial.
    auto nameCounts = std::unordered_map<std::string_view, std::size_t>{};

    for (auto const& node : nodes | std::views::values)
    {
      if (!node.nodeName.empty() && isSinkMediaClass(node.mediaClass))
      {
        ++nameCounts[node.nodeName];
      }
    }

    for (auto const& node : nodes | std::views::values)
    {
      if (node.optObjectSerial && !node.nodeName.empty() && isSinkMediaClass(node.mediaClass) &&
          nameCounts.at(node.nodeName) == 1)
      {
        devices.push_back({.id = DeviceId{std::format("{}{}", kPipeWireNodePrefix, node.nodeName)},
                           .displayName = node.nodeNick.empty() ? node.nodeName : node.nodeNick,
                           .description = node.nodeNick.empty() ? "" : node.nodeName,
                           .isDefault = false,
                           .backendId = kBackendPipeWire});
      }
    }

    return devices;
  }

  void PipeWireMonitor::State::refresh()
  {
    if (stopping.load(std::memory_order_acquire))
    {
      return;
    }

    auto optBatch = prepareRefreshBatch();

    if (!optBatch)
    {
      return;
    }

    if (monitorHooksPtr)
    {
      invokeHook(monitorHooksPtr->onRefreshPrepared, "PipeWire refresh-prepared observer");
    }

    if (optBatch->graphUpdates.empty() && optBatch->deviceSlots.empty())
    {
      return;
    }

    dispatcher.dispatch(
      [&]
      {
        for (auto const& update : optBatch->graphUpdates)
        {
          notifySlot(update.slotPtr, update.graph, "PipeWire graph observer");
        }

        for (auto const& weakSlotPtr : optBatch->deviceSlots)
        {
          notifySlot(weakSlotPtr, optBatch->devices, "PipeWire device observer");
        }
      },
      shutdownSource.get_token());
  }

  std::optional<PipeWireMonitor::State::RefreshBatch> PipeWireMonitor::State::prepareRefreshBatch()
  {
    auto batch = RefreshBatch{};

    // PipeWire objects and their derived state are touched only while the native
    // loop is locked. The guard ends before any user callback is considered.
    auto guard = PwThreadLoopGuard{threadLoopPtr.get()};
    auto const lock = std::scoped_lock{mutex};

    if (stopping.load(std::memory_order_relaxed))
    {
      return std::nullopt;
    }

    if (!monitorHooksPtr)
    {
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
    }

    for (auto const& sub : graphSubscriptions)
    {
      auto graph = flow::Graph{};
      bool publish = false;

      if (monitorHooksPtr)
      {
        publish = true;

        if (monitorHooksPtr->graphForRoute)
        {
          graph = monitorHooksPtr->graphForRoute(sub.routeAnchor);
        }
      }
      else if (auto const optParsedId = detail::parsePipeWireUint32(sub.routeAnchor.c_str());
               optParsedId && *optParsedId != PW_ID_ANY)
      {
        populateGraph(graph, *optParsedId);
        publish = true;
      }

      if (publish)
      {
        batch.graphUpdates.push_back({.slotPtr = sub.slotPtr, .graph = std::move(graph)});
      }
    }

    if (!deviceSubscriptions.empty())
    {
      batch.devices = enumerateSinks();

      batch.deviceSlots.assign(deviceSubscriptions.begin(), deviceSubscriptions.end());
    }

    return batch;
  }

  void PipeWireMonitor::State::syncStreamBindings(std::unordered_set<std::uint32_t> const& subscribedStreamIds)
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
      bindingPtr->state = this;
      bindingPtr->proxyPtr.reset(static_cast<::pw_node*>(node));
      auto const params = std::to_array<std::uint32_t>({SPA_PARAM_Format, SPA_PARAM_Props});
      ::pw_node_subscribe_params(
        bindingPtr->proxyPtr.get(), utility::layout::asLegacyPtr<std::uint32_t>(params.data()), params.size());
      ::pw_node_enum_params(bindingPtr->proxyPtr.get(), 1, SPA_PARAM_Format, 0, UINT32_MAX, nullptr);
      ::pw_node_enum_params(bindingPtr->proxyPtr.get(), 2, SPA_PARAM_Props, 0, UINT32_MAX, nullptr);
      auto* binding = bindingPtr.get();
      ::pw_node_add_listener(binding->proxyPtr.get(), binding->listener.get(), &kStreamNodeEvents, binding);
      streamNodeBindings[streamId] = std::move(bindingPtr);
    }
  }

  void PipeWireMonitor::State::syncSinkBindings()
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
          bindingPtr->state = this;
          bindingPtr->proxyPtr.reset(static_cast<::pw_node*>(proxy));

          auto const params = std::to_array<std::uint32_t>({SPA_PARAM_Format, SPA_PARAM_Props});
          ::pw_node_subscribe_params(
            bindingPtr->proxyPtr.get(), utility::layout::asLegacyPtr<std::uint32_t>(params.data()), params.size());
          constexpr std::uint32_t kFormatSequence = 1;
          constexpr std::uint32_t kPropsSequence = 2;
          ::pw_node_enum_params(bindingPtr->proxyPtr.get(), kFormatSequence, SPA_PARAM_Format, 0, UINT32_MAX, nullptr);
          ::pw_node_enum_params(bindingPtr->proxyPtr.get(), kPropsSequence, SPA_PARAM_Props, 0, UINT32_MAX, nullptr);

          auto* sinkBinding = bindingPtr.get();
          ::pw_node_add_listener(
            sinkBinding->proxyPtr.get(), sinkBinding->listener.get(), &kSinkNodeEvents, sinkBinding);
          sinkNodeBindings[id] = std::move(bindingPtr);
        }
      }
    }
  }

  PipeWireMonitor::State::StreamGraphSelection PipeWireMonitor::State::selectStreamGraphNodes(
    std::uint32_t streamId) const
  {
    auto selection = StreamGraphSelection{};

    if (streamId != PW_ID_ANY)
    {
      selection.downstreamNodeIds.push_back(streamId);
      selection.downstreamNodeIdSet.insert(streamId);
    }

    for (std::size_t i = 0; i < selection.downstreamNodeIds.size(); ++i)
    {
      auto const currentNodeId = selection.downstreamNodeIds[i];

      for (auto const& [_, link] : links)
      {
        if (!isActiveLink(static_cast<::pw_link_state>(link.state)) || link.outputNodeId != currentNodeId ||
            link.inputNodeId == PW_ID_ANY)
        {
          continue;
        }

        if (selection.downstreamNodeIdSet.insert(link.inputNodeId).second)
        {
          selection.downstreamNodeIds.push_back(link.inputNodeId);
        }
      }
    }

    selection.includedNodeIds = selection.downstreamNodeIdSet;

    for (auto const& [_, link] : links)
    {
      if (isActiveLink(static_cast<::pw_link_state>(link.state)) &&
          selection.downstreamNodeIdSet.contains(link.inputNodeId))
      {
        selection.includedNodeIds.insert(link.outputNodeId);
      }
    }

    return selection;
  }

  flow::Node PipeWireMonitor::State::convertToAudioNode(
    std::uint32_t id,
    std::uint32_t streamId,
    std::unordered_set<std::uint32_t> const& downstreamNodeIdSet) const
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
    else if (downstreamNodeIdSet.contains(id))
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
      auto const& observed = optFormatIt->second;
      node.optFormat = observed.format;
      bool const hasCompleteNativeSet = !observed.hasUnsupportedPositions && observed.nativeSpeakerMask != 0U &&
                                        std::popcount(observed.nativeSpeakerMask) == observed.format.channels;
      node.channelDelivery = {
        .isMapped = hasCompleteNativeSet,
        .nativeSpeakerMask = observed.nativeSpeakerMask,
      };
    }

    if (auto const propsIt = sinkPropsMap.find(id); propsIt != sinkPropsMap.end())
    {
      auto const& sinkProps = propsIt->second;
      auto const volumeCls = sinkProps.classifyVolume(isRs ? SinkProps::VolumeClassificationScope::Stream
                                                           : SinkProps::VolumeClassificationScope::Sink);
      node.hardwareVolumeNotUnity = volumeCls.hardwareNotUnity;
      node.softwareVolumeNotUnity = volumeCls.softwareNotUnity;
      node.maxSoftwareGain = volumeCls.maxSoftwareGain;
      node.minSoftwareGain = volumeCls.minSoftwareGain;
      node.unclassifiedVolumeNotUnity = volumeCls.unclassifiedNotUnity;
      node.maxUnclassifiedGain = volumeCls.maxUnclassifiedGain;
      node.minUnclassifiedGain = volumeCls.minUnclassifiedGain;
      node.isMuted = sinkProps.isMuted || sinkProps.isSoftMuted;
    }

    return node;
  }

  void PipeWireMonitor::State::populateGraph(flow::Graph& graph, std::uint32_t streamId) const
  {
    auto const selection = selectStreamGraphNodes(streamId);

    for (auto id : selection.includedNodeIds)
    {
      auto node = convertToAudioNode(id, streamId, selection.downstreamNodeIdSet);

      if (!node.id.empty())
      {
        graph.nodes.push_back(std::move(node));
      }
    }

    for (auto const& [_, link] : links)
    {
      if (isActiveLink(static_cast<::pw_link_state>(link.state)) &&
          selection.includedNodeIds.contains(link.outputNodeId) && selection.includedNodeIds.contains(link.inputNodeId))
      {
        graph.connections.push_back({.sourceId = std::format("{}", link.outputNodeId),
                                     .destinationId = std::format("{}", link.inputNodeId),
                                     .isActive = true});
      }
    }
  }
} // namespace ao::audio::backend
