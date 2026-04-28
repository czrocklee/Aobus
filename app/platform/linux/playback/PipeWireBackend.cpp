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
    return state == PW_LINK_STATE_PAUSED || state == PW_LINK_STATE_ACTIVE;
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

  std::optional<app::core::AudioFormat> parseRawStreamFormat(::spa_pod const* param)
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

    auto format = app::core::AudioFormat{};
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

  void ensurePipeWireInit()
  {
    struct PwInitGuard
    {
      PwInitGuard() { ::pw_init(nullptr, nullptr); }
      ~PwInitGuard() { ::pw_deinit(); }
    };
    static PwInitGuard guard;
  }

} // namespace

namespace app::playback
{
  using namespace app::core::backend;

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

    void setNegotiatedFormat(std::optional<app::core::AudioFormat> format)
    {
      auto const lock = std::lock_guard<std::mutex>{_mutex};
      _negotiatedStreamFormat = format;
    }

    void setDevicesChangedCallback(IDeviceDiscovery::OnDevicesChangedCallback callback)
    {
      auto const lock = std::lock_guard<std::mutex>{_mutex};
      _onDevicesChanged = std::move(callback);
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
    std::vector<AudioDevice> enumerateSinks() const
    {
      auto devices = std::vector<AudioDevice>{};
      auto const lock = std::lock_guard<std::mutex>{_mutex};

      PLAYBACK_LOG_TRACE("PipeWireMonitor::enumerateSinks: tracking {} nodes", _nodes.size());

      for (auto const& [id, node] : _nodes)
      {
        if (isSinkMediaClass(node.mediaClass))
        {
          auto const deviceId = node.objectSerial ? std::format("{}", *node.objectSerial) : std::format("{}", id);
          auto const displayName =
            (node.nodeNick.empty() ? (node.nodeName.empty() ? node.objectPath : node.nodeName) : node.nodeNick);
          auto const description = (node.nodeNick.empty() ? "" : node.nodeName);

          // Shared variant
          devices.push_back({.id = deviceId,
                             .displayName = displayName,
                             .description = description,
                             .isDefault = false,
                             .backendKind = BackendKind::PipeWire});

          // Exclusive variant
          devices.push_back({.id = deviceId,
                             .displayName = std::format("{} (Exclusive)", displayName),
                             .description = description,
                             .isDefault = false,
                             .backendKind = BackendKind::PipeWireExclusive});
        }
      }
      return devices;
    }

    std::string getTargetNameForDevice(std::string_view deviceId) const
    {
      auto const lock = std::lock_guard<std::mutex>{_mutex};
      for (auto const& [id, node] : _nodes)
      {
        if (isSinkMediaClass(node.mediaClass))
        {
          auto const currentDeviceId =
            node.objectSerial ? std::format("{}", *node.objectSerial) : std::format("{}", id);
          if (currentDeviceId == deviceId)
          {
            return node.nodeName.empty() ? std::format("{}", id) : node.nodeName;
          }
        }
      }
      return std::string{deviceId};
    }

  private:
    ::pw_thread_loop* _loop;
    ::pw_core* _core;
    ::pw_stream* _stream = nullptr;
    AudioRenderCallbacks _callbacks;
    IDeviceDiscovery::OnDevicesChangedCallback _onDevicesChanged;

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
    std::optional<app::core::AudioFormat> _negotiatedStreamFormat = {};
    std::optional<app::core::AudioFormat> _sinkFormat = {};
    SinkProps _sinkProps = {};
    SpaSourcePtr _refreshEvent;
  };

