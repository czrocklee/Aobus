// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "platform/linux/playback/PipeWireMonitor.h"
#include "core/Log.h"
#include "platform/linux/playback/detail/PipeWireShared.h"

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
#include <cstring>
#include <format>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace app::playback
{
  // --- Internal Types (Monitor only) ---

  struct NodeRecord final
  {
    std::uint32_t version = 0;
    std::string mediaClass;
    std::string nodeName;
    std::string nodeNick;
    std::string nodeDescription;
    std::string objectPath;
    std::optional<std::uint32_t> objectSerial;
    std::optional<std::uint32_t> driverId;
  };

  struct LinkRecord final
  {
    std::uint32_t outputNodeId = PW_ID_ANY;
    std::uint32_t inputNodeId = PW_ID_ANY;
    ::pw_link_state state = PW_LINK_STATE_INIT;
  };

  struct SinkProps final
  {
    std::optional<float> volume;
    std::optional<bool> mute;
    std::vector<float> channelVolumes;
    std::optional<bool> softMute;
    std::vector<float> softVolumes;
  };

  enum class NodeBindingRole
  {
    Stream,
    Sink,
  };

  // --- Internal Helpers (Monitor only) ---

  bool isSinkMediaClass(std::string const& mediaClass)
  {
    return mediaClass == "Audio/Sink" || mediaClass == "Audio/Duplex" || mediaClass.ends_with("/Sink");
  }

  bool isActiveLink(::pw_link_state state) noexcept
  {
    return state == PW_LINK_STATE_PAUSED || state == PW_LINK_STATE_ACTIVE;
  }

  std::string lookupProperty(::spa_dict const* props, char const* key)
  {
    auto const* value = props ? ::spa_dict_lookup(props, key) : nullptr;
    return value ? std::string(value) : std::string{};
  }

  std::string formatStreamFormat(app::core::AudioFormat const& format)
  {
    auto const sampleType = format.isFloat ? "float" : "pcm";
    return std::format("{}Hz/{}-bit/{}ch {}", format.sampleRate, format.bitDepth, format.channels, sampleType);
  }

  NodeRecord parseNodeRecord(std::uint32_t version, ::spa_dict const* props)
  {
    auto record = NodeRecord{};
    record.version = version;
    record.mediaClass = lookupProperty(props, PW_KEY_MEDIA_CLASS);
    record.nodeName = lookupProperty(props, PW_KEY_NODE_NAME);
    record.nodeNick = lookupProperty(props, PW_KEY_NODE_NICK);
    record.nodeDescription = lookupProperty(props, PW_KEY_NODE_DESCRIPTION);
    record.objectPath = lookupProperty(props, PW_KEY_OBJECT_PATH);

    if (auto const serial = detail::parseUintProperty(::spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL)))
      record.objectSerial = serial;

    if (auto const id = detail::parseUintProperty(::spa_dict_lookup(props, "node.driver-id")))
      record.driverId = id;
    else if (auto const id = detail::parseUintProperty(::spa_dict_lookup(props, "node.driver")))
      record.driverId = id;

    return record;
  }

  std::uint8_t spaFormatToBitDepth(std::uint32_t spaFmt)
  {
    switch (spaFmt)
    {
      case SPA_AUDIO_FORMAT_S16_LE:
      case SPA_AUDIO_FORMAT_S16_BE: return 16;
      case SPA_AUDIO_FORMAT_S24_LE:
      case SPA_AUDIO_FORMAT_S24_BE: return 24;
      case SPA_AUDIO_FORMAT_S24_32_LE:
      case SPA_AUDIO_FORMAT_S24_32_BE:
      case SPA_AUDIO_FORMAT_S32_LE:
      case SPA_AUDIO_FORMAT_S32_BE:
      case SPA_AUDIO_FORMAT_F32_LE:
      case SPA_AUDIO_FORMAT_F32_BE: return 32;
      case SPA_AUDIO_FORMAT_F64_LE:
      case SPA_AUDIO_FORMAT_F64_BE: return 64;
      default: return 0;
    }
  }

  void collectIntValues(::spa_pod const* pod, std::vector<std::uint32_t>& output)
  {
    if (pod == nullptr) return;
    if (::spa_pod_is_int(pod))
    {
      std::int32_t val = 0;
      if (::spa_pod_get_int(pod, &val) == 0)
      {
        if (std::ranges::find(output, static_cast<std::uint32_t>(val)) == output.end())
          output.push_back(static_cast<std::uint32_t>(val));
      }
    }
    else if (::spa_pod_is_choice(pod))
    {
      auto const* choice = reinterpret_cast<::spa_pod_choice const*>(pod);
      auto const n_vals = SPA_POD_CHOICE_N_VALUES(choice);
      auto const type = SPA_POD_CHOICE_VALUE_TYPE(choice);
      if (n_vals == 0 || type != SPA_TYPE_Int) return;

      auto const* vals = static_cast<std::int32_t const*>(SPA_POD_CHOICE_VALUES(choice));
      auto const choice_type = SPA_POD_CHOICE_TYPE(choice);

      if (choice_type == SPA_CHOICE_Enum || choice_type == SPA_CHOICE_None)
      {
        for (std::uint32_t i = 0; i < n_vals; ++i)
        {
          std::int32_t val = vals[i];
          if (std::ranges::find(output, static_cast<std::uint32_t>(val)) == output.end())
            output.push_back(static_cast<std::uint32_t>(val));
        }
      }
      else if (choice_type == SPA_CHOICE_Range)
      {
        std::int32_t min = (n_vals > 1) ? vals[1] : vals[0];
        std::int32_t max = (n_vals > 2) ? vals[2] : min;

        static constexpr std::uint32_t commonRates[] = {44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000};
        for (auto r : commonRates)
        {
          if (r >= static_cast<std::uint32_t>(min) && r <= static_cast<std::uint32_t>(max))
          {
            if (std::ranges::find(output, r) == output.end()) output.push_back(r);
          }
        }
        if (std::ranges::find(output, static_cast<std::uint32_t>(vals[0])) == output.end())
          output.push_back(static_cast<std::uint32_t>(vals[0]));
      }
    }
  }

  void collectIdValues(::spa_pod const* pod, std::vector<std::uint32_t>& output)
  {
    if (pod == nullptr) return;
    if (::spa_pod_is_id(pod))
    {
      std::uint32_t val = 0;
      if (::spa_pod_get_id(pod, &val) == 0)
      {
        if (std::ranges::find(output, val) == output.end()) output.push_back(val);
      }
    }
    else if (::spa_pod_is_choice(pod))
    {
      auto const* choice = reinterpret_cast<::spa_pod_choice const*>(pod);
      auto const n_vals = SPA_POD_CHOICE_N_VALUES(choice);
      auto const type = SPA_POD_CHOICE_VALUE_TYPE(choice);
      if (n_vals == 0 || type != SPA_TYPE_Id) return;

      auto const* vals = static_cast<std::uint32_t const*>(SPA_POD_CHOICE_VALUES(choice));
      auto const choice_type = SPA_POD_CHOICE_TYPE(choice);

      if (choice_type == SPA_CHOICE_Enum || choice_type == SPA_CHOICE_None)
      {
        for (std::uint32_t i = 0; i < n_vals; ++i)
        {
          std::uint32_t val = vals[i];
          if (std::ranges::find(output, val) == output.end()) output.push_back(val);
        }
      }
    }
  }

  void parseEnumFormat(::spa_pod const* param, app::core::backend::DeviceCapabilities& caps)
  {
    if (param == nullptr || !::spa_pod_is_object(param)) return;

    ::spa_pod_prop const* prop = nullptr;
    auto const* obj = reinterpret_cast<::spa_pod_object const*>(param);

    SPA_POD_OBJECT_FOREACH(obj, prop)
    {
      if (prop->key == SPA_FORMAT_AUDIO_format)
      {
        std::vector<std::uint32_t> formats;
        collectIdValues(&prop->value, formats);
        for (auto f : formats)
        {
          auto bd = spaFormatToBitDepth(f);
          if (bd > 0 && std::ranges::find(caps.bitDepths, bd) == caps.bitDepths.end()) caps.bitDepths.push_back(bd);
        }
      }
      else if (prop->key == SPA_FORMAT_AUDIO_rate)
      {
        collectIntValues(&prop->value, caps.sampleRates);
      }
      else if (prop->key == SPA_FORMAT_AUDIO_channels)
      {
        std::vector<std::uint32_t> channels;
        collectIntValues(&prop->value, channels);
        for (auto c : channels)
        {
          if (c > 0 && c <= 255)
          {
            auto c8 = static_cast<std::uint8_t>(c);
            if (std::ranges::find(caps.channelCounts, c8) == caps.channelCounts.end()) caps.channelCounts.push_back(c8);
          }
        }
      }
    }
  }

  bool copyFloatArray(::spa_pod const& pod, std::vector<float>& output)
  {
    auto values = std::array<float, 16>{};
    auto const count = ::spa_pod_copy_array(&pod, SPA_TYPE_Float, values.data(), values.size());
    if (count == 0) return false;
    output.assign(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(count));
    return true;
  }

  void mergeSinkProps(SinkProps& sinkProps, ::spa_pod const* param)
  {
    if (param == nullptr) return;
    if (auto const* p = ::spa_pod_find_prop(param, nullptr, SPA_PROP_volume))
    {
      float v = 0.0F;
      if (::spa_pod_get_float(&p->value, &v) == 0) sinkProps.volume = v;
    }
    if (auto const* p = ::spa_pod_find_prop(param, nullptr, SPA_PROP_mute))
    {
      bool v = false;
      if (::spa_pod_get_bool(&p->value, &v) == 0) sinkProps.mute = v;
    }
    if (auto const* p = ::spa_pod_find_prop(param, nullptr, SPA_PROP_channelVolumes))
      copyFloatArray(p->value, sinkProps.channelVolumes);
    if (auto const* p = ::spa_pod_find_prop(param, nullptr, SPA_PROP_softMute))
    {
      bool v = false;
      if (::spa_pod_get_bool(&p->value, &v) == 0) sinkProps.softMute = v;
    }
    if (auto const* p = ::spa_pod_find_prop(param, nullptr, SPA_PROP_softVolumes))
      copyFloatArray(p->value, sinkProps.softVolumes);
  }

  // --- PipeWireMonitor Impl ---

  struct PipeWireMonitor::Impl
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
      std::function<void(app::core::backend::AudioGraph const&)> callback;
    };

    Impl()
    {
      detail::ensurePipeWireInit();
      threadLoop.reset(::pw_thread_loop_new("PipeWireMonitor", nullptr));
      if (!threadLoop) return;

      context.reset(::pw_context_new(::pw_thread_loop_get_loop(threadLoop.get()), nullptr, 0));
      if (!context) return;

      if (::pw_thread_loop_start(threadLoop.get()) < 0) return;

      ::pw_thread_loop_lock(threadLoop.get());
      core.reset(::pw_context_connect(context.get(), nullptr, 0));
      ::pw_thread_loop_unlock(threadLoop.get());
    }

    ~Impl()
    {
      if (threadLoop) ::pw_thread_loop_stop(threadLoop.get());
      refreshEvent.reset();
      linkBindings.clear();
      streamNodeBindings.clear();
      sinkNodeBindings.clear();
      registryListener.reset();
      registry.reset();
      coreListener.reset();
      nodeFormatMap.clear();
      sinkPropsMap.clear();
      core.reset();
      context.reset();
      threadLoop.reset();
    }

    detail::PwThreadLoopPtr threadLoop;
    detail::PwContextPtr context;
    detail::PwCorePtr core;
    std::function<void()> onDevicesChanged;

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
    std::unordered_map<std::uint32_t, app::core::AudioFormat> nodeFormatMap;
    std::unordered_map<std::uint32_t, app::core::backend::DeviceCapabilities> sinkCapabilitiesMap;
    std::unordered_map<std::uint32_t, SinkProps> sinkPropsMap;
    detail::SpaSourcePtr refreshEvent;

    std::uint64_t nextSubscriptionId = 1;
    std::vector<GraphSubscription> subscriptions;

    void triggerRefresh()
    {
      if (refreshEvent)
        ::pw_loop_signal_event(::pw_thread_loop_get_loop(threadLoop.get()), refreshEvent.get());
      else
        refresh();
    }

    void refresh();
  };

  // --- Anonymous Namespace for Callbacks ---

  namespace
  {
    void onCoreDone(void* data, std::uint32_t, int seq)
    {
      auto* impl = static_cast<PipeWireMonitor::Impl*>(data);
      {
        auto const lock = std::lock_guard<std::mutex>{impl->mutex};
        if (seq == impl->coreSyncSeq)
        {
        } // Initial population done
      }
      impl->triggerRefresh();
    }

    void onRegistryGlobal(void* data,
                          std::uint32_t id,
                          std::uint32_t,
                          char const* type,
                          std::uint32_t version,
                          ::spa_dict const* props)
    {
      auto* impl = static_cast<PipeWireMonitor::Impl*>(data);
      bool isNode = (::strcmp(type, PW_TYPE_INTERFACE_Node) == 0);
      bool isLink = (::strcmp(type, PW_TYPE_INTERFACE_Link) == 0);
      if (!isNode && !isLink) return;

      static ::pw_link_events const linkEvents = {.version = PW_VERSION_LINK_EVENTS,
                                                  .info = [](void* data, ::pw_link_info const* info)
                                                  {
                                                    if (!info) return;
                                                    auto* impl = static_cast<PipeWireMonitor::Impl*>(data);
                                                    {
                                                      auto const lock = std::lock_guard<std::mutex>{impl->mutex};
                                                      auto& link = impl->links[info->id];
                                                      link.outputNodeId = info->output_node_id;
                                                      link.inputNodeId = info->input_node_id;
                                                      if (info->change_mask & PW_LINK_CHANGE_MASK_STATE)
                                                        link.state = info->state;
                                                    }
                                                    impl->triggerRefresh();
                                                  }};

      {
        auto const lock = std::lock_guard<std::mutex>{impl->mutex};
        if (isNode)
          impl->nodes[id] = parseNodeRecord(version, props);
        else if (isLink)
        {
          auto* proxy = static_cast<::pw_link*>(
            ::pw_registry_bind(impl->registry.get(), id, PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, 0));
          if (proxy)
          {
            auto& b = impl->linkBindings[id];
            b.id = id;
            b.proxy.reset(proxy);
            ::pw_link_add_listener(b.proxy.get(), b.listener.get(), &linkEvents, impl);
          }
        }
      }
      impl->triggerRefresh();
      if (isNode)
      {
        auto const lock = std::lock_guard<std::mutex>{impl->mutex};
        if (isSinkMediaClass(impl->nodes[id].mediaClass))
        {
          if (impl->onDevicesChanged) impl->onDevicesChanged();
        }
      }
    }

    void onRegistryGlobalRemove(void* data, std::uint32_t id)
    {
      auto* impl = static_cast<PipeWireMonitor::Impl*>(data);
      bool needsRefresh = false, deviceRemoved = false;
      {
        auto const lock = std::lock_guard<std::mutex>{impl->mutex};
        if (auto it = impl->linkBindings.find(id); it != impl->linkBindings.end()) impl->linkBindings.erase(it);
        if (auto it = impl->streamNodeBindings.find(id); it != impl->streamNodeBindings.end())
        {
          impl->streamNodeBindings.erase(it);
          impl->nodeFormatMap.erase(id);
          needsRefresh = true;
        }
        if (auto it = impl->sinkNodeBindings.find(id); it != impl->sinkNodeBindings.end())
        {
          impl->sinkNodeBindings.erase(it);
          impl->nodeFormatMap.erase(id);
          impl->sinkCapabilitiesMap.erase(id);
          impl->sinkPropsMap.erase(id);
          needsRefresh = true;
        }
        if (impl->nodes.contains(id))
        {
          if (isSinkMediaClass(impl->nodes[id].mediaClass)) deviceRemoved = true;
          impl->nodes.erase(id);
          needsRefresh = true;
        }
        if (impl->links.contains(id))
        {
          impl->links.erase(id);
          needsRefresh = true;
        }
      }
      if (needsRefresh) impl->triggerRefresh();
      if (deviceRemoved && impl->onDevicesChanged) impl->onDevicesChanged();
    }

    void onNodeInfo(void* data, ::pw_node_info const* info)
    {
      if (!info) return;
      auto* binding = static_cast<PipeWireMonitor::Impl::NodeBinding*>(data);
      auto* impl = binding->impl;
      {
        auto const lock = std::lock_guard<std::mutex>{impl->mutex};
        if (info->change_mask & PW_NODE_CHANGE_MASK_PROPS)
        {
          auto version = static_cast<std::uint32_t>(PW_VERSION_NODE);
          if (auto it = impl->nodes.find(info->id); it != impl->nodes.end()) version = it->second.version;
          impl->nodes[info->id] = parseNodeRecord(version, info->props);
        }
      }
      impl->triggerRefresh();
    }

    void onNodeParam(void* data, int, std::uint32_t id, std::uint32_t, std::uint32_t, ::spa_pod const* param)
    {
      auto* binding = static_cast<PipeWireMonitor::Impl::NodeBinding*>(data);
      auto* impl = binding->impl;
      {
        auto const lock = std::lock_guard<std::mutex>{impl->mutex};
        if (id == SPA_PARAM_Format)
        {
          if (auto fmt = detail::parseRawStreamFormat(param))
          {
            impl->nodeFormatMap[binding->id] = *fmt;
          }
        }
        else if (binding->role == NodeBindingRole::Sink && id == SPA_PARAM_EnumFormat)
        {
          if (!impl->nodeFormatMap.contains(binding->id))
          {
            if (auto fmt = detail::parseRawStreamFormat(param)) impl->nodeFormatMap[binding->id] = *fmt;
          }
          auto& caps = impl->sinkCapabilitiesMap[binding->id];
          parseEnumFormat(param, caps);
        }
        else if (binding->role == NodeBindingRole::Sink && id == SPA_PARAM_Props)
        {
          mergeSinkProps(impl->sinkPropsMap[binding->id], param);
        }
      }
      impl->triggerRefresh();
    }

    ::pw_core_events const coreEvents = {.version = PW_VERSION_CORE_EVENTS, .done = onCoreDone};
    ::pw_registry_events const registryEvents = {.version = PW_VERSION_REGISTRY_EVENTS,
                                                 .global = onRegistryGlobal,
                                                 .global_remove = onRegistryGlobalRemove};
    ::pw_node_events const streamNodeEvents = {.version = PW_VERSION_NODE_EVENTS,
                                               .info = onNodeInfo,
                                               .param = onNodeParam};
    ::pw_node_events const sinkNodeEvents = {.version = PW_VERSION_NODE_EVENTS,
                                             .info = onNodeInfo,
                                             .param = onNodeParam};

    void onRefreshEvent(void* data, std::uint64_t)
    {
      static_cast<PipeWireMonitor::Impl*>(data)->refresh();
    }
  }

  // --- PipeWireMonitor Implementation ---

  PipeWireMonitor::PipeWireMonitor()
    : _impl(std::make_unique<Impl>())
  {
  }

  PipeWireMonitor::~PipeWireMonitor() = default;

  void PipeWireMonitor::start()
  {
    if (!_impl->threadLoop) return;
    _impl->refreshEvent.get_deleter().loop = _impl->threadLoop.get();
    _impl->refreshEvent.reset(
      ::pw_loop_add_event(::pw_thread_loop_get_loop(_impl->threadLoop.get()), onRefreshEvent, _impl.get()));

    ::pw_thread_loop_lock(_impl->threadLoop.get());
    if (_impl->core)
    {
      _impl->registry.reset(
        reinterpret_cast<::pw_registry*>(::pw_core_get_registry(_impl->core.get(), PW_VERSION_REGISTRY, 0)));
      if (_impl->registry)
      {
        ::pw_registry_add_listener(_impl->registry.get(), _impl->registryListener.get(), &registryEvents, _impl.get());
        ::pw_core_add_listener(_impl->core.get(), _impl->coreListener.get(), &coreEvents, _impl.get());
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

  void PipeWireMonitor::setDevicesChangedCallback(std::function<void()> callback)
  {
    auto const lock = std::lock_guard<std::mutex>{_impl->mutex};
    _impl->onDevicesChanged = std::move(callback);
  }

  std::vector<app::core::backend::AudioDevice> PipeWireMonitor::enumerateSinks() const
  {
    auto devices = std::vector<app::core::backend::AudioDevice>{};
    auto const lock = std::lock_guard<std::mutex>{_impl->mutex};
    for (auto const& [id, node] : _impl->nodes)
    {
      if (isSinkMediaClass(node.mediaClass))
      {
        auto const deviceId = node.objectSerial ? std::format("{}", *node.objectSerial) : std::format("{}", id);
        auto const displayName =
          (node.nodeNick.empty() ? (node.nodeName.empty() ? node.objectPath : node.nodeName) : node.nodeNick);
        auto const description = (node.nodeNick.empty() ? "" : node.nodeName);
        auto caps = app::core::backend::DeviceCapabilities{};
        if (auto const it = _impl->sinkCapabilitiesMap.find(id); it != _impl->sinkCapabilitiesMap.end())
          caps = it->second;

        devices.push_back({.id = deviceId,
                           .displayName = displayName,
                           .description = description,
                           .isDefault = false,
                           .backendKind = app::core::backend::BackendKind::PipeWire,
                           .capabilities = {}});
        devices.push_back({.id = deviceId,
                           .displayName = std::format("{} (Exclusive)", displayName),
                           .description = description,
                           .isDefault = false,
                           .backendKind = app::core::backend::BackendKind::PipeWireExclusive,
                           .capabilities = caps});
      }
    }
    return devices;
  }

  std::optional<std::uint32_t> PipeWireMonitor::findSinkIdByName(std::string_view name) const
  {
    auto const lock = std::lock_guard<std::mutex>{_impl->mutex};
    for (auto const& [id, node] : _impl->nodes)
    {
      if (isSinkMediaClass(node.mediaClass) && node.nodeName == name) return id;
    }
    return std::nullopt;
  }

  std::uint64_t PipeWireMonitor::subscribeGraph(std::string_view routeAnchor,
                                                std::function<void(app::core::backend::AudioGraph const&)> callback)
  {
    auto const lock = std::lock_guard<std::mutex>{_impl->mutex};
    auto id = _impl->nextSubscriptionId++;
    _impl->subscriptions.push_back(
      {.id = id, .routeAnchor = std::string(routeAnchor), .callback = std::move(callback)});
    _impl->triggerRefresh();
    return id;
  }

  void PipeWireMonitor::unsubscribeGraph(std::uint64_t id)
  {
    auto const lock = std::lock_guard<std::mutex>{_impl->mutex};
    auto it = std::ranges::find_if(_impl->subscriptions, [id](auto const& sub) { return sub.id == id; });
    if (it != _impl->subscriptions.end())
    {
      _impl->subscriptions.erase(it);
    }
  }

  void PipeWireMonitor::refresh()
  {
    _impl->refresh();
  }

  void PipeWireMonitor::Impl::refresh()
  {
    ::pw_thread_loop_lock(threadLoop.get());
    {
      auto lock = std::unique_lock<std::mutex>{mutex};
      auto subscribedStreamIds = std::unordered_set<std::uint32_t>{};
      for (auto const& sub : subscriptions)
      {
        if (auto const parsedId = detail::parseUintProperty(sub.routeAnchor.c_str()))
          subscribedStreamIds.insert(*parsedId);
      }

      for (auto it = streamNodeBindings.begin(); it != streamNodeBindings.end();)
      {
        if (!subscribedStreamIds.contains(it->first))
        {
          nodeFormatMap.erase(it->first);
          it = streamNodeBindings.erase(it);
        }
        else
          ++it;
      }

      for (auto streamId : subscribedStreamIds)
      {
        if (streamNodeBindings.contains(streamId)) continue;

        auto const it = nodes.find(streamId);
        if (!registry || it == nodes.end()) continue;

        auto* node = static_cast<::pw_node*>(
          ::pw_registry_bind(registry.get(),
                             streamId,
                             PW_TYPE_INTERFACE_Node,
                             std::min(it->second.version, static_cast<std::uint32_t>(PW_VERSION_NODE)),
                             0));
        if (!node) continue;

        auto binding = std::make_unique<NodeBinding>();
        binding->id = streamId;
        binding->impl = this;
        binding->role = NodeBindingRole::Stream;
        binding->proxy.reset(node);
        std::uint32_t params[] = {SPA_PARAM_Format};
        ::pw_node_subscribe_params(binding->proxy.get(), params, 1);
        ::pw_node_enum_params(binding->proxy.get(), 1, SPA_PARAM_Format, 0, -1, nullptr);
        auto* bindingPtr = binding.get();
        ::pw_node_add_listener(bindingPtr->proxy.get(), bindingPtr->listener.get(), &streamNodeEvents, bindingPtr);
        streamNodeBindings[streamId] = std::move(binding);
      }

      for (auto const& [id, node] : nodes)
      {
        if (isSinkMediaClass(node.mediaClass) && !sinkNodeBindings.contains(id))
        {
          auto* proxy = static_cast<::pw_node*>(::pw_registry_bind(
            registry.get(), id, PW_TYPE_INTERFACE_Node, std::min(node.version, (std::uint32_t)PW_VERSION_NODE), 0));
          if (proxy)
          {
            auto b = std::make_unique<NodeBinding>();
            b->id = id;
            b->impl = this;
            b->role = NodeBindingRole::Sink;
            b->proxy.reset(proxy);
            std::uint32_t ps[] = {SPA_PARAM_Format, SPA_PARAM_EnumFormat, SPA_PARAM_Props};
            ::pw_node_subscribe_params(b->proxy.get(), ps, 3);
            ::pw_node_enum_params(b->proxy.get(), 1, SPA_PARAM_Format, 0, -1, nullptr);
            ::pw_node_enum_params(b->proxy.get(), 2, SPA_PARAM_EnumFormat, 0, -1, nullptr);
            ::pw_node_enum_params(b->proxy.get(), 3, SPA_PARAM_Props, 0, -1, nullptr);
            auto* p = b.get();
            ::pw_node_add_listener(p->proxy.get(), p->listener.get(), &sinkNodeEvents, p);
            sinkNodeBindings[id] = std::move(b);
          }
        }
      }

      auto executeGraphCallback =
        [&](std::uint32_t streamId, std::function<void(app::core::backend::AudioGraph const&)> const& cb)
      {
        app::core::backend::AudioGraph graph;
        auto localReachableNodes = std::vector<std::uint32_t>{};
        auto localReachableSet = std::unordered_set<std::uint32_t>{};
        if (streamId != PW_ID_ANY)
        {
          localReachableNodes.push_back(streamId);
          localReachableSet.insert(streamId);
        }

        for (std::size_t i = 0; i < localReachableNodes.size(); ++i)
        {
          auto curr = localReachableNodes[i];
          for (auto const& [_, link] : links)
          {
            if (!isActiveLink(link.state) || link.outputNodeId != curr || link.inputNodeId == PW_ID_ANY) continue;
            if (localReachableSet.insert(link.inputNodeId).second) localReachableNodes.push_back(link.inputNodeId);
          }
        }

        auto localFullSet = localReachableSet;
        for (auto const& [_, link] : links)
          if (isActiveLink(link.state) && localReachableSet.contains(link.inputNodeId))
            localFullSet.insert(link.outputNodeId);

        for (auto id : localFullSet)
        {
          auto it = nodes.find(id);
          if (it == nodes.end())
          {
            if (id == streamId)
            {
              app::core::backend::AudioNode node{.id = std::format("{}", id),
                                                 .type = app::core::backend::AudioNodeType::Stream,
                                                 .name = "RockStudio Playback",
                                                 .format = std::nullopt};
              graph.nodes.push_back(std::move(node));
            }
            continue;
          }
          bool isSink = isSinkMediaClass(it->second.mediaClass);
          bool isRs = (id == streamId);
          auto type =
            isRs ? app::core::backend::AudioNodeType::Stream
                 : (isSink ? app::core::backend::AudioNodeType::Sink
                           : (localReachableSet.contains(id) ? app::core::backend::AudioNodeType::Intermediary
                                                             : app::core::backend::AudioNodeType::ExternalSource));
          app::core::backend::AudioNode node{
            .id = std::format("{}", id),
            .type = type,
            .name =
              (it->second.nodeNick.empty() ? (it->second.nodeName.empty() ? it->second.objectPath : it->second.nodeName)
                                           : it->second.nodeNick),
            .objectPath = it->second.objectPath};
          if (auto const formatIt = nodeFormatMap.find(id); formatIt != nodeFormatMap.end())
            node.format = formatIt->second;

          if (isSink)
          {
            if (auto const propsIt = sinkPropsMap.find(id); propsIt != sinkPropsMap.end())
            {
              auto const& sinkProps = propsIt->second;
              auto const isUnity = [](float v) { return std::abs(v - 1.0F) < 1e-4F; };
              bool volumeAtUnity = (!sinkProps.volume || isUnity(*sinkProps.volume)) &&
                                   std::ranges::all_of(sinkProps.channelVolumes, isUnity) &&
                                   std::ranges::all_of(sinkProps.softVolumes, isUnity);
              node.volumeNotUnity = !volumeAtUnity;
              node.isMuted = (sinkProps.mute && *sinkProps.mute) || (sinkProps.softMute && *sinkProps.softMute);
            }
          }
          graph.nodes.push_back(std::move(node));
        }

        for (auto const& [_, link] : links)
        {
          if (isActiveLink(link.state) && localFullSet.contains(link.outputNodeId) &&
              localFullSet.contains(link.inputNodeId))
            graph.links.push_back({.sourceId = std::format("{}", link.outputNodeId),
                                   .destId = std::format("{}", link.inputNodeId),
                                   .isActive = true});
        }
        cb(graph);
      };

      for (auto const& sub : subscriptions)
      {
        auto parsedId = detail::parseUintProperty(sub.routeAnchor.c_str());
        std::uint32_t targetId = parsedId ? *parsedId : PW_ID_ANY;
        if (targetId != PW_ID_ANY && sub.callback)
        {
          executeGraphCallback(targetId, sub.callback);
        }
      }

      lock.unlock();
    }
    ::pw_thread_loop_unlock(threadLoop.get());
  }

} // namespace app::playback
