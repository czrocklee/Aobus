// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "platform/linux/playback/PipeWireBackend.h"
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
#include <cerrno>
#include <charconv>
#include <cstring>
#include <format>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
  struct PwProxyDeleter final
  {
    void operator()(void* p) const noexcept { ::pw_proxy_destroy(static_cast<::pw_proxy*>(p)); }
  };

  template<typename T>
  using PwProxyPtr = std::unique_ptr<T, PwProxyDeleter>;

  class SpaHookGuard final
  {
  public:
    SpaHookGuard() noexcept { std::memset(&_hook, 0, sizeof(_hook)); }
    ~SpaHookGuard() { reset(); }

    SpaHookGuard(SpaHookGuard const&) = delete;
    SpaHookGuard& operator=(SpaHookGuard const&) = delete;
    SpaHookGuard(SpaHookGuard&&) = delete;
    SpaHookGuard& operator=(SpaHookGuard&&) = delete;

    void reset() noexcept
    {
      // A hook is "set" if its link is part of a list.
      // In SPA, an uninitialized or removed link has next = NULL or points to itself depending on init style.
      // We use memset(0) so next == NULL means not set.
      if (_hook.link.next != nullptr)
      {
        ::spa_hook_remove(&_hook);
      }
      std::memset(&_hook, 0, sizeof(_hook));
    }

    ::spa_hook* get() noexcept { return &_hook; }

  private:
    ::spa_hook _hook;
  };

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

  bool isSinkMediaClass(std::string const& mediaClass)
  {
    return mediaClass == "Audio/Sink" || mediaClass.ends_with("/Sink");
  }

  bool isActiveLink(::pw_link_state state) noexcept
  {
    return static_cast<int>(state) >= PW_LINK_STATE_INIT;
  }

  std::optional<std::uint32_t> parseUintProperty(char const* value)
  {
    if (value == nullptr || *value == '\0')
    {
      return std::nullopt;
    }

    char* end = nullptr;
    auto const parsed = ::strtoul(value, &end, 10);

    if (end == value)
    {
      return std::nullopt;
    }

    return static_cast<std::uint32_t>(parsed);
  }

  std::string lookupProperty(::spa_dict const* props, char const* key)
  {
    auto const* value = props ? ::spa_dict_lookup(props, key) : nullptr;
    return value ? std::string(value) : std::string{};
  }

  std::string formatStreamFormat(app::core::playback::StreamFormat const& format)
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
    {
      record.objectSerial = serial;
    }

    if (auto const id = parseUintProperty(::spa_dict_lookup(props, "node.driver-id")))
    {
      record.driverId = id;
    }
    else if (auto const id = parseUintProperty(::spa_dict_lookup(props, "node.driver")))
    {
      record.driverId = id;
    }

    return record;
  }

  std::optional<app::core::playback::StreamFormat> parseRawStreamFormat(::spa_pod const* param)
  {
    if (param == nullptr)
    {
      return std::nullopt;
    }

    auto info = ::spa_audio_info_raw{};

    if (::spa_format_audio_raw_parse(param, &info) < 0)
    {
      return std::nullopt;
    }

    auto format = app::core::playback::StreamFormat{};
    format.sampleRate = info.rate;
    format.channels = static_cast<std::uint8_t>(info.channels);
    format.isInterleaved = true;

    if (info.format == SPA_AUDIO_FORMAT_S16 || info.format == SPA_AUDIO_FORMAT_S16_LE ||
        info.format == SPA_AUDIO_FORMAT_S16_BE)
    {
      format.bitDepth = 16;
      format.isFloat = false;
    }
    else if (info.format == SPA_AUDIO_FORMAT_S24 || info.format == SPA_AUDIO_FORMAT_S24_LE ||
             info.format == SPA_AUDIO_FORMAT_S24_BE)
    {
      format.bitDepth = 24;
      format.isFloat = false;
    }
    else if (info.format == SPA_AUDIO_FORMAT_S24_32 || info.format == SPA_AUDIO_FORMAT_S24_32_LE ||
             info.format == SPA_AUDIO_FORMAT_S24_32_BE || info.format == SPA_AUDIO_FORMAT_S32 ||
             info.format == SPA_AUDIO_FORMAT_S32_LE || info.format == SPA_AUDIO_FORMAT_S32_BE)
    {
      format.bitDepth = 32;
      format.isFloat = false;
    }
    else if (info.format == SPA_AUDIO_FORMAT_F32 || info.format == SPA_AUDIO_FORMAT_F32_LE ||
             info.format == SPA_AUDIO_FORMAT_F32_BE)
    {
      format.bitDepth = 32;
      format.isFloat = true;
    }
    else if (info.format == SPA_AUDIO_FORMAT_F64 || info.format == SPA_AUDIO_FORMAT_F64_LE ||
             info.format == SPA_AUDIO_FORMAT_F64_BE)
    {
      format.bitDepth = 64;
      format.isFloat = true;
    }
    else
    {
      return std::nullopt;
    }

    return format;
  }

  bool copyFloatArray(::spa_pod const& pod, std::vector<float>& output)
  {
    auto values = std::array<float, 16>{};
    auto const count = ::spa_pod_copy_array(&pod, SPA_TYPE_Float, values.data(), values.size());

    if (count == 0)
    {
      return false;
    }

    output.assign(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(count));
    return true;
  }

  void mergeSinkProps(SinkProps& sinkProps, ::spa_pod const* param)
  {
    if (param == nullptr)
    {
      return;
    }

    if (auto const* volumeProp = ::spa_pod_find_prop(param, nullptr, SPA_PROP_volume))
    {
      float value = 0.0F;

      if (::spa_pod_get_float(&volumeProp->value, &value) == 0)
      {
        sinkProps.volume = value;
      }
    }

    if (auto const* muteProp = ::spa_pod_find_prop(param, nullptr, SPA_PROP_mute))
    {
      bool value = false;

      if (::spa_pod_get_bool(&muteProp->value, &value) == 0)
      {
        sinkProps.mute = value;
      }
    }

    if (auto const* channelVolumesProp = ::spa_pod_find_prop(param, nullptr, SPA_PROP_channelVolumes))
    {
      copyFloatArray(channelVolumesProp->value, sinkProps.channelVolumes);
    }

    if (auto const* softMuteProp = ::spa_pod_find_prop(param, nullptr, SPA_PROP_softMute))
    {
      bool value = false;

      if (::spa_pod_get_bool(&softMuteProp->value, &value) == 0)
      {
        sinkProps.softMute = value;
      }
    }

    if (auto const* softVolumesProp = ::spa_pod_find_prop(param, nullptr, SPA_PROP_softVolumes))
    {
      copyFloatArray(softVolumesProp->value, sinkProps.softVolumes);
    }
  }

  // RAII Deleters
  struct PwThreadLoopDeleter final
  {
    void operator()(::pw_thread_loop* p) const noexcept { ::pw_thread_loop_destroy(p); }
  };

  struct PwContextDeleter final
  {
    void operator()(::pw_context* p) const noexcept { ::pw_context_destroy(p); }
  };

  struct PwCoreDeleter final
  {
    void operator()(::pw_core* p) const noexcept { ::pw_core_disconnect(p); }
  };

  struct PwStreamDeleter final
  {
    void operator()(::pw_stream* p) const noexcept { ::pw_stream_destroy(p); }
  };

  struct SpaSourceDeleter final
  {
    ::pw_thread_loop* loop = nullptr;
    void operator()(::spa_source* p) const noexcept
    {
      if (p && loop)
      {
        ::pw_loop_destroy_source(::pw_thread_loop_get_loop(loop), p);
      }
    }
  };

  // Smart Pointers
  using PwThreadLoopPtr = std::unique_ptr<::pw_thread_loop, PwThreadLoopDeleter>;
  using PwContextPtr = std::unique_ptr<::pw_context, PwContextDeleter>;
  using PwCorePtr = std::unique_ptr<::pw_core, PwCoreDeleter>;
  using PwStreamPtr = std::unique_ptr<::pw_stream, PwStreamDeleter>;
  using SpaSourcePtr = std::unique_ptr<::spa_source, SpaSourceDeleter>;

} // namespace

