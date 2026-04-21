// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "PipeWireBackend.h"

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
#include <spa/utils/defs.h>
#include <spa/utils/dict.h>
#include <spa/utils/type.h>
}

#include <algorithm>
#include <array>
#include <charconv>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
  using SinkStatus = app::playback::BackendFormatInfo::SinkStatus;

  constexpr float kUnityThreshold = 0.999F;

  struct NodeRecord final
  {
    std::uint32_t version = 0;
    std::string mediaClass;
    std::string nodeName;
    std::string nodeNick;
    std::string nodeDescription;
    std::string objectPath;
    std::optional<std::uint32_t> driverId;
  };

  struct LinkRecord final
  {
    std::uint32_t outputNodeId = PW_ID_ANY;
    std::uint32_t inputNodeId = PW_ID_ANY;
    pw_link_state state = PW_LINK_STATE_INIT;
  };

  struct SinkProps final
  {
    std::optional<float> volume;
    std::optional<bool> mute;
    std::vector<float> channelVolumes;
    std::optional<bool> softMute;
    std::vector<float> softVolumes;
  };

  bool sameStreamFormat(app::playback::StreamFormat const& lhs, app::playback::StreamFormat const& rhs) noexcept
  {
    return lhs.sampleRate == rhs.sampleRate && lhs.channels == rhs.channels && lhs.bitDepth == rhs.bitDepth &&
           lhs.isFloat == rhs.isFloat && lhs.isInterleaved == rhs.isInterleaved;
  }

  // Lossless format change:
  // - Sample rate, channel count, and memory layout must match exactly
  // - Bit-depth expansion (int->int or int->float) is lossless; reduction is lossy
  // - Int->float is lossless only when float has enough mantissa bits (F32 has 24 bits)
  // - Float->int is always lossy (requires requantization and typically dither)
  bool isLosslessFormatChange(app::playback::StreamFormat const& src, app::playback::StreamFormat const& dst) noexcept
  {
    if (src.sampleRate != dst.sampleRate || src.channels != dst.channels || src.isInterleaved != dst.isInterleaved)
    {
      return false;
    }

    if (src.isFloat == dst.isFloat)
    {
      return src.bitDepth <= dst.bitDepth;
    }

    if (!src.isFloat && dst.isFloat)
    {
      if (dst.bitDepth == 32)
      {
        return src.bitDepth <= 24;
      }
      if (dst.bitDepth == 64)
      {
        return src.bitDepth <= 32;
      }
      return false;
    }

    return false;
  }

  void appendLine(std::string& text, std::string_view line)
  {
    if (line.empty())
    {
      return;
    }

    if (!text.empty())
    {
      text += '\n';
    }

    text += line;
  }

  bool isSinkMediaClass(std::string const& mediaClass)
  {
    return mediaClass == "Audio/Sink" || mediaClass.ends_with("/Sink");
  }

  bool isActiveLink(pw_link_state state) noexcept
  {
    // Consider any link that isn't errored or unlinked as part of the graph path
    return static_cast<int>(state) >= PW_LINK_STATE_INIT;
  }

  std::optional<std::uint32_t> parseUintProperty(char const* value)
  {
    if (value == nullptr || *value == '\0')
    {
      return std::nullopt;
    }

    char* end = nullptr;
    auto const parsed = std::strtoul(value, &end, 10);
    if (end == value)
    {
      return std::nullopt;
    }

    return static_cast<std::uint32_t>(parsed);
  }

  std::string lookupProperty(spa_dict const* props, char const* key)
  {
    auto const* value = props ? spa_dict_lookup(props, key) : nullptr;
    return value ? std::string(value) : std::string{};
  }

  NodeRecord parseNodeRecord(std::uint32_t version, spa_dict const* props)
  {
    NodeRecord record;
    record.version = version;
    record.mediaClass = lookupProperty(props, PW_KEY_MEDIA_CLASS);
    record.nodeName = lookupProperty(props, PW_KEY_NODE_NAME);
    record.nodeNick = lookupProperty(props, PW_KEY_NODE_NICK);
    record.nodeDescription = lookupProperty(props, PW_KEY_NODE_DESCRIPTION);
    record.objectPath = lookupProperty(props, PW_KEY_OBJECT_PATH);
    
    // Try multiple possible keys for driver ID
    if (auto const id = parseUintProperty(spa_dict_lookup(props, "node.driver-id"))) record.driverId = id;
    else if (auto const id = parseUintProperty(spa_dict_lookup(props, "node.driver"))) record.driverId = id;
    
    return record;
  }

  std::string nodeDisplayName(NodeRecord const& record)
  {
    if (!record.nodeNick.empty())
    {
      return record.nodeNick;
    }
    if (!record.nodeDescription.empty())
    {
      return record.nodeDescription;
    }
    if (!record.nodeName.empty())
    {
      return record.nodeName;
    }
    if (!record.objectPath.empty())
    {
      return record.objectPath;
    }
    return "Audio sink";
  }

  std::string formatText(app::playback::StreamFormat const& format)
  {
    std::ostringstream stream;
    stream << (format.sampleRate / 1000.0) << " kHz / " << static_cast<int>(format.bitDepth)
           << " bit / ";
    if (format.channels == 1)
    {
      stream << "Mono";
    }
    else if (format.channels == 2)
    {
      stream << "Stereo";
    }
    else
    {
      stream << static_cast<int>(format.channels) << " ch";
    }
    if (format.isFloat)
    {
      stream << " / float";
    }
    return stream.str();
  }

  std::optional<app::playback::StreamFormat> parseRawStreamFormat(spa_pod const* param)
  {
    if (param == nullptr)
    {
      return std::nullopt;
    }

    auto info = spa_audio_info_raw{};
    if (spa_format_audio_raw_parse(param, &info) < 0)
    {
      return std::nullopt;
    }

    app::playback::StreamFormat format;
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

  bool copyFloatArray(spa_pod const& pod, std::vector<float>& output)
  {
    std::array<float, 16> values{};
    auto const count = spa_pod_copy_array(&pod, SPA_TYPE_Float, values.data(), values.size());
    if (count == 0)
    {
      return false;
    }

    output.assign(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(count));
    return true;
  }

  void mergeSinkProps(SinkProps& sinkProps, spa_pod const* param)
  {
    if (param == nullptr)
    {
      return;
    }

    if (auto const* volumeProp = spa_pod_find_prop(param, nullptr, SPA_PROP_volume))
    {
      float value = 0.0F;
      if (spa_pod_get_float(&volumeProp->value, &value) == 0)
      {
        sinkProps.volume = value;
      }
    }

    if (auto const* muteProp = spa_pod_find_prop(param, nullptr, SPA_PROP_mute))
    {
      bool value = false;
      if (spa_pod_get_bool(&muteProp->value, &value) == 0)
      {
        sinkProps.mute = value;
      }
    }

    if (auto const* channelVolumesProp = spa_pod_find_prop(param, nullptr, SPA_PROP_channelVolumes))
    {
      copyFloatArray(channelVolumesProp->value, sinkProps.channelVolumes);
    }

    if (auto const* softMuteProp = spa_pod_find_prop(param, nullptr, SPA_PROP_softMute))
    {
      bool value = false;
      if (spa_pod_get_bool(&softMuteProp->value, &value) == 0)
      {
        sinkProps.softMute = value;
      }
    }

    if (auto const* softVolumesProp = spa_pod_find_prop(param, nullptr, SPA_PROP_softVolumes))
    {
      copyFloatArray(softVolumesProp->value, sinkProps.softVolumes);
    }
  }

  bool anyVolumeNotUnity(std::vector<float> const& volumes)
  {
    return std::any_of(volumes.begin(), volumes.end(), [](float value) { return value < kUnityThreshold; });
  }
} // namespace