  namespace
  {
    class PipeWireDiscovery final : public IDeviceDiscovery
    {
    public:
      PipeWireDiscovery()
      {
        ensurePipeWireInit();

        _threadLoop.reset(::pw_thread_loop_new("PipeWireDiscovery", nullptr));
        if (!_threadLoop) return;

        _context.reset(::pw_context_new(::pw_thread_loop_get_loop(_threadLoop.get()), nullptr, 0));
        if (!_context) return;

        if (::pw_thread_loop_start(_threadLoop.get()) < 0) return;

        ::pw_thread_loop_lock(_threadLoop.get());
        _core.reset(::pw_context_connect(_context.get(), nullptr, 0));
        if (_core)
        {
          _monitor = std::make_unique<PipeWireMonitor>(_threadLoop.get(), _core.get(), AudioRenderCallbacks{});
          _monitor->start();
        }
        ::pw_thread_loop_unlock(_threadLoop.get());
      }

      ~PipeWireDiscovery() override
      {
        if (_threadLoop)
        {
          ::pw_thread_loop_stop(_threadLoop.get());
        }

        _monitor.reset();
        _core.reset();
        _context.reset();
        _threadLoop.reset();
      }

      void setDevicesChangedCallback(OnDevicesChangedCallback callback) override
      {
        if (_monitor)
        {
          _monitor->setDevicesChangedCallback(std::move(callback));
        }
      }

      std::vector<AudioDevice> enumerateDevices() override
      {
        if (!_monitor) return {};
        auto devices = _monitor->enumerateSinks();
        // Add virtual "System Default" entry
        devices.insert(devices.begin(),
                       {.id = "",
                        .displayName = "System Default",
                        .description = "PipeWire",
                        .isDefault = true,
                        .backendKind = BackendKind::PipeWire});
        return devices;
      }

      std::unique_ptr<IAudioBackend> createBackend(AudioDevice const& device) override
      {
        return std::make_unique<PipeWireBackend>(device);
      }

    private:
      PwThreadLoopPtr _threadLoop;
      PwContextPtr _context;
      PwCorePtr _core;
      std::unique_ptr<PipeWireMonitor> _monitor;
    };
  } // namespace

