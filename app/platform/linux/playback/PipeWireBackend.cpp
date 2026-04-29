// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "platform/linux/playback/PipeWireBackend.h"
#include "platform/linux/playback/detail/PipeWireShared.h"
#include "core/Log.h"

extern "C"
{
#include <pipewire/pipewire.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/pod/builder.h>
}

#include <algorithm>
#include <atomic>
#include <format>
#include <mutex>

namespace app::playback
{
  using namespace detail;

  /**
   * @brief Implementation of PipeWireBackend.
   */
  struct PipeWireBackend::Impl final
  {
    Impl() { ensurePipeWireInit(); }

    ~Impl()
    {
      if (_threadLoop) ::pw_thread_loop_stop(_threadLoop.get());
      _streamListener.reset();
      _stream.reset();
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
    void handleStreamStateChanged(enum pw_stream_state oldState, enum pw_stream_state newState, char const* errorMessage);
    void handleStreamDrained();

    // Members
    app::core::backend::AudioRenderCallbacks _callbacks;
    app::core::AudioFormat _format;
    std::atomic<bool> _drainPending = false;
    std::string _lastError;
    bool _strictFormatRequired = false;
    bool _strictFormatRejected = false;
    bool _routeAnchorReported = false;

    PwThreadLoopPtr _threadLoop;
    PwContextPtr _context;
    PwCorePtr _core;
    PwStreamPtr _stream;
    SpaHookGuard _streamListener;
  };

  namespace
  {
    void onStreamProcess(void* data) { static_cast<PipeWireBackend::Impl*>(data)->handleStreamProcess(); }
    void onStreamParamChanged(void* data, std::uint32_t id, ::spa_pod const* param) { static_cast<PipeWireBackend::Impl*>(data)->handleStreamParamChanged(id, param); }
    void onStreamStateChanged(void* data, enum pw_stream_state oldState, enum pw_stream_state newState, char const* errorMessage)
    {
      static_cast<PipeWireBackend::Impl*>(data)->handleStreamStateChanged(oldState, newState, errorMessage);
    }
    void onStreamDrained(void* data) { static_cast<PipeWireBackend::Impl*>(data)->handleStreamDrained(); }

    ::pw_stream_events const streamEvents = [] {
      auto e = ::pw_stream_events{};
      e.version = PW_VERSION_STREAM_EVENTS;
      e.state_changed = onStreamStateChanged;
      e.param_changed = onStreamParamChanged;
      e.process = onStreamProcess;
      e.drained = onStreamDrained;
      return e;
    }();
  }

  void PipeWireBackend::Impl::handleStreamProcess()
  {
    auto* buffer = ::pw_stream_dequeue_buffer(_stream.get());
    if (buffer == nullptr) return;

    auto* data = buffer->buffer->datas[0].data;
    if (data == nullptr) { ::pw_stream_queue_buffer(_stream.get(), buffer); return; }

    auto const max_size = buffer->buffer->datas[0].maxsize;
    auto stride = buffer->buffer->datas[0].chunk->stride;
    if (stride == 0 && _format.channels > 0 && _format.bitDepth > 0) stride = _format.channels * (_format.bitDepth / 8);
    if (stride == 0) { ::pw_stream_queue_buffer(_stream.get(), buffer); return; }

    auto const bytesRead = _callbacks.readPcm(_callbacks.userData, {static_cast<std::byte*>(data), max_size});
    if (bytesRead > 0)
    {
      buffer->buffer->datas[0].chunk->offset = 0;
      buffer->buffer->datas[0].chunk->size = bytesRead;
      buffer->buffer->datas[0].chunk->stride = stride;
      ::pw_stream_queue_buffer(_stream.get(), buffer);
      _callbacks.onPositionAdvanced(_callbacks.userData, static_cast<std::uint32_t>(bytesRead / stride));
    }
    else
    {
      buffer->buffer->datas[0].chunk->offset = 0;
      buffer->buffer->datas[0].chunk->size = 0;
      ::pw_stream_queue_buffer(_stream.get(), buffer);
      if (_callbacks.isSourceDrained && _callbacks.isSourceDrained(_callbacks.userData)) ::pw_stream_flush(_stream.get(), true);
    }
  }

  void PipeWireBackend::Impl::handleStreamParamChanged(std::uint32_t id, ::spa_pod const* param)
  {
    if (param == nullptr || id != SPA_PARAM_Format) return;
    if (auto negotiated = parseRawStreamFormat(param))
    {
      _format = *negotiated;
      PLAYBACK_LOG_INFO("Negotiated PipeWire format: {}Hz, {}b, {} channels",
                        _format.sampleRate, _format.bitDepth, _format.channels);
      if (_callbacks.onFormatChanged)
      {
        _callbacks.onFormatChanged(_callbacks.userData, _format);
      }
    }
  }

  void PipeWireBackend::Impl::handleStreamStateChanged(enum pw_stream_state, enum pw_stream_state newState, char const* errorMessage)
  {
    if (newState == PW_STREAM_STATE_ERROR)
    {
      setError(errorMessage ? errorMessage : "Unknown PipeWire stream error");
      if (_callbacks.onBackendError) _callbacks.onBackendError(_callbacks.userData, _lastError);
    }
    else if (newState == PW_STREAM_STATE_PAUSED || newState == PW_STREAM_STATE_STREAMING)
    {
      if (!_routeAnchorReported && _callbacks.onRouteReady && _stream)
      {
        auto id = ::pw_stream_get_node_id(_stream.get());
        if (id != PW_ID_ANY)
        {
          _routeAnchorReported = true;
          _callbacks.onRouteReady(_callbacks.userData, std::format("{}", id));
        }
      }
    }
  }

