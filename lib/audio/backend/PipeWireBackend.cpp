// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/backend/PipeWireBackend.h>

#include "detail/DecoderOutput.h"
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/OpenedPcmMode.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/Property.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/backend/detail/AudioBackendVolumeMath.h>
#include <ao/audio/backend/detail/PipeWireFormatParsing.h>
#include <ao/audio/backend/detail/PipeWireRuntime.h>
#include <ao/utility/Raii.h>

extern "C"
{
#include <pipewire/loop.h>
#include <pipewire/pipewire.h>
#include <spa/param/param.h>
#include <spa/param/props.h>
#include <spa/pod/iter.h>
#include <spa/pod/pod.h>
#include <spa/support/loop.h>
#include <spa/utils/defs.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <exception>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::audio::backend
{
  using namespace detail;

  namespace
  {
    constexpr std::size_t kPodBufferSize = 1024;
    constexpr std::int64_t kOpenTimeoutNanoseconds = 5'000'000'000;

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

    template<std::size_t Size, typename Callback>
    // Literal extent is the realtime static-context contract.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
    void invokePipeWireRealtimeCallback(char const (&context)[Size],
                                        Callback&& callback,
                                        std::source_location const location = std::source_location::current()) noexcept
    {
      try
      {
        std::forward<Callback>(callback)();
      }
      catch (...)
      {
        AO_RT_FATAL_EXCEPTION_AT(location, context);
      }
    }

    std::int32_t completeLoopBarrier(::spa_loop* /*loop*/,
                                     bool /*async*/,
                                     std::uint32_t /*seq*/,
                                     void const* /*data*/,
                                     std::size_t /*size*/,
                                     void* /*userData*/) noexcept
    {
      return 0;
    }

    void waitForLoopBarrier(::pw_loop* loop)
    {
      if (loop == nullptr)
      {
        return;
      }

      auto const result = ::pw_loop_invoke(loop, completeLoopBarrier, SPA_ID_INVALID, nullptr, 0, true, nullptr);

      if (result < 0)
      {
        AO_FATAL("PipeWire loop barrier failed with code {}", result);
      }
    }
  } // namespace

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
    void handleStreamProcess();
    void handleStreamParamChanged(std::uint32_t id, ::spa_pod const* param);
    void handleStreamStateChanged(::pw_stream_state oldState, ::pw_stream_state newState, char const* errorMessage);
    void handleStreamDrained();
    static std::int32_t dispatchDrainComplete(::spa_loop* loop,
                                              bool async,
                                              std::uint32_t sequence,
                                              void const* data,
                                              std::size_t size,
                                              void* userData) noexcept;

    static ::pw_stream_events const streamEvents;

    // Members
    PipeWireEnvironmentGuard envGuard;
    RenderTarget* renderTarget = nullptr;
    SignalFormat sourceFormat;
    PcmFormat format;
    std::optional<SignalFormat> optLastSourceFormat;
    std::optional<PcmFormat> optLastNegotiatedFormat;
    std::vector<SampleEncoding> offeredEncodings;
    std::optional<Error> optOpenError;
    std::optional<std::string> optPendingRouteAnchor;
    ::pw_stream_state streamState = PW_STREAM_STATE_UNCONNECTED;
    bool opening = false;
    std::atomic<bool> renderAdmissionOpen{false};
    std::atomic<bool> drainPending{false};
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
    void stopRenderCycles();
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

  ::pw_stream_events const PipeWireBackend::Impl::streamEvents = []
  {
    auto ev = ::pw_stream_events{};
    ev.version = PW_VERSION_STREAM_EVENTS;
    ev.state_changed =
      [](void* data, ::pw_stream_state oldState, ::pw_stream_state newState, char const* errorMessage) noexcept
    {
      invokePipeWireCallback("PipeWire stream-state callback",
                             [=]
                             { static_cast<Impl*>(data)->handleStreamStateChanged(oldState, newState, errorMessage); });
    };
    ev.param_changed = [](void* data, std::uint32_t id, ::spa_pod const* param) noexcept
    {
      invokePipeWireCallback(
        "PipeWire stream-parameter callback", [=] { static_cast<Impl*>(data)->handleStreamParamChanged(id, param); });
    };
    ev.process = [](void* data) noexcept
    {
      invokePipeWireRealtimeCallback("Unhandled exception escaped PipeWire process callback",
                                     [=] { static_cast<Impl*>(data)->handleStreamProcess(); });
    };
    ev.drained = [](void* data) noexcept
    {
      invokePipeWireCallback(
        "PipeWire stream-drained callback", [=] { static_cast<Impl*>(data)->handleStreamDrained(); });
    };
    return ev;
  }();

  void PipeWireBackend::Impl::handleStreamProcess()
  {
    if (!renderAdmissionOpen.load(std::memory_order_acquire))
    {
      return;
    }

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

    if (stride == 0 && format.channels > 0 && format.encoding != SampleEncoding::Unknown)
    {
      stride = static_cast<std::int32_t>(frameBytes(format));
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
      drainPending.store(true, std::memory_order_release);

      if (::pw_stream_flush(streamPtr.get(), true) < 0)
      {
        drainPending.store(false, std::memory_order_release);
      }
      else
      {
        // The native drained event is delivered on the main loop. Close
        // project render admission before this data-loop callback returns;
        // handleStreamDrained() marshals the final target call back here.
        renderAdmissionOpen.store(false, std::memory_order_release);
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
    if (auto optNegotiated = parseRawStreamFormat(param); optNegotiated)
    {
      if (optNegotiated->sampleRate != sourceFormat.sampleRate || optNegotiated->channels != sourceFormat.channels ||
          !std::ranges::contains(offeredEncodings, optNegotiated->encoding))
      {
        optOpenError = Error{
          .code = Error::Code::FormatRejected, .message = "PipeWire negotiated a PCM mode outside the lossless offer"};
      }
      else if (format.encoding != SampleEncoding::Unknown && !samePcmMode(format, *optNegotiated))
      {
        optOpenError =
          Error{.code = Error::Code::FormatRejected, .message = "PipeWire changed PCM mode after stream activation"};
      }
      else if (format.encoding == SampleEncoding::Unknown)
      {
        format = *optNegotiated;
      }
    }
    else
    {
      optOpenError =
        Error{.code = Error::Code::FormatRejected, .message = "PipeWire negotiated an unrecognized raw PCM format"};
    }

    if (threadLoopPtr)
    {
      ::pw_thread_loop_signal(threadLoopPtr.get(), false);
    }

    if (!opening && optOpenError && renderTarget != nullptr)
    {
      renderTarget->handleBackendError(optOpenError->message);
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
          if (!opening && renderTarget != nullptr)
          {
            renderTarget->handlePropertyChanged(propertySnapshot(PropertyId::Volume, PropertyValue{volFloat}));
          }
        }
      }
    }

    if (auto const* prop = ::spa_pod_find_prop(param, nullptr, SPA_PROP_mute); prop != nullptr)
    {
      if (bool muteBool = false; ::spa_pod_get_bool(&prop->value, &muteBool) == 0)
      {
        if (muteBool != muted.exchange(muteBool))
        {
          if (!opening && renderTarget != nullptr)
          {
            renderTarget->handlePropertyChanged(propertySnapshot(PropertyId::Muted, PropertyValue{muteBool}));
          }
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
      auto const* const message = errorMessage != nullptr ? errorMessage : "Unknown PipeWire error";
      optOpenError = Error{.code = Error::Code::InitFailed, .message = message};

      if (!opening && renderTarget != nullptr)
      {
        renderTarget->handleBackendError(message);
      }
    }
    else if (newState == PW_STREAM_STATE_PAUSED || newState == PW_STREAM_STATE_STREAMING)
    {
      if (!routeAnchorReported && streamPtr)
      {
        if (auto id = ::pw_stream_get_node_id(streamPtr.get()); id != PW_ID_ANY)
        {
          routeAnchorReported = true;
          optPendingRouteAnchor = std::format("{}", id);

          if (!opening && renderTarget != nullptr)
          {
            renderTarget->handleRouteReady(*optPendingRouteAnchor);
            optPendingRouteAnchor.reset();
          }
        }
      }
    }

    streamState = newState;

    if (threadLoopPtr)
    {
      ::pw_thread_loop_signal(threadLoopPtr.get(), false);
    }
  }

  void PipeWireBackend::Impl::handleStreamDrained()
  {
    if (!drainPending.exchange(false, std::memory_order_acq_rel))
    {
      return;
    }

    auto* const dataLoop = ::pw_stream_get_data_loop(streamPtr.get());

    if (dataLoop == nullptr)
    {
      AO_FATAL("PipeWire drained event has no stream data loop");
    }

    auto const result = ::pw_loop_invoke(dataLoop, dispatchDrainComplete, SPA_ID_INVALID, nullptr, 0, false, this);

    if (result < 0)
    {
      AO_FATAL("PipeWire drain-complete handoff failed with code {}", result);
    }
  }

  std::int32_t PipeWireBackend::Impl::dispatchDrainComplete(::spa_loop* /*loop*/,
                                                            bool /*async*/,
                                                            std::uint32_t /*sequence*/,
                                                            void const* /*data*/,
                                                            std::size_t /*size*/,
                                                            void* userData) noexcept
  {
    auto& impl = *static_cast<Impl*>(userData);
    AO_RT_INVARIANT(impl.renderTarget != nullptr, "PipeWire drain target must remain alive");
    impl.renderTarget->handleDrainComplete();
    return 0;
  }

  void PipeWireBackend::Impl::stopRenderCycles()
  {
    // Admission is project-owned: a future asynchronous PipeWire deactivation
    // can no longer admit a RenderTarget call after this store.
    renderAdmissionOpen.store(false, std::memory_order_release);

    auto* dataLoop = static_cast<::pw_loop*>(nullptr);
    auto* mainLoop = static_cast<::pw_loop*>(nullptr);
    {
      auto guard = PwThreadLoopGuard{threadLoopPtr.get()};

      if (!streamPtr)
      {
        drainPending.store(false, std::memory_order_release);
        return;
      }

      std::ignore = ::pw_stream_set_active(streamPtr.get(), false);
      dataLoop = ::pw_stream_get_data_loop(streamPtr.get());
      mainLoop = ::pw_thread_loop_get_loop(threadLoopPtr.get());
    }

    // A process callback that observed open admission is serialized before
    // this round-trip. It has therefore returned, including all RenderTarget
    // notifications and any request that queued a drained event.
    waitForLoopBarrier(dataLoop);

    // Suppress that cycle's drain and run every already-queued main-loop drain
    // dispatch before returning. Never block on a loop while holding the
    // thread-loop lock.
    drainPending.store(false, std::memory_order_release);
    waitForLoopBarrier(mainLoop);

    // A drained main-loop callback accepted just before suppression may have
    // posted its final target call after the first data-loop barrier.
    waitForLoopBarrier(dataLoop);
  }

  void PipeWireBackend::Impl::applyCachedControls() const
  {
    if (!streamPtr)
    {
      return;
    }

    auto vol = volume.load(std::memory_order_relaxed);
    // PipeWire's stream-control setter is a variadic C API.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    ::pw_stream_set_control(streamPtr.get(), SPA_PROP_volume, 1, &vol);

    auto mutedFloat = muted.load(std::memory_order_relaxed) ? 1.0F : 0.0F;
    // PipeWire's stream-control setter is a variadic C API.
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

  std::optional<PcmFormat> PipeWireBackend::prewarmFormatHint(SignalFormat const& sourceFormat) const noexcept
  {
    if (_exclusiveMode && _implPtr->optLastSourceFormat == sourceFormat && _implPtr->optLastNegotiatedFormat)
    {
      return _implPtr->optLastNegotiatedFormat;
    }

    return Backend::prewarmFormatHint(sourceFormat);
  }

  // PipeWire negotiation must remain one transaction under the native thread-loop lock.
  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  Result<OpenedPcmMode> PipeWireBackend::open(SignalFormat const& sourceFormat, RenderTarget* target)
  {
    bool const useExclusive = _exclusiveMode && !_targetDeviceId.empty();

    if (!_implPtr->threadLoopPtr || !_implPtr->corePtr)
    {
      return makeError(Error::Code::InitFailed, "PipeWire not initialized");
    }

    close();
    auto offeredEncodings = ::ao::audio::detail::losslessPcmEncodings(sourceFormat);

    if (offeredEncodings.empty())
    {
      return makeError(Error::Code::NotSupported, "No lossless PCM encoding is available for PipeWire");
    }

    if (!useExclusive)
    {
      // Shared PipeWire can convert the preferred source-native client format.
      // Keep that mode deterministic so explicit-start worker preparation can
      // normally be reused after native negotiation. Exclusive mode retains
      // every lossless candidate because the target may reject the first one.
      offeredEncodings.resize(1);
    }

    auto optError = std::optional<Error>{};
    auto negotiatedFormat = PcmFormat{};
    auto optRouteAnchor = std::optional<std::string>{};
    {
      auto guard = PwThreadLoopGuard{_implPtr->threadLoopPtr.get()};
      _implPtr->renderTarget = target;
      _implPtr->sourceFormat = sourceFormat;
      _implPtr->format = {};
      _implPtr->offeredEncodings = offeredEncodings;
      _implPtr->optOpenError.reset();
      _implPtr->optPendingRouteAnchor.reset();
      _implPtr->streamState = PW_STREAM_STATE_UNCONNECTED;
      _implPtr->routeAnchorReported = false;
      _implPtr->opening = true;

      // PipeWire's property constructor is a variadic C API terminated by null keys.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
      auto propsPtr = utility::makeUniquePtr<::pw_properties_free>(::pw_properties_new(nullptr, nullptr));
      ::pw_properties_set(propsPtr.get(), PW_KEY_MEDIA_TYPE, "Audio");
      ::pw_properties_set(propsPtr.get(), PW_KEY_MEDIA_CATEGORY, "Playback");
      ::pw_properties_set(propsPtr.get(), PW_KEY_MEDIA_ROLE, "Music");
      ::pw_properties_set(propsPtr.get(), PW_KEY_APP_NAME, "Aobus");
      ::pw_properties_set(propsPtr.get(), PW_KEY_APP_ID, "io.github.Aobus");
      ::pw_properties_set(propsPtr.get(), PW_KEY_NODE_NAME, "Aobus Playback");
      ::pw_properties_set(propsPtr.get(), PW_KEY_NODE_RATE, std::format("1/{}", sourceFormat.sampleRate).c_str());

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
        optError = Error{.code = Error::Code::InitFailed, .message = "Failed to create PipeWire stream"};
      }
      else
      {
        _implPtr->streamListener.reset();
        ::pw_stream_add_listener(
          _implPtr->streamPtr.get(), _implPtr->streamListener.get(), &Impl::streamEvents, _implPtr.get());

        auto buffer = std::array<std::byte, kPodBufferSize>{};
        auto const* param = detail::buildRawStreamFormatOffer(buffer, sourceFormat, offeredEncodings);

        if (param == nullptr)
        {
          optError = Error{.code = Error::Code::FormatRejected, .message = "Failed to build PipeWire PCM offer"};
        }
        else
        {
          auto params = std::array<::spa_pod const*, 1>{param};
          auto flags = static_cast<::pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_INACTIVE |
                                                      PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS);

          if (useExclusive)
          {
            flags = static_cast<::pw_stream_flags>(flags | PW_STREAM_FLAG_EXCLUSIVE | PW_STREAM_FLAG_NO_CONVERT);
          }

          if (::pw_stream_connect(_implPtr->streamPtr.get(), PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params.data(), 1) <
              0)
          {
            optError = Error{.code = Error::Code::InitFailed, .message = "Failed to connect PipeWire stream"};
          }
          else if (::pw_stream_set_active(_implPtr->streamPtr.get(), true) < 0)
          {
            optError = Error{.code = Error::Code::InitFailed, .message = "Failed to activate PipeWire PCM negotiation"};
          }
          else
          {
            // An inactive PipeWire stream is not guaranteed to negotiate its
            // Format param. Activate it while render admission remains closed,
            // then return it to the prepared state before decoder setup.
            auto deadline = ::timespec{};
            ::pw_thread_loop_get_time(_implPtr->threadLoopPtr.get(), &deadline, kOpenTimeoutNanoseconds);

            while (!_implPtr->optOpenError && (_implPtr->format.encoding == SampleEncoding::Unknown ||
                                               (_implPtr->streamState != PW_STREAM_STATE_PAUSED &&
                                                _implPtr->streamState != PW_STREAM_STATE_STREAMING)))
            {
              if (::pw_thread_loop_timed_wait_full(_implPtr->threadLoopPtr.get(), &deadline) < 0)
              {
                optError =
                  Error{.code = Error::Code::InitFailed, .message = "Timed out waiting for PipeWire PCM negotiation"};
                break;
              }
            }

            if (_implPtr->optOpenError)
            {
              optError = _implPtr->optOpenError;
            }
            else if (!optError)
            {
              negotiatedFormat = _implPtr->format;
              optRouteAnchor = std::move(_implPtr->optPendingRouteAnchor);
              _implPtr->applyCachedControls();
              _implPtr->outputControlAvailable.store(!useExclusive, std::memory_order_relaxed);
            }

            if (::pw_stream_set_active(_implPtr->streamPtr.get(), false) < 0 && !optError)
            {
              optError = Error{
                .code = Error::Code::InitFailed, .message = "Failed to suspend PipeWire stream after PCM negotiation"};
            }
          }
        }
      }

      _implPtr->opening = false;
    }

    if (optError)
    {
      close();
      return std::unexpected{std::move(*optError)};
    }

    if (optRouteAnchor && target != nullptr)
    {
      target->handleRouteReady(*optRouteAnchor);
    }

    _implPtr->optLastSourceFormat = sourceFormat;
    _implPtr->optLastNegotiatedFormat = negotiatedFormat;

    // PipeWire negotiates a client stream format; the graph node behind it may
    // resample, remix, or requantize without reporting a direct endpoint. The
    // endpoint therefore stays unconfirmed and only lossless client formats are
    // offered, in both shared and exclusive mode.
    return OpenedPcmMode{.clientFormat = negotiatedFormat};
  }

  void PipeWireBackend::start()
  {
    auto guard = PwThreadLoopGuard{_implPtr->threadLoopPtr.get()};

    if (!_implPtr->streamPtr)
    {
      return;
    }

    _implPtr->drainPending.store(false, std::memory_order_release);
    _implPtr->renderAdmissionOpen.store(true, std::memory_order_release);

    if (::pw_stream_set_active(_implPtr->streamPtr.get(), true) < 0)
    {
      _implPtr->renderAdmissionOpen.store(false, std::memory_order_release);
    }
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
    _implPtr->drainPending.store(false, std::memory_order_release);
    auto guard = PwThreadLoopGuard{_implPtr->threadLoopPtr.get()};

    if (!_implPtr->streamPtr)
    {
      return;
    }

    ::pw_stream_flush(_implPtr->streamPtr.get(), false);
  }

  void PipeWireBackend::stop()
  {
    _implPtr->stopRenderCycles();
  }

  void PipeWireBackend::close()
  {
    _implPtr->stopRenderCycles();
    _implPtr->destroyStream();
    _implPtr->renderTarget = nullptr;
    _implPtr->format = {};
    _implPtr->offeredEncodings.clear();
    _implPtr->opening = false;
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

        // PipeWire's stream-control setter is a variadic C API.
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

        // PipeWire's stream-control setter is a variadic C API.
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