namespace app::playback
{

  struct PipeWireBackend::RegistryMonitorState final
  {
    struct LinkBinding final
    {
      std::uint32_t id = PW_ID_ANY;
      pw_link* proxy = nullptr;
      spa_hook listener = {};
    };

    struct NodeBinding final
    {
      std::uint32_t id = PW_ID_ANY;
      pw_node* proxy = nullptr;
      spa_hook listener = {};
    };

    pw_registry* registry = nullptr;
    spa_hook registryListener = {};
    spa_hook coreListener = {};
    int coreSyncSeq = -1;
    std::uint32_t streamNodeId = PW_ID_ANY;
    std::unordered_map<std::uint32_t, NodeRecord> nodes;
    std::unordered_map<std::uint32_t, LinkRecord> links;
    std::unordered_map<std::uint32_t, LinkBinding> linkBindings;
    NodeBinding sinkNodeBinding;
    NodeBinding streamNodeBinding;
    std::optional<StreamFormat> negotiatedStreamFormat;
    std::optional<StreamFormat> sinkFormat;
    SinkProps sinkProps;
  };

  static void onStreamProcess(void* data)
  {
    auto* self = static_cast<PipeWireBackend*>(data);
    self->process();
  }

  static void onStreamParamChanged(void* data, std::uint32_t id, spa_pod const* param)
  {
    auto* self = static_cast<PipeWireBackend*>(data);
    self->handleStreamParamChanged(id, param);
  }

  static void onStreamStateChanged(void* data,
                                   pw_stream_state oldState,
                                   pw_stream_state newState,
                                   char const* errorMessage)
  {
    auto* self = static_cast<PipeWireBackend*>(data);
    self->handleStreamStateChanged(static_cast<int>(oldState), static_cast<int>(newState), errorMessage);
  }

  static void onStreamDrained(void* data)
  {
    auto* self = static_cast<PipeWireBackend*>(data);
    self->handleDrained();
  }

  static void onRegistryGlobal(void* data,
                               std::uint32_t id,
                               [[maybe_unused]] std::uint32_t permissions,
                               char const* type,
                               std::uint32_t version,
                               spa_dict const* props)
  {
    auto* self = static_cast<PipeWireBackend*>(data);
    self->handleRegistryGlobal(id, type, version, props);
  }

  static void onRegistryGlobalRemove(void* data, std::uint32_t id)
  {
    auto* self = static_cast<PipeWireBackend*>(data);
    self->handleRegistryGlobalRemove(id);
  }

  static void onLinkInfo(void* data, pw_link_info const* info)
  {
    auto* self = static_cast<PipeWireBackend*>(data);
    self->handleLinkInfo(info);
  }

  static void onNodeInfo(void* data, pw_node_info const* info)
  {
    auto* self = static_cast<PipeWireBackend*>(data);
    self->handleSinkNodeInfo(info);
  }

  static void onSinkNodeParam([[maybe_unused]] void* data,
                              [[maybe_unused]] int seq,
                              std::uint32_t id,
                              [[maybe_unused]] std::uint32_t index,
                              [[maybe_unused]] std::uint32_t next,
                              spa_pod const* param)
  {
    auto* self = static_cast<PipeWireBackend*>(data);
    self->handleSinkNodeParam(id, param);
  }