namespace app::playback
{
  using namespace app::core::playback;

  /**
   * @brief Internal class responsible for monitoring the PipeWire graph and discovering sinks.
   */
  class PipeWireMonitor final
  {
  public:
    struct LinkBinding final
    {
      std::uint32_t id = PW_ID_ANY;
      PwProxyPtr<::pw_link> proxy;
      SpaHookGuard listener;

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
      PwProxyPtr<::pw_node> proxy;
      SpaHookGuard listener;

      void reset()
      {
        id = PW_ID_ANY;
        listener.reset();
        proxy.reset();
      }
    };

    PipeWireMonitor(::pw_thread_loop* loop, ::pw_core* core, AudioRenderCallbacks callbacks)
      : _loop(loop), _core(core), _callbacks(callbacks)
    {
    }

    ~PipeWireMonitor() { stop(); }

    void start();

    void stop()
    {
      auto const lock = std::lock_guard<std::mutex>{_mutex};
      _refreshEvent.reset();

      _linkBindings.clear();
      _sinkNodeBinding.reset();
      _streamNodeBinding.reset();

      _registryListener.reset();
      _registry.reset();

      _coreListener.reset();

      _nodes.clear();
      _links.clear();
    }

    void triggerRefresh()
    {
      if (_refreshEvent)
      {
        ::pw_loop_signal_event(::pw_thread_loop_get_loop(_loop), _refreshEvent.get());
      }
      else
      {
        refresh();
      }
    }

    void refresh();

    void setStream(::pw_stream* stream)
    {
      auto const lock = std::lock_guard<std::mutex>{_mutex};
      _stream = stream;
      _streamNodeId = stream ? ::pw_stream_get_node_id(stream) : PW_ID_ANY;
    }

    void setCallbacks(AudioRenderCallbacks callbacks)
    {
      auto const lock = std::lock_guard<std::mutex>{_mutex};
      _callbacks = callbacks;
    }

    void setNegotiatedFormat(std::optional<StreamFormat> format)
    {
      auto const lock = std::lock_guard<std::mutex>{_mutex};
      _negotiatedStreamFormat = format;
    }

    // Event Handlers
    void handleRegistryGlobal(std::uint32_t id, char const* type, std::uint32_t version, ::spa_dict const* props);
    void handleRegistryGlobalRemove(std::uint32_t id);
    void handleLinkInfo(::pw_link_info const* info);
    void handleNodeInfo(::pw_node_info const* info);
    void handleSinkNodeParam(std::uint32_t id, ::spa_pod const* param);
    void handleCoreDone(int seq);

    // Static callbacks
    static void onRegistryGlobal(void* data,
                                 std::uint32_t id,
                                 std::uint32_t /*permissions*/,
                                 char const* type,
                                 std::uint32_t version,
                                 ::spa_dict const* props)
    {
      static_cast<PipeWireMonitor*>(data)->handleRegistryGlobal(id, type, version, props);
    }
    static void onRegistryGlobalRemove(void* data, std::uint32_t id)
    {
      static_cast<PipeWireMonitor*>(data)->handleRegistryGlobalRemove(id);
    }
    static void onLinkInfo(void* data, ::pw_link_info const* info)
    {
      static_cast<PipeWireMonitor*>(data)->handleLinkInfo(info);
    }
    static void onNodeInfo(void* data, ::pw_node_info const* info)
    {
      static_cast<PipeWireMonitor*>(data)->handleNodeInfo(info);
    }
    static void onSinkNodeParam(void* data,
                                int /*seq*/,
                                std::uint32_t id,
                                std::uint32_t /*index*/,
                                std::uint32_t /*next*/,
                                ::spa_pod const* param)
    {
      static_cast<PipeWireMonitor*>(data)->handleSinkNodeParam(id, param);
    }
    static void onCoreDone(void* data, std::uint32_t /*id*/, int seq)
    {
      static_cast<PipeWireMonitor*>(data)->handleCoreDone(seq);
    }
    static void onRefreshEvent(void* data, std::uint64_t /*count*/) { static_cast<PipeWireMonitor*>(data)->refresh(); }

