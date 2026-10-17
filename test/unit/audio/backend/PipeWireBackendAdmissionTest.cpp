// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/PipeWireBackend.h"
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/Property.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

extern "C"
{
#include <pipewire/port.h>
#include <pipewire/stream.h>
#include <spa/buffer/buffer.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/param.h>
#include <spa/pod/builder.h>
#include <spa/pod/pod.h>
#include <spa/utils/hook.h>
}

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string_view>

namespace ao::audio::backend::test
{
  namespace
  {
    // Fixed 16-byte per-slot sentinels. Stereo S16 reads four frames of two
    // slots; quad S16 reads two frames of four slots. The bytes are literals,
    // not a reimplementation of the production permutation.
    constexpr auto kSlotSentinelBytes = std::to_array<std::byte>({std::byte{0x11},
                                                                  std::byte{0x10},
                                                                  std::byte{0x21},
                                                                  std::byte{0x20},
                                                                  std::byte{0x12},
                                                                  std::byte{0x10},
                                                                  std::byte{0x22},
                                                                  std::byte{0x20},
                                                                  std::byte{0x13},
                                                                  std::byte{0x10},
                                                                  std::byte{0x23},
                                                                  std::byte{0x20},
                                                                  std::byte{0x14},
                                                                  std::byte{0x10},
                                                                  std::byte{0x24},
                                                                  std::byte{0x20}});

    // Per-frame exchange of the two S16 slots for native [FR, FL].
    constexpr auto kStereoPermutedBytes = std::to_array<std::byte>({std::byte{0x21},
                                                                    std::byte{0x20},
                                                                    std::byte{0x11},
                                                                    std::byte{0x10},
                                                                    std::byte{0x22},
                                                                    std::byte{0x20},
                                                                    std::byte{0x12},
                                                                    std::byte{0x10},
                                                                    std::byte{0x23},
                                                                    std::byte{0x20},
                                                                    std::byte{0x13},
                                                                    std::byte{0x10},
                                                                    std::byte{0x24},
                                                                    std::byte{0x20},
                                                                    std::byte{0x14},
                                                                    std::byte{0x10}});

    constexpr std::size_t kFormatPodBytes = 1024;
    constexpr char const* kNativeErrorMessage = "PipeWire admission test stream error";

    enum class TerminalObservation : std::uint8_t
    {
      Format,
      Error,
    };

    ::spa_pod const* buildNativeFormat(std::span<std::byte> buffer,
                                       std::uint32_t channels,
                                       std::span<std::uint32_t const> positions) noexcept
    {
      if (buffer.size() < 64 || channels == 0 || positions.size() != channels ||
          positions.size() > SPA_AUDIO_MAX_CHANNELS)
      {
        return nullptr;
      }

      auto info = ::spa_audio_info_raw{};
      info.format = SPA_AUDIO_FORMAT_S16_LE;
      info.rate = 48000;
      info.channels = channels;
      info.flags = 0;
      std::ranges::copy(positions, std::span<std::uint32_t>{info.position}.begin());
      auto builder = ::spa_pod_builder{};
      ::spa_pod_builder_init(&builder, buffer.data(), static_cast<std::uint32_t>(buffer.size()));
      return ::spa_format_audio_raw_build(&builder, SPA_PARAM_Format, &info);
    }

    // Records RenderTarget callbacks only. It does not apply Engine settlement.
    class RecordingRenderTarget final : public RenderTarget
    {
    public:
      explicit RecordingRenderTarget(std::uint32_t positionFrames) noexcept
        : _positionFrames{positionFrames}
      {
      }

      RenderPcmResult renderPcm(std::span<std::byte> output) noexcept override
      {
        ++_renderCount;

        if (output.size() != kSlotSentinelBytes.size())
        {
          ++_shortRenderCount;
          return {};
        }

        std::ranges::copy(kSlotSentinelBytes, output.begin());
        return {.bytesWritten = kSlotSentinelBytes.size(), .positionFrames = _positionFrames};
      }

      void handleUnderrun() noexcept override {}

      void handlePositionAdvanced(std::uint32_t frames) noexcept override
      {
        ++_positionCount;
        _lastPositionFrames = frames;
      }

      void handleDrainComplete() noexcept override { ++_drainCount; }

      void handleRouteReady(std::string_view /*routeAnchor*/) noexcept override {}