  static void onCoreDone(void* data, std::uint32_t id, int seq)
  {
    auto* self = static_cast<PipeWireBackend*>(data);
    self->handleCoreDone(id, seq);
  }

  static const pw_core_events coreEvents = {
    .version = PW_VERSION_CORE_EVENTS,
    .done = onCoreDone,
  };

  static const pw_stream_events streamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = onStreamStateChanged,
    .param_changed = onStreamParamChanged,
    .process = onStreamProcess,
    .drained = onStreamDrained,
  };

  static const pw_registry_events registryEvents = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = onRegistryGlobal,
    .global_remove = onRegistryGlobalRemove,
  };

  static const pw_link_events linkEvents = {
    .version = PW_VERSION_LINK_EVENTS,
    .info = onLinkInfo,
  };

  static const pw_node_events streamNodeEvents = {
    .version = PW_VERSION_NODE_EVENTS,
    .info = onNodeInfo,
  };

  static const pw_node_events sinkNodeEvents = {
    .version = PW_VERSION_NODE_EVENTS,
    .info = onNodeInfo,
    .param = onSinkNodeParam,
  };

  PipeWireBackend::PipeWireBackend()
  {
    pw_init(nullptr, nullptr);
  }

  PipeWireBackend::~PipeWireBackend()
  {
    stop();
    close();
  }

  BackendFormatInfo PipeWireBackend::formatInfo() const
  {
    std::lock_guard<std::mutex> lock(_infoMutex);
    return _formatInfo;
  }

  void PipeWireBackend::destroyResources() noexcept
  {
    _drainPending = false;

    if (_threadLoop)
    {
      pw_thread_loop_lock(_threadLoop);
    }

    {
      std::lock_guard<std::mutex> lock(_infoMutex);
      if (_monitorState)
      {
        if (_monitorState->sinkNodeBinding.proxy)
        {
          spa_hook_remove(&_monitorState->sinkNodeBinding.listener);
          pw_proxy_destroy(reinterpret_cast<pw_proxy*>(_monitorState->sinkNodeBinding.proxy));
          _monitorState->sinkNodeBinding = {};
        }

        if (_monitorState->streamNodeBinding.proxy)
        {
          spa_hook_remove(&_monitorState->streamNodeBinding.listener);
          pw_proxy_destroy(reinterpret_cast<pw_proxy*>(_monitorState->streamNodeBinding.proxy));
          _monitorState->streamNodeBinding = {};
        }

        for (auto& [_, binding] : _monitorState->linkBindings)
        {
          if (binding.proxy)
          {
            spa_hook_remove(&binding.listener);
            pw_proxy_destroy(reinterpret_cast<pw_proxy*>(binding.proxy));
          }
        }
        _monitorState->linkBindings.clear();

        if (_monitorState->registry)
        {
          spa_hook_remove(&_monitorState->registryListener);
          pw_proxy_destroy(reinterpret_cast<pw_proxy*>(_monitorState->registry));
          _monitorState->registry = nullptr;
        }

        _monitorState.reset();
      }

      _formatInfo = {};
    }

    if (_stream)
    {
      pw_stream_destroy(_stream);
      _stream = nullptr;
    }

    if (_threadLoop)
    {
      pw_thread_loop_unlock(_threadLoop);
      pw_thread_loop_stop(_threadLoop);
    }

    if (_context)
    {
      pw_context_destroy(_context);
      _context = nullptr;
    }

    if (_threadLoop)
    {
      pw_thread_loop_destroy(_threadLoop);
      _threadLoop = nullptr;
    }
  }

  void PipeWireBackend::setError(std::string message)
  {
    _lastError = std::move(message);
  }

  bool PipeWireBackend::open(StreamFormat const& format, AudioRenderCallbacks callbacks)
  {
    close();

    _callbacks = callbacks;
    _format = format;
    _lastError.clear();

    {
      std::lock_guard<std::mutex> lock(_infoMutex);
      _formatInfo = {};
      _formatInfo.isExclusive = false;
      _formatInfo.conversionReason = "Shared PipeWire playback cannot guarantee bit-perfect output";
      _monitorState = std::make_unique<RegistryMonitorState>();
    }

    _threadLoop = pw_thread_loop_new("rockstudio-pw", nullptr);
    if (!_threadLoop)
    {
      setError("Failed to create PipeWire thread loop");
      return false;
    }

    _context = pw_context_new(pw_thread_loop_get_loop(_threadLoop), nullptr, 0);
    if (!_context)
    {
      setError("Failed to create PipeWire context");
      close();
      return false;
    }

    if (pw_thread_loop_start(_threadLoop) < 0)
    {
      setError("Failed to start PipeWire thread loop");
      close();
      return false;
    }

    pw_thread_loop_lock(_threadLoop);

    std::string nodeRateStr = "1/" + std::to_string(_format.sampleRate);
    pw_properties* props = pw_properties_new(PW_KEY_MEDIA_TYPE,
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

    _stream = pw_stream_new_simple(pw_thread_loop_get_loop(_threadLoop), "playback", props, &streamEvents, this);
    if (!_stream)
    {
      pw_thread_loop_unlock(_threadLoop);
      setError("Failed to create PipeWire stream");
      close();
      return false;
    }

    std::uint8_t buffer[1024];
    spa_pod_builder builder;
    spa_pod_builder_init(&builder, buffer, sizeof(buffer));

    spa_pod_frame frame;
    spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);

    int spaFormat = SPA_AUDIO_FORMAT_S16_LE;
    if (_format.bitDepth == 24)
    {
      spaFormat = SPA_AUDIO_FORMAT_S24_LE;
    }
    else if (_format.bitDepth == 32)
    {
      spaFormat = SPA_AUDIO_FORMAT_S32_LE;
    }

    spa_pod_builder_add(&builder,
                        SPA_FORMAT_mediaType,
                        SPA_POD_Id(SPA_MEDIA_TYPE_audio),
                        SPA_FORMAT_mediaSubtype,
                        SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                        SPA_FORMAT_AUDIO_format,
                        SPA_POD_Id(spaFormat),
                        SPA_FORMAT_AUDIO_rate,
                        SPA_POD_Int(_format.sampleRate),
                        SPA_FORMAT_AUDIO_channels,
                        SPA_POD_Int(_format.channels),
                        0);

    spa_pod_builder_pop(&builder, &frame);

    auto* param = reinterpret_cast<spa_pod*>(buffer);
    spa_pod const* params[] = {param};

    auto const ret = pw_stream_connect(_stream,
                                       PW_DIRECTION_OUTPUT,
                                       PW_ID_ANY,
                                       static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                                                    PW_STREAM_FLAG_MAP_BUFFERS |
                                                                    PW_STREAM_FLAG_INACTIVE),
                                       params,
                                       1);

    pw_thread_loop_unlock(_threadLoop);

    if (ret < 0)
    {
      setError("Failed to connect PipeWire stream: " + std::to_string(-ret));
      close();
      return false;
    }

    refreshMonitorState();
    return true;
  }

  void PipeWireBackend::start()
  {
    if (!_stream || !_threadLoop)
    {
      return;
    }

    _drainPending = false;

    pw_thread_loop_lock(_threadLoop);
    pw_stream_set_active(_stream, true);
    pw_thread_loop_unlock(_threadLoop);
  }

  void PipeWireBackend::pause()
  {
    if (!_stream || !_threadLoop)
    {
      return;
    }

    pw_thread_loop_lock(_threadLoop);
    pw_stream_set_active(_stream, false);
    pw_thread_loop_unlock(_threadLoop);
  }

  void PipeWireBackend::resume()
  {
    if (!_stream || !_threadLoop)
    {
      return;
    }

    pw_thread_loop_lock(_threadLoop);
    pw_stream_set_active(_stream, true);
    pw_thread_loop_unlock(_threadLoop);
  }

  void PipeWireBackend::flush()
  {
    if (!_stream || !_threadLoop)
    {
      return;
    }

    _drainPending = false;

    pw_thread_loop_lock(_threadLoop);
    pw_stream_flush(_stream, false);
    pw_thread_loop_unlock(_threadLoop);
  }

  void PipeWireBackend::drain()
  {
    if (!_stream || !_threadLoop || _drainPending)
    {
      return;
    }

    _drainPending = true;
    pw_thread_loop_lock(_threadLoop);
    pw_stream_flush(_stream, true);
    pw_thread_loop_unlock(_threadLoop);
  }

  void PipeWireBackend::stop()
  {
    if (!_stream || !_threadLoop)
    {
      return;
    }

    _drainPending = false;

    pw_thread_loop_lock(_threadLoop);
    pw_stream_set_active(_stream, false);
    pw_thread_loop_unlock(_threadLoop);
  }

  void PipeWireBackend::close()
  {
    destroyResources();
  }

  void PipeWireBackend::handleStreamParamChanged(std::uint32_t id, spa_pod const* param)
  {
    if (id != SPA_PARAM_Format)
    {
      return;
    }

    auto const parsedFormat = parseRawStreamFormat(param);
    if (!parsedFormat)
    {
      return;
    }

    {
      std::lock_guard<std::mutex> lock(_infoMutex);
      if (_monitorState)
      {
        _monitorState->negotiatedStreamFormat = parsedFormat;
      }
    }

    refreshMonitorState();
  }

  void PipeWireBackend::handleStreamStateChanged(int oldState, int newState, char const* errorMessage)
  {
    (void)oldState;

    if (newState == PW_STREAM_STATE_ERROR)
    {
      if (errorMessage && *errorMessage)
      {
        setError(errorMessage);
      }
      return;
    }

    if (newState == PW_STREAM_STATE_PAUSED || newState == PW_STREAM_STATE_STREAMING)
    {
      refreshMonitorState();
    }
  }

  void PipeWireBackend::handleRegistryGlobal(std::uint32_t id,
                                             char const* type,
                                             std::uint32_t version,
                                             spa_dict const* props)
  {
    {
      std::lock_guard<std::mutex> lock(_infoMutex);
      if (!_monitorState)
      {
        return;
      }

      if (_monitorState->streamNodeId == PW_ID_ANY && _stream)
      {
        auto const streamNodeId = pw_stream_get_node_id(_stream);
        if (streamNodeId != PW_ID_ANY)
        {
          _monitorState->streamNodeId = streamNodeId;
        }
      }

      if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0)
      {
        auto record = parseNodeRecord(version, props);
        if (_monitorState->streamNodeId == PW_ID_ANY)
        {
          auto const appName = lookupProperty(props, PW_KEY_APP_NAME);
          if (record.nodeName == "RockStudio Playback" || appName == "RockStudio")
          {
            _monitorState->streamNodeId = id;
          }
        }
        _monitorState->nodes[id] = std::move(record);
      }
      else if (std::strcmp(type, PW_TYPE_INTERFACE_Link) == 0)
      {
        auto& link = _monitorState->links[id];
        link.outputNodeId = parseUintProperty(spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_NODE)).value_or(PW_ID_ANY);
        link.inputNodeId = parseUintProperty(spa_dict_lookup(props, PW_KEY_LINK_INPUT_NODE)).value_or(PW_ID_ANY);
        link.state = PW_LINK_STATE_ACTIVE; // Assume active until specific info arrives

        if (_monitorState->registry && !_monitorState->linkBindings.contains(id))
        {
          auto* proxy = static_cast<pw_link*>(pw_registry_bind(_monitorState->registry,
                                                              id,
                                                              PW_TYPE_INTERFACE_Link,
                                                              std::min(version, std::uint32_t(PW_VERSION_LINK)),
                                                              0));
          if (proxy)
          {
            auto& binding = _monitorState->linkBindings[id];
            binding.id = id;
            binding.proxy = proxy;
            pw_link_add_listener(binding.proxy, &binding.listener, &linkEvents, this);
          }
        }
      }
    }

    refreshMonitorState();
  }

  void PipeWireBackend::handleRegistryGlobalRemove(std::uint32_t id)
  {
    {
      std::lock_guard<std::mutex> lock(_infoMutex);
      if (!_monitorState)
      {
        return;
      }

      if (auto binding = _monitorState->linkBindings.find(id); binding != _monitorState->linkBindings.end())
      {
        if (binding->second.proxy)
        {
          spa_hook_remove(&binding->second.listener);
          pw_proxy_destroy(reinterpret_cast<pw_proxy*>(binding->second.proxy));
        }
        _monitorState->linkBindings.erase(binding);
      }

      if (_monitorState->sinkNodeBinding.id == id)
      {
        if (_monitorState->sinkNodeBinding.proxy)
        {
          spa_hook_remove(&_monitorState->sinkNodeBinding.listener);
          pw_proxy_destroy(reinterpret_cast<pw_proxy*>(_monitorState->sinkNodeBinding.proxy));
        }
        _monitorState->sinkNodeBinding = {};
        _monitorState->sinkFormat.reset();
        _monitorState->sinkProps = {};
      }

      if (_monitorState->streamNodeBinding.id == id)
      {
        if (_monitorState->streamNodeBinding.proxy)
        {
          spa_hook_remove(&_monitorState->streamNodeBinding.listener);
          pw_proxy_destroy(reinterpret_cast<pw_proxy*>(_monitorState->streamNodeBinding.proxy));
        }
        _monitorState->streamNodeBinding = {};
      }

      _monitorState->nodes.erase(id);
      _monitorState->links.erase(id);

      if (_monitorState->streamNodeId == id)
      {
        _monitorState->streamNodeId = PW_ID_ANY;
      }
    }

    refreshMonitorState();
  }

  void PipeWireBackend::handleLinkInfo(pw_link_info const* info)
  {
    if (info == nullptr)
    {
      return;
    }

    {
      std::lock_guard<std::mutex> lock(_infoMutex);
      if (!_monitorState)
      {
        return;
      }

      auto& link = _monitorState->links[info->id];
      link.outputNodeId = info->output_node_id;
      link.inputNodeId = info->input_node_id;
      if (info->change_mask & PW_LINK_CHANGE_MASK_STATE)
      {
        link.state = info->state;
      }
    }

    refreshMonitorState();
  }

  void PipeWireBackend::handleSinkNodeInfo(pw_node_info const* info)
  {
    if (info == nullptr)
    {
      return;
    }

    {
      std::lock_guard<std::mutex> lock(_infoMutex);
      if (!_monitorState)
      {
        return;
      }

      if (info->change_mask & PW_NODE_CHANGE_MASK_PROPS)
      {
        auto version = std::uint32_t(PW_VERSION_NODE);
        if (auto const existing = _monitorState->nodes.find(info->id); existing != _monitorState->nodes.end())
        {
          version = existing->second.version;
        }

        _monitorState->nodes[info->id] = parseNodeRecord(version, info->props);
      }
    }

    refreshMonitorState();
  }

  void PipeWireBackend::handleSinkNodeParam(std::uint32_t id, spa_pod const* param)
  {
    {
      std::lock_guard<std::mutex> lock(_infoMutex);
      if (!_monitorState)
      {
        return;
      }

      if (id == SPA_PARAM_Format)
      {
        _monitorState->sinkFormat = parseRawStreamFormat(param);
      }
      else if (id == SPA_PARAM_Props)
      {
        mergeSinkProps(_monitorState->sinkProps, param);
      }
    }

    refreshMonitorState();
  }

  void PipeWireBackend::ensureRegistryMonitor()
  {
    if (!_threadLoop || !_stream)
    {
      return;
    }

    pw_thread_loop_lock(_threadLoop);
    {
      std::lock_guard<std::mutex> lock(_infoMutex);
      if (!_monitorState || _monitorState->registry)
      {
        pw_thread_loop_unlock(_threadLoop);
        return;
      }

      if (auto* core = pw_stream_get_core(_stream))
      {
        auto* registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
        if (registry)
        {
          _monitorState->registry = registry;
          pw_registry_add_listener(_monitorState->registry, &_monitorState->registryListener, &registryEvents, this);
          
          // Add core listener for sync
          pw_core_add_listener(core, &_monitorState->coreListener, &coreEvents, this);
          _monitorState->coreSyncSeq = pw_core_sync(core, PW_ID_CORE, 0);
        }
      }
    }
    pw_thread_loop_unlock(_threadLoop);
  }

  void PipeWireBackend::handleCoreDone(std::uint32_t id, int seq)
  {
    (void)id;
    {
      std::lock_guard<std::mutex> lock(_infoMutex);
      if (_monitorState && seq == _monitorState->coreSyncSeq)
      {
        // Registry is now initially populated
      }
    }
    refreshMonitorState();
  }

  void PipeWireBackend::refreshMonitorState()
  {
    if (!_threadLoop)
    {
      return;
    }

    ensureRegistryMonitor();

    pw_thread_loop_lock(_threadLoop);
    {
      std::lock_guard<std::mutex> lock(_infoMutex);
      if (!_monitorState)
      {
        pw_thread_loop_unlock(_threadLoop);
        return;
      }

      if (_stream)
      {
        auto const streamNodeId = pw_stream_get_node_id(_stream);
        if (streamNodeId != PW_ID_ANY)
        {
          _monitorState->streamNodeId = streamNodeId;
        }
      }

      // First fallback: search by exact name
      if (_monitorState->streamNodeId == PW_ID_ANY)
      {
        for (auto const& [id, node] : _monitorState->nodes)
        {
          if (node.nodeName == "RockStudio Playback")
          {
            _monitorState->streamNodeId = id;
            break;
          }
        }
      }

      // Final fallback if we still don't have a node ID: use any node with application.name = RockStudio
      if (_monitorState->streamNodeId == PW_ID_ANY)
      {
        for (auto const& [id, node] : _monitorState->nodes)
        {
          if (node.nodeName == "RockStudio Playback" || node.nodeName.find("RockStudio") != std::string::npos)
          {
             _monitorState->streamNodeId = id;
             break;
          }
        }
      }

      // Ensure we are bound to our own stream node to get full info (driver-id, etc.)
      if (_monitorState->streamNodeId != PW_ID_ANY && _monitorState->streamNodeBinding.id != _monitorState->streamNodeId)
      {
        if (_monitorState->streamNodeBinding.proxy)
        {
          spa_hook_remove(&_monitorState->streamNodeBinding.listener);
          pw_proxy_destroy(reinterpret_cast<pw_proxy*>(_monitorState->streamNodeBinding.proxy));
        }
        _monitorState->streamNodeBinding = {};

        auto const nodeIt = _monitorState->nodes.find(_monitorState->streamNodeId);
        if (_monitorState->registry && nodeIt != _monitorState->nodes.end())
        {
          auto* node = static_cast<pw_node*>(pw_registry_bind(_monitorState->registry,
                                                              _monitorState->streamNodeId,
                                                              PW_TYPE_INTERFACE_Node,
                                                              std::min(nodeIt->second.version, std::uint32_t(PW_VERSION_NODE)),
                                                              0));
          if (node)
          {
            _monitorState->streamNodeBinding.id = _monitorState->streamNodeId;
            _monitorState->streamNodeBinding.proxy = node;
            pw_node_add_listener(_monitorState->streamNodeBinding.proxy,
                                 &_monitorState->streamNodeBinding.listener,
                                 &streamNodeEvents,
                                 this);
          }
        }
      }

      std::vector<std::uint32_t> reachableNodes;
      std::unordered_set<std::uint32_t> reachableSet;
      if (_monitorState->streamNodeId != PW_ID_ANY)
      {
        reachableNodes.push_back(_monitorState->streamNodeId);
        reachableSet.insert(_monitorState->streamNodeId);
      }

      for (std::size_t index = 0; index < reachableNodes.size(); ++index)
      {
        auto const currentNodeId = reachableNodes[index];
        for (auto const& [_, link] : _monitorState->links)
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

      std::vector<std::uint32_t> sinkCandidates;
      std::vector<std::string> intermediaryNodes;
      for (auto const nodeId : reachableNodes)
      {
        if (nodeId == _monitorState->streamNodeId)
        {
          continue;
        }

        auto const nodeIt = _monitorState->nodes.find(nodeId);
        if (nodeIt == _monitorState->nodes.end())
        {
          continue;
        }

        if (isSinkMediaClass(nodeIt->second.mediaClass))
        {
          sinkCandidates.push_back(nodeId);
        }
        else
        {
          intermediaryNodes.push_back(nodeDisplayName(nodeIt->second));
        }
      }

      auto desiredSinkNodeId = sinkCandidates.empty() ? PW_ID_ANY : sinkCandidates.front();
      
      // If no reachable sink via links, ALWAYS check the driverId of our node
      if (desiredSinkNodeId == PW_ID_ANY && _monitorState->streamNodeId != PW_ID_ANY)
      {
        auto const streamNodeIt = _monitorState->nodes.find(_monitorState->streamNodeId);
        if (streamNodeIt != _monitorState->nodes.end() && streamNodeIt->second.driverId)
        {
          auto const driverNodeId = *streamNodeIt->second.driverId;
          auto const driverNodeIt = _monitorState->nodes.find(driverNodeId);
          if (driverNodeIt != _monitorState->nodes.end() && isSinkMediaClass(driverNodeIt->second.mediaClass))
          {
            desiredSinkNodeId = driverNodeId;
          }
        }
      }

      if (_monitorState->sinkNodeBinding.id != desiredSinkNodeId)
      {
        if (_monitorState->sinkNodeBinding.proxy)
        {
          spa_hook_remove(&_monitorState->sinkNodeBinding.listener);
          pw_proxy_destroy(reinterpret_cast<pw_proxy*>(_monitorState->sinkNodeBinding.proxy));
        }
        _monitorState->sinkNodeBinding = {};
        _monitorState->sinkFormat.reset();
        _monitorState->sinkProps = {};

        if (desiredSinkNodeId != PW_ID_ANY)
        {
          auto const nodeIt = _monitorState->nodes.find(desiredSinkNodeId);
          if (_monitorState->registry && nodeIt != _monitorState->nodes.end())
          {
            auto* node = static_cast<pw_node*>(pw_registry_bind(_monitorState->registry,
                                                                desiredSinkNodeId,
                                                                PW_TYPE_INTERFACE_Node,
                                                                std::min(nodeIt->second.version, std::uint32_t(PW_VERSION_NODE)),
                                                                0));
            if (node)
            {
              _monitorState->sinkNodeBinding.id = desiredSinkNodeId;
              _monitorState->sinkNodeBinding.proxy = node;
              pw_node_add_listener(_monitorState->sinkNodeBinding.proxy,
                                   &_monitorState->sinkNodeBinding.listener,
                                   &sinkNodeEvents,
                                   this);

              std::array<std::uint32_t, 2> params{SPA_PARAM_Format, SPA_PARAM_Props};
              pw_node_subscribe_params(_monitorState->sinkNodeBinding.proxy, params.data(), params.size());
              pw_node_enum_params(_monitorState->sinkNodeBinding.proxy, 1, SPA_PARAM_Format, 0, std::numeric_limits<std::uint32_t>::max(), nullptr);
              pw_node_enum_params(_monitorState->sinkNodeBinding.proxy, 2, SPA_PARAM_Props, 0, std::numeric_limits<std::uint32_t>::max(), nullptr);
            }
          }
        }
      }

      BackendFormatInfo info;
      info.isExclusive = false;
      info.conversionReason = "Shared PipeWire playback cannot guarantee bit-perfect output";
      info.streamFormat = _monitorState->negotiatedStreamFormat;
      info.deviceFormat = _monitorState->sinkFormat;

      if (desiredSinkNodeId != PW_ID_ANY)
      {
        auto const sinkNodeIt = _monitorState->nodes.find(desiredSinkNodeId);
        if (sinkNodeIt != _monitorState->nodes.end())
        {
          info.sinkName = nodeDisplayName(sinkNodeIt->second);
        }
      }

      std::cout << "[DEBUG] refreshMonitorState: streamNodeId=" << _monitorState->streamNodeId 
                << " streamNodeName='" << ((_monitorState->streamNodeId != PW_ID_ANY && _monitorState->nodes.count(_monitorState->streamNodeId)) ? _monitorState->nodes[_monitorState->streamNodeId].nodeName : "") << "'"
                << " reachable=" << reachableNodes.size() 
                << " candidates=" << sinkCandidates.size() 
                << " desiredSink=" << desiredSinkNodeId 
                << " sinkName='" << info.sinkName << "'" << std::endl;

      std::vector<std::string> warningIssues;
      std::vector<std::string> badIssues;

      if (_monitorState->streamNodeId == PW_ID_ANY)
      {
        warningIssues.emplace_back("PipeWire has not reported the playback stream node yet.");
      }

      if (!_monitorState->negotiatedStreamFormat)
      {
        warningIssues.emplace_back("The negotiated PipeWire stream format is not available yet.");
      }

      if (sinkCandidates.size() > 1)
      {
        badIssues.emplace_back("The stream is linked to multiple downstream sink nodes.");
      }

      if (!intermediaryNodes.empty())
      {
        std::ostringstream details;
        details << "Audio reaches the sink through intermediary PipeWire nodes: ";
        for (std::size_t index = 0; index < intermediaryNodes.size(); ++index)
        {
          if (index > 0)
          {
            details << ", ";
          }
          details << intermediaryNodes[index];
        }
        warningIssues.push_back(details.str());
      }

      if (desiredSinkNodeId == PW_ID_ANY)
      {
        warningIssues.emplace_back("No downstream Audio/Sink node is linked to the stream yet.");
      }
      else
      {
        if (!_monitorState->sinkFormat)
        {
          warningIssues.emplace_back("The active sink format is not available yet.");
        }

        if (_monitorState->negotiatedStreamFormat && _monitorState->sinkFormat &&
            !isLosslessFormatChange(*_monitorState->negotiatedStreamFormat, *_monitorState->sinkFormat))
        {
          badIssues.emplace_back("The PipeWire stream format does not match the sink format.");
        }

        std::unordered_set<std::uint32_t> otherUpstreamNodes;
        for (auto const& [_, link] : _monitorState->links)
        {
          if (!isActiveLink(link.state) || link.inputNodeId != desiredSinkNodeId || link.outputNodeId == PW_ID_ANY)
          {
            continue;
          }

          if (!reachableSet.contains(link.outputNodeId))
          {
            otherUpstreamNodes.insert(link.outputNodeId);
          }
        }

        if (!otherUpstreamNodes.empty())
        {
          std::ostringstream details;
          details << "Other active stream nodes are also feeding this sink";
          auto first = true;
          for (auto const nodeId : otherUpstreamNodes)
          {
            auto const nodeIt = _monitorState->nodes.find(nodeId);
            if (first)
            {
              details << ": ";
              first = false;
            }
            else
            {
              details << ", ";
            }
            details << (nodeIt != _monitorState->nodes.end() ? nodeDisplayName(nodeIt->second)
                                                             : std::string{"node "} + std::to_string(nodeId));
          }
          badIssues.push_back(details.str());
        }

        if (_monitorState->sinkProps.mute.value_or(false) || _monitorState->sinkProps.softMute.value_or(false))
        {
          badIssues.emplace_back("The sink is muted.");
        }

        if ((_monitorState->sinkProps.volume && *_monitorState->sinkProps.volume < kUnityThreshold) ||
            anyVolumeNotUnity(_monitorState->sinkProps.channelVolumes) ||
            anyVolumeNotUnity(_monitorState->sinkProps.softVolumes))
        {
          badIssues.emplace_back("The sink volume is not at unity.");
        }
      }

      if (!badIssues.empty())
      {
        info.sinkStatus = SinkStatus::Bad;
      }
      else if (!warningIssues.empty())
      {
        info.sinkStatus = SinkStatus::Warning;
      }
      else if (desiredSinkNodeId != PW_ID_ANY)
      {
        info.sinkStatus = SinkStatus::Good;
      }

      appendLine(info.sinkTooltip, "Best-effort PipeWire server inspection");
      switch (info.sinkStatus)
      {
        case SinkStatus::Good:
          appendLine(info.sinkTooltip, "Status: the current shared PipeWire path looks bit-perfect.");
          break;
        case SinkStatus::Warning:
          appendLine(info.sinkTooltip, "Status: still inspecting the PipeWire path or waiting for complete sink details.");
          break;
        case SinkStatus::Bad:
          appendLine(info.sinkTooltip, "Status: the current PipeWire path is not bit-perfect.");
          break;
        case SinkStatus::None: break;
      }
      if (_monitorState->streamNodeId != PW_ID_ANY)
      {
        appendLine(info.sinkTooltip, "Stream node id: " + std::to_string(_monitorState->streamNodeId));
      }
      if (info.streamFormat)
      {
        appendLine(info.sinkTooltip, "PipeWire stream format: " + formatText(*info.streamFormat));
      }

      if (desiredSinkNodeId != PW_ID_ANY)
      {
        appendLine(info.sinkTooltip,
                   "Sink node: " + (info.sinkName.empty() ? std::to_string(desiredSinkNodeId) : info.sinkName) +
                     " (id " + std::to_string(desiredSinkNodeId) + ")");

        if (auto const sinkNodeIt = _monitorState->nodes.find(desiredSinkNodeId); sinkNodeIt != _monitorState->nodes.end() &&
                                                               !sinkNodeIt->second.objectPath.empty())
        {
          appendLine(info.sinkTooltip, "Sink path: " + sinkNodeIt->second.objectPath);
        }
      }

      if (info.deviceFormat)
      {
        appendLine(info.sinkTooltip, "Sink format: " + formatText(*info.deviceFormat));
      }

      if (!badIssues.empty())
      {
        appendLine(info.sinkTooltip, "Problems:");
        for (auto const& issue : badIssues)
        {
          appendLine(info.sinkTooltip, "- " + issue);
        }
      }

      if (!warningIssues.empty())
      {
        appendLine(info.sinkTooltip, "Observations:");
        for (auto const& issue : warningIssues)
        {
          appendLine(info.sinkTooltip, "- " + issue);
        }
      }

      _formatInfo = std::move(info);
    }
    pw_thread_loop_unlock(_threadLoop);
  }

  void PipeWireBackend::process()
  {
    if (!_callbacks.readPcm)
    {
      return;
    }

    auto* buffer = pw_stream_dequeue_buffer(_stream);
    if (!buffer)
    {
      if (_callbacks.onUnderrun)
      {
        _callbacks.onUnderrun(_callbacks.userData);
      }
      return;
    }

    auto* data = static_cast<std::byte*>(buffer->buffer->datas[0].data);
    auto const size = buffer->buffer->datas[0].maxsize;
    auto const bytesPerSample = (_format.bitDepth == 24) ? 3U : (_format.bitDepth == 32) ? 4U : 2U;
    auto const frameBytes = static_cast<std::size_t>(_format.channels) * bytesPerSample;
    auto const requestSize = frameBytes > 0 ? size - (size % frameBytes) : 0;

    if (data && requestSize > 0)
    {
      std::span<std::byte> output(data, requestSize);
      auto const read = _callbacks.readPcm(_callbacks.userData, output);
      auto const alignedRead = read - (read % frameBytes);

      if (alignedRead == 0 && _callbacks.isSourceDrained && _callbacks.isSourceDrained(_callbacks.userData))
      {
        pw_stream_return_buffer(_stream, buffer);
        drain();
        return;
      }

      buffer->buffer->datas[0].chunk->offset = 0;
      buffer->buffer->datas[0].chunk->size = static_cast<std::uint32_t>(alignedRead);
      buffer->buffer->datas[0].chunk->stride = static_cast<std::int32_t>(frameBytes);

      if (_callbacks.onPositionAdvanced && frameBytes > 0 && alignedRead > 0)
      {
        auto const frames = static_cast<std::uint32_t>(alignedRead / frameBytes);
        _callbacks.onPositionAdvanced(_callbacks.userData, frames);
      }
    }

    pw_stream_queue_buffer(_stream, buffer);
  }

  void PipeWireBackend::handleDrained() noexcept
  {
    _drainPending = false;
    if (_callbacks.onDrainComplete)
    {
      _callbacks.onDrainComplete(_callbacks.userData);
    }
  }

} // namespace app::playback