  public:
    std::vector<app::core::playback::AudioDevice> enumerateSinks() const
    {
      auto devices = std::vector<app::core::playback::AudioDevice>{};
      auto const lock = std::lock_guard<std::mutex>{_mutex};

      PLAYBACK_LOG_DEBUG("PipeWireMonitor::enumerateSinks: tracking {} nodes", _nodes.size());

      for (auto const& [id, node] : _nodes)
      {
        if (isSinkMediaClass(node.mediaClass))
        {
          auto const deviceId = node.objectSerial ? std::to_string(*node.objectSerial) : std::to_string(id);
          devices.push_back(
            {.id = std::move(deviceId),
             .displayName =
               (node.nodeNick.empty() ? (node.nodeName.empty() ? node.objectPath : node.nodeName) : node.nodeNick),
             .isDefault = false});
        }
      }
      return devices;
    }

  private:
    ::pw_thread_loop* _loop;
    ::pw_core* _core;
    ::pw_stream* _stream = nullptr;
    AudioRenderCallbacks _callbacks;

    mutable std::mutex _mutex;
    PwProxyPtr<::pw_registry> _registry;
    SpaHookGuard _registryListener;
    SpaHookGuard _coreListener;
    std::int32_t _coreSyncSeq = -1;
    std::uint32_t _streamNodeId = PW_ID_ANY;
    std::unordered_map<std::uint32_t, NodeRecord> _nodes = {};
    std::unordered_map<std::uint32_t, LinkRecord> _links = {};
    std::unordered_map<std::uint32_t, LinkBinding> _linkBindings = {};
    NodeBinding _sinkNodeBinding = {};
    NodeBinding _streamNodeBinding = {};
    std::optional<StreamFormat> _negotiatedStreamFormat = {};
    std::optional<StreamFormat> _sinkFormat = {};
    SinkProps _sinkProps = {};
    SpaSourcePtr _refreshEvent;
  };

  // Event structure static definitions
  namespace
  {
    ::pw_core_events const coreEvents = []
    {
      auto e = ::pw_core_events{};
      e.version = PW_VERSION_CORE_EVENTS;
      e.done = PipeWireMonitor::onCoreDone;
      return e;
    }();

    ::pw_registry_events const registryEvents = []
    {
      auto e = ::pw_registry_events{};
      e.version = PW_VERSION_REGISTRY_EVENTS;
      e.global = PipeWireMonitor::onRegistryGlobal;
      e.global_remove = PipeWireMonitor::onRegistryGlobalRemove;
      return e;
    }();

    ::pw_link_events const linkEvents = []
    {
      auto e = ::pw_link_events{};
      e.version = PW_VERSION_LINK_EVENTS;
      e.info = PipeWireMonitor::onLinkInfo;
      return e;
    }();

    ::pw_node_events const streamNodeEvents = []
    {
      auto e = ::pw_node_events{};
      e.version = PW_VERSION_NODE_EVENTS;
      e.info = PipeWireMonitor::onNodeInfo;
      return e;
    }();

    ::pw_node_events const sinkNodeEvents = []
    {
      auto e = ::pw_node_events{};
      e.version = PW_VERSION_NODE_EVENTS;
      e.info = PipeWireMonitor::onNodeInfo;
      e.param = PipeWireMonitor::onSinkNodeParam;
      return e;
    }();
  }

  // PipeWireMonitor Implementation
  void PipeWireMonitor::start()
  {
    _refreshEvent.get_deleter().loop = _loop;
    _refreshEvent.reset(::pw_loop_add_event(::pw_thread_loop_get_loop(_loop), onRefreshEvent, this));

    // Initialize registry monitor immediately using the provided core
    ::pw_thread_loop_lock(_loop);
    if (_core)
    {
      _registry.reset(::pw_core_get_registry(_core, PW_VERSION_REGISTRY, 0));
      if (_registry)
      {
        ::pw_registry_add_listener(_registry.get(), _registryListener.get(), &registryEvents, this);
        ::pw_core_add_listener(_core, _coreListener.get(), &coreEvents, this);
        _coreSyncSeq = ::pw_core_sync(_core, PW_ID_CORE, 0);
      }
    }
    ::pw_thread_loop_unlock(_loop);

    triggerRefresh();
  }

