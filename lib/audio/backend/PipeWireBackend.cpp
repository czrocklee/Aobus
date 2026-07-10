// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/Error.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/Format.h>
#include <ao/audio/Property.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/backend/PipeWireBackend.h>
#include <ao/audio/backend/detail/AudioBackendVolumeMath.h>
#include <ao/audio/backend/detail/PipeWireFormatParsing.h>
#include <ao/audio/backend/detail/PipeWireRuntime.h>
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
#include <spa/utils/type.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>

namespace ao::audio::backend
{
  using namespace detail;

  namespace
  {
    constexpr std::size_t kPodBufferSize = 1024;
  }

  /**
   * @brief Implementation of PipeWireBackend.
   */
  struct PipeWireBackend::Impl final
  {
    Impl() = default;

    ~Impl()
    {
      if (threadLoopPtr)
      {
        ::pw_thread_loop_stop(threadLoopPtr.get());
      }

      {
        auto guard = PwThreadLoopGuard{threadLoopPtr.get()};
        streamListener.reset();
        streamPtr.reset();
        corePtr.reset();
        contextPtr.reset();
      }

      threadLoopPtr.reset();
    }

    void destroyStream()
    {
      auto guard = PwThreadLoopGuard{threadLoopPtr.get()};

      streamListener.reset();
      streamPtr.reset();
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

    static ::pw_stream_events const streamEvents;

    // Members
    PipeWireEnvironmentGuard envGuard;
    RenderTarget* renderTarget = nullptr;
    Format format;
    std::atomic<bool> drainPending = false;
    bool strictFormatRequired = false;
    bool strictFormatRejected = false;
    bool routeAnchorReported = false;

    PwThreadLoopPtr threadLoopPtr;
    PwContextPtr contextPtr;
    PwCorePtr corePtr;
    PwStreamPtr streamPtr;
    SpaHookGuard streamListener;

    std::atomic<float> volume{1.0F};
    std::atomic<bool> muted{false};
    std::atomic<bool> outputControlAvailable{true};

    void applyCachedControls() const;
    PropertyInfo volumePropertyInfo() const noexcept
    {
      return {.canRead = true,
              .canWrite = true,
              .isAvailable = outputControlAvailable.load(std::memory_order_relaxed),
              .emitsChangeNotifications = true,
              .isHardwareAssisted = false};
    }

    PropertyInfo mutedPropertyInfo() const noexcept
    {
      return {.canRead = true,
              .canWrite = true,
              .isAvailable = outputControlAvailable.load(std::memory_order_relaxed),
              .emitsChangeNotifications = true,
              .isHardwareAssisted = false};
    }

    PropertyInfo propertyInfo(PropertyId id) const noexcept
    {
      if (id == PropertyId::Volume)
      {
        return volumePropertyInfo();
      }

      if (id == PropertyId::Muted)
      {
        return mutedPropertyInfo();
      }

      return {};
    }

    PropertySnapshot propertySnapshot(PropertyId id, PropertyValue const& value) const
    {
      return {.id = id, .optValue = value, .info = propertyInfo(id)};
    }

  private:
    void handleFormatParam(::spa_pod const* param);
    void handlePropsParam(::spa_pod const* param);
  };

  ::pw_stream_events const PipeWireBackend::Impl::streamEvents = [] // NOLINT(bugprone-throwing-static-initialization)
  {
    auto ev = ::pw_stream_events{};
    ev.version = PW_VERSION_STREAM_EVENTS;
    ev.state_changed = [](void* data, ::pw_stream_state oldState, ::pw_stream_state newState, char const* errorMessage)
    { static_cast<Impl*>(data)->handleStreamStateChanged(oldState, newState, errorMessage); };
    ev.param_changed = [](void* data, std::uint32_t id, ::spa_pod const* param)
    { static_cast<Impl*>(data)->handleStreamParamChanged(id, param); };
    ev.process = [](void* data) { static_cast<Impl*>(data)->handleStreamProcess(); };
    ev.drained = [](void* data) { static_cast<Impl*>(data)->handleStreamDrained(); };
    return ev;
  }();