  std::unique_ptr<IDeviceDiscovery> PipeWireBackend::createDiscovery()
  {
    return std::make_unique<PipeWireDiscovery>();
  }

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
      if (isNode)
      {
        _nodes[id] = parseNodeRecord(version, props);
      }
      else if (isLink)
      {
        auto* const proxy =
          static_cast<::pw_link*>(::pw_registry_bind(_registry.get(), id, PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, 0));

        if (proxy)
        {
          auto& binding = _linkBindings[id];
          binding.id = id;
          binding.proxy.reset(proxy);
          ::pw_link_add_listener(binding.proxy.get(), binding.listener.get(), &linkEvents, this);
        }
      }
    }
    triggerRefresh();

    if (isNode)
    {
      auto const lock = std::lock_guard<std::mutex>{_mutex};
      if (isSinkMediaClass(_nodes[id].mediaClass))
      {
        if (_onDevicesChanged) _onDevicesChanged();
      }
    }
  }

  void PipeWireMonitor::handleRegistryGlobalRemove(std::uint32_t id)
  {
    bool needsRefresh = false;
    bool deviceRemoved = false;
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
        needsRefresh = true;
      }

      if (_streamNodeBinding.id == id)
      {
        _streamNodeBinding.reset();
        needsRefresh = true;
      }

      if (_nodes.contains(id))
      {
        if (isSinkMediaClass(_nodes[id].mediaClass))
        {
          deviceRemoved = true;
        }
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
    if (deviceRemoved && _onDevicesChanged)
    {
      _onDevicesChanged();
    }
  }

  void PipeWireMonitor::handleLinkInfo(::pw_link_info const* info)
  {
    if (info == nullptr) return;

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
    if (info == nullptr) return;

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
    if (!_loop) return;

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

        if (_registry && desiredSinkNodeId != PW_ID_ANY)
        {
          auto const nodeIt = _nodes.find(desiredSinkNodeId);
          if (nodeIt != _nodes.end())
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

              std::uint32_t params[] = {SPA_PARAM_Format, SPA_PARAM_Props};
              ::pw_node_subscribe_params(_sinkNodeBinding.proxy.get(), params, std::size(params));
              ::pw_node_enum_params(_sinkNodeBinding.proxy.get(), 1, SPA_PARAM_Format, 0, -1, nullptr);
              ::pw_node_enum_params(_sinkNodeBinding.proxy.get(), 2, SPA_PARAM_Props, 0, -1, nullptr);
              ::pw_node_add_listener(
                _sinkNodeBinding.proxy.get(), _sinkNodeBinding.listener.get(), &sinkNodeEvents, this);
            }
          }
        }
      }

      if (_callbacks.onGraphChanged)
      {
        auto graph = AudioGraph{};

        // Identify all nodes that are relevant:
        // 1. RockStudio path nodes (reachableSet)
        // 2. Any node that has an active link INTO a RockStudio path node
        auto fullSet = reachableSet;
        for (auto const& [_, link] : _links)
        {
          if (isActiveLink(link.state) && reachableSet.contains(link.inputNodeId))
          {
            fullSet.insert(link.outputNodeId);
          }
        }

        for (auto const nodeId : fullSet)
        {
          auto const it = _nodes.find(nodeId);
          if (it == _nodes.end()) continue;

          auto const& record = it->second;
          bool isSink = isSinkMediaClass(record.mediaClass);
          bool isRsStream = (nodeId == _streamNodeId);

          auto nodeType = AudioNodeType::Intermediary;
          if (isRsStream)
            nodeType = AudioNodeType::Stream;
          else if (isSink)
            nodeType = AudioNodeType::Sink;
          else if (!reachableSet.contains(nodeId))
            nodeType = AudioNodeType::ExternalSource;

          auto node =
            AudioNode{.id = std::format("{}", nodeId),
                      .type = nodeType,
                      .name = (record.nodeNick.empty() ? (record.nodeName.empty() ? record.objectPath : record.nodeName)
                                                       : record.nodeNick),
                      .objectPath = record.objectPath};

          if (isRsStream)
          {
            node.format = _negotiatedStreamFormat;
          }
          else if (isSink && nodeId == desiredSinkNodeId)
          {
            node.format = _sinkFormat;

            auto const isUnity = [](float v) { return std::abs(v - 1.0F) < 1e-4F; };
            bool const volumeAtUnity = (!_sinkProps.volume.has_value() || isUnity(*_sinkProps.volume)) &&
                                       std::ranges::all_of(_sinkProps.channelVolumes, isUnity) &&
                                       std::ranges::all_of(_sinkProps.softVolumes, isUnity);

            node.volumeNotUnity = !volumeAtUnity;
            node.isMuted = (_sinkProps.mute.has_value() && *_sinkProps.mute) ||
                           (_sinkProps.softMute.has_value() && *_sinkProps.softMute);
          }

          graph.nodes.push_back(std::move(node));
        }

        for (auto const& [_, link] : _links)
        {
          if (!isActiveLink(link.state)) continue;
          if (fullSet.contains(link.outputNodeId) && fullSet.contains(link.inputNodeId))
          {
            graph.links.push_back(
              {.sourceId = std::format("{}", link.outputNodeId), .destId = std::format("{}", link.inputNodeId)});
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
    Impl() { ensurePipeWireInit(); }

    ~Impl()
    {
      if (_threadLoop)
      {
        ::pw_thread_loop_stop(_threadLoop.get());
      }

      // 1. First remove the listener from the stream
      _streamListener.reset();

      // 2. Then it's safe to destroy the stream
      _stream.reset();

      // 3. Stop and destroy the monitor
      if (_monitor)
      {
        _monitor->stop();
      }
      _monitor.reset();

      // 4. Finally release core context and loop
      _core.reset();
      _context.reset();
      _threadLoop.reset();
    }

    void destroyStream()
    {
      if (_threadLoop) ::pw_thread_loop_lock(_threadLoop.get());
      _streamListener.reset();
      _stream.reset();
      if (_threadLoop) ::pw_thread_loop_unlock(_threadLoop.get());
    }

    void setError(std::string message)
    {
      PLAYBACK_LOG_ERROR("PipeWire error: {}", message);
      _lastError = std::move(message);
    }

    // Event Handlers
    void handleStreamProcess();
    void handleStreamParamChanged(std::uint32_t id, ::spa_pod const* param);
    void handleStreamStateChanged(enum pw_stream_state oldState,
                                  enum pw_stream_state newState,
                                  char const* errorMessage);
    void handleStreamDrained();

    // Static callbacks
    static void onStreamProcess(void* data) { static_cast<Impl*>(data)->handleStreamProcess(); }
    static void onStreamParamChanged(void* data, std::uint32_t id, ::spa_pod const* param)
    {
      static_cast<Impl*>(data)->handleStreamParamChanged(id, param);
    }
    static void onStreamStateChanged(void* data,
                                     enum pw_stream_state oldState,
                                     enum pw_stream_state newState,
                                     char const* errorMessage)
    {
      static_cast<Impl*>(data)->handleStreamStateChanged(oldState, newState, errorMessage);
    }
    static void onStreamDrained(void* data) { static_cast<Impl*>(data)->handleStreamDrained(); }

    // Members
    AudioRenderCallbacks _callbacks;
    app::core::AudioFormat _format;
    std::atomic<bool> _drainPending = false;
    std::string _lastError;
    bool _strictFormatRequired = false;
    bool _strictFormatRejected = false;

    PwThreadLoopPtr _threadLoop;
    PwContextPtr _context;
    PwCorePtr _core;
    PwStreamPtr _stream;
    SpaHookGuard _streamListener;
    std::unique_ptr<PipeWireMonitor> _monitor;
  };

  namespace
  {
    ::pw_stream_events const streamEvents = []
    {
      auto e = ::pw_stream_events{};
      e.version = PW_VERSION_STREAM_EVENTS;
      e.process = PipeWireBackend::Impl::onStreamProcess;
      e.param_changed = PipeWireBackend::Impl::onStreamParamChanged;
      e.state_changed = PipeWireBackend::Impl::onStreamStateChanged;
      e.drained = PipeWireBackend::Impl::onStreamDrained;
      return e;
    }();
  }

  void PipeWireBackend::Impl::handleStreamProcess()
  {
    auto* buffer = ::pw_stream_dequeue_buffer(_stream.get());
    if (buffer == nullptr) return;

    auto* data = buffer->buffer->datas[0].data;
    if (data == nullptr)
    {
      ::pw_stream_queue_buffer(_stream.get(), buffer);
      return;
    }

    auto const max_size = buffer->buffer->datas[0].maxsize;
    auto stride = buffer->buffer->datas[0].chunk->stride;

    if (stride == 0 && _format.channels > 0 && _format.bitDepth > 0)
    {
      stride = _format.channels * (_format.bitDepth / 8);
    }

    if (stride == 0)
    {
      PLAYBACK_LOG_TRACE("PipeWire: Skipping process cycle, stride not yet available");
      ::pw_stream_queue_buffer(_stream.get(), buffer);
      return;
    }

    auto const bytesRead = _callbacks.readPcm(_callbacks.userData, {static_cast<std::byte*>(data), max_size});

    static int logThrottle = 0;
    if (bytesRead > 0 && ++logThrottle % 100 == 0)
    {
      PLAYBACK_LOG_TRACE("PipeWire: Pushing {} bytes to stream", bytesRead);
    }

    if (bytesRead > 0)
    {
      buffer->buffer->datas[0].chunk->offset = 0;
      buffer->buffer->datas[0].chunk->size = bytesRead;
      buffer->buffer->datas[0].chunk->stride = stride;
      ::pw_stream_queue_buffer(_stream.get(), buffer);

      auto const framesRead = bytesRead / stride;
      _callbacks.onPositionAdvanced(_callbacks.userData, static_cast<std::uint32_t>(framesRead));
    }
    else
    {
      // No data read, maybe EOF
      buffer->buffer->datas[0].chunk->offset = 0;
      buffer->buffer->datas[0].chunk->size = 0;
      ::pw_stream_queue_buffer(_stream.get(), buffer);

      if (_callbacks.isSourceDrained(_callbacks.userData))
      {
        PLAYBACK_LOG_DEBUG("PipeWire: Source drained, flushing stream");
        ::pw_stream_flush(_stream.get(), true);
      }
    }
  }

  void PipeWireBackend::Impl::handleStreamParamChanged(std::uint32_t id, ::spa_pod const* param)
  {
    if (param == nullptr || id != SPA_PARAM_Format) return;

    auto const negotiated = parseRawStreamFormat(param);
    if (negotiated)
    {
      PLAYBACK_LOG_INFO("PipeWire stream format negotiated: {}", formatStreamFormat(*negotiated));
      _format = *negotiated; // Update internal format with negotiated values
      if (_monitor) _monitor->setNegotiatedFormat(negotiated);
    }
  }

  void PipeWireBackend::Impl::handleStreamStateChanged(enum pw_stream_state /*oldState*/,
                                                       enum pw_stream_state newState,
                                                       char const* errorMessage)
  {
    if (newState == PW_STREAM_STATE_ERROR)
    {
      setError(errorMessage ? errorMessage : "Unknown PipeWire stream error");
      if (_callbacks.onBackendError)
      {
        _callbacks.onBackendError(_callbacks.userData, _lastError);
      }
    }
  }

  void PipeWireBackend::Impl::handleStreamDrained()
  {
    _drainPending = false;
    if (_callbacks.onDrainComplete)
    {
      _callbacks.onDrainComplete(_callbacks.userData);
    }
  }

  PipeWireBackend::PipeWireBackend(AudioDevice const& device)
    : _impl{std::make_unique<Impl>()}
    , _targetDeviceId{device.id}
    , _exclusiveMode{device.backendKind == BackendKind::PipeWireExclusive}
  {
    PLAYBACK_LOG_DEBUG(
      "PipeWireBackend: Creating backend instance for device '{}' (exclusive={})", _targetDeviceId, _exclusiveMode);

    _impl->_threadLoop.reset(::pw_thread_loop_new("PipeWireBackend", nullptr));
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
        }
        ::pw_thread_loop_unlock(_impl->_threadLoop.get());
      }
    }
  }

  PipeWireBackend::~PipeWireBackend()
  {
    PLAYBACK_LOG_DEBUG("PipeWireBackend: Destroying backend instance");
  }

  bool PipeWireBackend::open(app::core::AudioFormat const& format, AudioRenderCallbacks callbacks)
  {
    PLAYBACK_LOG_INFO("PipeWireBackend: Opening stream with format {}Hz/{}b/{}ch",
                      format.sampleRate,
                      (int)format.bitDepth,
                      (int)format.channels);

    _impl->_callbacks = callbacks;
    _impl->_format = format;
    _impl->_lastError.clear();

    bool const useExclusiveMode = _exclusiveMode && !_targetDeviceId.empty();
    _impl->_strictFormatRequired = useExclusiveMode;
    _impl->_strictFormatRejected = false;

    if (!_impl->_threadLoop || !_impl->_context || !_impl->_core)
    {
      _impl->setError("PipeWire not initialized or connection failed");
      return false;
    }

    ::pw_thread_loop_lock(_impl->_threadLoop.get());
    _impl->_monitor->setCallbacks(callbacks);

    std::string nodeRateStr = std::format("1/{}", _impl->_format.sampleRate);
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
      // The targetDeviceId we store is the objectSerial (as a string)
      ::pw_properties_set(props, PW_KEY_TARGET_OBJECT, _targetDeviceId.c_str());

      if (useExclusiveMode)
      {
        ::pw_properties_set(props, PW_KEY_NODE_EXCLUSIVE, "true");
        // Passive ensures we don't move if the target is busy; we want an error instead of a fallback
        //::pw_properties_set(props, PW_KEY_NODE_PASSIVE, "true");
      }
    }

    _impl->_stream.reset(::pw_stream_new(_impl->_core.get(), "RockStudio Playback", props));
    if (!_impl->_stream)
    {
      _impl->setError("Failed to create PipeWire stream");
      ::pw_thread_loop_unlock(_impl->_threadLoop.get());
      return false;
    }

    _impl->_streamListener.reset();
    ::pw_stream_add_listener(_impl->_stream.get(), _impl->_streamListener.get(), &streamEvents, _impl.get());
    _impl->_monitor->setStream(_impl->_stream.get());

    auto const alsaFormat = (format.bitDepth == 16)   ? SPA_AUDIO_FORMAT_S16_LE
                            : (format.bitDepth == 24) ? SPA_AUDIO_FORMAT_S24_32_LE
                                                      : SPA_AUDIO_FORMAT_S32_LE;

    std::uint8_t buffer[1024];
    ::spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    // Build the format object precisely as before
    ::spa_pod_frame f;
    ::spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    ::spa_pod_builder_add(&b,
                          SPA_FORMAT_mediaType,
                          SPA_POD_Id(SPA_MEDIA_TYPE_audio),
                          SPA_FORMAT_mediaSubtype,
                          SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                          SPA_FORMAT_AUDIO_format,
                          SPA_POD_Id(alsaFormat),
                          SPA_FORMAT_AUDIO_rate,
                          SPA_POD_Int(format.sampleRate),
                          SPA_FORMAT_AUDIO_channels,
                          SPA_POD_Int(format.channels),
                          0);
    ::spa_pod const* param = static_cast<::spa_pod*>(::spa_pod_builder_pop(&b, &f));
    ::spa_pod const* params[] = {param};

    auto flags = static_cast<::pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS);
    if (useExclusiveMode)
    {
      flags = static_cast<::pw_stream_flags>(flags | PW_STREAM_FLAG_EXCLUSIVE | PW_STREAM_FLAG_NO_CONVERT);
    }

    if (::pw_stream_connect(_impl->_stream.get(), PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1) < 0)
    {
      _impl->setError("Failed to connect PipeWire stream");
      ::pw_thread_loop_unlock(_impl->_threadLoop.get());
      return false;
    }

    ::pw_thread_loop_unlock(_impl->_threadLoop.get());
    return true;
  }

  void PipeWireBackend::start()
  {
    if (!_impl || !_impl->_stream || !_impl->_threadLoop) return;
    PLAYBACK_LOG_DEBUG("PipeWireBackend: Starting playback");
    ::pw_thread_loop_lock(_impl->_threadLoop.get());
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
    start();
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
    PLAYBACK_LOG_DEBUG("PipeWireBackend: Stopping playback");
    _impl->_drainPending = false;
    ::pw_thread_loop_lock(_impl->_threadLoop.get());
    ::pw_stream_set_active(_impl->_stream.get(), false);
    ::pw_thread_loop_unlock(_impl->_threadLoop.get());
  }

  void PipeWireBackend::close()
  {
    PLAYBACK_LOG_DEBUG("PipeWireBackend: Closing stream");
    if (_impl) _impl->destroyStream();
  }

  void PipeWireBackend::setExclusiveMode(bool exclusive)
  {
    if (_exclusiveMode == exclusive) return;
    _exclusiveMode = exclusive;
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

  BackendKind PipeWireBackend::kind() const noexcept
  {
    return _exclusiveMode ? BackendKind::PipeWireExclusive : BackendKind::PipeWire;
  }

  std::string_view PipeWireBackend::lastError() const noexcept
  {
    if (_impl) return _impl->_lastError;
    return "";
  }

} // namespace app::playback