      void handleFormatChanged(PcmFormat const& /*format*/) noexcept override {}

      void handlePropertyChanged(PropertySnapshot /*snapshot*/) noexcept override {}

      void handleBackendError(std::string_view message) noexcept override
      {
        ++_errorCount;
        auto const count = std::min(message.size(), _errorMessage.size() - 1);
        std::ranges::copy(message.substr(0, count), _errorMessage.begin());
        _errorMessage[count] = '\0';
        _errorMessageLength = count;
        _errorTruncated = message.size() > count;
      }

      std::size_t renderCount() const noexcept { return _renderCount; }

      std::size_t shortRenderCount() const noexcept { return _shortRenderCount; }

      std::size_t positionCount() const noexcept { return _positionCount; }

      std::uint32_t lastPositionFrames() const noexcept { return _lastPositionFrames; }

      std::size_t drainCount() const noexcept { return _drainCount; }

      std::size_t errorCount() const noexcept { return _errorCount; }

      bool isErrorTruncated() const noexcept { return _errorTruncated; }

      std::string_view errorMessage() const noexcept { return {_errorMessage.data(), _errorMessageLength}; }

    private:
      std::uint32_t _positionFrames = 0;
      std::size_t _renderCount = 0;
      std::size_t _shortRenderCount = 0;
      std::size_t _positionCount = 0;
      std::uint32_t _lastPositionFrames = 0;
      std::size_t _drainCount = 0;
      std::size_t _errorCount = 0;
      std::size_t _errorMessageLength = 0;
      bool _errorTruncated = false;
      std::array<char, 96> _errorMessage{};
    };

    // One armed stream. Not a production registry and not a private Impl view.
    class CallbackSourceScript final
    {
    public:
      CallbackSourceScript() noexcept
      {
        _data.type = SPA_DATA_MemPtr;
        _data.flags = SPA_DATA_FLAG_READWRITE;
        _data.fd = -1;
        _data.maxsize = static_cast<std::uint32_t>(_marker.size());
        _data.data = _marker.data();
        _data.chunk = &_chunk;
        _spaBuffer.n_datas = 1;
        _spaBuffer.datas = &_data;
        _pwBuffer.buffer = &_spaBuffer;
      }

      ~CallbackSourceScript() = default;

      CallbackSourceScript(CallbackSourceScript const&) = delete;
      CallbackSourceScript& operator=(CallbackSourceScript const&) = delete;
      CallbackSourceScript(CallbackSourceScript&&) = delete;
      CallbackSourceScript& operator=(CallbackSourceScript&&) = delete;

      bool tryPrepareOpeningFormats(std::uint32_t channels,
                                    std::span<std::uint32_t const> firstPositions,
                                    std::span<std::uint32_t const> secondPositions) noexcept
      {
        _firstFormat = buildNativeFormat(_firstPod, channels, firstPositions);
        _secondFormat = nullptr;

        if (!secondPositions.empty())
        {
          _secondFormat = buildNativeFormat(_secondPod, channels, secondPositions);
        }

        return _firstFormat != nullptr && (secondPositions.empty() || _secondFormat != nullptr);
      }

      // Claims the first add_listener while armed. Leaves the caller's spa_hook
      // untouched so the zero SpaHookGuard stays a no-op. Does not register a
      // native listener.
      bool tryCaptureListener(::pw_stream* stream, ::pw_stream_events const* events, void* data) noexcept
      {
        if (_retired)
        {
          return false;
        }

        if (stream == nullptr || events == nullptr)
        {
          ++_unexpectedCallCount;
          return true;
        }

        if (_ownedStream == nullptr)
        {
          _ownedStream = stream;
          _events = *events;
          _opaqueData = data;
          ++_listenerCount;
          _hasParamChanged = events->param_changed != nullptr;
          _hasStateChanged = events->state_changed != nullptr;
          _hasProcess = events->process != nullptr;
          return true;
        }

        if (stream == _ownedStream)
        {
          ++_unexpectedCallCount;
          return true;
        }

        return false;
      }

      bool tryConnect(::pw_stream* stream, int& result) noexcept
      {
        if (_retired || !isOwnedStream(stream))
        {
          return false;
        }

        ++_connectCount;

        if (_connectCount == 1)
        {
          deliverFormat(_firstFormat);
        }
        else
        {
          ++_unexpectedCallCount;
        }

        result = 0;
        return true;
      }

