// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CoreAudioBackend.h"

#include "detail/AudioBackendDrainTail.h"
#include "detail/AudioBackendRenderBuffer.h"
#include "detail/BackendGraphRegistry.h"
#include "detail/CallbackFence.h"
#include "detail/CoreAudioDeviceDiscovery.h"
#include "detail/CoreAudioError.h"
#include "detail/CoreAudioFormat.h"
#include "detail/CoreAudioGraph.h"
#include "detail/CoreAudioLatency.h"
#include "detail/CoreAudioRenderBuffer.h"
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
#include <ao/audio/SignalFormat.h>
#include <ao/utility/CallbackStackScope.h>
#include <ao/utility/ThreadName.h>

#include <AudioToolbox/AUComponent.h>
#include <AudioToolbox/AudioComponent.h>
#include <AudioToolbox/AudioOutputUnit.h>
#include <AudioToolbox/AudioUnit.h>
#include <AudioToolbox/AudioUnitProperties.h>
#include <CoreAudio/AudioHardware.h>
#include <CoreAudio/AudioHardwareBase.h>
#include <CoreAudioTypes/CoreAudioBaseTypes.h>
#include <MacTypes.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <expected>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace ao::audio::backend
{
  namespace
  {
    constexpr ::UInt32 kMinimumMaximumFramesPerSlice = 4096U;
    constexpr auto kGlobalMain = ::AudioObjectPropertyAddress{.mSelector = 0U,
                                                              .mScope = ::kAudioObjectPropertyScopeGlobal,
                                                              .mElement = ::kAudioObjectPropertyElementMain};
    constexpr auto kOutputMain = ::AudioObjectPropertyAddress{.mSelector = 0U,
                                                              .mScope = ::kAudioObjectPropertyScopeOutput,
                                                              .mElement = ::kAudioObjectPropertyElementMain};
    constexpr auto kDevicePropertyAddresses =
      std::array{::AudioObjectPropertyAddress{.mSelector = ::kAudioDevicePropertyDeviceIsAlive,
                                              .mScope = ::kAudioObjectPropertyScopeGlobal,
                                              .mElement = ::kAudioObjectPropertyElementMain},
                 ::AudioObjectPropertyAddress{.mSelector = ::kAudioDevicePropertyNominalSampleRate,
                                              .mScope = ::kAudioObjectPropertyScopeGlobal,
                                              .mElement = ::kAudioObjectPropertyElementMain},
                 ::AudioObjectPropertyAddress{.mSelector = ::kAudioDevicePropertyStreamConfiguration,
                                              .mScope = ::kAudioObjectPropertyScopeOutput,
                                              .mElement = ::kAudioObjectPropertyElementMain}};

    bool canAdmitSourceFormat(SignalFormat const& sourceFormat) noexcept
    {
      return sourceFormat.sampleRate != 0U && sourceFormat.channels != 0U &&
             ::ao::audio::detail::preferredLosslessPcmEncoding(sourceFormat).has_value();
    }

    enum class CoreAudioFault : std::uint8_t
    {
      None,
      DeviceLost,
      RenderBufferShape,
      RenderFrameLimit,
      RenderTargetMissing,
    };

    class CallbackLeave final
    {
    public:
      explicit CallbackLeave(detail::CallbackFence& fence) noexcept
        : _fence{fence}
      {
      }

      ~CallbackLeave() { _fence.leave(); }

      CallbackLeave(CallbackLeave const&) = delete;
      CallbackLeave& operator=(CallbackLeave const&) = delete;
      CallbackLeave(CallbackLeave&&) = delete;
      CallbackLeave& operator=(CallbackLeave&&) = delete;

    private:
      detail::CallbackFence& _fence;
    };

    class AudioUnitOwner final
    {
    public:
      explicit AudioUnitOwner(::AudioUnit unit) noexcept
        : _unit{unit}
      {
      }

      ~AudioUnitOwner()
      {
        if (_unit != nullptr)
        {
          if (_initialized)
          {
            ::AudioUnitUninitialize(_unit);
          }

          ::AudioComponentInstanceDispose(_unit);
        }
      }

      AudioUnitOwner(AudioUnitOwner const&) = delete;
      AudioUnitOwner& operator=(AudioUnitOwner const&) = delete;
      AudioUnitOwner(AudioUnitOwner&&) = delete;
      AudioUnitOwner& operator=(AudioUnitOwner&&) = delete;

      void markInitialized() noexcept { _initialized = true; }

      ::AudioUnit release() noexcept
      {
        auto* const result = _unit;
        _unit = nullptr;
        _initialized = false;
        return result;
      }

    private:
      ::AudioUnit _unit = nullptr;
      bool _initialized = false;
    };

    template<typename Value>
    ::OSStatus readAudioObjectValue(::AudioObjectID const object,
                                    ::AudioObjectPropertyAddress address,
                                    Value& value) noexcept
    {
      auto size = static_cast<::UInt32>(sizeof(Value));
      auto const status =
        ::AudioObjectGetPropertyData(object, &address, 0U, nullptr, &size, static_cast<void*>(std::addressof(value)));
      return status == ::noErr && size != sizeof(Value) ? ::kAudioHardwareBadPropertySizeError : status;
    }

    template<typename Value>
    ::OSStatus readAudioUnitProperty(::AudioUnit const unit,
                                     ::AudioUnitPropertyID const property,
                                     ::AudioUnitScope const scope,
                                     ::AudioUnitElement const element,
                                     Value& value) noexcept
    {
      auto size = static_cast<::UInt32>(sizeof(Value));
      auto const status = ::AudioUnitGetProperty(unit, property, scope, element, &value, &size);
      return status == ::noErr && size != sizeof(Value) ? ::kAudioHardwareBadPropertySizeError : status;
    }

    // One native attempt to install a client format on the HAL output unit and
    // read the applied stream format back. A track-incompatible rejection keeps
    // the caller's candidate walk alive; every other failure is terminal.
    std::expected<::AudioStreamBasicDescription, BackendOpenFailure> tryCoreAudioClientFormat(
      ::AudioUnit const unit,
      ::AudioStreamBasicDescription const& description)
    {
      if (auto const status = ::AudioUnitSetProperty(
            unit, ::kAudioUnitProperty_StreamFormat, ::kAudioUnitScope_Input, 0U, &description, sizeof(description));
          status != ::noErr)
      {
        return std::unexpected{BackendOpenFailure{
          .error =
            detail::makeCoreAudioError(status, "configure Core Audio client format", Error::Code::InitFailed).error(),
          .cause = detail::coreAudioClientFormatSetFailureCause(status)}};
      }

      auto readBack = ::AudioStreamBasicDescription{};

      if (auto const status =
            readAudioUnitProperty(unit, ::kAudioUnitProperty_StreamFormat, ::kAudioUnitScope_Input, 0U, readBack);
          status != ::noErr)
      {
        return std::unexpected{BackendOpenFailure{
          .error = detail::makeCoreAudioError(status, "read Core Audio client format", Error::Code::InitFailed).error(),
          .cause = BackendOpenFailure::Cause::OutputFailure}};
      }

      return readBack;
    }

    std::vector<::AudioObjectID> outputStreams(::AudioObjectID const device)
    {
      auto address = kOutputMain;
      address.mSelector = ::kAudioDevicePropertyStreams;
      ::UInt32 byteCount = 0U;

      if (::AudioObjectGetPropertyDataSize(device, &address, 0U, nullptr, &byteCount) != ::noErr ||
          byteCount % sizeof(::AudioObjectID) != 0U)
      {
        return {};
      }

      auto streams = std::vector<::AudioObjectID>(byteCount / sizeof(::AudioObjectID));

      if (byteCount != 0U &&
          ::AudioObjectGetPropertyData(device, &address, 0U, nullptr, &byteCount, streams.data()) != ::noErr)
      {
        return {};
      }

      streams.resize(byteCount / sizeof(::AudioObjectID));
      return streams;
    }

    std::uint64_t scalarFrames(::AudioObjectID const object,
                               ::AudioObjectPropertySelector const selector,
                               ::AudioObjectPropertyScope const scope) noexcept
    {
      auto address = ::AudioObjectPropertyAddress{
        .mSelector = selector, .mScope = scope, .mElement = ::kAudioObjectPropertyElementMain};
      ::UInt32 frames = 0U;
      return readAudioObjectValue(object, address, frames) == ::noErr ? frames : 0U;
    }

    Result<std::uint64_t> presentationTailFrames(::AudioUnit const unit,
                                                 ::AudioObjectID const device,
                                                 ::AudioStreamBasicDescription const& deviceFormat,
                                                 std::uint32_t const clientSampleRate)
    {
      std::uint64_t streamLatency = 0U;

      for (auto const stream : outputStreams(device))
      {
        streamLatency = std::max(
          streamLatency, scalarFrames(stream, ::kAudioStreamPropertyLatency, ::kAudioObjectPropertyScopeGlobal));
      }

      ::Float64 deviceSampleRate = deviceFormat.mSampleRate;
      auto sampleRateAddress = kGlobalMain;
      sampleRateAddress.mSelector = ::kAudioDevicePropertyNominalSampleRate;
      readAudioObjectValue(device, sampleRateAddress, deviceSampleRate);

      ::Float64 audioUnitLatency = 0.0;
      readAudioUnitProperty(unit, ::kAudioUnitProperty_Latency, ::kAudioUnitScope_Global, 0U, audioUnitLatency);

      return detail::coreAudioPresentationTailFrames(
        {.ioBufferFrames =
           scalarFrames(device, ::kAudioDevicePropertyBufferFrameSize, ::kAudioObjectPropertyScopeGlobal),
         .safetyOffsetFrames =
           scalarFrames(device, ::kAudioDevicePropertySafetyOffset, ::kAudioObjectPropertyScopeOutput),
         .deviceLatencyFrames = scalarFrames(device, ::kAudioDevicePropertyLatency, ::kAudioObjectPropertyScopeOutput),
         .streamLatencyFrames = streamLatency,
         .audioUnitLatencySeconds = audioUnitLatency,
         .deviceSampleRate = deviceSampleRate,
         .clientSampleRate = clientSampleRate});
    }

    void silence(::AudioBufferList* const buffers) noexcept
    {
      if (buffers == nullptr)
      {
        return;
      }

      for (::UInt32 index = 0U; index < buffers->mNumberBuffers; ++index)
      {
        if (auto& buffer = buffers->mBuffers[index]; buffer.mData != nullptr && buffer.mDataByteSize != 0U)
        {
          std::memset(buffer.mData, 0, buffer.mDataByteSize);
        }
      }
    }

    std::string_view faultMessage(CoreAudioFault const fault) noexcept
    {
      switch (fault)
      {
        case CoreAudioFault::DeviceLost: return "Core Audio output device was disconnected";
        case CoreAudioFault::RenderBufferShape: return "Core Audio supplied an incompatible render buffer";
        case CoreAudioFault::RenderFrameLimit: return "Core Audio exceeded the configured render frame limit";
        case CoreAudioFault::RenderTargetMissing: return "Core Audio render callback has no target";
        case CoreAudioFault::None: return "Core Audio stream failed";
      }

      return "Core Audio stream failed";
    }

    struct CoreAudioRuntimeState final
    {
      CoreAudioRuntimeState(Device const& device, std::shared_ptr<detail::BackendGraphRegistry> graphRegistryPtr)
        : deviceUid{device.id.raw()}
        , deviceName{device.displayName.empty() ? device.id.raw() : device.displayName}
        , graphRegistryPtr{std::move(graphRegistryPtr)}
      {
      }

      std::string deviceUid{};
      std::string deviceName{};
      std::shared_ptr<detail::BackendGraphRegistry> graphRegistryPtr{};

      mutable std::mutex nativeMutex{};
      ::AudioUnit unit = nullptr;
      ::AudioObjectID deviceId = ::kAudioObjectUnknown;
      bool started = false;
      std::size_t registeredDeviceListenerCount = 0U;

      detail::CallbackFence renderFence{};
      detail::CallbackFence deviceListenerFence{};
      RenderTarget* renderTarget = nullptr;
      // Recursive for supported non-render run controls, not self-callback teardown.
      std::recursive_mutex targetCallbackMutex{};
      // Protected by targetCallbackMutex; run-only changes do not retire the attachment.
      std::uint64_t targetGeneration = 0U;

      std::vector<std::byte> stagingBuffer{};
      std::size_t bytesPerFrame = 0U;
      ::UInt32 maximumFramesPerSlice = 0U;
      std::atomic<std::uint64_t> presentationTailFrameCount{0U};
      detail::AudioBackendDrainTail drainTail{};

      mutable std::mutex graphPublicationMutex{};
      mutable std::mutex graphStateMutex{};
      std::optional<PcmFormat> optClientFormat{};
      std::optional<SignalFormat> optDeviceFormat{};
      bool graphPublished = false;

      mutable std::mutex propertyMutex{};
      float cachedVolume = 1.0F;
      bool cachedMuted = false;

      std::atomic<std::uint64_t> generation{1U};
      std::atomic<bool> runActive{false};
      std::atomic<bool> renderAllowed{false};
      std::atomic<std::uint64_t> pendingDrainGeneration{0U};
      std::atomic<bool> pendingGraphPublication{false};
      std::atomic<std::uint64_t> pendingDeviceGeneration{0U};
      std::atomic<std::uint64_t> pendingFault{0U};
      std::atomic<bool> controlStop{false};
      std::counting_semaphore<> controlSignal{0};
      std::uint64_t deliveredDrainGeneration = 0U;
      std::uint64_t deliveredErrorGeneration = 0U;

      void signalDrainReady(std::uint64_t drainGeneration) noexcept;
      void signalDeviceChange() noexcept;
      void signalFault(CoreAudioFault fault) noexcept;
      void requestGraphPublication() noexcept;
      void controlLoop();
      void handleDrain(std::uint64_t drainGeneration);
      void handleDeviceChange(std::uint64_t deviceGeneration);
      void handleFault(std::uint64_t encodedFault);
      void publishGraph() const;
      void reportError(std::uint64_t errorGeneration, std::string_view message);
    };

    // Caller holds targetCallbackMutex while observing the borrowed target.
    bool isCurrentOpenTarget(CoreAudioRuntimeState const& state,
                             std::uint64_t const openGeneration,
                             RenderTarget const& target) noexcept
    {
      return state.targetGeneration == openGeneration && state.renderTarget == &target;
    }

    void CoreAudioRuntimeState::signalDrainReady(std::uint64_t const drainGeneration) noexcept
    {
      if (std::uint64_t expected = 0U; pendingDrainGeneration.compare_exchange_strong(
            expected, drainGeneration, std::memory_order_release, std::memory_order_relaxed))
      {
        controlSignal.release();
      }
    }

    void CoreAudioRuntimeState::signalDeviceChange() noexcept
    {
      pendingDeviceGeneration.store(generation.load(std::memory_order_acquire), std::memory_order_release);
      controlSignal.release();
    }

    void CoreAudioRuntimeState::signalFault(CoreAudioFault const fault) noexcept
    {
      auto const currentGeneration = generation.load(std::memory_order_acquire);
      auto const encoded = (currentGeneration << 8U) | static_cast<std::uint8_t>(fault);

      if (std::uint64_t expected = 0U;
          pendingFault.compare_exchange_strong(expected, encoded, std::memory_order_release, std::memory_order_relaxed))
      {
        controlSignal.release();
      }
    }

    void CoreAudioRuntimeState::requestGraphPublication() noexcept
    {
      if (graphRegistryPtr && !pendingGraphPublication.exchange(true, std::memory_order_acq_rel))
      {
        controlSignal.release();
      }
    }

    void CoreAudioRuntimeState::controlLoop()
    {
      setCurrentThreadName("CoreAudioControl");

      while (!controlStop.load(std::memory_order_acquire))
      {
        controlSignal.acquire();

        if (controlStop.load(std::memory_order_acquire))
        {
          return;
        }

        if (auto const deviceGeneration = pendingDeviceGeneration.exchange(0U, std::memory_order_acq_rel);
            deviceGeneration != 0U)
        {
          handleDeviceChange(deviceGeneration);
        }

        if (auto const encodedFault = pendingFault.exchange(0U, std::memory_order_acq_rel); encodedFault != 0U)
        {
          handleFault(encodedFault);
        }

        if (auto const drainGeneration = pendingDrainGeneration.exchange(0U, std::memory_order_acq_rel);
            drainGeneration != 0U)
        {
          handleDrain(drainGeneration);
        }

        // Hand off property publication rather than calling graph observers on
        // the setter's stack. Another thread's target callback may still be active;
        // observer-driven close waits for it through targetCallbackMutex.
        if (pendingGraphPublication.exchange(false, std::memory_order_acq_rel))
        {
          publishGraph();
        }
      }
    }

    void CoreAudioRuntimeState::handleDrain(std::uint64_t const drainGeneration)
    {
      auto const callbackLock = std::scoped_lock{targetCallbackMutex};

      if (generation.load(std::memory_order_acquire) != drainGeneration ||
          !runActive.exchange(false, std::memory_order_acq_rel))
      {
        return;
      }

      renderFence.closeAndWait();
      {
        auto const lock = std::scoped_lock{nativeMutex};

        if (unit != nullptr && started)
        {
          if (::AudioOutputUnitStop(unit) != ::noErr)
          {
            signalFault(CoreAudioFault::DeviceLost);
            return;
          }

          started = false;
        }
      }

      if (generation.load(std::memory_order_relaxed) == drainGeneration && renderTarget != nullptr &&
          deliveredDrainGeneration != drainGeneration)
      {
        deliveredDrainGeneration = drainGeneration;
        auto const callbackScope = utility::CallbackStackScope{this};
        renderTarget->handleDrainComplete();
      }
    }

    void CoreAudioRuntimeState::handleDeviceChange(std::uint64_t const deviceGeneration)
    {
      if (generation.load(std::memory_order_acquire) != deviceGeneration)
      {
        return;
      }

      ::AudioObjectID device = ::kAudioObjectUnknown;
      {
        auto const lock = std::scoped_lock{nativeMutex};

        if (generation.load(std::memory_order_relaxed) != deviceGeneration || unit == nullptr)
        {
          return;
        }

        device = deviceId;
      }

      auto aliveAddress = kGlobalMain;
      aliveAddress.mSelector = ::kAudioDevicePropertyDeviceIsAlive;
      ::UInt32 alive = 0U;
      auto const aliveStatus = readAudioObjectValue(device, aliveAddress, alive);

      if (aliveStatus != ::noErr || alive == 0U)
      {
        signalFault(CoreAudioFault::DeviceLost);
        return;
      }

      auto outputFormat = ::AudioStreamBasicDescription{};
      std::uint32_t clientSampleRate = 0U;
      {
        auto const lock = std::scoped_lock{graphStateMutex};

        if (optClientFormat)
        {
          clientSampleRate = optClientFormat->sampleRate;
        }
      }
      auto optTailFrames = std::optional<std::uint64_t>{};
      ::OSStatus formatStatus = ::noErr;
      {
        auto const lock = std::scoped_lock{nativeMutex};

        if (unit == nullptr)
        {
          return;
        }

        formatStatus =
          readAudioUnitProperty(unit, ::kAudioUnitProperty_StreamFormat, ::kAudioUnitScope_Output, 0U, outputFormat);

        if (formatStatus == ::noErr)
        {
          if (auto const tailFramesRes = presentationTailFrames(unit, device, outputFormat, clientSampleRate);
              tailFramesRes)
          {
            optTailFrames = *tailFramesRes;
          }
        }
      }

      if (formatStatus != ::noErr)
      {
        if (detail::isCoreAudioDeviceLossStatus(formatStatus))
        {
          signalFault(CoreAudioFault::DeviceLost);
        }

        return;
      }

      auto const signalRes = detail::coreAudioSignalFormat(outputFormat);
      {
        auto const lock = std::scoped_lock{graphStateMutex};

        if (generation.load(std::memory_order_relaxed) != deviceGeneration)
        {
          return;
        }

        optDeviceFormat = signalRes ? std::optional{*signalRes} : std::nullopt;

        if (optTailFrames)
        {
          presentationTailFrameCount.store(*optTailFrames, std::memory_order_release);
        }
      }

      publishGraph();
    }

    void CoreAudioRuntimeState::handleFault(std::uint64_t const encodedFault)
    {
      auto const callbackLock = std::scoped_lock{targetCallbackMutex};
      auto const faultGeneration = encodedFault >> 8U;
      auto const fault = static_cast<CoreAudioFault>(encodedFault & 0xffU);

      if (generation.load(std::memory_order_acquire) != faultGeneration)
      {
        return;
      }

      runActive.store(false, std::memory_order_release);
      renderAllowed.store(false, std::memory_order_release);
      renderFence.closeAndWait();
      {
        auto const lock = std::scoped_lock{nativeMutex};

        if (unit != nullptr && started)
        {
          ::AudioOutputUnitStop(unit);
          started = false;
        }
      }

      reportError(faultGeneration, faultMessage(fault));
    }

    void CoreAudioRuntimeState::publishGraph() const
    {
      if (!graphRegistryPtr)
      {
        return;
      }

      auto pendingDelivery = detail::BackendGraphRegistry::PendingDelivery{};
      {
        auto const publicationLock = std::scoped_lock{graphPublicationMutex};
        auto state = detail::CoreAudioRouteState{};
        {
          auto const lock = std::scoped_lock{graphStateMutex};

          if (!graphPublished)
          {
            return;
          }

          state.routeAnchor = deviceUid;
          state.deviceName = deviceName;
          state.optClientFormat = optClientFormat;
          state.optDeviceFormat = optDeviceFormat;
        }

        {
          auto const lock = std::scoped_lock{propertyMutex};
          state.volume = cachedVolume;
          state.muted = cachedMuted;
        }

        // Order the registry commit against close without retaining an owner
        // lock while registry delivery waits for or invokes an observer.
        pendingDelivery = graphRegistryPtr->preparePublish(state.routeAnchor, detail::coreAudioGraph(state));
      }

      std::move(pendingDelivery).deliver();
    }

    void CoreAudioRuntimeState::reportError(std::uint64_t const errorGeneration, std::string_view const message)
    {
      auto const callbackLock = std::scoped_lock{targetCallbackMutex};

      if (generation.load(std::memory_order_relaxed) == errorGeneration && renderTarget != nullptr &&
          deliveredErrorGeneration != errorGeneration)
      {
        deliveredErrorGeneration = errorGeneration;
        auto const callbackScope = utility::CallbackStackScope{this};
        renderTarget->handleBackendError(message);
      }
    }

    ::OSStatus coreAudioRenderCallback(void* const context,
                                       ::AudioUnitRenderActionFlags* const actionFlags,
                                       ::AudioTimeStamp const* /*timeStamp*/,
                                       ::UInt32 /*busNumber*/,
                                       ::UInt32 const frameCount,
                                       ::AudioBufferList* const outputData) noexcept
    {
      auto& state = *static_cast<CoreAudioRuntimeState*>(context);

      if (!state.renderFence.tryEnter())
      {
        silence(outputData);

        if (actionFlags != nullptr)
        {
          *actionFlags |= ::kAudioUnitRenderAction_OutputIsSilence;
        }

        return ::noErr;
      }

      auto const callbackLeave = CallbackLeave{state.renderFence};

      if (frameCount > state.maximumFramesPerSlice)
      {
        silence(outputData);

        if (actionFlags != nullptr)
        {
          *actionFlags |= ::kAudioUnitRenderAction_OutputIsSilence;
        }

        state.signalFault(CoreAudioFault::RenderFrameLimit);
        return ::noErr;
      }

      auto const byteCount = static_cast<std::size_t>(frameCount) * state.bytesPerFrame;
      auto const outputBuffer = detail::bindCoreAudioRenderBuffer(outputData, state.stagingBuffer, byteCount);

      if (!outputBuffer.valid)
      {
        silence(outputData);

        if (actionFlags != nullptr)
        {
          *actionFlags |= ::kAudioUnitRenderAction_OutputIsSilence;
        }

        state.signalFault(CoreAudioFault::RenderBufferShape);
        return ::noErr;
      }

      if (!state.renderAllowed.load(std::memory_order_acquire))
      {
        std::ranges::fill(outputBuffer.output, std::byte{0});

        if (actionFlags != nullptr)
        {
          *actionFlags |= ::kAudioUnitRenderAction_OutputIsSilence;
        }

        if (state.drainTail.consume(frameCount) == detail::DrainTailEvent::Completed)
        {
          state.signalDrainReady(state.generation.load(std::memory_order_relaxed));
        }

        return ::noErr;
      }

      auto* const target = state.renderTarget;

      if (target == nullptr)
      {
        std::ranges::fill(outputBuffer.output, std::byte{0});

        if (actionFlags != nullptr)
        {
          *actionFlags |= ::kAudioUnitRenderAction_OutputIsSilence;
        }

        state.signalFault(CoreAudioFault::RenderTargetMissing);
        return ::noErr;
      }

      auto const callbackScope = utility::CallbackStackScope{&state};
      auto const renderResult = target->renderPcm(std::span{state.stagingBuffer}.first(byteCount));
      auto const prepared = detail::prepareAudioBackendRenderBuffer(
        std::span{state.stagingBuffer}.first(byteCount), state.bytesPerFrame, renderResult);

      if (outputBuffer.output.data() != state.stagingBuffer.data())
      {
        std::memcpy(outputBuffer.output.data(), state.stagingBuffer.data(), byteCount);
      }

      if (prepared.underrun)
      {
        target->handleUnderrun();
      }

      if (prepared.positionFrames != 0U)
      {
        target->handlePositionAdvanced(prepared.positionFrames);
      }

      if (prepared.renderedFrames == 0U && actionFlags != nullptr)
      {
        *actionFlags |= ::kAudioUnitRenderAction_OutputIsSilence;
      }

      if (prepared.drained)
      {
        state.renderAllowed.store(false, std::memory_order_release);
        auto const silentSuffixFrames = prepared.framesProvided - prepared.renderedFrames;

        if (state.drainTail.start(state.presentationTailFrameCount.load(std::memory_order_acquire),
                                  silentSuffixFrames) == detail::DrainTailEvent::Completed)
        {
          state.signalDrainReady(state.generation.load(std::memory_order_relaxed));
        }
      }

      return ::noErr;
    }

    ::OSStatus coreAudioDeviceChanged(::AudioObjectID /*objectId*/,
                                      ::UInt32 /*addressCount*/,
                                      ::AudioObjectPropertyAddress const* /*addresses*/,
                                      void* const context) noexcept
    {
      auto& state = *static_cast<CoreAudioRuntimeState*>(context);

      if (!state.deviceListenerFence.tryEnter())
      {
        return ::noErr;
      }

      auto const callbackLeave = CallbackLeave{state.deviceListenerFence};
      state.signalDeviceChange();
      return ::noErr;
    }

    Result<std::size_t> installDeviceListeners(::AudioObjectID const deviceId, CoreAudioRuntimeState& state)
    {
      state.deviceListenerFence.open();
      std::size_t registeredListenerCount = 0U;

      for (auto const& address : kDevicePropertyAddresses)
      {
        if (auto const status = ::AudioObjectAddPropertyListener(deviceId, &address, &coreAudioDeviceChanged, &state);
            status != ::noErr)
        {
          state.deviceListenerFence.close();

          while (registeredListenerCount != 0U)
          {
            --registeredListenerCount;
            auto const& registeredAddress = kDevicePropertyAddresses[registeredListenerCount];
            ::AudioObjectRemovePropertyListener(deviceId, &registeredAddress, &coreAudioDeviceChanged, &state);
          }

          state.deviceListenerFence.wait();
          return detail::makeCoreAudioError(status, "monitor Core Audio output device", Error::Code::InitFailed);
        }

        ++registeredListenerCount;
      }

      return registeredListenerCount;
    }
  } // namespace

  struct CoreAudioBackend::Impl final
  {
    std::shared_ptr<CoreAudioRuntimeState> statePtr;
    std::jthread controlThread;

    Impl(Device const& device, std::shared_ptr<detail::BackendGraphRegistry> graphRegistryPtr)
      : statePtr{std::make_shared<CoreAudioRuntimeState>(device, std::move(graphRegistryPtr))}
      , controlThread{[statePtr = statePtr]
                      {
                        try
                        {
                          statePtr->controlLoop();
                        }
                        catch (...)
                        {
                          AO_FATAL_EXCEPTION(std::current_exception(), "Core Audio control thread");
                        }
                      }}
    {
    }

    ~Impl()
    {
      statePtr->controlStop.store(true, std::memory_order_release);
      statePtr->controlSignal.release();

      if (controlThread.joinable())
      {
        if (controlThread.get_id() == std::this_thread::get_id())
        {
          controlThread.detach();
        }
        else
        {
          controlThread.join();
        }
      }
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
  };

  CoreAudioBackend::CoreAudioBackend(Device const& device, ProfileId const& profile)
    : CoreAudioBackend{device, profile, nullptr}
  {
  }

  CoreAudioBackend::CoreAudioBackend(Device const& device,
                                     ProfileId const& /*profile*/,
                                     std::shared_ptr<detail::BackendGraphRegistry> graphRegistryPtr)
    : _implPtr{std::make_unique<Impl>(device, std::move(graphRegistryPtr))}
  {
  }

  CoreAudioBackend::~CoreAudioBackend()
  {
    try
    {
      close();
    }
    catch (...)
    {
      AO_FATAL_EXCEPTION(std::current_exception(), "Core Audio backend destruction");
    }
  }

  std::optional<PcmFormat> CoreAudioBackend::prewarmFormatHint(SignalFormat const& sourceFormat) const noexcept
  {
    if (sourceFormat.speakerMask != 0U)
    {
      return std::nullopt;
    }

    return Backend::prewarmFormatHint(sourceFormat);
  }

  BackendOpenResult CoreAudioBackend::open(SignalFormat const& sourceFormat, RenderTarget& target)
  {
    auto const statePtr = _implPtr->statePtr;
    AO_EXPECTS(!utility::CallbackStackScope::containsIdentity(statePtr.get()),
               "Cannot open Core Audio backend from its RenderTarget callback");
    // Reject deterministic source refusals before this backend's own close().
    // Engine closes its previous playback before calling open().
    if (!canAdmitSourceFormat(sourceFormat))
    {
      if (sourceFormat.sampleRate == 0U || sourceFormat.channels == 0U)
      {
        return std::unexpected{BackendOpenFailure{
          .error =
            makeError(Error::Code::InvalidInput, "Core Audio PCM formats require a sample rate and channels").error(),
          .cause = BackendOpenFailure::Cause::OutputFailure}};
      }

      return std::unexpected{BackendOpenFailure{
        .error =
          makeError(Error::Code::FormatRejected, "No lossless Core Audio client PCM encoding is available").error(),
        .cause = BackendOpenFailure::Cause::TrackRouteIncompatible}};
    }

    // Identified sources require native role admission beyond the ASBD storage check.
    if (sourceFormat.speakerMask != 0U)
    {
      return std::unexpected{BackendOpenFailure{
        .error = makeError(Error::Code::FormatRejected, "Core Audio source speaker identity is not supported").error(),
        .cause = BackendOpenFailure::Cause::TrackRouteIncompatible}};
    }

    close();
    auto const deviceRes = detail::coreAudioOutputDeviceId(statePtr->deviceUid);

    if (!deviceRes)
    {
      return std::unexpected{
        BackendOpenFailure{.error = deviceRes.error(), .cause = BackendOpenFailure::Cause::OutputFailure}};
    }

    auto const componentDescription =
      ::AudioComponentDescription{.componentType = ::kAudioUnitType_Output,
                                  .componentSubType = ::kAudioUnitSubType_HALOutput,
                                  .componentManufacturer = ::kAudioUnitManufacturer_Apple,
                                  .componentFlags = 0U,
                                  .componentFlagsMask = 0U};
    auto* const component = ::AudioComponentFindNext(nullptr, &componentDescription);

    if (component == nullptr)
    {
      return std::unexpected{BackendOpenFailure{
        .error = makeError(Error::Code::InitFailed, "Core Audio HAL output component is unavailable").error(),
        .cause = BackendOpenFailure::Cause::OutputFailure}};
    }

    ::AudioUnit rawUnit = nullptr;

    if (auto const status = ::AudioComponentInstanceNew(component, &rawUnit); status != ::noErr)
    {
      return std::unexpected{BackendOpenFailure{
        .error =
          detail::makeCoreAudioError(status, "create Core Audio HAL output unit", Error::Code::InitFailed).error(),
        .cause = BackendOpenFailure::Cause::OutputFailure}};
    }

    auto unitOwner = AudioUnitOwner{rawUnit};

    ::UInt32 enabled = 1U;

    if (auto const status = ::AudioUnitSetProperty(
          rawUnit, ::kAudioOutputUnitProperty_EnableIO, ::kAudioUnitScope_Output, 0U, &enabled, sizeof(enabled));
        status != ::noErr)
    {
      return std::unexpected{BackendOpenFailure{
        .error = detail::makeCoreAudioError(status, "enable Core Audio output", Error::Code::InitFailed).error(),
        .cause = BackendOpenFailure::Cause::OutputFailure}};
    }

    ::UInt32 disabled = 0U;

    if (auto const status = ::AudioUnitSetProperty(
          rawUnit, ::kAudioOutputUnitProperty_EnableIO, ::kAudioUnitScope_Input, 1U, &disabled, sizeof(disabled));
        status != ::noErr)
    {
      return std::unexpected{BackendOpenFailure{
        .error = detail::makeCoreAudioError(status, "disable Core Audio input", Error::Code::InitFailed).error(),
        .cause = BackendOpenFailure::Cause::OutputFailure}};
    }

    auto const deviceId = *deviceRes;

    if (auto const status = ::AudioUnitSetProperty(
          rawUnit, ::kAudioOutputUnitProperty_CurrentDevice, ::kAudioUnitScope_Global, 0U, &deviceId, sizeof(deviceId));
        status != ::noErr)
    {
      return std::unexpected{BackendOpenFailure{
        .error =
          detail::makeCoreAudioError(status, "select Core Audio output device", Error::Code::DeviceNotFound).error(),
        .cause = BackendOpenFailure::Cause::OutputFailure}};
    }

    auto bufferFramesAddress = kGlobalMain;
    bufferFramesAddress.mSelector = ::kAudioDevicePropertyBufferFrameSize;
    ::UInt32 currentBufferFrames = 0U;
    readAudioObjectValue(deviceId, bufferFramesAddress, currentBufferFrames);
    auto variableBufferFramesAddress = kGlobalMain;
    variableBufferFramesAddress.mSelector = ::kAudioDevicePropertyUsesVariableBufferFrameSizes;
    ::UInt32 variableBufferFrames = 0U;
    readAudioObjectValue(deviceId, variableBufferFramesAddress, variableBufferFrames);
    ::UInt32 requestedMaximumFrames =
      std::max({kMinimumMaximumFramesPerSlice, currentBufferFrames, variableBufferFrames});

    if (auto const status = ::AudioUnitSetProperty(rawUnit,
                                                   ::kAudioUnitProperty_MaximumFramesPerSlice,
                                                   ::kAudioUnitScope_Global,
                                                   0U,
                                                   &requestedMaximumFrames,
                                                   sizeof(requestedMaximumFrames));
        status != ::noErr)
    {
      return std::unexpected{BackendOpenFailure{
        .error = detail::makeCoreAudioError(status, "set Core Audio maximum frames per slice", Error::Code::InitFailed)
                   .error(),
        .cause = BackendOpenFailure::Cause::OutputFailure}};
    }

    ::UInt32 maximumFrames = 0U;

    if (auto const status = readAudioUnitProperty(
          rawUnit, ::kAudioUnitProperty_MaximumFramesPerSlice, ::kAudioUnitScope_Global, 0U, maximumFrames);
        status != ::noErr || maximumFrames == 0U)
    {
      return std::unexpected{BackendOpenFailure{
        .error = detail::makeCoreAudioError(status == ::noErr ? ::kAudioHardwareBadPropertySizeError : status,
                                            "read Core Audio maximum frames per slice",
                                            Error::Code::InitFailed)
                   .error(),
        .cause = BackendOpenFailure::Cause::OutputFailure}};
    }

    auto const selectedFormatRes =
      detail::selectLosslessCoreAudioClientFormat(sourceFormat,
                                                  [rawUnit](::AudioStreamBasicDescription const& description)
                                                    -> std::expected<::AudioStreamBasicDescription, BackendOpenFailure>
                                                  { return tryCoreAudioClientFormat(rawUnit, description); });

    if (!selectedFormatRes)
    {
      return std::unexpected{selectedFormatRes.error()};
    }

    auto const selectedFormat = *selectedFormatRes;

    auto const bytesPerFrame = frameBytes(selectedFormat);

    if (bytesPerFrame == 0U || maximumFrames > std::numeric_limits<std::size_t>::max() / bytesPerFrame ||
        static_cast<std::size_t>(maximumFrames) * bytesPerFrame > std::numeric_limits<::UInt32>::max())
    {
      return std::unexpected{BackendOpenFailure{
        .error = makeError(Error::Code::ValueTooLarge, "Core Audio render staging buffer is too large").error(),
        .cause = BackendOpenFailure::Cause::OutputFailure}};
    }

    auto stagingBuffer = std::vector<std::byte>(static_cast<std::size_t>(maximumFrames) * bytesPerFrame);

    auto const callback =
      ::AURenderCallbackStruct{.inputProc = &coreAudioRenderCallback, .inputProcRefCon = statePtr.get()};

    if (auto const status = ::AudioUnitSetProperty(
          rawUnit, ::kAudioUnitProperty_SetRenderCallback, ::kAudioUnitScope_Input, 0U, &callback, sizeof(callback));
        status != ::noErr)
    {
      return std::unexpected{BackendOpenFailure{
        .error =
          detail::makeCoreAudioError(status, "install Core Audio render callback", Error::Code::InitFailed).error(),
        .cause = BackendOpenFailure::Cause::OutputFailure}};
    }

    if (auto const status = ::AudioUnitInitialize(rawUnit); status != ::noErr)
    {
      return std::unexpected{BackendOpenFailure{
        .error =
          detail::makeCoreAudioError(status, "initialize Core Audio HAL output unit", Error::Code::InitFailed).error(),
        .cause = BackendOpenFailure::Cause::OutputFailure}};
    }

    unitOwner.markInitialized();

    auto outputDescription = ::AudioStreamBasicDescription{};

    if (auto const status = readAudioUnitProperty(
          rawUnit, ::kAudioUnitProperty_StreamFormat, ::kAudioUnitScope_Output, 0U, outputDescription);
        status != ::noErr)
    {
      return std::unexpected{BackendOpenFailure{
        .error = detail::makeCoreAudioError(status, "read Core Audio device format", Error::Code::InitFailed).error(),
        .cause = BackendOpenFailure::Cause::OutputFailure}};
    }

    auto const tailFramesRes = presentationTailFrames(rawUnit, deviceId, outputDescription, selectedFormat.sampleRate);

    if (!tailFramesRes)
    {
      return std::unexpected{
        BackendOpenFailure{.error = tailFramesRes.error(), .cause = BackendOpenFailure::Cause::OutputFailure}};
    }

    float volume = 1.0F;
    bool muted = false;
    {
      auto const lock = std::scoped_lock{statePtr->propertyMutex};
      volume = statePtr->cachedVolume;
      muted = statePtr->cachedMuted;
    }

    if (auto const status = ::AudioUnitSetParameter(
          rawUnit, ::kHALOutputParam_Volume, ::kAudioUnitScope_Global, 0U, muted ? 0.0F : volume, 0U);
        status != ::noErr)
    {
      return std::unexpected{BackendOpenFailure{
        .error = detail::makeCoreAudioError(status, "restore Core Audio output volume", Error::Code::IoError).error(),
        .cause = BackendOpenFailure::Cause::OutputFailure}};
    }

    statePtr->generation.fetch_add(1U, std::memory_order_acq_rel);
    statePtr->pendingDrainGeneration.store(0U, std::memory_order_release);
    statePtr->pendingFault.store(0U, std::memory_order_release);
    statePtr->runActive.store(false, std::memory_order_release);
    statePtr->renderAllowed.store(false, std::memory_order_release);
    {
      auto const lock = std::scoped_lock{statePtr->nativeMutex};
      statePtr->unit = unitOwner.release();
      statePtr->deviceId = deviceId;
      statePtr->started = false;
      statePtr->bytesPerFrame = bytesPerFrame;
      statePtr->maximumFramesPerSlice = maximumFrames;
      statePtr->presentationTailFrameCount.store(*tailFramesRes, std::memory_order_release);
      statePtr->stagingBuffer = std::move(stagingBuffer);
    }

    {
      auto const lock = std::scoped_lock{statePtr->graphStateMutex};
      auto const signalRes = detail::coreAudioSignalFormat(outputDescription);
      statePtr->optClientFormat = selectedFormat;
      statePtr->optDeviceFormat = signalRes ? std::optional{*signalRes} : std::nullopt;
    }

    std::uint64_t openGeneration = 0U;
    {
      auto const callbackLock = std::scoped_lock{statePtr->targetCallbackMutex};
      openGeneration = ++statePtr->targetGeneration;
      statePtr->renderTarget = &target;
      statePtr->deliveredDrainGeneration = 0U;
      statePtr->deliveredErrorGeneration = 0U;
    }

    auto const registeredListenerCountRes = installDeviceListeners(deviceId, *statePtr);

    if (!registeredListenerCountRes)
    {
      close();
      return std::unexpected{BackendOpenFailure{
        .error = registeredListenerCountRes.error(), .cause = BackendOpenFailure::Cause::OutputFailure}};
    }

    {
      auto const lock = std::scoped_lock{statePtr->nativeMutex};
      statePtr->registeredDeviceListenerCount = *registeredListenerCountRes;
    }

    {
      // Open-origin notifications share close's target lifetime boundary.
      // No native, property, graph-state or publication lock spans these calls.
      auto const callbackLock = std::scoped_lock{statePtr->targetCallbackMutex};

      if (!isCurrentOpenTarget(*statePtr, openGeneration, target))
      {
        return OpenedPcmMode{.clientFormat = selectedFormat};
      }

      {
        auto const lock = std::scoped_lock{statePtr->graphStateMutex};
        statePtr->graphPublished = true;
      }

      auto const callbackScope = utility::CallbackStackScope{statePtr.get()};
      statePtr->renderTarget->handleRouteReady(statePtr->deviceUid);
    }

    {
      auto const callbackLock = std::scoped_lock{statePtr->targetCallbackMutex};

      // Close may retire this open between notifications. Check the generation
      // as well as identity: a later open can attach the same target again.
      if (!isCurrentOpenTarget(*statePtr, openGeneration, target))
      {
        return OpenedPcmMode{.clientFormat = selectedFormat};
      }

      auto const callbackScope = utility::CallbackStackScope{statePtr.get()};
      statePtr->renderTarget->handleFormatChanged(selectedFormat);

      if (!isCurrentOpenTarget(*statePtr, openGeneration, target))
      {
        return OpenedPcmMode{.clientFormat = selectedFormat};
      }
    }

    statePtr->publishGraph();
    return OpenedPcmMode{.clientFormat = selectedFormat};
  }

  void CoreAudioBackend::start()
  {
    auto const statePtr = _implPtr->statePtr;
    auto const callbackLock = std::scoped_lock{statePtr->targetCallbackMutex};

    if (statePtr->renderTarget == nullptr)
    {
      return;
    }

    // Keep admission and native activation in close's serialized lifetime boundary.
    // A graph observer's start has no outer target-notification guard of its own.
    auto const runGeneration = statePtr->generation.fetch_add(1U, std::memory_order_acq_rel) + 1U;
    statePtr->pendingDrainGeneration.store(0U, std::memory_order_release);
    statePtr->pendingFault.store(0U, std::memory_order_release);
    statePtr->drainTail.reset();
    statePtr->deliveredDrainGeneration = 0U;
    statePtr->deliveredErrorGeneration = 0U;
    statePtr->renderAllowed.store(true, std::memory_order_release);
    statePtr->runActive.store(true, std::memory_order_release);
    statePtr->renderFence.open();

    ::OSStatus status = ::noErr;
    {
      auto const lock = std::scoped_lock{statePtr->nativeMutex};

      if (statePtr->unit == nullptr)
      {
        statePtr->renderFence.closeAndWait();
        statePtr->runActive.store(false, std::memory_order_release);
        return;
      }

      status = ::AudioOutputUnitStart(statePtr->unit);
      statePtr->started = status == ::noErr;
    }

    if (status != ::noErr)
    {
      statePtr->renderFence.closeAndWait();
      statePtr->runActive.store(false, std::memory_order_release);
      auto const error = detail::makeCoreAudioError(status, "start Core Audio output", Error::Code::IoError).error();
      statePtr->reportError(runGeneration, error.message);
    }
  }

  void CoreAudioBackend::pause()
  {
    auto const statePtr = _implPtr->statePtr;
    auto const callbackLock = std::scoped_lock{statePtr->targetCallbackMutex};
    statePtr->renderFence.closeAndWait();
    auto const lock = std::scoped_lock{statePtr->nativeMutex};

    if (statePtr->unit != nullptr && statePtr->started)
    {
      if (auto const status = ::AudioOutputUnitStop(statePtr->unit); status != ::noErr)
      {
        statePtr->signalFault(CoreAudioFault::DeviceLost);
      }

      statePtr->started = false;
    }
  }

  void CoreAudioBackend::resume()
  {
    auto const statePtr = _implPtr->statePtr;
    auto const callbackLock = std::scoped_lock{statePtr->targetCallbackMutex};

    if (!statePtr->runActive.load(std::memory_order_acquire))
    {
      return;
    }

    statePtr->renderFence.open();
    auto const lock = std::scoped_lock{statePtr->nativeMutex};

    if (statePtr->unit == nullptr)
    {
      statePtr->renderFence.closeAndWait();
      statePtr->runActive.store(false, std::memory_order_release);
      return;
    }

    if (!statePtr->started)
    {
      if (auto const status = ::AudioOutputUnitStart(statePtr->unit); status != ::noErr)
      {
        statePtr->renderFence.closeAndWait();
        statePtr->runActive.store(false, std::memory_order_release);
        statePtr->signalFault(CoreAudioFault::DeviceLost);
        return;
      }

      statePtr->started = true;
    }
  }

  void CoreAudioBackend::flush()
  {
    auto const statePtr = _implPtr->statePtr;
    auto const callbackLock = std::scoped_lock{statePtr->targetCallbackMutex};
    stop();
    ::OSStatus status = ::noErr;
    {
      auto const lock = std::scoped_lock{statePtr->nativeMutex};

      if (statePtr->unit != nullptr)
      {
        status = ::AudioUnitReset(statePtr->unit, ::kAudioUnitScope_Global, 0U);
      }
    }

    if (status != ::noErr)
    {
      auto const generation = statePtr->generation.load(std::memory_order_relaxed);
      auto const error = detail::makeCoreAudioError(status, "reset Core Audio output", Error::Code::IoError).error();
      statePtr->reportError(generation, error.message);
    }
  }

  void CoreAudioBackend::stop()
  {
    auto const statePtr = _implPtr->statePtr;
    auto const callbackLock = std::scoped_lock{statePtr->targetCallbackMutex};
    statePtr->generation.fetch_add(1U, std::memory_order_acq_rel);
    statePtr->runActive.store(false, std::memory_order_release);
    statePtr->renderAllowed.store(false, std::memory_order_release);
    statePtr->renderFence.closeAndWait();
    statePtr->drainTail.reset();
    auto const lock = std::scoped_lock{statePtr->nativeMutex};

    if (statePtr->unit != nullptr && statePtr->started)
    {
      ::AudioOutputUnitStop(statePtr->unit);
      statePtr->started = false;
    }
  }

  void CoreAudioBackend::close()
  {
    auto const statePtr = _implPtr->statePtr;
    AO_EXPECTS(!utility::CallbackStackScope::containsIdentity(statePtr.get()),
               "Cannot close Core Audio backend from its RenderTarget callback");
    statePtr->deviceListenerFence.close();

    ::AudioUnit unit = nullptr;
    ::AudioObjectID deviceId = ::kAudioObjectUnknown;
    std::size_t registeredListenerCount = 0U;
    {
      auto const callbackLock = std::scoped_lock{statePtr->targetCallbackMutex};
      // An admitted open notification can reenter start while close waits for
      // this mutex. Establish final render quiescence after it has returned,
      // before detaching any render storage or revoking the target.
      ++statePtr->targetGeneration;
      statePtr->generation.fetch_add(1U, std::memory_order_acq_rel);
      statePtr->runActive.store(false, std::memory_order_release);
      statePtr->renderAllowed.store(false, std::memory_order_release);
      statePtr->renderFence.closeAndWait();
      {
        auto const lock = std::scoped_lock{statePtr->nativeMutex};
        unit = statePtr->unit;
        deviceId = statePtr->deviceId;
        registeredListenerCount = std::exchange(statePtr->registeredDeviceListenerCount, 0U);
        statePtr->unit = nullptr;
        statePtr->deviceId = ::kAudioObjectUnknown;
        statePtr->started = false;
        statePtr->stagingBuffer.clear();
        statePtr->bytesPerFrame = 0U;
        statePtr->maximumFramesPerSlice = 0U;
        statePtr->presentationTailFrameCount.store(0U, std::memory_order_release);
      }

      // Render quiescence and callback serialization precede target revocation.
      // Do not reacquire this mutex while holding graphPublicationMutex below.
      statePtr->renderTarget = nullptr;
      statePtr->deliveredDrainGeneration = 0U;
      statePtr->deliveredErrorGeneration = 0U;
      statePtr->drainTail.reset();
    }

    auto pendingDelivery = detail::BackendGraphRegistry::PendingDelivery{};
    {
      auto const publicationLock = std::scoped_lock{statePtr->graphPublicationMutex};
      auto routeAnchor = std::string{};
      bool clearGraph = false;
      {
        auto const lock = std::scoped_lock{statePtr->graphStateMutex};
        routeAnchor = statePtr->deviceUid;
        clearGraph = statePtr->graphPublished;
        statePtr->graphPublished = false;
        statePtr->optClientFormat.reset();
        statePtr->optDeviceFormat.reset();
      }

      if (clearGraph && statePtr->graphRegistryPtr && !routeAnchor.empty())
      {
        pendingDelivery = statePtr->graphRegistryPtr->prepareClear(routeAnchor);
      }
    }

    while (registeredListenerCount != 0U)
    {
      --registeredListenerCount;
      auto const& address = kDevicePropertyAddresses[registeredListenerCount];
      ::AudioObjectRemovePropertyListener(deviceId, &address, &coreAudioDeviceChanged, statePtr.get());
    }

    statePtr->deviceListenerFence.wait();

    if (unit != nullptr)
    {
      ::AudioOutputUnitStop(unit);
      ::AudioUnitUninitialize(unit);
      ::AudioComponentInstanceDispose(unit);
    }

    std::move(pendingDelivery).deliver();
  }

  Result<> CoreAudioBackend::setProperty(PropertyId const id, PropertyValue const& value)
  {
    auto const statePtr = _implPtr->statePtr;
    {
      auto const lock = std::scoped_lock{statePtr->propertyMutex};

      if (id == PropertyId::Volume)
      {
        auto const* requested = std::get_if<float>(&value);

        if (requested == nullptr || !std::isfinite(*requested))
        {
          return makeError(Error::Code::InvalidInput, "Core Audio volume must be a finite number");
        }

        auto const requestedValue = std::clamp(*requested, 0.0F, 1.0F);
        {
          auto const nativeLock = std::scoped_lock{statePtr->nativeMutex};

          if (statePtr->unit != nullptr)
          {
            if (auto const status = ::AudioUnitSetParameter(statePtr->unit,
                                                            ::kHALOutputParam_Volume,
                                                            ::kAudioUnitScope_Global,
                                                            0U,
                                                            statePtr->cachedMuted ? 0.0F : requestedValue,
                                                            0U);
                status != ::noErr)
            {
              return detail::makeCoreAudioError(status, "set Core Audio output volume", Error::Code::IoError);
            }
          }
        }

        statePtr->cachedVolume = requestedValue;
      }
      else if (id == PropertyId::Muted)
      {
        auto const* requested = std::get_if<bool>(&value);

        if (requested == nullptr)
        {
          return makeError(Error::Code::InvalidInput, "Core Audio mute requires a boolean value");
        }

        {
          auto const nativeLock = std::scoped_lock{statePtr->nativeMutex};

          if (statePtr->unit != nullptr)
          {
            if (auto const status = ::AudioUnitSetParameter(statePtr->unit,
                                                            ::kHALOutputParam_Volume,
                                                            ::kAudioUnitScope_Global,
                                                            0U,
                                                            *requested ? 0.0F : statePtr->cachedVolume,
                                                            0U);
                status != ::noErr)
            {
              return detail::makeCoreAudioError(status, "set Core Audio output mute", Error::Code::IoError);
            }
          }
        }

        statePtr->cachedMuted = *requested;
      }
      else
      {
        return makeError(Error::Code::NotSupported);
      }
    }

    statePtr->requestGraphPublication();
    return {};
  }

  Result<PropertyValue> CoreAudioBackend::property(PropertyId const id) const
  {
    auto const statePtr = _implPtr->statePtr;
    auto const lock = std::scoped_lock{statePtr->propertyMutex};

    if (id == PropertyId::Volume)
    {
      return statePtr->cachedVolume;
    }

    if (id == PropertyId::Muted)
    {
      return statePtr->cachedMuted;
    }

    return makeError(Error::Code::NotSupported);
  }

  PropertyInfo CoreAudioBackend::queryProperty(PropertyId const id) const noexcept
  {
    if (id != PropertyId::Volume && id != PropertyId::Muted)
    {
      return {};
    }

    auto const statePtr = _implPtr->statePtr;
    auto const lock = std::scoped_lock{statePtr->nativeMutex};
    return {.canRead = true,
            .canWrite = true,
            .isAvailable = statePtr->unit != nullptr,
            .emitsChangeNotifications = false,
            .isHardwareAssisted = false};
  }

  BackendId CoreAudioBackend::backendId() const
  {
    return kBackendCoreAudio;
  }

  ProfileId CoreAudioBackend::profileId() const
  {
    return kProfileShared;
  }
} // namespace ao::audio::backend
