// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/backend/PipeWireBackend.h>
#include <ao/audio/backend/detail/PipeWireShared.h>
#include <ao/utility/Log.h>
#include <ao/utility/Raii.h>

extern "C"
{
#include <pipewire/pipewire.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/param/param.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <mutex>

namespace ao::audio::backend
{
  using namespace detail;

  namespace
  {
    constexpr float kVolumeEpsilon = 0.0001F;
    constexpr std::size_t kPodBufferSize = 1024;
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

      _streamListener.reset();
      _stream.reset();
      _core.reset();
      _context.reset();
      _threadLoop.reset();
    }

    void destroyStream()
    {
      if (_threadLoop)
      {
        ::pw_thread_loop_lock(_threadLoop.get());
      }

      _streamListener.reset();
      _stream.reset();

      if (_threadLoop)
      {
        ::pw_thread_loop_unlock(_threadLoop.get());
      }
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    // Event Handlers
    void handleStreamProcess();
    void handleStreamParamChanged(std::uint32_t id, ::spa_pod const* param);
    void handleStreamStateChanged(::pw_stream_state oldState, ::pw_stream_state newState, char const* errorMessage);
    void handleStreamDrained();

  private:
    void handleFormatParam(::spa_pod const* param);
    void handlePropsParam(::spa_pod const* param);

  public:
    // Static C callback thunks for PipeWire
    static void onStreamProcess(void* data);
    static void onStreamParamChanged(void* data, std::uint32_t id, ::spa_pod const* param);
    static void onStreamStateChanged(void* data,
                                     ::pw_stream_state oldState,
                                     ::pw_stream_state newState,
                                     char const* errorMessage);
    static void onStreamDrained(void* data);

    static inline ::pw_stream_events const streamEvents = []
    {
      auto ev = ::pw_stream_events{};
      ev.version = PW_VERSION_STREAM_EVENTS;
      ev.state_changed = onStreamStateChanged;
      ev.param_changed = onStreamParamChanged;
      ev.process = onStreamProcess;
      ev.drained = onStreamDrained;
      return ev;
    }();

    // Members
    ao::audio::IRenderTarget* _renderTarget = nullptr;
    ao::audio::Format _format;
    std::atomic<bool> _drainPending = false;
    bool _strictFormatRequired = false;
    bool _strictFormatRejected = false;
    bool _routeAnchorReported = false;

    PwThreadLoopPtr _threadLoop;
    PwContextPtr _context;
    PwCorePtr _core;
    PwStreamPtr _stream;
    SpaHookGuard _streamListener;

    std::atomic<float> _volume{1.0F};
    std::atomic<bool> _muted{false};
    bool _volumeAvailable = true;
  };

  void PipeWireBackend::Impl::onStreamProcess(void* data)
  {
    static_cast<Impl*>(data)->handleStreamProcess();
  }

  void PipeWireBackend::Impl::onStreamParamChanged(void* data, std::uint32_t id, ::spa_pod const* param)
  {
    static_cast<Impl*>(data)->handleStreamParamChanged(id, param);
  }

  void PipeWireBackend::Impl::onStreamStateChanged(void* data,
                                                   ::pw_stream_state oldState,
                                                   ::pw_stream_state newState,
                                                   char const* errorMessage)
  {
    static_cast<Impl*>(data)->handleStreamStateChanged(oldState, newState, errorMessage);
  }

  void PipeWireBackend::Impl::onStreamDrained(void* data)
  {
    static_cast<Impl*>(data)->handleStreamDrained();
  }

  void PipeWireBackend::Impl::handleStreamProcess()
  {
    auto* buffer = ::pw_stream_dequeue_buffer(_stream.get());
    if (buffer == nullptr)
    {
      return;
    }

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
      ::pw_stream_queue_buffer(_stream.get(), buffer);
      return;
    }

    auto const bytesRead = _renderTarget->readPcm({static_cast<std::byte*>(data), max_size});

    if (bytesRead > 0)
    {
      buffer->buffer->datas[0].chunk->offset = 0;
      buffer->buffer->datas[0].chunk->size = bytesRead;
      buffer->buffer->datas[0].chunk->stride = stride;
      ::pw_stream_queue_buffer(_stream.get(), buffer);
      _renderTarget->onPositionAdvanced(static_cast<std::uint32_t>(bytesRead / stride));
    }

    else
    {
      buffer->buffer->datas[0].chunk->offset = 0;
      buffer->buffer->datas[0].chunk->size = 0;
      ::pw_stream_queue_buffer(_stream.get(), buffer);

      if (_renderTarget->isSourceDrained())
      {
        ::pw_stream_flush(_stream.get(), true);
      }
    }
  }

  void PipeWireBackend::Impl::handleStreamParamChanged(std::uint32_t id, ::spa_pod const* param)
  {
    if (param == nullptr)
    {
      return;
    }

    if (id == SPA_PARAM_Format)
    {
      handleFormatParam(param);
    }
    else if (id == SPA_PARAM_Props)
    {
      handlePropsParam(param);
    }
  }

  void PipeWireBackend::Impl::handleFormatParam(::spa_pod const* param)
  {
    if (auto negotiated = parseRawStreamFormat(param))
    {
      _format = *negotiated;
      AUDIO_LOG_INFO(
        "Negotiated PipeWire format: {}Hz, {}b, {} channels", _format.sampleRate, _format.bitDepth, _format.channels);

      _renderTarget->onFormatChanged(_format);
    }
  }

  void PipeWireBackend::Impl::handlePropsParam(::spa_pod const* param)
  {
    // Parse SPA_PROP_volume and SPA_PROP_mute from the Props pod
    if (auto const* prop = ::spa_pod_find_prop(param, nullptr, SPA_PROP_volume))
    {
      float volFloat = 0.0F;

      if (::spa_pod_get_float(&prop->value, &volFloat) == 0)
      {
        if (std::abs(volFloat - _volume.exchange(volFloat)) > kVolumeEpsilon)
        {
          _renderTarget->onPropertyChanged(ao::audio::PropertyId::Volume);
        }
      }
    }

    if (auto const* prop = ::spa_pod_find_prop(param, nullptr, SPA_PROP_mute))
    {
      bool muteBool = false;

      if (::spa_pod_get_bool(&prop->value, &muteBool) == 0)
      {
        if (muteBool != _muted.exchange(muteBool))
        {
          _renderTarget->onPropertyChanged(ao::audio::PropertyId::Muted);
        }
      }
    }
  }

  void PipeWireBackend::Impl::handleStreamStateChanged(::pw_stream_state /*oldState*/,
                                                       ::pw_stream_state newState,
                                                       char const* errorMessage)
  {
    if (newState == PW_STREAM_STATE_ERROR)
    {
      _renderTarget->onBackendError(errorMessage ? errorMessage : "Unknown PipeWire error");
    }
    else if (newState == PW_STREAM_STATE_PAUSED || newState == PW_STREAM_STATE_STREAMING)
    {
      if (!_routeAnchorReported && _stream)
      {
        auto id = ::pw_stream_get_node_id(_stream.get());
        if (id != PW_ID_ANY)
        {
          _routeAnchorReported = true;
          _renderTarget->onRouteReady(std::format("{}", id));
        }
      }
    }
  }

  void PipeWireBackend::Impl::handleStreamDrained()
  {
    _drainPending = false;
    _renderTarget->onDrainComplete();
  }

  PipeWireBackend::PipeWireBackend(ao::audio::Device const& device, ao::audio::ProfileId const& profile)
    : _impl{std::make_unique<Impl>()}
    , _targetDeviceId{device.id}
    , _exclusiveMode{profile == ao::audio::kProfileExclusive}
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

  ao::Result<> PipeWireBackend::open(ao::audio::Format const& format, ao::audio::IRenderTarget* target)
  {
    _impl->_renderTarget = target;
    _impl->_format = format;
    _impl->_routeAnchorReported = false;
    bool useExclusive = _exclusiveMode && !_targetDeviceId.empty();

    if (!_impl->_threadLoop)
    {
      return ao::makeError(ao::Error::Code::InitFailed, "PipeWire not initialized");
    }

    ::pw_thread_loop_lock(_impl->_threadLoop.get());
    auto* rawProps = ::pw_properties_new(PW_KEY_MEDIA_TYPE, // NOLINT(cppcoreguidelines-pro-type-vararg)
                                         "Audio",
                                         PW_KEY_MEDIA_CATEGORY,
                                         "Playback",
                                         PW_KEY_MEDIA_CATEGORY,
                                         "Music",
                                         PW_KEY_APP_NAME,
                                         "Aobus",
                                         PW_KEY_APP_ID,
                                         "io.github.Aobus",
                                         PW_KEY_NODE_NAME,
                                         "Aobus Playback",
                                         nullptr);
    auto props = ao::utility::makeUniquePtr<::pw_properties_free>(rawProps);
    ::pw_properties_set(props.get(), PW_KEY_NODE_RATE, std::format("1/{}", format.sampleRate).c_str());

    if (!_targetDeviceId.empty())
    {
      ::pw_properties_set(props.get(), PW_KEY_TARGET_OBJECT, _targetDeviceId.c_str());

      if (useExclusive)
      {
        ::pw_properties_set(props.get(), PW_KEY_NODE_EXCLUSIVE, "true");
      }
    }

    _impl->_stream.reset(::pw_stream_new(_impl->_core.get(), "Aobus Playback", props.release()));
    if (!_impl->_stream)
    {
      ::pw_thread_loop_unlock(_impl->_threadLoop.get());
      return ao::makeError(ao::Error::Code::InitFailed, "Failed to create stream");
    }

    _impl->_streamListener.reset();
    ::pw_stream_add_listener(_impl->_stream.get(), _impl->_streamListener.get(), &Impl::streamEvents, _impl.get());

    auto spaFmt = SPA_AUDIO_FORMAT_S16_LE;

    if (format.bitDepth == 32)
    {
      spaFmt = (format.validBits == 24) ? SPA_AUDIO_FORMAT_S24_32_LE : SPA_AUDIO_FORMAT_S32_LE;
    }
    else if (format.bitDepth == 24)
    {
      spaFmt = SPA_AUDIO_FORMAT_S24_LE;
    }

    auto buffer = std::array<std::uint8_t, kPodBufferSize>{};
    auto builder = ::spa_pod_builder{};
    ::spa_pod_builder_init(&builder, buffer.data(), buffer.size());
    auto frame = ::spa_pod_frame{};
    ::spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    ::spa_pod_builder_add(&builder, // NOLINT(cppcoreguidelines-pro-type-vararg)
                          SPA_FORMAT_mediaType,
                          SPA_POD_Id(SPA_MEDIA_TYPE_audio),
                          SPA_FORMAT_mediaSubtype,
                          SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                          SPA_FORMAT_AUDIO_format,
                          SPA_POD_Id(spaFmt),
                          SPA_FORMAT_AUDIO_rate,
                          SPA_POD_Int(format.sampleRate),
                          SPA_FORMAT_AUDIO_channels,
                          SPA_POD_Int(format.channels),
                          0);

    if (format.channels == 2)
    {
      auto position = std::array<std::uint32_t, 2>{SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR};
      ::spa_pod_builder_add(&builder, // NOLINT(cppcoreguidelines-pro-type-vararg)
                            SPA_FORMAT_AUDIO_position,
                            SPA_POD_Array(sizeof(std::uint32_t), SPA_TYPE_Id, position.size(), position.data()),
                            0);
    }

    auto const* param = static_cast<::spa_pod const*>(::spa_pod_builder_pop(&builder, &frame));
    auto params = std::array<::spa_pod const*, 1>{param};
    auto flags = static_cast<::pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                                PW_STREAM_FLAG_RT_PROCESS);

    if (useExclusive)
    {
      flags = static_cast<::pw_stream_flags>(flags | PW_STREAM_FLAG_EXCLUSIVE | PW_STREAM_FLAG_NO_CONVERT);
    }

    if (::pw_stream_connect(_impl->_stream.get(), PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params.data(), 1) < 0)
    {
      ::pw_thread_loop_unlock(_impl->_threadLoop.get());
      return ao::makeError(ao::Error::Code::InitFailed, "Failed to connect stream");
    }

    _impl->_volumeAvailable = !useExclusive; // Default to available in shared mode

    ::pw_thread_loop_unlock(_impl->_threadLoop.get());
    return {};
  }

  void PipeWireBackend::start()
  {
    if (!_impl->_stream)
    {
      return;
    }

    ::pw_thread_loop_lock(_impl->_threadLoop.get());
    ::pw_stream_set_active(_impl->_stream.get(), true);
    ::pw_thread_loop_unlock(_impl->_threadLoop.get());
  }

  void PipeWireBackend::pause()
  {
    if (!_impl->_stream)
    {
      return;
    }

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
    if (!_impl->_stream)
    {
      return;
    }

    _impl->_drainPending = false;
    ::pw_thread_loop_lock(_impl->_threadLoop.get());
    ::pw_stream_flush(_impl->_stream.get(), false);
    ::pw_thread_loop_unlock(_impl->_threadLoop.get());
  }

  void PipeWireBackend::stop()
  {
    if (!_impl->_stream)
    {
      return;
    }

    _impl->_drainPending = false;
    ::pw_thread_loop_lock(_impl->_threadLoop.get());
    ::pw_stream_set_active(_impl->_stream.get(), false);
    ::pw_thread_loop_unlock(_impl->_threadLoop.get());
  }

  void PipeWireBackend::close()
  {
    _impl->destroyStream();
  }

  void PipeWireBackend::setExclusiveMode(bool exclusive)
  {
    if (_exclusiveMode == exclusive)
    {
      return;
    }

    _exclusiveMode = exclusive;

    if (_impl->_stream && !_targetDeviceId.empty())
    {
      if (auto const openResult = open(_impl->_format, _impl->_renderTarget); !openResult)
      {
        AUDIO_LOG_ERROR("Failed to reopen stream after exclusive mode change: {}", openResult.error().message);
      }
    }
  }

  bool PipeWireBackend::isExclusiveMode() const noexcept
  {
    return _exclusiveMode;
  }

  ao::audio::BackendId PipeWireBackend::backendId() const noexcept
  {
    return ao::audio::kBackendPipeWire;
  }

  ao::audio::ProfileId PipeWireBackend::profileId() const noexcept
  {
    return _exclusiveMode ? ao::audio::kProfileExclusive : ao::audio::kProfileShared;
  }

  ao::Result<> PipeWireBackend::setProperty(ao::audio::PropertyId id, ao::audio::PropertyValue const& value)
  {
    if (id == ao::audio::PropertyId::Volume)
    {
      auto const volume = std::get<float>(value);
      auto const clamped = std::clamp(volume, 0.0F, 1.0F);
      _impl->_volume.store(clamped, std::memory_order_relaxed);

      if (!_impl->_stream)
      {
        return {};
      }

      auto vol = static_cast<float>(clamped);

      auto buf = std::array<std::uint8_t, 128>{};
      auto builder = ::spa_pod_builder{};
      ::spa_pod_builder_init(&builder, buf.data(), buf.size());

      auto frame = ::spa_pod_frame{};
      ::spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
      ::spa_pod_builder_add(&builder, SPA_PROP_volume, SPA_POD_Float(clamped), 0);
      auto const* param = static_cast<::spa_pod const*>(::spa_pod_builder_pop(&builder, &frame));

      ::pw_thread_loop_lock(_impl->_threadLoop.get());
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
      ::pw_stream_set_control(_impl->_stream.get(), SPA_PROP_volume, 1, &vol);
      ::pw_stream_update_params(_impl->_stream.get(), &param, 1);
      ::pw_thread_loop_unlock(_impl->_threadLoop.get());
      return {};
    }

    if (id == ao::audio::PropertyId::Muted)
    {
      auto const muted = std::get<bool>(value);
      _impl->_muted.store(muted, std::memory_order_relaxed);

      if (!_impl->_stream)
      {
        return {};
      }

      auto buf = std::array<std::uint8_t, 128>{};
      auto builder = ::spa_pod_builder{};
      ::spa_pod_builder_init(&builder, buf.data(), buf.size());

      auto frame = ::spa_pod_frame{};
      ::spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
      ::spa_pod_builder_add(&builder, SPA_PROP_mute, SPA_POD_Bool(muted), 0);
      auto const* param = static_cast<::spa_pod const*>(::spa_pod_builder_pop(&builder, &frame));

      ::pw_thread_loop_lock(_impl->_threadLoop.get());
      ::pw_stream_update_params(_impl->_stream.get(), &param, 1);
      ::pw_thread_loop_unlock(_impl->_threadLoop.get());
      return {};
    }

    return std::unexpected(ao::Error{.code = ao::Error::Code::NotSupported});
  }

  ao::Result<ao::audio::PropertyValue> PipeWireBackend::getProperty(ao::audio::PropertyId id) const
  {
    if (id == ao::audio::PropertyId::Volume)
    {
      return _impl->_volume.load(std::memory_order_relaxed);
    }

    if (id == ao::audio::PropertyId::Muted)
    {
      return _impl->_muted.load(std::memory_order_relaxed);
    }

    return std::unexpected(ao::Error{.code = ao::Error::Code::NotSupported});
  }

  ao::audio::PropertyInfo PipeWireBackend::queryProperty(ao::audio::PropertyId id) const noexcept
  {
    if (id == ao::audio::PropertyId::Volume || id == ao::audio::PropertyId::Muted)
    {
      return {.canRead = true,
              .canWrite = true,
              .isAvailable = _impl && _impl->_volumeAvailable,
              .emitsChangeNotifications = true};
    }

    return {};
  }
} // namespace ao::audio::backend