      bool trySetActive(::pw_stream* stream, bool active, int& result) noexcept
      {
        if (_retired || !isOwnedStream(stream))
        {
          return false;
        }

        if (active)
        {
          ++_trueActivationCount;

          // One-shot: the open negotiation activation may publish the second
          // observation. A later start must not replay it.
          if (!_activationObservationDelivered)
          {
            _activationObservationDelivered = true;

            if (_secondFormat != nullptr)
            {
              deliverFormat(_secondFormat);
            }

            deliverPaused();
          }

          result = 0;
          return true;
        }

        result = 0;
        return true;
      }

      bool tryDequeue(::pw_stream* stream, ::pw_buffer*& buffer) noexcept
      {
        if (_retired || !isOwnedStream(stream))
        {
          return false;
        }

        ++_dequeueCount;
        buffer = &_pwBuffer;
        return true;
      }

      bool tryQueue(::pw_stream* stream, ::pw_buffer* buffer, int& result) noexcept
      {
        if (_retired || !isOwnedStream(stream))
        {
          return false;
        }

        if (buffer != &_pwBuffer || buffer->buffer == nullptr || buffer->buffer->n_datas < 1 ||
            buffer->buffer->datas == nullptr || buffer->buffer->datas[0].chunk == nullptr ||
            buffer->buffer->datas[0].data == nullptr)
        {
          ++_unexpectedCallCount;
          result = -1;
          return true;
        }

        auto const* chunk = buffer->buffer->datas[0].chunk;
        auto const* bytes = static_cast<std::byte const*>(buffer->buffer->datas[0].data);
        auto const copyCount = std::min(static_cast<std::size_t>(chunk->size), _queuedBytes.size());
        std::ranges::copy(std::span<std::byte const>{bytes, copyCount}, _queuedBytes.begin());
        _queuedOffset = chunk->offset;
        _queuedSize = chunk->size;
        _queuedStride = chunk->stride;
        ++_queueCount;
        result = 0;
        return true;
      }

      // External callback SOURCE fake: this invokes the captured compiled
      // events.process on the test thread. It is ABI wiring only, not a native
      // realtime-affinity, scheduling, or SDK-fence test.
      void invokeProcess() noexcept
      {
        if (_retired || _events.process == nullptr || _opaqueData == nullptr)
        {
          ++_unexpectedCallCount;
          return;
        }

        ++_processDispatchCount;
        _events.process(_opaqueData);
      }

      void invokeFormat(::spa_pod const* pod) noexcept { deliverFormat(pod); }

      void invokePaused() noexcept { deliverPaused(); }

      void invokeError(char const* message) noexcept
      {
        if (_retired || _events.state_changed == nullptr || _opaqueData == nullptr || message == nullptr)
        {
          ++_unexpectedCallCount;
          return;
        }

        _events.state_changed(_opaqueData, PW_STREAM_STATE_PAUSED, PW_STREAM_STATE_ERROR, message);
      }

      // Drops captured callbacks after close. Does not rebind another stream.
      void retire() noexcept
      {
        _retired = true;
        _events = {};
        _opaqueData = nullptr;
        _ownedStream = nullptr;
        _firstFormat = nullptr;
        _secondFormat = nullptr;
      }

      ::spa_pod const* openingFormat() const noexcept { return _firstFormat; }

      std::size_t listenerCount() const noexcept { return _listenerCount; }

      bool hasParamChanged() const noexcept { return _hasParamChanged; }

      bool hasStateChanged() const noexcept { return _hasStateChanged; }

      bool hasProcess() const noexcept { return _hasProcess; }

      std::uint32_t eventsVersion() const noexcept { return _events.version; }

      std::size_t connectCount() const noexcept { return _connectCount; }

      std::size_t trueActivationCount() const noexcept { return _trueActivationCount; }

      std::size_t dequeueCount() const noexcept { return _dequeueCount; }

      std::size_t queueCount() const noexcept { return _queueCount; }

      std::size_t processDispatchCount() const noexcept { return _processDispatchCount; }

      std::size_t unexpectedCallCount() const noexcept { return _unexpectedCallCount; }

      std::uint32_t queuedOffset() const noexcept { return _queuedOffset; }

      std::uint32_t queuedSize() const noexcept { return _queuedSize; }