  void PipeWireBackend::Impl::handleStreamProcess() const
  {
    auto* buffer = ::pw_stream_dequeue_buffer(streamPtr.get());

    if (buffer == nullptr)
    {
      return;
    }

    auto* data = buffer->buffer->datas[0].data;

    if (data == nullptr)
    {
      ::pw_stream_queue_buffer(streamPtr.get(), buffer);
      return;
    }

    auto const maxSize = buffer->buffer->datas[0].maxsize;
    auto stride = buffer->buffer->datas[0].chunk->stride;

    if (stride == 0 && format.channels > 0 && format.bitDepth > 0)
    {
      stride = format.channels * (format.bitDepth / 8);
    }

    if (stride == 0)
    {
      ::pw_stream_queue_buffer(streamPtr.get(), buffer);
      return;
    }

    // Honor the RenderTarget frame-alignment contract: only ever request and
    // commit whole frames, even when the PipeWire buffer's maxsize is not a
    // whole multiple of the frame stride.
    auto const strideBytes = static_cast<std::size_t>(stride);
    auto const requestBytes = (static_cast<std::size_t>(maxSize) / strideBytes) * strideBytes;
    auto commitPcm = [&](RenderPcmResult const& result) -> bool
    {
      auto const bytesRead = result.bytesWritten;
      auto const framesRead = bytesRead / strideBytes;

      if (framesRead == 0)
      {
        return false;
      }

      auto const committedBytes = framesRead * strideBytes;
      buffer->buffer->datas[0].chunk->offset = 0;
      buffer->buffer->datas[0].chunk->size = static_cast<std::uint32_t>(committedBytes);
      buffer->buffer->datas[0].chunk->stride = static_cast<std::int32_t>(stride);
      ::pw_stream_queue_buffer(streamPtr.get(), buffer);
      renderTarget->handlePositionAdvanced(result.positionFrames);
      return true;
    };

    auto const renderResult = renderTarget->renderPcm({static_cast<std::byte*>(data), requestBytes});

    if (commitPcm(renderResult))
    {
      return;
    }

    buffer->buffer->datas[0].chunk->offset = 0;
    buffer->buffer->datas[0].chunk->size = 0;
    ::pw_stream_queue_buffer(streamPtr.get(), buffer);

    if (renderResult.drained)
    {
      ::pw_stream_flush(streamPtr.get(), true);
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
    if (auto optNegotiated = parseRawStreamFormat(param); optNegotiated)
    {
      format = *optNegotiated;
      renderTarget->handleFormatChanged(format);
    }
  }

  void PipeWireBackend::Impl::handlePropsParam(::spa_pod const* param)
  {
    // Parse SPA_PROP_volume and SPA_PROP_mute from the Props pod
    if (auto const* prop = ::spa_pod_find_prop(param, nullptr, SPA_PROP_volume); prop != nullptr)
    {
      if (float volFloat = 0.0F; ::spa_pod_get_float(&prop->value, &volFloat) == 0)
      {
        if (std::abs(volFloat - volume.exchange(volFloat)) > detail::kVolumeEpsilon)
        {
          renderTarget->handlePropertyChanged(propertySnapshot(PropertyId::Volume, PropertyValue{volFloat}));
        }
      }
    }

    if (auto const* prop = ::spa_pod_find_prop(param, nullptr, SPA_PROP_mute); prop != nullptr)
    {
      if (bool muteBool = false; ::spa_pod_get_bool(&prop->value, &muteBool) == 0)
      {
        if (muteBool != muted.exchange(muteBool))
        {
          renderTarget->handlePropertyChanged(propertySnapshot(PropertyId::Muted, PropertyValue{muteBool}));
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
      renderTarget->handleBackendError(errorMessage != nullptr ? errorMessage : "Unknown PipeWire error");
    }
    else if (newState == PW_STREAM_STATE_PAUSED || newState == PW_STREAM_STATE_STREAMING)
    {
      if (!routeAnchorReported && streamPtr)
      {
        if (auto id = ::pw_stream_get_node_id(streamPtr.get()); id != PW_ID_ANY)
        {
          routeAnchorReported = true;
          renderTarget->handleRouteReady(std::format("{}", id));
        }
      }
    }
  }

  void PipeWireBackend::Impl::handleStreamDrained()
  {
    drainPending = false;
    renderTarget->handleDrainComplete();
  }

  void PipeWireBackend::Impl::applyCachedControls() const
  {
    if (!streamPtr)
    {
      return;
    }

    auto vol = volume.load(std::memory_order_relaxed);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    ::pw_stream_set_control(streamPtr.get(), SPA_PROP_volume, 1, &vol);

    auto mutedFloat = muted.load(std::memory_order_relaxed) ? 1.0F : 0.0F;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    ::pw_stream_set_control(streamPtr.get(), SPA_PROP_mute, 1, &mutedFloat);
  }

  PipeWireBackend::PipeWireBackend(Device const& device, ProfileId const& profile)
    : _implPtr{std::make_unique<Impl>()}, _targetDeviceId{device.id}, _exclusiveMode{profile == kProfileExclusive}
  {
    _implPtr->threadLoopPtr.reset(::pw_thread_loop_new("PipeWireBackend", nullptr));

    if (_implPtr->threadLoopPtr)
    {
      _implPtr->contextPtr.reset(
        ::pw_context_new(::pw_thread_loop_get_loop(_implPtr->threadLoopPtr.get()), nullptr, 0));

      if (_implPtr->contextPtr)
      {
        ::pw_thread_loop_start(_implPtr->threadLoopPtr.get());
        {
          auto guard = PwThreadLoopGuard{_implPtr->threadLoopPtr.get()};
          _implPtr->corePtr.reset(::pw_context_connect(_implPtr->contextPtr.get(), nullptr, 0));
        }
      }
    }
  }

  PipeWireBackend::~PipeWireBackend() = default;

  Result<> PipeWireBackend::open(Format const& format, RenderTarget* target)
  {
    bool const useExclusive = _exclusiveMode && !_targetDeviceId.empty();

    if (!_implPtr->threadLoopPtr || !_implPtr->corePtr)
    {
      return makeError(Error::Code::InitFailed, "PipeWire not initialized");
    }

    auto guard = PwThreadLoopGuard{_implPtr->threadLoopPtr.get()};
    _implPtr->renderTarget = target;
    _implPtr->format = format;
    _implPtr->routeAnchorReported = false;

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    auto propsPtr = utility::makeUniquePtr<::pw_properties_free>(::pw_properties_new(nullptr, nullptr));
    ::pw_properties_set(propsPtr.get(), PW_KEY_MEDIA_TYPE, "Audio");
    ::pw_properties_set(propsPtr.get(), PW_KEY_MEDIA_CATEGORY, "Playback");
    ::pw_properties_set(propsPtr.get(), PW_KEY_MEDIA_ROLE, "Music");
    ::pw_properties_set(propsPtr.get(), PW_KEY_APP_NAME, "Aobus");
    ::pw_properties_set(propsPtr.get(), PW_KEY_APP_ID, "io.github.Aobus");
    ::pw_properties_set(propsPtr.get(), PW_KEY_NODE_NAME, "Aobus Playback");
    ::pw_properties_set(propsPtr.get(), PW_KEY_NODE_RATE, std::format("1/{}", format.sampleRate).c_str());

    if (!_targetDeviceId.empty())
    {
      ::pw_properties_set(propsPtr.get(), PW_KEY_TARGET_OBJECT, _targetDeviceId.c_str());

      if (useExclusive)
      {
        ::pw_properties_set(propsPtr.get(), PW_KEY_NODE_EXCLUSIVE, "true");
      }
    }

    _implPtr->streamPtr.reset(::pw_stream_new(_implPtr->corePtr.get(), "Aobus Playback", propsPtr.release()));

    if (!_implPtr->streamPtr)
    {
      return makeError(Error::Code::InitFailed, "Failed to create stream");
    }

    _implPtr->streamListener.reset();
    ::pw_stream_add_listener(
      _implPtr->streamPtr.get(), _implPtr->streamListener.get(), &Impl::streamEvents, _implPtr.get());

    auto spaFmt = SPA_AUDIO_FORMAT_S16_LE;

    if (format.isFloat && format.bitDepth == 32)
    {
      spaFmt = SPA_AUDIO_FORMAT_F32_LE;
    }
    else if (format.bitDepth == 32)
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

    ::spa_pod_builder_prop(&builder, SPA_FORMAT_mediaType, 0);
    ::spa_pod_builder_id(&builder, SPA_MEDIA_TYPE_audio);

    ::spa_pod_builder_prop(&builder, SPA_FORMAT_mediaSubtype, 0);
    ::spa_pod_builder_id(&builder, SPA_MEDIA_SUBTYPE_raw);

    ::spa_pod_builder_prop(&builder, SPA_FORMAT_AUDIO_format, 0);
    ::spa_pod_builder_id(&builder, spaFmt);

    ::spa_pod_builder_prop(&builder, SPA_FORMAT_AUDIO_rate, 0);
    ::spa_pod_builder_int(&builder, static_cast<int32_t>(format.sampleRate));

    ::spa_pod_builder_prop(&builder, SPA_FORMAT_AUDIO_channels, 0);
    ::spa_pod_builder_int(&builder, static_cast<int32_t>(format.channels));

    if (format.channels == 2)
    {
      auto position = std::array<std::uint32_t, 2>{SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR};
      ::spa_pod_builder_prop(&builder, SPA_FORMAT_AUDIO_position, 0);
      ::spa_pod_builder_array(&builder, sizeof(std::uint32_t), SPA_TYPE_Id, position.size(), position.data());
    }

    auto const* param = static_cast<::spa_pod const*>(::spa_pod_builder_pop(&builder, &frame));
    auto params = std::array<::spa_pod const*, 1>{param};
    auto flags = static_cast<::pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                                PW_STREAM_FLAG_RT_PROCESS);

    if (useExclusive)
    {
      flags = static_cast<::pw_stream_flags>(flags | PW_STREAM_FLAG_EXCLUSIVE | PW_STREAM_FLAG_NO_CONVERT);
    }

    if (::pw_stream_connect(_implPtr->streamPtr.get(), PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params.data(), 1) < 0)
    {
      return makeError(Error::Code::InitFailed, "Failed to connect stream");
    }

    _implPtr->applyCachedControls();
    _implPtr->outputControlAvailable.store(!useExclusive, std::memory_order_relaxed);

    return {};
  }

  void PipeWireBackend::start()
  {
    auto guard = PwThreadLoopGuard{_implPtr->threadLoopPtr.get()};

    if (!_implPtr->streamPtr)
    {
      return;
    }

    ::pw_stream_set_active(_implPtr->streamPtr.get(), true);
  }

  void PipeWireBackend::pause()
  {
    auto guard = PwThreadLoopGuard{_implPtr->threadLoopPtr.get()};

    if (!_implPtr->streamPtr)
    {
      return;
    }

    ::pw_stream_set_active(_implPtr->streamPtr.get(), false);
  }

  void PipeWireBackend::resume()
  {
    start();
  }

  void PipeWireBackend::flush()
  {
    _implPtr->drainPending = false;
    auto guard = PwThreadLoopGuard{_implPtr->threadLoopPtr.get()};

    if (!_implPtr->streamPtr)
    {
      return;
    }

    ::pw_stream_flush(_implPtr->streamPtr.get(), false);
  }

  void PipeWireBackend::stop()
  {
    _implPtr->drainPending = false;
    auto guard = PwThreadLoopGuard{_implPtr->threadLoopPtr.get()};

    if (!_implPtr->streamPtr)
    {
      return;
    }

    ::pw_stream_set_active(_implPtr->streamPtr.get(), false);
  }

  void PipeWireBackend::close()
  {
    _implPtr->destroyStream();
  }

  BackendId PipeWireBackend::backendId() const
  {
    return kBackendPipeWire;
  }

  ProfileId PipeWireBackend::profileId() const
  {
    return _exclusiveMode ? kProfileExclusive : kProfileShared;
  }

  Result<> PipeWireBackend::setProperty(PropertyId id, PropertyValue const& value)
  {
    if (id == PropertyId::Volume)
    {
      auto const volValue = std::get<float>(value);
      auto const clamped = std::clamp(volValue, 0.0F, 1.0F);
      _implPtr->volume.store(clamped, std::memory_order_relaxed);

      auto vol = clamped;
      {
        auto guard = PwThreadLoopGuard{_implPtr->threadLoopPtr.get()};

        if (!_implPtr->streamPtr)
        {
          return {};
        }

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        ::pw_stream_set_control(_implPtr->streamPtr.get(), SPA_PROP_volume, 1, &vol);
      }
      return {};
    }

    if (id == PropertyId::Muted)
    {
      auto const muted = std::get<bool>(value);
      _implPtr->muted.store(muted, std::memory_order_relaxed);

      auto mutedFloat = muted ? 1.0F : 0.0F;
      {
        auto guard = PwThreadLoopGuard{_implPtr->threadLoopPtr.get()};

        if (!_implPtr->streamPtr)
        {
          return {};
        }

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        ::pw_stream_set_control(_implPtr->streamPtr.get(), SPA_PROP_mute, 1, &mutedFloat);
      }
      return {};
    }

    return makeError(Error::Code::NotSupported);
  }

  Result<PropertyValue> PipeWireBackend::property(PropertyId id) const
  {
    if (id == PropertyId::Volume)
    {
      return PropertyValue{_implPtr->volume.load(std::memory_order_relaxed)};
    }

    if (id == PropertyId::Muted)
    {
      return PropertyValue{_implPtr->muted.load(std::memory_order_relaxed)};
    }

    return makeError(Error::Code::NotSupported);
  }

  PropertyInfo PipeWireBackend::queryProperty(PropertyId id) const noexcept
  {
    if (id == PropertyId::Volume || id == PropertyId::Muted)
    {
      if (_implPtr != nullptr)
      {
        return _implPtr->propertyInfo(id);
      }

      return {.canRead = true, .canWrite = true, .emitsChangeNotifications = true};
    }

    return {};
  }
} // namespace ao::audio::backend
