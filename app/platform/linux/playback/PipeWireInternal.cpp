// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "platform/linux/playback/PipeWireInternal.h"
#include "core/Log.h"

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
  // --- RAII Deleters Implementation ---

  void PwProxyDeleter::operator()(void* p) const noexcept { ::pw_proxy_destroy(static_cast<::pw_proxy*>(p)); }
  void PwThreadLoopDeleter::operator()(::pw_thread_loop* p) const noexcept { ::pw_thread_loop_destroy(p); }
  void PwContextDeleter::operator()(::pw_context* p) const noexcept { ::pw_context_destroy(p); }
  void PwCoreDeleter::operator()(::pw_core* p) const noexcept { ::pw_core_disconnect(p); }
  void PwStreamDeleter::operator()(::pw_stream* p) const noexcept { ::pw_stream_destroy(p); }
  void PwRegistryDeleter::operator()(::pw_registry* p) const noexcept { ::pw_proxy_destroy(reinterpret_cast<::pw_proxy*>(p)); }
  void PwLinkDeleter::operator()(::pw_link* p) const noexcept { ::pw_proxy_destroy(reinterpret_cast<::pw_proxy*>(p)); }

  void SpaSourceDeleter::operator()(::spa_source* p) const noexcept
  {
    if (p && loop)
    {
      ::pw_loop_destroy_source(::pw_thread_loop_get_loop(loop), p);
    }
  }

  SpaHookGuard::SpaHookGuard() noexcept { std::memset(&_hook, 0, sizeof(_hook)); }
  SpaHookGuard::~SpaHookGuard() { reset(); }
  void SpaHookGuard::reset() noexcept
  {
    if (_hook.link.next != nullptr)
    {
      ::spa_hook_remove(&_hook);
    }
    std::memset(&_hook, 0, sizeof(_hook));
  }

  // --- Helpers Implementation ---

  void ensurePipeWireInit()
  {
    struct PwInitGuard
    {
      PwInitGuard() { ::pw_init(nullptr, nullptr); }
      ~PwInitGuard() { ::pw_deinit(); }
    };
    static PwInitGuard guard;
  }

  bool isSinkMediaClass(std::string const& mediaClass)
  {
    return mediaClass == "Audio/Sink" || mediaClass.ends_with("/Sink");
  }

  bool isActiveLink(::pw_link_state state) noexcept
  {
    return state == PW_LINK_STATE_PAUSED || state == PW_LINK_STATE_ACTIVE;
  }

  std::optional<std::uint32_t> parseUintProperty(char const* value)
  {
    if (value == nullptr || *value == '\0') return std::nullopt;
    char* end = nullptr;
    auto const parsed = ::strtoul(value, &end, 10);
    if (end == value) return std::nullopt;
    return static_cast<std::uint32_t>(parsed);
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

    if (auto const serial = parseUintProperty(::spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL)))
      record.objectSerial = serial;

    if (auto const id = parseUintProperty(::spa_dict_lookup(props, "node.driver-id")))
      record.driverId = id;
    else if (auto const id = parseUintProperty(::spa_dict_lookup(props, "node.driver")))
      record.driverId = id;

    return record;
  }

  std::optional<app::core::AudioFormat> parseRawStreamFormat(::spa_pod const* param)
  {
    if (param == nullptr) return std::nullopt;
    auto info = ::spa_audio_info_raw{};
    if (::spa_format_audio_raw_parse(param, &info) < 0) return std::nullopt;

    auto format = app::core::AudioFormat{};
    format.sampleRate = info.rate;
    format.channels = static_cast<std::uint8_t>(info.channels);
    format.isInterleaved = true;

    if (info.format == SPA_AUDIO_FORMAT_S16 || info.format == SPA_AUDIO_FORMAT_S16_LE || info.format == SPA_AUDIO_FORMAT_S16_BE)
    {
      format.bitDepth = 16;
      format.isFloat = false;
    }
    else if (info.format == SPA_AUDIO_FORMAT_S24 || info.format == SPA_AUDIO_FORMAT_S24_LE || info.format == SPA_AUDIO_FORMAT_S24_BE)
    {
      format.bitDepth = 24; format.validBits = 24; format.isFloat = false;
    }
    else if (info.format == SPA_AUDIO_FORMAT_S24_32 || info.format == SPA_AUDIO_FORMAT_S24_32_LE || info.format == SPA_AUDIO_FORMAT_S24_32_BE)
    {
      format.bitDepth = 32; format.validBits = 24; format.isFloat = false;
    }
    else if (info.format == SPA_AUDIO_FORMAT_S32 || info.format == SPA_AUDIO_FORMAT_S32_LE || info.format == SPA_AUDIO_FORMAT_S32_BE)
    {
      format.bitDepth = 32; format.validBits = 32; format.isFloat = false;
    }
    else if (info.format == SPA_AUDIO_FORMAT_F32 || info.format == SPA_AUDIO_FORMAT_F32_LE || info.format == SPA_AUDIO_FORMAT_F32_BE)
    {
      format.bitDepth = 32; format.validBits = 32; format.isFloat = true;
    }
    else if (info.format == SPA_AUDIO_FORMAT_F64 || info.format == SPA_AUDIO_FORMAT_F64_LE || info.format == SPA_AUDIO_FORMAT_F64_BE)
    {
      format.bitDepth = 64; format.validBits = 64; format.isFloat = true;
    }
    else return std::nullopt;

    return format;
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
    if (auto const* p = ::spa_pod_find_prop(param, nullptr, SPA_PROP_volume)) {
      float v = 0.0F; if (::spa_pod_get_float(&p->value, &v) == 0) sinkProps.volume = v;
    }
    if (auto const* p = ::spa_pod_find_prop(param, nullptr, SPA_PROP_mute)) {
      bool v = false; if (::spa_pod_get_bool(&p->value, &v) == 0) sinkProps.mute = v;
    }
    if (auto const* p = ::spa_pod_find_prop(param, nullptr, SPA_PROP_channelVolumes)) copyFloatArray(p->value, sinkProps.channelVolumes);
    if (auto const* p = ::spa_pod_find_prop(param, nullptr, SPA_PROP_softMute)) {
      bool v = false; if (::spa_pod_get_bool(&p->value, &v) == 0) sinkProps.softMute = v;
    }
    if (auto const* p = ::spa_pod_find_prop(param, nullptr, SPA_PROP_softVolumes)) copyFloatArray(p->value, sinkProps.softVolumes);
  }

  // --- PipeWireMonitor Internal Classes ---

  struct PipeWireMonitor::Impl
  {
    struct LinkBinding final
    {
      std::uint32_t id = PW_ID_ANY;
      PwProxyPtr<::pw_link> proxy;
      SpaHookGuard listener;
      void reset() { id = PW_ID_ANY; listener.reset(); proxy.reset(); }
    };

    struct NodeBinding final
    {
      std::uint32_t id = PW_ID_ANY;
      PipeWireMonitor* monitor = nullptr;
      PwProxyPtr<::pw_node> proxy;
      SpaHookGuard listener;
      void reset() { id = PW_ID_ANY; monitor = nullptr; listener.reset(); proxy.reset(); }
    };

    Impl(PipeWireMonitor* parent, ::pw_thread_loop* loop, ::pw_core* core, app::core::backend::AudioRenderCallbacks callbacks)
      : parent(parent), loop(loop), core(core), callbacks(callbacks) {}

    PipeWireMonitor* parent;
    ::pw_thread_loop* loop;
    ::pw_core* core;
    ::pw_stream* stream = nullptr;
    app::core::backend::AudioRenderCallbacks callbacks;
    std::function<void()> onDevicesChanged;

    mutable std::mutex mutex;
    PwRegistryPtr registry;
    SpaHookGuard registryListener;
    SpaHookGuard coreListener;
    std::int32_t coreSyncSeq = -1;
    std::uint32_t streamNodeId = PW_ID_ANY;
    std::unordered_map<std::uint32_t, NodeRecord> nodes;
    std::unordered_map<std::uint32_t, LinkRecord> links;
    std::unordered_map<std::uint32_t, LinkBinding> linkBindings;
    std::unordered_map<std::uint32_t, std::unique_ptr<NodeBinding>> sinkNodeBindings;
    NodeBinding streamNodeBinding;
    std::optional<app::core::AudioFormat> negotiatedStreamFormat;
    std::optional<app::core::AudioFormat> sinkFormat;
    std::unordered_map<std::uint32_t, app::core::backend::DeviceCapabilities> sinkCapabilitiesMap;
    SinkProps sinkProps;
    SpaSourcePtr refreshEvent;

    void triggerRefresh()
    {
      if (refreshEvent) ::pw_loop_signal_event(::pw_thread_loop_get_loop(loop), refreshEvent.get());
      else refresh();
    }

    void refresh();
  };

  // --- Static Event Handlers ---

  static ::pw_core_events const coreEvents = { .version = PW_VERSION_CORE_EVENTS, .done = PipeWireMonitor::onCoreDone };
  static ::pw_registry_events const registryEvents = { .version = PW_VERSION_REGISTRY_EVENTS, .global = PipeWireMonitor::onRegistryGlobal, .global_remove = PipeWireMonitor::onRegistryGlobalRemove };
  static ::pw_link_events const linkEvents = { .version = PW_VERSION_LINK_EVENTS, .info = PipeWireMonitor::onLinkInfo };
  static ::pw_node_events const streamNodeEvents = { .version = PW_VERSION_NODE_EVENTS, .info = PipeWireMonitor::onNodeInfo };
  static ::pw_node_events const sinkNodeEvents = { .version = PW_VERSION_NODE_EVENTS, .info = PipeWireMonitor::onNodeInfo, .param = PipeWireMonitor::onNodeParam };

  static void onRefreshEvent(void* data, std::uint64_t) { static_cast<PipeWireMonitor*>(data)->refresh(); }

  // --- PipeWireMonitor Implementation ---

  PipeWireMonitor::PipeWireMonitor(::pw_thread_loop* loop, ::pw_core* core, app::core::backend::AudioRenderCallbacks callbacks)
    : _impl(std::make_unique<Impl>(this, loop, core, callbacks)) {}

  PipeWireMonitor::~PipeWireMonitor() { stop(); }

  void PipeWireMonitor::start()
  {
    _impl->refreshEvent.get_deleter().loop = _impl->loop;
    _impl->refreshEvent.reset(::pw_loop_add_event(::pw_thread_loop_get_loop(_impl->loop), onRefreshEvent, this));

    ::pw_thread_loop_lock(_impl->loop);
    if (_impl->core)
    {
      _impl->registry.reset(reinterpret_cast<::pw_registry*>(::pw_core_get_registry(_impl->core, PW_VERSION_REGISTRY, 0)));
      if (_impl->registry)
      {
        ::pw_registry_add_listener(_impl->registry.get(), _impl->registryListener.get(), &registryEvents, this);
        ::pw_core_add_listener(_impl->core, _impl->coreListener.get(), &coreEvents, this);
        _impl->coreSyncSeq = ::pw_core_sync(_impl->core, PW_ID_CORE, 0);
      }
    }
    ::pw_thread_loop_unlock(_impl->loop);
    _impl->triggerRefresh();
  }

  void PipeWireMonitor::stop()
  {
    auto const lock = std::lock_guard<std::mutex>{_impl->mutex};
    _impl->refreshEvent.reset();
    _impl->linkBindings.clear();
    _impl->sinkNodeBindings.clear();
    _impl->streamNodeBinding.reset();
    _impl->registryListener.reset();
    _impl->registry.reset();
    _impl->coreListener.reset();
    _impl->nodes.clear();
    _impl->links.clear();
  }

  void PipeWireMonitor::setStream(::pw_stream* stream)
  {
    auto const lock = std::lock_guard<std::mutex>{_impl->mutex};
    _impl->stream = stream;
    _impl->streamNodeId = stream ? ::pw_stream_get_node_id(stream) : PW_ID_ANY;
    _impl->triggerRefresh();
  }

  void PipeWireMonitor::setCallbacks(app::core::backend::AudioRenderCallbacks callbacks)
  {
    auto const lock = std::lock_guard<std::mutex>{_impl->mutex};
    _impl->callbacks = callbacks;
    _impl->triggerRefresh();
  }

  void PipeWireMonitor::setNegotiatedFormat(std::optional<app::core::AudioFormat> format)
  {
    auto const lock = std::lock_guard<std::mutex>{_impl->mutex};
    _impl->negotiatedStreamFormat = format;
    _impl->triggerRefresh();
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
        auto const displayName = (node.nodeNick.empty() ? (node.nodeName.empty() ? node.objectPath : node.nodeName) : node.nodeNick);
        auto const description = (node.nodeNick.empty() ? "" : node.nodeName);
        auto caps = app::core::backend::DeviceCapabilities{};
        if (auto const it = _impl->sinkCapabilitiesMap.find(id); it != _impl->sinkCapabilitiesMap.end()) caps = it->second;

        devices.push_back({.id = deviceId, .displayName = displayName, .description = description, .isDefault = false, .backendKind = app::core::backend::BackendKind::PipeWire, .capabilities = caps});
        devices.push_back({.id = deviceId, .displayName = std::format("{} (Exclusive)", displayName), .description = description, .isDefault = false, .backendKind = app::core::backend::BackendKind::PipeWireExclusive, .capabilities = caps});
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

  void PipeWireMonitor::onRegistryGlobal(void* data, std::uint32_t id, std::uint32_t, char const* type, std::uint32_t version, ::spa_dict const* props)
  {
    auto* self = static_cast<PipeWireMonitor*>(data);
    bool isNode = (::strcmp(type, PW_TYPE_INTERFACE_Node) == 0);
    bool isLink = (::strcmp(type, PW_TYPE_INTERFACE_Link) == 0);
    if (!isNode && !isLink) return;

    {
      auto const lock = std::lock_guard<std::mutex>{self->_impl->mutex};
      if (isNode) self->_impl->nodes[id] = parseNodeRecord(version, props);
      else if (isLink) {
        auto* proxy = static_cast<::pw_link*>(::pw_registry_bind(self->_impl->registry.get(), id, PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, 0));
        if (proxy) {
          auto& b = self->_impl->linkBindings[id]; b.id = id; b.proxy.reset(proxy);
          ::pw_link_add_listener(b.proxy.get(), b.listener.get(), &linkEvents, self);
        }
      }
    }
    self->_impl->triggerRefresh();
    if (isNode) {
      auto const lock = std::lock_guard<std::mutex>{self->_impl->mutex};
      if (isSinkMediaClass(self->_impl->nodes[id].mediaClass)) {
        if (self->_impl->onDevicesChanged) self->_impl->onDevicesChanged();
      }
    }
  }

  void PipeWireMonitor::onRegistryGlobalRemove(void* data, std::uint32_t id)
  {
    auto* self = static_cast<PipeWireMonitor*>(data);
    bool needsRefresh = false, deviceRemoved = false;
    {
      auto const lock = std::lock_guard<std::mutex>{self->_impl->mutex};
      if (auto it = self->_impl->linkBindings.find(id); it != self->_impl->linkBindings.end()) self->_impl->linkBindings.erase(it);
      if (auto it = self->_impl->sinkNodeBindings.find(id); it != self->_impl->sinkNodeBindings.end()) {
        self->_impl->sinkNodeBindings.erase(it); self->_impl->sinkCapabilitiesMap.erase(id); needsRefresh = true;
      }
      if (self->_impl->streamNodeBinding.id == id) { self->_impl->streamNodeBinding.reset(); needsRefresh = true; }
      if (self->_impl->nodes.contains(id)) {
        if (isSinkMediaClass(self->_impl->nodes[id].mediaClass)) deviceRemoved = true;
        self->_impl->nodes.erase(id); needsRefresh = true;
      }
      if (self->_impl->links.contains(id)) { self->_impl->links.erase(id); needsRefresh = true; }
      if (self->_impl->streamNodeId == id) { self->_impl->streamNodeId = PW_ID_ANY; needsRefresh = true; }
    }
    if (needsRefresh) self->_impl->triggerRefresh();
    if (deviceRemoved && self->_impl->onDevicesChanged) self->_impl->onDevicesChanged();
  }

  void PipeWireMonitor::onLinkInfo(void* data, ::pw_link_info const* info)
  {
    if (!info) return;
    auto* self = static_cast<PipeWireMonitor*>(data);
    {
      auto const lock = std::lock_guard<std::mutex>{self->_impl->mutex};
      auto& link = self->_impl->links[info->id];
      link.outputNodeId = info->output_node_id;
      link.inputNodeId = info->input_node_id;
      if (info->change_mask & PW_LINK_CHANGE_MASK_STATE) link.state = info->state;
    }
    self->_impl->triggerRefresh();
  }

  void PipeWireMonitor::onNodeInfo(void* data, ::pw_node_info const* info)
  {
    if (!info) return;
    auto* binding = static_cast<Impl::NodeBinding*>(data);
    auto* self = binding->monitor;
    {
      auto const lock = std::lock_guard<std::mutex>{self->_impl->mutex};
      if (info->change_mask & PW_NODE_CHANGE_MASK_PROPS) {
        auto version = static_cast<std::uint32_t>(PW_VERSION_NODE);
        if (auto it = self->_impl->nodes.find(info->id); it != self->_impl->nodes.end()) version = it->second.version;
        self->_impl->nodes[info->id] = parseNodeRecord(version, info->props);
      }
    }
    self->_impl->triggerRefresh();
  }

  void PipeWireMonitor::onNodeParam(void* data, int, std::uint32_t id, std::uint32_t, std::uint32_t, ::spa_pod const* param)
  {
    auto* binding = static_cast<Impl::NodeBinding*>(data);
    auto* self = binding->monitor;
    {
      auto const lock = std::lock_guard<std::mutex>{self->_impl->mutex};
      if (id == SPA_PARAM_Format) self->_impl->sinkFormat = parseRawStreamFormat(param);
      else if (id == SPA_PARAM_EnumFormat) {
        if (auto fmt = parseRawStreamFormat(param)) {
          auto& caps = self->_impl->sinkCapabilitiesMap[binding->id];
          if (std::ranges::find(caps.sampleRates, fmt->sampleRate) == caps.sampleRates.end()) caps.sampleRates.push_back(fmt->sampleRate);
          if (std::ranges::find(caps.bitDepths, fmt->bitDepth) == caps.bitDepths.end()) caps.bitDepths.push_back(fmt->bitDepth);
          if (std::ranges::find(caps.channelCounts, fmt->channels) == caps.channelCounts.end()) caps.channelCounts.push_back(fmt->channels);
        }
      }
      else if (id == SPA_PARAM_Props) mergeSinkProps(self->_impl->sinkProps, param);
    }
    self->_impl->triggerRefresh();
  }

  void PipeWireMonitor::onCoreDone(void* data, std::uint32_t, int seq)
  {
    auto* self = static_cast<PipeWireMonitor*>(data);
    {
      auto const lock = std::lock_guard<std::mutex>{self->_impl->mutex};
      if (seq == self->_impl->coreSyncSeq) {} // Initial population done
    }
    self->_impl->triggerRefresh();
  }

  void PipeWireMonitor::refresh() { _impl->refresh(); }

  void PipeWireMonitor::Impl::refresh()
  {
    ::pw_thread_loop_lock(loop);
    {
      auto lock = std::unique_lock<std::mutex>{mutex};
      if (stream) { if (auto id = ::pw_stream_get_node_id(stream); id != PW_ID_ANY) streamNodeId = id; }
      if (streamNodeId == PW_ID_ANY) {
        for (auto const& [id, node] : nodes) { if (node.nodeName == "RockStudio Playback") { streamNodeId = id; break; } }
      }

      if (streamNodeId != PW_ID_ANY && streamNodeBinding.id != streamNodeId) {
        streamNodeBinding.reset();
        auto it = nodes.find(streamNodeId);
        if (registry && it != nodes.end()) {
          auto* node = static_cast<::pw_node*>(::pw_registry_bind(registry.get(), streamNodeId, PW_TYPE_INTERFACE_Node, std::min(it->second.version, (std::uint32_t)PW_VERSION_NODE), 0));
          if (node) { streamNodeBinding.id = streamNodeId; streamNodeBinding.proxy.reset(node); streamNodeBinding.monitor = parent;
            ::pw_node_add_listener(streamNodeBinding.proxy.get(), streamNodeBinding.listener.get(), &streamNodeEvents, &streamNodeBinding);
          }
        }
      }

      auto reachableNodes = std::vector<std::uint32_t>{};
      auto reachableSet = std::unordered_set<std::uint32_t>{};
      if (streamNodeId != PW_ID_ANY) { reachableNodes.push_back(streamNodeId); reachableSet.insert(streamNodeId); }

      for (std::size_t i = 0; i < reachableNodes.size(); ++i) {
        auto curr = reachableNodes[i];
        for (auto const& [_, link] : links) {
          if (!isActiveLink(link.state) || link.outputNodeId != curr || link.inputNodeId == PW_ID_ANY) continue;
          if (reachableSet.insert(link.inputNodeId).second) reachableNodes.push_back(link.inputNodeId);
        }
      }

      auto sinkCandidates = std::vector<std::uint32_t>{};
      for (auto id : reachableNodes) {
        if (id == streamNodeId) continue;
        if (auto it = nodes.find(id); it != nodes.end() && isSinkMediaClass(it->second.mediaClass)) sinkCandidates.push_back(id);
      }

      auto desiredSinkNodeId = sinkCandidates.empty() ? PW_ID_ANY : sinkCandidates.front();
      if (desiredSinkNodeId == PW_ID_ANY && streamNodeId != PW_ID_ANY) {
        if (auto it = nodes.find(streamNodeId); it != nodes.end() && it->second.driverId) {
          auto dId = *it->second.driverId;
          if (auto dit = nodes.find(dId); dit != nodes.end() && isSinkMediaClass(dit->second.mediaClass)) desiredSinkNodeId = dId;
        }
      }

      for (auto const& [id, node] : nodes) {
        if (isSinkMediaClass(node.mediaClass) && !sinkNodeBindings.contains(id)) {
          auto* proxy = static_cast<::pw_node*>(::pw_registry_bind(registry.get(), id, PW_TYPE_INTERFACE_Node, std::min(node.version, (std::uint32_t)PW_VERSION_NODE), 0));
          if (proxy) {
            auto b = std::make_unique<NodeBinding>(); b->id = id; b->monitor = parent; b->proxy.reset(proxy);
            std::uint32_t ps[] = {SPA_PARAM_Format, SPA_PARAM_EnumFormat, SPA_PARAM_Props};
            ::pw_node_subscribe_params(b->proxy.get(), ps, 3);
            ::pw_node_enum_params(b->proxy.get(), 1, SPA_PARAM_Format, 0, -1, nullptr);
            ::pw_node_enum_params(b->proxy.get(), 2, SPA_PARAM_EnumFormat, 0, -1, nullptr);
            ::pw_node_enum_params(b->proxy.get(), 3, SPA_PARAM_Props, 0, -1, nullptr);
            auto* p = b.get(); ::pw_node_add_listener(p->proxy.get(), p->listener.get(), &sinkNodeEvents, p);
            sinkNodeBindings[id] = std::move(b);
          }
        }
      }

      if (callbacks.onGraphChanged && callbacks.userData) {
        app::core::backend::AudioGraph graph;
        auto fullSet = reachableSet;
        for (auto const& [_, link] : links) if (isActiveLink(link.state) && reachableSet.contains(link.inputNodeId)) fullSet.insert(link.outputNodeId);

        for (auto id : fullSet) {
          auto it = nodes.find(id); if (it == nodes.end()) continue;
          bool isSink = isSinkMediaClass(it->second.mediaClass);
          bool isRs = (id == streamNodeId);
          auto type = isRs ? app::core::backend::AudioNodeType::Stream : (isSink ? app::core::backend::AudioNodeType::Sink : (reachableSet.contains(id) ? app::core::backend::AudioNodeType::Intermediary : app::core::backend::AudioNodeType::ExternalSource));
          app::core::backend::AudioNode node{.id = std::format("{}", id), .type = type, .name = (it->second.nodeNick.empty() ? (it->second.nodeName.empty() ? it->second.objectPath : it->second.nodeName) : it->second.nodeNick), .objectPath = it->second.objectPath};
          if (isRs) node.format = negotiatedStreamFormat;
          else if (isSink && id == desiredSinkNodeId) {
            node.format = sinkFormat;
            auto const isUnity = [](float v) { return std::abs(v - 1.0F) < 1e-4F; };
            bool volumeAtUnity = (!sinkProps.volume || isUnity(*sinkProps.volume)) && std::ranges::all_of(sinkProps.channelVolumes, isUnity) && std::ranges::all_of(sinkProps.softVolumes, isUnity);
            node.volumeNotUnity = !volumeAtUnity;
            node.isMuted = (sinkProps.mute && *sinkProps.mute) || (sinkProps.softMute && *sinkProps.softMute);
          }
          graph.nodes.push_back(std::move(node));
        }

        for (auto const& [_, link] : links) {
          if (isActiveLink(link.state) && fullSet.contains(link.outputNodeId) && fullSet.contains(link.inputNodeId))
            graph.links.push_back({.sourceId = std::format("{}", link.outputNodeId), .destId = std::format("{}", link.inputNodeId)});
        }
        lock.unlock();
        callbacks.onGraphChanged(callbacks.userData, graph);
      } else lock.unlock();
    }
    ::pw_thread_loop_unlock(loop);
  }

} // namespace app::playback