      std::int32_t queuedStride() const noexcept { return _queuedStride; }

      std::array<std::byte, 16> const& queuedBytes() const noexcept { return _queuedBytes; }

    private:
      bool isOwnedStream(::pw_stream* stream) const noexcept
      {
        return _ownedStream != nullptr && stream == _ownedStream;
      }

      void deliverFormat(::spa_pod const* pod) noexcept
      {
        if (_retired || _events.param_changed == nullptr || pod == nullptr || _opaqueData == nullptr)
        {
          ++_unexpectedCallCount;
          return;
        }

        _events.param_changed(_opaqueData, SPA_PARAM_Format, pod);
      }

      void deliverPaused() noexcept
      {
        if (_retired || _events.state_changed == nullptr || _opaqueData == nullptr)
        {
          ++_unexpectedCallCount;
          return;
        }

        _events.state_changed(_opaqueData, PW_STREAM_STATE_UNCONNECTED, PW_STREAM_STATE_PAUSED, nullptr);
      }

      ::pw_stream* _ownedStream = nullptr;
      ::pw_stream_events _events{};
      void* _opaqueData = nullptr;
      bool _retired = false;
      bool _hasParamChanged = false;
      bool _hasStateChanged = false;
      bool _hasProcess = false;
      bool _activationObservationDelivered = false;
      std::size_t _listenerCount = 0;
      std::size_t _connectCount = 0;
      std::size_t _trueActivationCount = 0;
      std::size_t _dequeueCount = 0;
      std::size_t _queueCount = 0;
      std::size_t _processDispatchCount = 0;
      std::size_t _unexpectedCallCount = 0;
      std::uint32_t _queuedOffset = 0;
      std::uint32_t _queuedSize = 0;
      std::int32_t _queuedStride = 0;
      std::array<std::byte, kFormatPodBytes> _firstPod{};
      std::array<std::byte, kFormatPodBytes> _secondPod{};
      ::spa_pod const* _firstFormat = nullptr;
      ::spa_pod const* _secondFormat = nullptr;
      std::array<std::byte, 16> _marker{};
      std::array<std::byte, 16> _queuedBytes{};
      ::spa_chunk _chunk{};
      ::spa_data _data{};
      ::spa_buffer _spaBuffer{};
      ::pw_buffer _pwBuffer{};
    };

    CallbackSourceScript*& activeCallbackSource() noexcept
    {
      thread_local CallbackSourceScript* script = nullptr;
      return script;
    }

    class [[nodiscard]] ScriptArm final
    {
    public:
      explicit ScriptArm(CallbackSourceScript& script) noexcept
        : _previous{activeCallbackSource()}
      {
        activeCallbackSource() = &script;
      }

      ~ScriptArm() noexcept { activeCallbackSource() = _previous; }

      ScriptArm(ScriptArm const&) = delete;
      ScriptArm& operator=(ScriptArm const&) = delete;
      ScriptArm(ScriptArm&&) = delete;
      ScriptArm& operator=(ScriptArm&&) = delete;

    private:
      CallbackSourceScript* _previous = nullptr;
    };

    Device unroutedDevice()
    {
      return {.id = DeviceId{}, .displayName = "Unrouted PipeWire admission", .backendId = kBackendPipeWire};
    }

    void requirePipeWireOpen(BackendOpenResult const& openedRes)
    {
      if (!openedRes && openedRes.error().error.message == "PipeWire not initialized")
      {
        if (auto const* required = std::getenv("AOBUS_REQUIRE_PIPEWIRE");
            required == nullptr || std::string_view{required} != "1")
        {
          SKIP("PipeWire not initialized");
        }
      }

      if (!openedRes)
      {
        INFO(openedRes.error().error.message);
      }

      REQUIRE(openedRes);
    }

    void requireCapturedEvents(CallbackSourceScript const& script)
    {
      REQUIRE(script.listenerCount() == 1);
      REQUIRE(script.eventsVersion() == PW_VERSION_STREAM_EVENTS);
      REQUIRE(script.hasParamChanged());
      REQUIRE(script.hasStateChanged());
      REQUIRE(script.hasProcess());
      CHECK(script.connectCount() == 1);
      CHECK(script.trueActivationCount() == 1);
      CHECK(script.unexpectedCallCount() == 0);
    }
  } // namespace