  void PipeWireMonitor::handleRegistryGlobal(std::uint32_t id,
                                             char const* type,
                                             std::uint32_t version,
                                             ::spa_dict const* props)
  {
    bool const isNode = (::strcmp(type, PW_TYPE_INTERFACE_Node) == 0);
    bool const isLink = (::strcmp(type, PW_TYPE_INTERFACE_Link) == 0);

    if (!isNode && !isLink)
    {
      return;
    }

    {
      auto const lock = std::lock_guard<std::mutex>{_mutex};

      if (_streamNodeId == PW_ID_ANY && _stream)
      {
        if (auto const streamNodeId = ::pw_stream_get_node_id(_stream); streamNodeId != PW_ID_ANY)
        {
          _streamNodeId = streamNodeId;
        }
      }

      if (isNode)
      {
        auto const record = parseNodeRecord(version, props);

        if (_streamNodeId == PW_ID_ANY)
        {
          auto const appName = lookupProperty(props, PW_KEY_APP_NAME);

          if (record.nodeName == "RockStudio Playback" || appName == "RockStudio")
          {
            _streamNodeId = id;
          }
        }

        _nodes[id] = std::move(record);
      }
      else if (isLink)
      {
        auto& link = _links[id];
        link.outputNodeId = parseUintProperty(::spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_NODE)).value_or(PW_ID_ANY);
        link.inputNodeId = parseUintProperty(::spa_dict_lookup(props, PW_KEY_LINK_INPUT_NODE)).value_or(PW_ID_ANY);
        link.state = PW_LINK_STATE_ACTIVE;

        if (_registry && !_linkBindings.contains(id))
        {
          auto* const proxy = static_cast<::pw_link*>(::pw_registry_bind(
            _registry.get(), id, PW_TYPE_INTERFACE_Link, std::min(version, std::uint32_t(PW_VERSION_LINK)), 0));
          if (proxy)
          {
            auto& binding = _linkBindings[id];
            binding.id = id;
            binding.proxy.reset(proxy);
            ::pw_link_add_listener(binding.proxy.get(), binding.listener.get(), &linkEvents, this);
          }
        }
      }
    }
    triggerRefresh();
  }

  void PipeWireMonitor::handleRegistryGlobalRemove(std::uint32_t id)
  {
    bool needsRefresh = false;
    {
      auto const lock = std::lock_guard<std::mutex>{_mutex};

      if (auto binding = _linkBindings.find(id); binding != _linkBindings.end())
      {
        _linkBindings.erase(binding);
      }

      if (_sinkNodeBinding.id == id)
      {
        _sinkNodeBinding.reset();
        _sinkFormat.reset();
        _sinkProps = {};
        needsRefresh = true;
      }

      if (_streamNodeBinding.id == id)
      {
        _streamNodeBinding.reset();
        needsRefresh = true;
      }

      if (_nodes.contains(id))
      {
        _nodes.erase(id);
        needsRefresh = true;
      }

      if (_links.contains(id))
      {
        _links.erase(id);
        needsRefresh = true;
      }

      if (_streamNodeId == id)
      {
        _streamNodeId = PW_ID_ANY;
        needsRefresh = true;
      }
    }

    if (needsRefresh)
    {
      triggerRefresh();
    }
  }

  void PipeWireMonitor::handleLinkInfo(::pw_link_info const* info)
  {
    if (info == nullptr)
    {
      return;
    }

    {
      auto const lock = std::lock_guard<std::mutex>{_mutex};
      auto& link = _links[info->id];
      link.outputNodeId = info->output_node_id;
      link.inputNodeId = info->input_node_id;

      if (info->change_mask & PW_LINK_CHANGE_MASK_STATE)
      {
        link.state = info->state;
      }
    }
    triggerRefresh();
  }

  void PipeWireMonitor::handleNodeInfo(::pw_node_info const* info)
  {
    if (info == nullptr)
    {
      return;
    }

    {
      auto const lock = std::lock_guard<std::mutex>{_mutex};
      if (info->change_mask & PW_NODE_CHANGE_MASK_PROPS)
      {
        auto version = static_cast<std::uint32_t>(PW_VERSION_NODE);
        if (auto const existing = _nodes.find(info->id); existing != _nodes.end())
        {
          version = existing->second.version;
        }
        _nodes[info->id] = parseNodeRecord(version, info->props);
      }
    }
    triggerRefresh();
  }

  void PipeWireMonitor::handleSinkNodeParam(std::uint32_t id, ::spa_pod const* param)
  {
    {
      auto const lock = std::lock_guard<std::mutex>{_mutex};
      if (id == SPA_PARAM_Format)
      {
        _sinkFormat = parseRawStreamFormat(param);
      }
      else if (id == SPA_PARAM_Props)
      {
        mergeSinkProps(_sinkProps, param);
      }
    }
    triggerRefresh();
  }

  void PipeWireMonitor::handleCoreDone(int seq)
  {
    {
      auto const lock = std::lock_guard<std::mutex>{_mutex};
      if (seq == _coreSyncSeq)
      {
        // Initial population done
      }
    }
    triggerRefresh();
  }

  void PipeWireMonitor::refresh()
  {
    if (!_loop)
    {
      return;
    }

    ::pw_thread_loop_lock(_loop);
    {
      auto const lock = std::lock_guard<std::mutex>{_mutex};

      if (_stream)
      {
        if (auto const streamNodeId = ::pw_stream_get_node_id(_stream); streamNodeId != PW_ID_ANY)
        {
          _streamNodeId = streamNodeId;
        }
      }

      if (_streamNodeId == PW_ID_ANY)
      {
        for (auto const& [id, node] : _nodes)
        {
          if (node.nodeName == "RockStudio Playback")
          {
            _streamNodeId = id;
            break;
          }
        }
      }

      if (_streamNodeId != PW_ID_ANY && _streamNodeBinding.id != _streamNodeId)
      {
        _streamNodeBinding.reset();
        auto const nodeIt = _nodes.find(_streamNodeId);
        if (_registry && nodeIt != _nodes.end())
        {
          auto* const node =
            static_cast<::pw_node*>(::pw_registry_bind(_registry.get(),
                                                       _streamNodeId,
                                                       PW_TYPE_INTERFACE_Node,
                                                       std::min(nodeIt->second.version, std::uint32_t(PW_VERSION_NODE)),
                                                       0));
          if (node)
          {
            _streamNodeBinding.id = _streamNodeId;
            _streamNodeBinding.proxy.reset(node);
            ::pw_node_add_listener(
              _streamNodeBinding.proxy.get(), _streamNodeBinding.listener.get(), &streamNodeEvents, this);
          }
        }
      }

      auto reachableNodes = std::vector<std::uint32_t>{};
      auto reachableSet = std::unordered_set<std::uint32_t>{};

      if (_streamNodeId != PW_ID_ANY)
      {
        reachableNodes.push_back(_streamNodeId);
        reachableSet.insert(_streamNodeId);
      }

      for (std::size_t index = 0; index < reachableNodes.size(); ++index)
      {
        auto const currentNodeId = reachableNodes[index];
        for (auto const& [_, link] : _links)
        {
          if (!isActiveLink(link.state) || link.outputNodeId != currentNodeId || link.inputNodeId == PW_ID_ANY)
          {
            continue;
          }

          if (reachableSet.insert(link.inputNodeId).second)
          {
            reachableNodes.push_back(link.inputNodeId);
          }
        }
      }

      auto sinkCandidates = std::vector<std::uint32_t>{};
      for (auto const nodeId : reachableNodes)
      {
        if (nodeId == _streamNodeId) continue;
        auto const nodeIt = _nodes.find(nodeId);
        if (nodeIt == _nodes.end()) continue;

        if (isSinkMediaClass(nodeIt->second.mediaClass))
        {
          sinkCandidates.push_back(nodeId);
        }
      }

      auto desiredSinkNodeId = sinkCandidates.empty() ? PW_ID_ANY : sinkCandidates.front();

      if (desiredSinkNodeId == PW_ID_ANY && _streamNodeId != PW_ID_ANY)
      {
        auto const streamNodeIt = _nodes.find(_streamNodeId);
        if (streamNodeIt != _nodes.end() && streamNodeIt->second.driverId)
        {
          auto const driverNodeId = *streamNodeIt->second.driverId;
          auto const driverNodeIt = _nodes.find(driverNodeId);
          if (driverNodeIt != _nodes.end() && isSinkMediaClass(driverNodeIt->second.mediaClass))
          {
            desiredSinkNodeId = driverNodeId;
          }
        }
      }

      if (_sinkNodeBinding.id != desiredSinkNodeId)
      {
        _sinkNodeBinding.reset();
        _sinkFormat.reset();
        _sinkProps = {};

        if (desiredSinkNodeId != PW_ID_ANY)
        {
          auto const nodeIt = _nodes.find(desiredSinkNodeId);
          if (_registry && nodeIt != _nodes.end())
          {
            auto* const node = static_cast<::pw_node*>(
              ::pw_registry_bind(_registry.get(),
                                 desiredSinkNodeId,
                                 PW_TYPE_INTERFACE_Node,
                                 std::min(nodeIt->second.version, std::uint32_t(PW_VERSION_NODE)),
                                 0));
            if (node)
            {
              _sinkNodeBinding.id = desiredSinkNodeId;
              _sinkNodeBinding.proxy.reset(node);
              ::pw_node_add_listener(
                _sinkNodeBinding.proxy.get(), _sinkNodeBinding.listener.get(), &sinkNodeEvents, this);

              auto params = std::to_array<std::uint32_t>({SPA_PARAM_Format, SPA_PARAM_Props});
              ::pw_node_subscribe_params(
                _sinkNodeBinding.proxy.get(), params.data(), static_cast<std::uint32_t>(params.size()));
              ::pw_node_enum_params(_sinkNodeBinding.proxy.get(),
                                    1,
                                    SPA_PARAM_Format,
                                    0,
                                    std::numeric_limits<std::uint32_t>::max(),
                                    nullptr);
              ::pw_node_enum_params(_sinkNodeBinding.proxy.get(),
                                    2,
                                    SPA_PARAM_Props,
                                    0,
                                    std::numeric_limits<std::uint32_t>::max(),
                                    nullptr);
            }
          }
        }
      }

      // Map internal state to AudioGraph
      if (_callbacks.onGraphChanged)
      {
        auto graph = AudioGraph{};

        // 1. Add our Stream Node
        if (_streamNodeId != PW_ID_ANY)
        {
          auto const it = _nodes.find(_streamNodeId);
          auto name = (it != _nodes.end()) ? (it->second.nodeNick.empty() ? it->second.nodeName : it->second.nodeNick)
                                           : "RockStudio Stream";

          graph.nodes.push_back({.id = std::to_string(_streamNodeId),
                                 .type = AudioNodeType::Stream,
                                 .name = std::move(name),
                                 .format = _negotiatedStreamFormat});
        }

        // 2. Add all reachable nodes (excluding our stream)
        for (auto const nodeId : reachableSet)
        {
          if (nodeId == _streamNodeId) continue;

          auto const it = _nodes.find(nodeId);
          if (it == _nodes.end()) continue;

          auto const& record = it->second;
          bool isSink = isSinkMediaClass(record.mediaClass);

          auto node =
            AudioNode{.id = std::to_string(nodeId),
                      .type = isSink ? AudioNodeType::Sink : AudioNodeType::Intermediary,
                      .name = (record.nodeNick.empty() ? (record.nodeName.empty() ? record.objectPath : record.nodeName)
                                                       : record.nodeNick),
                      .objectPath = record.objectPath};

          if (isSink && nodeId == desiredSinkNodeId)
          {
            node.format = _sinkFormat;
            node.volumeNotUnity =
              (_sinkProps.volume && *_sinkProps.volume < 0.999F) ||
              std::any_of(_sinkProps.channelVolumes.begin(),
                          _sinkProps.channelVolumes.end(),
                          [](float v) { return v < 0.999F; }) ||
              std::any_of(
                _sinkProps.softVolumes.begin(), _sinkProps.softVolumes.end(), [](float v) { return v < 0.999F; });
            node.isMuted = _sinkProps.mute.value_or(false) || _sinkProps.softMute.value_or(false);
          }

          graph.nodes.push_back(std::move(node));
        }

        // 4. Identify External Sources and ALL active links to our path nodes
        for (auto const& [linkId, link] : _links)
        {
          if (isActiveLink(link.state) && reachableSet.contains(link.inputNodeId))
          {
            if (reachableSet.contains(link.outputNodeId))
            {
              // Internal link (already handled by step 3, but we can consolidate here)
              graph.links.push_back(app::core::playback::AudioLink{.sourceId = std::to_string(link.outputNodeId),
                                                                   .destId = std::to_string(link.inputNodeId),
                                                                   .isActive = true});
            }
            else
            {
              // External source feeding into one of our path nodes
              auto const it = _nodes.find(link.outputNodeId);
              if (it != _nodes.end())
              {
                auto const& record = it->second;
                auto sourceId = "ext-" + std::to_string(link.outputNodeId);

                // Add the external source node if not already added
                if (std::find_if(graph.nodes.begin(),
                                 graph.nodes.end(),
                                 [&](auto const& n) { return n.id == sourceId; }) == graph.nodes.end())
                {
                  graph.nodes.push_back(app::core::playback::AudioNode{
                    .id = sourceId,
                    .type = AudioNodeType::ExternalSource,
                    .name = (record.nodeNick.empty() ? (record.nodeName.empty() ? record.objectPath : record.nodeName)
                                                     : record.nodeNick)});
                }

                // Link it
                graph.links.push_back(app::core::playback::AudioLink{
                  .sourceId = std::move(sourceId), .destId = std::to_string(link.inputNodeId), .isActive = true});
              }
            }
          }
        }

        _callbacks.onGraphChanged(_callbacks.userData, graph);
      }
    }
    ::pw_thread_loop_unlock(_loop);
  }

  /**
   * @brief Implementation of PipeWireBackend.
   */
  struct PipeWireBackend::Impl final
  {
    Impl() { ::pw_init(nullptr, nullptr); }

    ~Impl()
    {
      _drainPending = false;
      if (_threadLoop) ::pw_thread_loop_lock(_threadLoop.get());

      if (_monitor)
      {
        _monitor->stop();
        _monitor.reset();
      }

      _stream.reset();

      if (_threadLoop)
      {
        ::pw_thread_loop_unlock(_threadLoop.get());
        ::pw_thread_loop_stop(_threadLoop.get());
      }

      _core.reset();
      _context.reset();
      _threadLoop.reset();
    }

    void destroyStream() noexcept
    {
      _drainPending = false;
      if (_threadLoop) ::pw_thread_loop_lock(_threadLoop.get());

      _stream.reset();

      if (_monitor)
      {
        _monitor->setStream(nullptr);
      }

      if (_threadLoop)
      {
        ::pw_thread_loop_unlock(_threadLoop.get());
      }
    }

    void setError(std::string message)
    {
      PLAYBACK_LOG_ERROR("PipeWire error: {}", message);
      _lastError = std::move(message);
    }

    // Event Handlers
    void handleStreamProcess();
    void handleStreamParamChanged(std::uint32_t id, ::spa_pod const* param);
    void handleStreamStateChanged(std::int32_t oldState, std::int32_t newState, char const* errorMessage);
    void handleStreamDrained();

    // Static callbacks
    static void onStreamProcess(void* data) { static_cast<Impl*>(data)->handleStreamProcess(); }
    static void onStreamParamChanged(void* data, std::uint32_t id, ::spa_pod const* param)
    {
      static_cast<Impl*>(data)->handleStreamParamChanged(id, param);
    }
    static void onStreamStateChanged(void* data, std::int32_t oldState, std::int32_t newState, char const* errorMessage)
    {
      static_cast<Impl*>(data)->handleStreamStateChanged(oldState, newState, errorMessage);
    }
    static void onStreamDrained(void* data) { static_cast<Impl*>(data)->handleStreamDrained(); }

    // Members
    AudioRenderCallbacks _callbacks;
    StreamFormat _format;
    std::atomic<bool> _drainPending = false;
    std::string _lastError;
    bool _strictFormatRequired = false;
    bool _strictFormatRejected = false;

    PwThreadLoopPtr _threadLoop;
    PwContextPtr _context;
    PwCorePtr _core;
    PwStreamPtr _stream;
    std::unique_ptr<PipeWireMonitor> _monitor;
  };

  // Event structure static definitions
  namespace
  {
    ::pw_stream_events const streamEvents = []
    {
      auto e = ::pw_stream_events{};
      e.version = PW_VERSION_STREAM_EVENTS;
      e.state_changed = reinterpret_cast<void (*)(void*, ::pw_stream_state, ::pw_stream_state, char const*)>(
        PipeWireBackend::Impl::onStreamStateChanged);
      e.param_changed = PipeWireBackend::Impl::onStreamParamChanged;
      e.process = PipeWireBackend::Impl::onStreamProcess;
      e.drained = PipeWireBackend::Impl::onStreamDrained;
      return e;
    }();
  }

  // Impl Implementation
  void PipeWireBackend::Impl::handleStreamProcess()
  {
    static std::atomic<std::uint64_t> callCount{0};
    if (!_callbacks.readPcm) return;
    if (callCount.fetch_add(1) == 0)
    {
      PLAYBACK_LOG_INFO("PipeWireBackend: handleStreamProcess called (first invocation)");
    }
    auto* const buffer = ::pw_stream_dequeue_buffer(_stream.get());
    if (!buffer)
    {
      static std::atomic<std::uint64_t> nullCount{0};
      if (nullCount.fetch_add(1) < 5)
      {
        PLAYBACK_LOG_WARN("PipeWireBackend: handleStreamProcess: null buffer (count={})", nullCount.load());
      }
      if (_callbacks.onUnderrun) _callbacks.onUnderrun(_callbacks.userData);
      return;
    }

    auto* const data = static_cast<std::byte*>(buffer->buffer->datas[0].data);
    auto const size = buffer->buffer->datas[0].maxsize;
    auto const bytesPerSample = (_format.bitDepth == 24) ? 3U : (_format.bitDepth == 32) ? 4U : 2U;
    auto const frameBytes = static_cast<std::size_t>(_format.channels) * bytesPerSample;
    auto const requestSize = frameBytes > 0 ? size - (size % frameBytes) : 0;

    if (data && requestSize > 0)
    {
      auto output = std::span<std::byte>(data, requestSize);
      auto const read = _callbacks.readPcm(_callbacks.userData, output);
      auto const alignedRead = read - (read % frameBytes);

      if (alignedRead == 0 && _callbacks.isSourceDrained && _callbacks.isSourceDrained(_callbacks.userData))
      {
        ::pw_stream_return_buffer(_stream.get(), buffer);
        _drainPending = true;
        ::pw_stream_flush(_stream.get(), true);
        return;
      }

      buffer->buffer->datas[0].chunk->offset = 0;
      buffer->buffer->datas[0].chunk->size = static_cast<std::uint32_t>(alignedRead);
      buffer->buffer->datas[0].chunk->stride = static_cast<std::int32_t>(frameBytes);
      if (_callbacks.onPositionAdvanced && frameBytes > 0 && alignedRead > 0)
        _callbacks.onPositionAdvanced(_callbacks.userData, static_cast<std::uint32_t>(alignedRead / frameBytes));
    }
    ::pw_stream_queue_buffer(_stream.get(), buffer);
  }

  void PipeWireBackend::Impl::handleStreamParamChanged(std::uint32_t id, ::spa_pod const* param)
  {
    if (id != SPA_PARAM_Format) return;
    auto const negotiatedFormat = parseRawStreamFormat(param);

    if (_monitor) _monitor->setNegotiatedFormat(negotiatedFormat);

    if (_strictFormatRequired && negotiatedFormat && *negotiatedFormat != _format && !_strictFormatRejected)
    {
      _strictFormatRejected = true;
      auto const message =
        std::format("Selected PipeWire device does not support {} in exclusive mode; it negotiated {} instead. "
                    "Choose another device or use shared PipeWire mode.",
                    formatStreamFormat(_format),
                    formatStreamFormat(*negotiatedFormat));
      PLAYBACK_LOG_ERROR("{}", message);
      setError(message);
      if (_callbacks.onBackendError) _callbacks.onBackendError(_callbacks.userData, _lastError);
      return;
    }

    if (_monitor) _monitor->triggerRefresh();
  }

  void PipeWireBackend::Impl::handleStreamStateChanged(std::int32_t /*oldState*/,
                                                       std::int32_t newState,
                                                       char const* errorMessage)
  {
    PLAYBACK_LOG_INFO("PipeWireBackend: stream state changed to {}", newState);
    if (newState == PW_STREAM_STATE_ERROR)
    {
      PLAYBACK_LOG_ERROR("PipeWireBackend: stream error: {}", errorMessage ? errorMessage : "unknown");
      if (errorMessage && *errorMessage) setError(std::string{errorMessage});
      return;
    }
    if (newState == PW_STREAM_STATE_PAUSED || newState == PW_STREAM_STATE_STREAMING)
    {
      PLAYBACK_LOG_INFO("PipeWireBackend: stream state {}, triggering refresh", newState);
      if (_monitor) _monitor->triggerRefresh();
    }
  }

  void PipeWireBackend::Impl::handleStreamDrained()
  {
    _drainPending = false;
    if (_callbacks.onDrainComplete) _callbacks.onDrainComplete(_callbacks.userData);
  }

  // PipeWireBackend Implementation
  PipeWireBackend::PipeWireBackend()
    : _impl(std::make_unique<Impl>())
  {
    static std::once_flag pwInitFlag;
    std::call_once(pwInitFlag, []() { ::pw_init(nullptr, nullptr); });

    _impl->_threadLoop.reset(::pw_thread_loop_new("rockstudio-pw", nullptr));
    if (_impl->_threadLoop)
    {
      _impl->_context.reset(::pw_context_new(::pw_thread_loop_get_loop(_impl->_threadLoop.get()), nullptr, 0));
      if (_impl->_context)
      {
        ::pw_thread_loop_start(_impl->_threadLoop.get());
        ::pw_thread_loop_lock(_impl->_threadLoop.get());

        _impl->_core.reset(::pw_context_connect(_impl->_context.get(), nullptr, 0));
        if (_impl->_core)
        {
          _impl->_monitor =
            std::make_unique<PipeWireMonitor>(_impl->_threadLoop.get(), _impl->_core.get(), AudioRenderCallbacks{});
          _impl->_monitor->start();
          PLAYBACK_LOG_DEBUG("PipeWireBackend: successfully initialized core and monitor");
        }
        else
        {
          PLAYBACK_LOG_ERROR("PipeWireBackend: pw_context_connect failed, errno={}", errno);
        }

        ::pw_thread_loop_unlock(_impl->_threadLoop.get());
      }
      else
      {
        PLAYBACK_LOG_ERROR("PipeWireBackend: pw_context_new failed");
      }
    }
    else
    {
      PLAYBACK_LOG_ERROR("PipeWireBackend: pw_thread_loop_new failed");
    }
  }
  PipeWireBackend::~PipeWireBackend() = default;

  bool PipeWireBackend::open(StreamFormat const& format, AudioRenderCallbacks callbacks)
  {
    _impl->_callbacks = callbacks;
    _impl->_format = format;
    _impl->_lastError.clear();

    auto const useExclusiveMode = _exclusiveMode && !_targetDeviceId.empty();
    _impl->_strictFormatRequired = useExclusiveMode;
    _impl->_strictFormatRejected = false;

    if (!_impl->_threadLoop || !_impl->_context || !_impl->_core)
    {
      _impl->setError("PipeWire not initialized or connection failed");
      return false;
    }

    ::pw_thread_loop_lock(_impl->_threadLoop.get());

    // Update monitor with current callbacks (for graph/format updates)
    _impl->_monitor->setCallbacks(callbacks);

    std::string nodeRateStr = "1/" + std::to_string(_impl->_format.sampleRate);
    ::pw_properties* props = ::pw_properties_new(PW_KEY_MEDIA_TYPE,
                                                 "Audio",
                                                 PW_KEY_MEDIA_CATEGORY,
                                                 "Playback",
                                                 PW_KEY_APP_NAME,
                                                 "RockStudio",
                                                 PW_KEY_NODE_NAME,
                                                 "RockStudio Playback",
                                                 PW_KEY_NODE_RATE,
                                                 nodeRateStr.c_str(),
                                                 nullptr);

    if (!_targetDeviceId.empty())
    {
      ::pw_properties_set(props, PW_KEY_TARGET_OBJECT, _targetDeviceId.c_str());
    }
    else if (_exclusiveMode)
    {
      PLAYBACK_LOG_WARN("PipeWireBackend::open: exclusive mode requested without a target device; using default "
                        "routing until a device is selected");
    }

    _impl->_stream.reset(::pw_stream_new_simple(
      ::pw_thread_loop_get_loop(_impl->_threadLoop.get()), "playback", props, &streamEvents, _impl.get()));

    if (!_impl->_stream)
    {
      ::pw_thread_loop_unlock(_impl->_threadLoop.get());
      _impl->setError("Failed to create PipeWire stream");
      return false;
    }

    // Update monitor with active stream
    _impl->_monitor->setStream(_impl->_stream.get());

    auto buffer = std::array<std::uint8_t, 1024>{};
    auto builder = ::spa_pod_builder{};
    ::spa_pod_builder_init(&builder, buffer.data(), buffer.size());
    auto frame = ::spa_pod_frame{};
    // Exclusive mode requests a fixed hardware format; shared mode advertises the
    // formats we can supply and lets PipeWire negotiate the final stream format.
    auto const paramType = useExclusiveMode ? SPA_PARAM_Format : SPA_PARAM_EnumFormat;
    ::spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_Format, paramType);
    auto spaFormat = static_cast<std::int32_t>(SPA_AUDIO_FORMAT_S16_LE);
    if (_impl->_format.bitDepth == 24)
      spaFormat = SPA_AUDIO_FORMAT_S24_LE;
    else if (_impl->_format.bitDepth == 32)
      spaFormat = SPA_AUDIO_FORMAT_S32_LE;

    ::spa_pod_builder_add(&builder,
                          SPA_FORMAT_mediaType,
                          SPA_POD_Id(SPA_MEDIA_TYPE_audio),
                          SPA_FORMAT_mediaSubtype,
                          SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                          SPA_FORMAT_AUDIO_format,
                          SPA_POD_Id(spaFormat),
                          SPA_FORMAT_AUDIO_rate,
                          SPA_POD_Int(_impl->_format.sampleRate),
                          SPA_FORMAT_AUDIO_channels,
                          SPA_POD_Int(_impl->_format.channels),
                          0);
    ::spa_pod_builder_pop(&builder, &frame);
    auto* const param = reinterpret_cast<::spa_pod*>(buffer.data());
    auto params = std::to_array<::spa_pod const*>({param});

    auto flags = static_cast<::pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_AUTOCONNECT);
    if (useExclusiveMode)
    {
      flags = static_cast<::pw_stream_flags>(flags | PW_STREAM_FLAG_EXCLUSIVE | PW_STREAM_FLAG_NO_CONVERT);
    }

    PLAYBACK_LOG_INFO("PipeWireBackend::open: mode={}, target={}, flags={:#x}",
                      useExclusiveMode ? "exclusive" : "shared",
                      _targetDeviceId.empty() ? "<default>" : _targetDeviceId,
                      static_cast<int>(flags));

    auto const ret = ::pw_stream_connect(_impl->_stream.get(),
                                         PW_DIRECTION_OUTPUT,
                                         PW_ID_ANY,
                                         flags,
                                         params.data(),
                                         static_cast<std::uint32_t>(params.size()));
    ::pw_thread_loop_unlock(_impl->_threadLoop.get());

    if (ret < 0)
    {
      PLAYBACK_LOG_ERROR("PipeWireBackend::open: pw_stream_connect failed: ret={}", ret);
      _impl->setError("Failed to connect PipeWire stream: " + std::to_string(-ret));
      return false;
    }

    PLAYBACK_LOG_INFO("PipeWireBackend::open: pw_stream_connect succeeded");
    return true;
  }

  void PipeWireBackend::start()
  {
    if (!_impl || !_impl->_stream || !_impl->_threadLoop) return;
    _impl->_drainPending = false;
    ::pw_thread_loop_lock(_impl->_threadLoop.get());
    PLAYBACK_LOG_INFO("PipeWireBackend::start: activating stream");
    ::pw_stream_set_active(_impl->_stream.get(), true);
    ::pw_thread_loop_unlock(_impl->_threadLoop.get());
  }
  void PipeWireBackend::pause()
  {
    if (!_impl || !_impl->_stream || !_impl->_threadLoop) return;
    ::pw_thread_loop_lock(_impl->_threadLoop.get());
    ::pw_stream_set_active(_impl->_stream.get(), false);
    ::pw_thread_loop_unlock(_impl->_threadLoop.get());
  }
  void PipeWireBackend::resume()
  {
    if (!_impl || !_impl->_stream || !_impl->_threadLoop) return;
    ::pw_thread_loop_lock(_impl->_threadLoop.get());
    ::pw_stream_set_active(_impl->_stream.get(), true);
    ::pw_thread_loop_unlock(_impl->_threadLoop.get());
  }
  void PipeWireBackend::flush()
  {
    if (!_impl || !_impl->_stream || !_impl->_threadLoop) return;
    _impl->_drainPending = false;
    ::pw_thread_loop_lock(_impl->_threadLoop.get());
    ::pw_stream_flush(_impl->_stream.get(), false);
    ::pw_thread_loop_unlock(_impl->_threadLoop.get());
  }
  void PipeWireBackend::drain()
  {
    if (!_impl || !_impl->_stream || !_impl->_threadLoop || _impl->_drainPending) return;
    _impl->_drainPending = true;
    ::pw_thread_loop_lock(_impl->_threadLoop.get());
    ::pw_stream_flush(_impl->_stream.get(), true);
    ::pw_thread_loop_unlock(_impl->_threadLoop.get());
  }
  void PipeWireBackend::stop()
  {
    if (!_impl || !_impl->_stream || !_impl->_threadLoop) return;
    _impl->_drainPending = false;
    ::pw_thread_loop_lock(_impl->_threadLoop.get());
    ::pw_stream_set_active(_impl->_stream.get(), false);
    ::pw_thread_loop_unlock(_impl->_threadLoop.get());
  }
  void PipeWireBackend::close()
  {
    if (_impl) _impl->destroyStream();
  }

  std::vector<app::core::playback::AudioDevice> PipeWireBackend::enumerateDevices()
  {
    if (!_impl || !_impl->_monitor)
    {
      PLAYBACK_LOG_DEBUG("PipeWireBackend::enumerateDevices: _impl or _monitor is null");
      return {};
    }

    auto devices = _impl->_monitor->enumerateSinks();
    PLAYBACK_LOG_DEBUG("PipeWireBackend::enumerateDevices: got {} sinks from monitor", devices.size());

    // Add virtual "System Default" entry
    devices.insert(devices.begin(), {.id = "", .displayName = "System Default", .isDefault = true});

    return devices;
  }

  void PipeWireBackend::setDevice(std::string_view deviceId)
  {
    if (_targetDeviceId == deviceId) return;

    _targetDeviceId = deviceId;

    // If we are currently open, we need to restart the stream to apply the target
    if (_impl && _impl->_stream)
    {
      auto const currentFormat = _impl->_format;
      auto const currentCallbacks = _impl->_callbacks;
      open(currentFormat, currentCallbacks);
    }
  }

  std::string_view PipeWireBackend::currentDeviceId() const noexcept
  {
    return _targetDeviceId;
  }

  void PipeWireBackend::setExclusiveMode(bool exclusive)
  {
    if (_exclusiveMode == exclusive) return;
    _exclusiveMode = exclusive;

    // Only reopen if we already have a device set. If _targetDeviceId is empty
    // (e.g. exclusive mode requested before device selection), defer to the
    // upcoming setDevice() or setBackendAndDevice() call which carries the real device.
    if (_impl && _impl->_stream && !_targetDeviceId.empty())
    {
      auto const currentFormat = _impl->_format;
      auto const currentCallbacks = _impl->_callbacks;
      open(currentFormat, currentCallbacks);
    }
  }

  bool PipeWireBackend::isExclusiveMode() const noexcept
  {
    return _exclusiveMode;
  }

  app::core::playback::BackendKind PipeWireBackend::kind() const noexcept
  {
    return _exclusiveMode ? app::core::playback::BackendKind::PipeWireExclusive
                          : app::core::playback::BackendKind::PipeWire;
  }

  std::string_view PipeWireBackend::lastError() const noexcept
  {
    if (_impl) return _impl->_lastError;
    return "";
  }

} // namespace app::playback
