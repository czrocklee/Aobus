// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/backend/PipeWireBackend.h>

#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/Format.h>
#include <ao/audio/IRenderTarget.h>
#include <ao/audio/Property.h>
#include <ao/audio/backend/detail/PipeWireShared.h>
#include <ao/utility/Log.h>
#include <ao/utility/Raii.h>

extern "C"
{
#include <pipewire/pipewire.h>
#include <spa/param/audio/raw.h>
#include <spa/param/format.h>
#include <spa/param/param.h>
#include <spa/param/props.h>
#include <spa/pod/body.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/pod/pod.h>
#include <spa/pod/vararg.h>
#include <spa/utils/type.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>

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
      if (threadLoop)
      {
        ::pw_thread_loop_stop(threadLoop.get());
      }

      streamListener.reset();
      stream.reset();
      core.reset();
      context.reset();
      threadLoop.reset();
    }

    void destroyStream()
    {
      if (threadLoop)
      {
        ::pw_thread_loop_lock(threadLoop.get());
      }

      streamListener.reset();
      stream.reset();

      if (threadLoop)
      {
        ::pw_thread_loop_unlock(threadLoop.get());
      }
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    // Event Handlers
    void handleStreamProcess() const;
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
    IRenderTarget* renderTarget = nullptr;
    Format format;
    std::atomic<bool> drainPending = false;
    bool strictFormatRequired = false;
    bool strictFormatRejected = false;
    bool routeAnchorReported = false;

    PwThreadLoopPtr threadLoop;
    PwContextPtr context;
    PwCorePtr core;
    PwStreamPtr stream;
    SpaHookGuard streamListener;

    std::atomic<float> volume{1.0F};
    std::atomic<bool> muted{false};
    bool volumeAvailable = true;
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

  void PipeWireBackend::Impl::handleStreamProcess() const
  {
    auto* buffer = ::pw_stream_dequeue_buffer(stream.get());

    if (buffer == nullptr)
    {
      return;
    }

    auto* data = buffer->buffer->datas[0].data;

    if (data == nullptr)
    {
      ::pw_stream_queue_buffer(stream.get(), buffer);
      return;
    }

    auto const max_size = buffer->buffer->datas[0].maxsize;
    auto stride = buffer->buffer->datas[0].chunk->stride;

    if (stride == 0 && format.channels > 0 && format.bitDepth > 0)
    {
      stride = format.channels * (format.bitDepth / 8);
    }

    if (stride == 0)
    {
      ::pw_stream_queue_buffer(stream.get(), buffer);
      return;
    }

    auto const bytesRead = renderTarget->readPcm({static_cast<std::byte*>(data), max_size});

    if (bytesRead > 0)
    {
      buffer->buffer->datas[0].chunk->offset = 0;
      buffer->buffer->datas[0].chunk->size = bytesRead;
      buffer->buffer->datas[0].chunk->stride = stride;
      ::pw_stream_queue_buffer(stream.get(), buffer);
      renderTarget->onPositionAdvanced(static_cast<std::uint32_t>(bytesRead / stride));
    }

    else
    {
      buffer->buffer->datas[0].chunk->offset = 0;
      buffer->buffer->datas[0].chunk->size = 0;
      ::pw_stream_queue_buffer(stream.get(), buffer);

      if (renderTarget->isSourceDrained())
      {
        ::pw_stream_flush(stream.get(), true);
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
    if (auto optNegotiated = parseRawStreamFormat(param))
    {
      format = *optNegotiated;
      AUDIO_LOG_INFO(
        "Negotiated PipeWire format: {}Hz, {}b, {} channels", format.sampleRate, format.bitDepth, format.channels);

      renderTarget->onFormatChanged(format);
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
        if (std::abs(volFloat - volume.exchange(volFloat)) > kVolumeEpsilon)
        {
          renderTarget->onPropertyChanged(PropertyId::Volume);
        }
      }
    }

    if (auto const* prop = ::spa_pod_find_prop(param, nullptr, SPA_PROP_mute))
    {
      bool muteBool = false;

      if (::spa_pod_get_bool(&prop->value, &muteBool) == 0)
      {
        if (muteBool != muted.exchange(muteBool))
        {
          renderTarget->onPropertyChanged(PropertyId::Muted);
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
      renderTarget->onBackendError(errorMessage != nullptr ? errorMessage : "Unknown PipeWire error");
    }
    else if (newState == PW_STREAM_STATE_PAUSED || newState == PW_STREAM_STATE_STREAMING)
    {
      if (!routeAnchorReported && stream)
      {
        auto id = ::pw_stream_get_node_id(stream.get());

        if (id != PW_ID_ANY)
        {
          routeAnchorReported = true;
          renderTarget->onRouteReady(std::format("{}", id));
        }
      }
    }
  }

  void PipeWireBackend::Impl::handleStreamDrained()
  {
    drainPending = false;
    renderTarget->onDrainComplete();
  }

  PipeWireBackend::PipeWireBackend(Device const& device, ProfileId const& profile)
    : _impl{std::make_unique<Impl>()}, _targetDeviceId{device.id}, _exclusiveMode{profile == kProfileExclusive}
  {
    _impl->threadLoop.reset(::pw_thread_loop_new("PipeWireBackend", nullptr));

    if (_impl->threadLoop)
    {
      _impl->context.reset(::pw_context_new(::pw_thread_loop_get_loop(_impl->threadLoop.get()), nullptr, 0));

      if (_impl->context)
      {
        ::pw_thread_loop_start(_impl->threadLoop.get());
        ::pw_thread_loop_lock(_impl->threadLoop.get());
        _impl->core.reset(::pw_context_connect(_impl->context.get(), nullptr, 0));
        ::pw_thread_loop_unlock(_impl->threadLoop.get());
      }
    }
  }

  PipeWireBackend::~PipeWireBackend() = default;

  Result<> PipeWireBackend::open(Format const& format, IRenderTarget* target)
  {
    _impl->renderTarget = target;
    _impl->format = format;
    _impl->routeAnchorReported = false;
    bool const useExclusive = _exclusiveMode && !_targetDeviceId.empty();

    if (!_impl->threadLoop)
    {
      return makeError(Error::Code::InitFailed, "PipeWire not initialized");
    }

    ::pw_thread_loop_lock(_impl->threadLoop.get());
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
    auto props = utility::makeUniquePtr<::pw_properties_free>(rawProps);
    ::pw_properties_set(props.get(), PW_KEY_NODE_RATE, std::format("1/{}", format.sampleRate).c_str());

    if (!_targetDeviceId.empty())
    {
      ::pw_properties_set(props.get(), PW_KEY_TARGET_OBJECT, _targetDeviceId.c_str());

      if (useExclusive)
      {
        ::pw_properties_set(props.get(), PW_KEY_NODE_EXCLUSIVE, "true");
      }
    }

    _impl->stream.reset(::pw_stream_new(_impl->core.get(), "Aobus Playback", props.release()));

    if (!_impl->stream)
    {
      ::pw_thread_loop_unlock(_impl->threadLoop.get());
      return makeError(Error::Code::InitFailed, "Failed to create stream");
    }

    _impl->streamListener.reset();
    ::pw_stream_add_listener(_impl->stream.get(), _impl->streamListener.get(), &Impl::streamEvents, _impl.get());

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

    if (::pw_stream_connect(_impl->stream.get(), PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params.data(), 1) < 0)
    {
      ::pw_thread_loop_unlock(_impl->threadLoop.get());
      return makeError(Error::Code::InitFailed, "Failed to connect stream");
    }

    _impl->volumeAvailable = !useExclusive; // Default to available in shared mode

    ::pw_thread_loop_unlock(_impl->threadLoop.get());
    return {};
  }

  void PipeWireBackend::start()
  {
    if (!_impl->stream)
    {
      return;
    }

    ::pw_thread_loop_lock(_impl->threadLoop.get());
    ::pw_stream_set_active(_impl->stream.get(), true);
    ::pw_thread_loop_unlock(_impl->threadLoop.get());
  }

  void PipeWireBackend::pause()
  {
    if (!_impl->stream)
    {
      return;
    }

    ::pw_thread_loop_lock(_impl->threadLoop.get());
    ::pw_stream_set_active(_impl->stream.get(), false);
    ::pw_thread_loop_unlock(_impl->threadLoop.get());
  }

  void PipeWireBackend::resume()
  {
    start();
  }

  void PipeWireBackend::flush()
  {
    if (!_impl->stream)
    {
      return;
    }

    _impl->drainPending = false;
    ::pw_thread_loop_lock(_impl->threadLoop.get());
    ::pw_stream_flush(_impl->stream.get(), false);
    ::pw_thread_loop_unlock(_impl->threadLoop.get());
  }

  void PipeWireBackend::stop()
  {
    if (!_impl->stream)
    {
      return;
    }

    _impl->drainPending = false;
    ::pw_thread_loop_lock(_impl->threadLoop.get());
    ::pw_stream_set_active(_impl->stream.get(), false);
    ::pw_thread_loop_unlock(_impl->threadLoop.get());
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

    if (_impl->stream && !_targetDeviceId.empty())
    {
      if (auto const openResult = open(_impl->format, _impl->renderTarget); !openResult)
      {
        AUDIO_LOG_ERROR("Failed to reopen stream after exclusive mode change: {}", openResult.error().message);
      }
    }
  }

  bool PipeWireBackend::isExclusiveMode() const noexcept
  {
    return _exclusiveMode;
  }

  BackendId PipeWireBackend::backendId() const noexcept
  {
    return kBackendPipeWire;
  }

  ProfileId PipeWireBackend::profileId() const noexcept
  {
    return _exclusiveMode ? kProfileExclusive : kProfileShared;
  }

  Result<> PipeWireBackend::setProperty(PropertyId id, PropertyValue const& value)
  {
    if (id == PropertyId::Volume)
    {
      auto const volValue = std::get<float>(value);
      auto const clamped = std::clamp(volValue, 0.0F, 1.0F);
      _impl->volume.store(clamped, std::memory_order_relaxed);

      if (!_impl->stream)
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

      ::pw_thread_loop_lock(_impl->threadLoop.get());
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
      ::pw_stream_set_control(_impl->stream.get(), SPA_PROP_volume, 1, &vol);
      ::pw_stream_update_params(_impl->stream.get(), &param, 1);
      ::pw_thread_loop_unlock(_impl->threadLoop.get());
      return {};
    }

    if (id == PropertyId::Muted)
    {
      auto const muted = std::get<bool>(value);
      _impl->muted.store(muted, std::memory_order_relaxed);

      if (!_impl->stream)
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

      ::pw_thread_loop_lock(_impl->threadLoop.get());
      ::pw_stream_update_params(_impl->stream.get(), &param, 1);
      ::pw_thread_loop_unlock(_impl->threadLoop.get());
      return {};
    }

    return std::unexpected(Error{.code = Error::Code::NotSupported});
  }

  Result<PropertyValue> PipeWireBackend::getProperty(PropertyId id) const
  {
    if (id == PropertyId::Volume)
    {
      return PropertyValue{_impl->volume.load(std::memory_order_relaxed)};
    }

    if (id == PropertyId::Muted)
    {
      return PropertyValue{_impl->muted.load(std::memory_order_relaxed)};
    }

    return std::unexpected(Error{.code = Error::Code::NotSupported});
  }

  PropertyInfo PipeWireBackend::queryProperty(PropertyId id) const noexcept
  {
    if (id == PropertyId::Volume || id == PropertyId::Muted)
    {
      return {.canRead = true,
              .canWrite = true,
              .isAvailable = _impl && _impl->volumeAvailable,
              .emitsChangeNotifications = true};
    }

    return {};
  }
} // namespace ao::audio::backend