  // ELF --wrap owns these five executable bindings. The asm labels attach
  // ordinary C++ names to those ELF symbols. Reserved linker spellings stay in
  // the label strings and are not C++ identifiers.
  extern "C"
  {
    void nativeStreamAddListener(::pw_stream* stream,
                                 ::spa_hook* listener,
                                 ::pw_stream_events const* events,
                                 void* data) asm("__real_pw_stream_add_listener");
    void scriptedStreamAddListener(::pw_stream* stream,
                                   ::spa_hook* listener,
                                   ::pw_stream_events const* events,
                                   void* data) asm("__wrap_pw_stream_add_listener");
    int nativeStreamConnect(::pw_stream* stream,
                            ::pw_direction direction,
                            std::uint32_t targetId,
                            ::pw_stream_flags flags,
                            ::spa_pod const** params,
                            std::uint32_t paramCount) asm("__real_pw_stream_connect");
    int scriptedStreamConnect(::pw_stream* stream,
                              ::pw_direction direction,
                              std::uint32_t targetId,
                              ::pw_stream_flags flags,
                              ::spa_pod const** params,
                              std::uint32_t paramCount) asm("__wrap_pw_stream_connect");
    int nativeStreamSetActive(::pw_stream* stream, bool active) asm("__real_pw_stream_set_active");
    int scriptedStreamSetActive(::pw_stream* stream, bool active) asm("__wrap_pw_stream_set_active");
    ::pw_buffer* nativeStreamDequeueBuffer(::pw_stream* stream) asm("__real_pw_stream_dequeue_buffer");
    ::pw_buffer* scriptedStreamDequeueBuffer(::pw_stream* stream) asm("__wrap_pw_stream_dequeue_buffer");
    int nativeStreamQueueBuffer(::pw_stream* stream, ::pw_buffer* buffer) asm("__real_pw_stream_queue_buffer");
    int scriptedStreamQueueBuffer(::pw_stream* stream, ::pw_buffer* buffer) asm("__wrap_pw_stream_queue_buffer");

    void scriptedStreamAddListener(::pw_stream* stream,
                                   ::spa_hook* listener,
                                   ::pw_stream_events const* events,
                                   void* data)
    {
      if (activeCallbackSource() != nullptr && activeCallbackSource()->tryCaptureListener(stream, events, data))
      {
        return;
      }

      nativeStreamAddListener(stream, listener, events, data);
    }

    int scriptedStreamConnect(::pw_stream* stream,
                              ::pw_direction direction,
                              std::uint32_t targetId,
                              ::pw_stream_flags flags,
                              ::spa_pod const** params,
                              std::uint32_t paramCount)
    {
      if (std::int32_t status = 0;
          activeCallbackSource() != nullptr && activeCallbackSource()->tryConnect(stream, status))
      {
        return status;
      }

      return nativeStreamConnect(stream, direction, targetId, flags, params, paramCount);
    }

    int scriptedStreamSetActive(::pw_stream* stream, bool active)
    {
      if (std::int32_t status = 0;
          activeCallbackSource() != nullptr && activeCallbackSource()->trySetActive(stream, active, status))
      {
        return status;
      }

      return nativeStreamSetActive(stream, active);
    }

    ::pw_buffer* scriptedStreamDequeueBuffer(::pw_stream* stream)
    {
      if (::pw_buffer* buffer = nullptr;
          activeCallbackSource() != nullptr && activeCallbackSource()->tryDequeue(stream, buffer))
      {
        return buffer;
      }

      return nativeStreamDequeueBuffer(stream);
    }

    int scriptedStreamQueueBuffer(::pw_stream* stream, ::pw_buffer* buffer)
    {
      if (std::int32_t status = 0;
          activeCallbackSource() != nullptr && activeCallbackSource()->tryQueue(stream, buffer, status))
      {
        return status;
      }

      return nativeStreamQueueBuffer(stream, buffer);
    }
  }