  void PipeWireBackend::Impl::handleStreamDrained()
  {
    _drainPending = false;
    if (_callbacks.onDrainComplete) _callbacks.onDrainComplete(_callbacks.userData);
  }

  PipeWireBackend::PipeWireBackend(app::core::backend::AudioDevice const& device)
    : _impl{std::make_unique<Impl>()}
    , _targetDeviceId{device.id}
    , _exclusiveMode{device.backendKind == app::core::backend::BackendKind::PipeWireExclusive}
  {
    _impl->_threadLoop.reset(::pw_thread_loop_new("PipeWireBackend", nullptr));
    if (_impl->_threadLoop)
    {
      _impl->_context.reset(::pw_context_new(::pw_thread_loop_get_loop(_impl->_threadLoop.get()), nullptr, 0));
      if (_impl->_context)
      {
        ::pw_thread_loop_start(_impl->_threadLoop.get());
        ::pw_thread_loop_lock(_impl->_threadLoop.get());
        _impl->_core.reset(::pw_context_connect(_impl->_context.get(), nullptr, 0));
        ::pw_thread_loop_unlock(_impl->_threadLoop.get());
      }
    }
  }

  PipeWireBackend::~PipeWireBackend() = default;

  bool PipeWireBackend::open(app::core::AudioFormat const& format, app::core::backend::AudioRenderCallbacks callbacks)
  {
    if (format.sampleRate == 0) { _impl->_callbacks = {}; _impl->destroyStream(); return true; }
    _impl->_callbacks = callbacks;
    _impl->_format = format;
    _impl->_lastError.clear();
    _impl->_routeAnchorReported = false;
    bool useExclusive = _exclusiveMode && !_targetDeviceId.empty();
    if (!_impl->_threadLoop || !_impl->_context || !_impl->_core) { _impl->setError("PipeWire not initialized"); return false; }

    ::pw_thread_loop_lock(_impl->_threadLoop.get());
    ::pw_properties* props = ::pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_APP_NAME, "RockStudio", PW_KEY_NODE_NAME, "RockStudio Playback", nullptr);
    if (!_targetDeviceId.empty()) {
      ::pw_properties_set(props, PW_KEY_TARGET_OBJECT, _targetDeviceId.c_str());
      if (useExclusive) ::pw_properties_set(props, PW_KEY_NODE_EXCLUSIVE, "true");
    }

    _impl->_stream.reset(::pw_stream_new(_impl->_core.get(), "RockStudio Playback", props));
    if (!_impl->_stream) { _impl->setError("Failed to create stream"); ::pw_thread_loop_unlock(_impl->_threadLoop.get()); return false; }
    _impl->_streamListener.reset();
    ::pw_stream_add_listener(_impl->_stream.get(), _impl->_streamListener.get(), &streamEvents, _impl.get());

    auto spaFmt = SPA_AUDIO_FORMAT_S16_LE;
    if (format.bitDepth == 32) spaFmt = (format.validBits == 24) ? SPA_AUDIO_FORMAT_S24_32_LE : SPA_AUDIO_FORMAT_S32_LE;
    else if (format.bitDepth == 24) spaFmt = SPA_AUDIO_FORMAT_S24_LE;

    std::uint8_t buffer[1024]; ::spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    ::spa_pod_frame f; ::spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    ::spa_pod_builder_add(&b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_audio), SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_AUDIO_format, SPA_POD_Id(spaFmt), SPA_FORMAT_AUDIO_rate, SPA_POD_Int(format.sampleRate), SPA_FORMAT_AUDIO_channels, SPA_POD_Int(format.channels), 0);
    ::spa_pod const* param = static_cast<::spa_pod*>(::spa_pod_builder_pop(&b, &f));
    ::spa_pod const* params[] = {param};
    auto flags = static_cast<::pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS);
    if (useExclusive) flags = static_cast<::pw_stream_flags>(flags | PW_STREAM_FLAG_EXCLUSIVE | PW_STREAM_FLAG_NO_CONVERT);

    if (::pw_stream_connect(_impl->_stream.get(), PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1) < 0) { _impl->setError("Failed to connect stream"); ::pw_thread_loop_unlock(_impl->_threadLoop.get()); return false; }
    ::pw_thread_loop_unlock(_impl->_threadLoop.get());
    return true;
  }

  void PipeWireBackend::start()
  {
    if (!_impl || !_impl->_stream || !_impl->_threadLoop) return;
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
    _impl->_drainPending = false;
    ::pw_thread_loop_lock(_impl->_threadLoop.get());
    ::pw_stream_set_active(_impl->_stream.get(), false);
    ::pw_thread_loop_unlock(_impl->_threadLoop.get());
  }

  void PipeWireBackend::close()
  {
    if (_impl) _impl->destroyStream();
  }

  void PipeWireBackend::setExclusiveMode(bool exclusive)
  {
    if (_exclusiveMode == exclusive) return;
    _exclusiveMode = exclusive;
    if (_impl && _impl->_stream && !_targetDeviceId.empty()) open(_impl->_format, _impl->_callbacks);
  }

  bool PipeWireBackend::isExclusiveMode() const noexcept
  {
    return _exclusiveMode;
  }

  app::core::backend::BackendKind PipeWireBackend::kind() const noexcept
  {
    return _exclusiveMode ? app::core::backend::BackendKind::PipeWireExclusive : app::core::backend::BackendKind::PipeWire;
  }

  std::string_view PipeWireBackend::lastError() const noexcept
  {
    return _impl ? _impl->_lastError : "";
  }
} // namespace app::playback