  TEST_CASE("PipeWireBackend - terminal native callbacks revoke rendering and block restart",
            "[audio][integration][pipewire]")
  {
    auto terminalObservation = TerminalObservation::Format;
    auto expectedError = std::string_view{};

    SECTION("terminal actual Format")
    {
      terminalObservation = TerminalObservation::Format;
      expectedError = "PipeWire changed PCM mode after stream activation";
    }

    SECTION("native ERROR")
    {
      terminalObservation = TerminalObservation::Error;
      expectedError = kNativeErrorMessage;
    }

    // Mask 0x3 declares FL/FR. It is not a claim that every two-channel cohort
    // offers FL/FR: mask 0 alone offers that default, and a known 0x30 source
    // would declare RL/RR and accept a same-set reorder. Native [FR, FL] is
    // this source's set in reverse slot order. [RL, RR] arrives after that
    // admission, so the sticky failure is the post-admission PCM mode change,
    // not the initial exact-offer position conflict.
    auto const admittedRoles = std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_FL});
    auto script = CallbackSourceScript{};
    REQUIRE(script.tryPrepareOpeningFormats(2, admittedRoles, {}));
    auto const arm = ScriptArm{script};
    auto target = RecordingRenderTarget{4};
    auto backend = PipeWireBackend{unroutedDevice(), kProfileShared};
    auto const source = SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .speakerMask = 0x3};
    auto const openedRes = backend.open(source, target);
    requirePipeWireOpen(openedRes);
    CHECK(openedRes->clientFormat.sampleRate == 48000);
    CHECK(openedRes->clientFormat.channels == 2);
    CHECK(openedRes->clientFormat.encoding == SampleEncoding::Signed16Le);
    CHECK(openedRes->clientFormat.speakerMask == 0x3);
    CHECK(openedRes->channelDelivery.isMapped);
    CHECK(openedRes->channelDelivery.nativeSpeakerMask == 0x3);
    CHECK_FALSE(openedRes->optEndpoint);
    requireCapturedEvents(script);

    auto const dispatchBeforeStart = script.processDispatchCount();
    auto const dequeueBeforeStart = script.dequeueCount();
    script.invokeProcess();
    CHECK(script.processDispatchCount() == dispatchBeforeStart + 1);
    CHECK(script.dequeueCount() == dequeueBeforeStart);
    CHECK(target.renderCount() == 0);
    CHECK(script.queueCount() == 0);

    auto const activationBeforeStart = script.trueActivationCount();
    backend.start();
    CHECK(script.trueActivationCount() == activationBeforeStart + 1);
    script.invokeProcess();
    CHECK(script.processDispatchCount() == dispatchBeforeStart + 2);
    CHECK(script.dequeueCount() == dequeueBeforeStart + 1);
    CHECK(target.renderCount() == 1);
    CHECK(target.shortRenderCount() == 0);
    CHECK(script.queueCount() == 1);
    CHECK(script.queuedOffset() == 0);
    CHECK(script.queuedSize() == kSlotSentinelBytes.size());
    CHECK(script.queuedStride() == 4);
    CHECK(script.queuedBytes() == kStereoPermutedBytes);
    CHECK(target.positionCount() == 1);
    CHECK(target.lastPositionFrames() == 4);
    CHECK(target.drainCount() == 0);

    auto const errorsBeforeFault = target.errorCount();

    if (terminalObservation == TerminalObservation::Format)
    {
      auto podBuffer = std::array<std::byte, kFormatPodBytes>{};
      auto const conflictingRoles = std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR});
      auto const* pod = buildNativeFormat(podBuffer, 2, conflictingRoles);
      REQUIRE(pod != nullptr);
      script.invokeFormat(pod);
    }
    else
    {
      script.invokeError(kNativeErrorMessage);
    }

    CHECK(target.errorCount() == errorsBeforeFault + 1);
    CHECK_FALSE(target.isErrorTruncated());
    CHECK(target.errorMessage() == expectedError);

    auto const dispatchAfterFault = script.processDispatchCount();
    auto const dequeueAfterFault = script.dequeueCount();
    auto const renderAfterFault = target.renderCount();
    auto const queueAfterFault = script.queueCount();
    script.invokeProcess();
    CHECK(script.processDispatchCount() == dispatchAfterFault + 1);
    CHECK(script.dequeueCount() == dequeueAfterFault);
    CHECK(target.renderCount() == renderAfterFault);
    CHECK(script.queueCount() == queueAfterFault);

    auto const activationAfterFault = script.trueActivationCount();
    backend.start();
    CHECK(script.trueActivationCount() == activationAfterFault);
    script.invokeProcess();
    CHECK(script.processDispatchCount() == dispatchAfterFault + 2);
    CHECK(script.dequeueCount() == dequeueAfterFault);
    CHECK(target.renderCount() == renderAfterFault);
    CHECK(script.queueCount() == queueAfterFault);

    // The same accepted format plus PAUSED must not clear the sticky failure.
    // A later accepted format may notify again; do not pin that count.
    script.invokeFormat(script.openingFormat());
    script.invokePaused();
    backend.start();
    CHECK(script.trueActivationCount() == activationAfterFault);
    script.invokeProcess();
    CHECK(script.processDispatchCount() == dispatchAfterFault + 3);
    CHECK(script.dequeueCount() == dequeueAfterFault);
    CHECK(target.renderCount() == renderAfterFault);
    CHECK(script.queueCount() == queueAfterFault);
    CHECK(script.unexpectedCallCount() == 0);

    backend.close();
    script.retire();
  }

  TEST_CASE("PipeWireBackend - open reports the latest accepted roles without changing zero-source transport",
            "[audio][integration][pipewire]")
  {
    auto secondPositions = std::array<std::uint32_t, 4>{};
    std::uint32_t expectedNativeMask = 0;
    bool expectedMapped = false;

    SECTION("second complete changed WAVE set")
    {
      secondPositions = std::to_array<std::uint32_t>(
        {SPA_AUDIO_CHANNEL_FC, SPA_AUDIO_CHANNEL_LFE, SPA_AUDIO_CHANNEL_SL, SPA_AUDIO_CHANNEL_SR});
      expectedNativeMask = 0x60c;
      expectedMapped = true;
    }

    SECTION("second CUSTOM/unknown")
    {
      secondPositions = std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_START_Custom,
                                                      SPA_AUDIO_CHANNEL_START_Custom + 1U,
                                                      SPA_AUDIO_CHANNEL_START_Custom + 2U,
                                                      SPA_AUDIO_CHANNEL_START_Custom + 3U});
      expectedNativeMask = 0;
      expectedMapped = false;
    }

    auto const firstPositions = std::to_array<std::uint32_t>(
      {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR});
    auto script = CallbackSourceScript{};
    REQUIRE(script.tryPrepareOpeningFormats(4, firstPositions, secondPositions));
    auto const arm = ScriptArm{script};
    auto target = RecordingRenderTarget{2};
    auto backend = PipeWireBackend{unroutedDevice(), kProfileShared};
    auto const source = SignalFormat{.sampleRate = 48000, .channels = 4, .precisionBits = 16, .speakerMask = 0};
    auto const openedRes = backend.open(source, target);
    requirePipeWireOpen(openedRes);
    CHECK(openedRes->clientFormat.sampleRate == 48000);
    CHECK(openedRes->clientFormat.channels == 4);
    CHECK(openedRes->clientFormat.encoding == SampleEncoding::Signed16Le);
    CHECK(openedRes->clientFormat.speakerMask == 0);
    CHECK(openedRes->channelDelivery.nativeSpeakerMask == expectedNativeMask);
    CHECK(openedRes->channelDelivery.nativeSpeakerMask != 0x33);
    CHECK(openedRes->channelDelivery.isMapped == expectedMapped);
    CHECK_FALSE(openedRes->optEndpoint);
    requireCapturedEvents(script);

    auto const dispatchBeforeStart = script.processDispatchCount();
    script.invokeProcess();
    CHECK(script.processDispatchCount() == dispatchBeforeStart + 1);
    CHECK(script.dequeueCount() == 0);
    CHECK(target.renderCount() == 0);
    CHECK(script.queueCount() == 0);

    backend.start();
    CHECK(script.trueActivationCount() == 2);
    script.invokeProcess();
    CHECK(script.processDispatchCount() == dispatchBeforeStart + 2);
    CHECK(script.dequeueCount() == 1);
    CHECK(target.renderCount() == 1);
    CHECK(target.shortRenderCount() == 0);
    CHECK(script.queueCount() == 1);
    CHECK(script.queuedOffset() == 0);
    CHECK(script.queuedSize() == kSlotSentinelBytes.size());
    CHECK(script.queuedStride() == 8);
    CHECK(script.queuedBytes() == kSlotSentinelBytes);
    CHECK(target.positionCount() == 1);
    CHECK(target.lastPositionFrames() == 2);
    CHECK(target.drainCount() == 0);
    CHECK(script.unexpectedCallCount() == 0);

    backend.close();
    script.retire();
  }
} // namespace ao::audio::backend::test
