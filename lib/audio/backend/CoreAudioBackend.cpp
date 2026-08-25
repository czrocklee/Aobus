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

#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/OpenedPcmMode.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/Property.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/flow/Graph.h>
#include <ao/utility/ThreadName.h>

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <format>
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
#include <vector>

namespace ao::audio::backend
{
  namespace
  {
    constexpr ::UInt32 kMinimumMaximumFramesPerSlice = 4096U;
    constexpr auto kGlobalMain = ::AudioObjectPropertyAddress{
      0U, ::kAudioObjectPropertyScopeGlobal, ::kAudioObjectPropertyElementMain};
    constexpr auto kOutputMain = ::AudioObjectPropertyAddress{
      0U, ::kAudioObjectPropertyScopeOutput, ::kAudioObjectPropertyElementMain};
    constexpr auto kDevicePropertyAddresses = std::array{
      ::AudioObjectPropertyAddress{::kAudioDevicePropertyDeviceIsAlive,
                                   ::kAudioObjectPropertyScopeGlobal,
                                   ::kAudioObjectPropertyElementMain},
      ::AudioObjectPropertyAddress{::kAudioDevicePropertyNominalSampleRate,
                                   ::kAudioObjectPropertyScopeGlobal,
                                   ::kAudioObjectPropertyElementMain},
      ::AudioObjectPropertyAddress{::kAudioDevicePropertyStreamConfiguration,
                                   ::kAudioObjectPropertyScopeOutput,
                                   ::kAudioObjectPropertyElementMain}};

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
      auto const status = ::AudioObjectGetPropertyData(object, &address, 0U, nullptr, &size, &value);
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

    std::vector<::AudioStreamID> outputStreams(::AudioDeviceID const device)
    {
      auto address = kOutputMain;
      address.mSelector = ::kAudioDevicePropertyStreams;
      ::UInt32 byteCount = 0U;
      if (::AudioObjectGetPropertyDataSize(device, &address, 0U, nullptr, &byteCount) != ::noErr ||
          byteCount % sizeof(::AudioStreamID) != 0U)
      {
        return {};
      }

      auto streams = std::vector<::AudioStreamID>(byteCount / sizeof(::AudioStreamID));
      if (byteCount != 0U &&
          ::AudioObjectGetPropertyData(device, &address, 0U, nullptr, &byteCount, streams.data()) != ::noErr)
      {
        return {};
      }
      streams.resize(byteCount / sizeof(::AudioStreamID));
      return streams;
    }

    std::uint64_t scalarFrames(::AudioObjectID const object,
                               ::AudioObjectPropertySelector const selector,
                               ::AudioObjectPropertyScope const scope) noexcept
    {
      auto address = ::AudioObjectPropertyAddress{selector, scope, ::kAudioObjectPropertyElementMain};
      ::UInt32 frames = 0U;
      return readAudioObjectValue(object, address, frames) == ::noErr ? frames : 0U;
    }

    Result<std::uint64_t> presentationTailFrames(::AudioUnit const unit,
                                                  ::AudioDeviceID const device,
                                                  ::AudioStreamBasicDescription const& deviceFormat,
                                                  std::uint32_t const clientSampleRate)
    {
      auto streamLatency = std::uint64_t{0U};
      for (auto const stream : outputStreams(device))
      {
        streamLatency = std::max(streamLatency,
                                 scalarFrames(stream,
                                              ::kAudioStreamPropertyLatency,
                                              ::kAudioObjectPropertyScopeGlobal));
      }

      ::Float64 deviceSampleRate = deviceFormat.mSampleRate;
      auto sampleRateAddress = kGlobalMain;
      sampleRateAddress.mSelector = ::kAudioDevicePropertyNominalSampleRate;
      readAudioObjectValue(device, sampleRateAddress, deviceSampleRate);

      ::Float64 audioUnitLatency = 0.0;
      readAudioUnitProperty(unit,
                            ::kAudioUnitProperty_Latency,
                            ::kAudioUnitScope_Global,
                            0U,
                            audioUnitLatency);

      return detail::coreAudioPresentationTailFrames(
        {.ioBufferFrames = scalarFrames(device,
                                        ::kAudioDevicePropertyBufferFrameSize,
                                        ::kAudioObjectPropertyScopeGlobal),
         .safetyOffsetFrames = scalarFrames(device,
                                            ::kAudioDevicePropertySafetyOffset,
                                            ::kAudioObjectPropertyScopeOutput),
         .deviceLatencyFrames = scalarFrames(device,
                                             ::kAudioDevicePropertyLatency,
                                             ::kAudioObjectPropertyScopeOutput),
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
        auto& buffer = buffers->mBuffers[index];
        if (buffer.mData != nullptr && buffer.mDataByteSize != 0U)
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
      CoreAudioRuntimeState(Device const& device,
                            std::shared_ptr<detail::BackendGraphRegistry> graphRegistry)
        : deviceUid{device.id.raw()}
        , deviceName{device.displayName.empty() ? device.id.raw() : device.displayName}
        , graphRegistryPtr{std::move(graphRegistry)}
      {
      }

      std::string deviceUid{};
      std::string deviceName{};
      std::shared_ptr<detail::BackendGraphRegistry> graphRegistryPtr{};

      mutable std::mutex nativeMutex{};
      ::AudioUnit unit = nullptr;
      ::AudioDeviceID deviceId = ::kAudioObjectUnknown;
      bool started = false;
      std::size_t registeredDeviceListenerCount = 0U;

      detail::CallbackFence renderFence{};
      detail::CallbackFence deviceListenerFence{};
      RenderTarget* renderTarget = nullptr;
      std::recursive_mutex targetCallbackMutex{};

      std::vector<std::byte> stagingBuffer{};
      std::size_t bytesPerFrame = 0U;
      ::UInt32 maximumFramesPerSlice = 0U;
      std::atomic<std::uint64_t> presentationTailFrameCount{0U};
      detail::AudioBackendDrainTail drainTail{};

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
      std::atomic<std::uint64_t> pendingDeviceGeneration{0U};
      std::atomic<std::uint64_t> pendingFault{0U};
      std::atomic<bool> controlStop{false};
      std::counting_semaphore<> controlSignal{0};
      std::uint64_t deliveredDrainGeneration = 0U;
      std::uint64_t deliveredErrorGeneration = 0U;

      void signalDrainReady(std::uint64_t drainGeneration) noexcept;
      void signalDeviceChange() noexcept;
      void signalFault(CoreAudioFault fault) noexcept;
      void controlLoop();
      void handleDrain(std::uint64_t drainGeneration);
      void handleDeviceChange(std::uint64_t deviceGeneration);
      void handleFault(std::uint64_t encodedFault);
      void publishGraph() const;
      void reportError(std::uint64_t errorGeneration, std::string_view message);
    };

    void CoreAudioRuntimeState::signalDrainReady(std::uint64_t const drainGeneration) noexcept
    {
      auto expected = std::uint64_t{0U};
      if (pendingDrainGeneration.compare_exchange_strong(
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
      auto expected = std::uint64_t{0U};
      if (pendingFault.compare_exchange_strong(
            expected, encoded, std::memory_order_release, std::memory_order_relaxed))
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
      }
    }

    void CoreAudioRuntimeState::handleDrain(std::uint64_t const drainGeneration)
    {
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

      auto const callbackLock = std::scoped_lock{targetCallbackMutex};
      if (generation.load(std::memory_order_relaxed) == drainGeneration && renderTarget != nullptr &&
          deliveredDrainGeneration != drainGeneration)
      {
        deliveredDrainGeneration = drainGeneration;
        renderTarget->handleDrainComplete();
      }
    }

    void CoreAudioRuntimeState::handleDeviceChange(std::uint64_t const deviceGeneration)
    {
      if (generation.load(std::memory_order_acquire) != deviceGeneration)
      {
        return;
      }

      auto device = ::AudioDeviceID{::kAudioObjectUnknown};
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
      auto clientSampleRate = std::uint32_t{0U};
      {
        auto const lock = std::scoped_lock{graphStateMutex};
        if (optClientFormat)
        {
          clientSampleRate = optClientFormat->sampleRate;
        }
      }
      auto optTailFrames = std::optional<std::uint64_t>{};
      auto formatStatus = ::OSStatus{::noErr};
      {
        auto const lock = std::scoped_lock{nativeMutex};
        if (unit == nullptr)
        {
          return;
        }
        formatStatus = readAudioUnitProperty(unit,
                                             ::kAudioUnitProperty_StreamFormat,
                                             ::kAudioUnitScope_Output,
                                             0U,
                                             outputFormat);
        if (formatStatus == ::noErr)
        {
          if (auto const tailFramesRes =
                presentationTailFrames(unit, device, outputFormat, clientSampleRate);
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
      graphRegistryPtr->publish(state.routeAnchor, detail::coreAudioGraph(state));
    }

    void CoreAudioRuntimeState::reportError(std::uint64_t const errorGeneration, std::string_view const message)
    {
      auto const callbackLock = std::scoped_lock{targetCallbackMutex};
      if (generation.load(std::memory_order_relaxed) == errorGeneration && renderTarget != nullptr &&
          deliveredErrorGeneration != errorGeneration)
      {
        deliveredErrorGeneration = errorGeneration;
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
        if (state.drainTail.consume(frameCount))
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
        if (state.drainTail.start(
              state.presentationTailFrameCount.load(std::memory_order_acquire), silentSuffixFrames))
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

  Result<OpenedPcmMode> CoreAudioBackend::open(SignalFormat const& sourceFormat, RenderTarget& target)
  {
    close();
    auto const statePtr = _implPtr->statePtr;
    auto const deviceRes = detail::coreAudioOutputDeviceId(statePtr->deviceUid);
    if (!deviceRes)
    {
      return std::unexpected(deviceRes.error());
    }

    auto const componentDescription = ::AudioComponentDescription{.componentType = ::kAudioUnitType_Output,
                                                                   .componentSubType = ::kAudioUnitSubType_HALOutput,
                                                                   .componentManufacturer =
                                                                     ::kAudioUnitManufacturer_Apple,
                                                                   .componentFlags = 0U,
                                                                   .componentFlagsMask = 0U};
    auto const component = ::AudioComponentFindNext(nullptr, &componentDescription);
    if (component == nullptr)
    {
      return makeError(Error::Code::InitFailed, "Core Audio HAL output component is unavailable");
    }

    ::AudioUnit rawUnit = nullptr;
    if (auto const status = ::AudioComponentInstanceNew(component, &rawUnit); status != ::noErr)
    {
      return detail::makeCoreAudioError(status, "create Core Audio HAL output unit", Error::Code::InitFailed);
    }
    auto unitOwner = AudioUnitOwner{rawUnit};

    auto enabled = ::UInt32{1U};
    if (auto const status = ::AudioUnitSetProperty(rawUnit,
                                                   ::kAudioOutputUnitProperty_EnableIO,
                                                   ::kAudioUnitScope_Output,
                                                   0U,
                                                   &enabled,
                                                   sizeof(enabled));
        status != ::noErr)
    {
      return detail::makeCoreAudioError(status, "enable Core Audio output", Error::Code::InitFailed);
    }
    auto disabled = ::UInt32{0U};
    if (auto const status = ::AudioUnitSetProperty(rawUnit,
                                                   ::kAudioOutputUnitProperty_EnableIO,
                                                   ::kAudioUnitScope_Input,
                                                   1U,
                                                   &disabled,
                                                   sizeof(disabled));
        status != ::noErr)
    {
      return detail::makeCoreAudioError(status, "disable Core Audio input", Error::Code::InitFailed);
    }
    auto const deviceId = *deviceRes;
    if (auto const status = ::AudioUnitSetProperty(rawUnit,
                                                   ::kAudioOutputUnitProperty_CurrentDevice,
                                                   ::kAudioUnitScope_Global,
                                                   0U,
                                                   &deviceId,
                                                   sizeof(deviceId));
        status != ::noErr)
    {
      return detail::makeCoreAudioError(status, "select Core Audio output device", Error::Code::DeviceNotFound);
    }

    auto bufferFramesAddress = kGlobalMain;
    bufferFramesAddress.mSelector = ::kAudioDevicePropertyBufferFrameSize;
    ::UInt32 currentBufferFrames = 0U;
    readAudioObjectValue(deviceId, bufferFramesAddress, currentBufferFrames);
    auto variableBufferFramesAddress = kGlobalMain;
    variableBufferFramesAddress.mSelector = ::kAudioDevicePropertyUsesVariableBufferFrameSizes;
    ::UInt32 variableBufferFrames = 0U;
    readAudioObjectValue(deviceId, variableBufferFramesAddress, variableBufferFrames);
    auto requestedMaximumFrames =
      std::max({kMinimumMaximumFramesPerSlice, currentBufferFrames, variableBufferFrames});
    if (auto const status = ::AudioUnitSetProperty(rawUnit,
                                                   ::kAudioUnitProperty_MaximumFramesPerSlice,
                                                   ::kAudioUnitScope_Global,
                                                   0U,
                                                   &requestedMaximumFrames,
                                                   sizeof(requestedMaximumFrames));
        status != ::noErr)
    {
      return detail::makeCoreAudioError(status, "set Core Audio maximum frames per slice", Error::Code::InitFailed);
    }
    auto maximumFrames = ::UInt32{0U};
    if (auto const status = readAudioUnitProperty(rawUnit,
                                                  ::kAudioUnitProperty_MaximumFramesPerSlice,
                                                  ::kAudioUnitScope_Global,
                                                  0U,
                                                  maximumFrames);
        status != ::noErr || maximumFrames == 0U)
    {
      return detail::makeCoreAudioError(status == ::noErr ? ::kAudioHardwareBadPropertySizeError : status,
                                        "read Core Audio maximum frames per slice",
                                        Error::Code::InitFailed);
    }

    auto const selectedFormatRes = detail::selectLosslessCoreAudioClientFormat(
      sourceFormat,
      [rawUnit](::AudioStreamBasicDescription const& description)
        -> Result<::AudioStreamBasicDescription>
      {
        if (auto const status = ::AudioUnitSetProperty(rawUnit,
                                                       ::kAudioUnitProperty_StreamFormat,
                                                       ::kAudioUnitScope_Input,
                                                       0U,
                                                       &description,
                                                       sizeof(description));
            status != ::noErr)
        {
          return detail::makeCoreAudioError(
            status, "configure Core Audio client format", Error::Code::InitFailed);
        }

        auto readBack = ::AudioStreamBasicDescription{};
        if (auto const status = readAudioUnitProperty(rawUnit,
                                                      ::kAudioUnitProperty_StreamFormat,
                                                      ::kAudioUnitScope_Input,
                                                      0U,
                                                      readBack);
            status != ::noErr)
        {
          return detail::makeCoreAudioError(
            status, "read Core Audio client format", Error::Code::InitFailed);
        }
        return readBack;
      });
    if (!selectedFormatRes)
    {
      return std::unexpected(selectedFormatRes.error());
    }
    auto const selectedFormat = *selectedFormatRes;

    auto const bytesPerFrame = frameBytes(selectedFormat);
    if (bytesPerFrame == 0U || maximumFrames > std::numeric_limits<std::size_t>::max() / bytesPerFrame ||
        static_cast<std::size_t>(maximumFrames) * bytesPerFrame > std::numeric_limits<::UInt32>::max())
    {
      return makeError(Error::Code::ValueTooLarge, "Core Audio render staging buffer is too large");
    }
    auto stagingBuffer = std::vector<std::byte>(static_cast<std::size_t>(maximumFrames) * bytesPerFrame);

    auto const callback = ::AURenderCallbackStruct{.inputProc = &coreAudioRenderCallback,
                                                    .inputProcRefCon = statePtr.get()};
    if (auto const status = ::AudioUnitSetProperty(rawUnit,
                                                   ::kAudioUnitProperty_SetRenderCallback,
                                                   ::kAudioUnitScope_Input,
                                                   0U,
                                                   &callback,
                                                   sizeof(callback));
        status != ::noErr)
    {
      return detail::makeCoreAudioError(status, "install Core Audio render callback", Error::Code::InitFailed);
    }
    if (auto const status = ::AudioUnitInitialize(rawUnit); status != ::noErr)
    {
      return detail::makeCoreAudioError(status, "initialize Core Audio HAL output unit", Error::Code::InitFailed);
    }
    unitOwner.markInitialized();

    auto outputDescription = ::AudioStreamBasicDescription{};
    if (auto const status = readAudioUnitProperty(rawUnit,
                                                  ::kAudioUnitProperty_StreamFormat,
                                                  ::kAudioUnitScope_Output,
                                                  0U,
                                                  outputDescription);
        status != ::noErr)
    {
      return detail::makeCoreAudioError(status, "read Core Audio device format", Error::Code::InitFailed);
    }
    auto const tailFramesRes = presentationTailFrames(rawUnit, deviceId, outputDescription, selectedFormat.sampleRate);
    if (!tailFramesRes)
    {
      return std::unexpected(tailFramesRes.error());
    }

    auto volume = 1.0F;
    auto muted = false;
    {
      auto const lock = std::scoped_lock{statePtr->propertyMutex};
      volume = statePtr->cachedVolume;
      muted = statePtr->cachedMuted;
    }
    if (auto const status = ::AudioUnitSetParameter(rawUnit,
                                                    ::kHALOutputParam_Volume,
                                                    ::kAudioUnitScope_Global,
                                                    0U,
                                                    muted ? 0.0F : volume,
                                                    0U);
        status != ::noErr)
    {
      return detail::makeCoreAudioError(status, "restore Core Audio output volume", Error::Code::IoError);
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
    {
      auto const callbackLock = std::scoped_lock{statePtr->targetCallbackMutex};
      statePtr->renderTarget = &target;
      statePtr->deliveredDrainGeneration = 0U;
      statePtr->deliveredErrorGeneration = 0U;
    }

    statePtr->deviceListenerFence.open();
    auto registeredListenerCount = std::size_t{0U};
    for (auto const& address : kDevicePropertyAddresses)
    {
      if (auto const status = ::AudioObjectAddPropertyListener(
            deviceId, &address, &coreAudioDeviceChanged, statePtr.get());
          status != ::noErr)
      {
        statePtr->deviceListenerFence.close();
        while (registeredListenerCount != 0U)
        {
          --registeredListenerCount;
          auto const& registeredAddress = kDevicePropertyAddresses[registeredListenerCount];
          ::AudioObjectRemovePropertyListener(
            deviceId, &registeredAddress, &coreAudioDeviceChanged, statePtr.get());
        }
        statePtr->deviceListenerFence.wait();
        close();
        return detail::makeCoreAudioError(
          status, "monitor Core Audio output device", Error::Code::InitFailed);
      }
      ++registeredListenerCount;
    }
    {
      auto const lock = std::scoped_lock{statePtr->nativeMutex};
      statePtr->registeredDeviceListenerCount = registeredListenerCount;
    }
    {
      auto const lock = std::scoped_lock{statePtr->graphStateMutex};
      statePtr->graphPublished = true;
    }

    target.handleRouteReady(statePtr->deviceUid);
    target.handleFormatChanged(selectedFormat);
    statePtr->publishGraph();
    return OpenedPcmMode{.clientFormat = selectedFormat};
  }

  void CoreAudioBackend::start()
  {
    auto const statePtr = _implPtr->statePtr;
    auto const runGeneration = statePtr->generation.fetch_add(1U, std::memory_order_acq_rel) + 1U;
    statePtr->pendingDrainGeneration.store(0U, std::memory_order_release);
    statePtr->pendingFault.store(0U, std::memory_order_release);
    statePtr->drainTail.reset();
    {
      auto const callbackLock = std::scoped_lock{statePtr->targetCallbackMutex};
      statePtr->deliveredDrainGeneration = 0U;
      statePtr->deliveredErrorGeneration = 0U;
    }
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
    stop();
    auto const statePtr = _implPtr->statePtr;
    auto status = ::OSStatus{::noErr};
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
    statePtr->generation.fetch_add(1U, std::memory_order_acq_rel);
    statePtr->runActive.store(false, std::memory_order_release);
    statePtr->renderAllowed.store(false, std::memory_order_release);
    statePtr->renderFence.closeAndWait();
    statePtr->deviceListenerFence.close();

    auto routeAnchor = std::string{};
    auto clearGraph = false;
    {
      auto const lock = std::scoped_lock{statePtr->graphStateMutex};
      routeAnchor = statePtr->deviceUid;
      clearGraph = statePtr->graphPublished;
      statePtr->graphPublished = false;
      statePtr->optClientFormat.reset();
      statePtr->optDeviceFormat.reset();
    }

    ::AudioUnit unit = nullptr;
    ::AudioDeviceID deviceId = ::kAudioObjectUnknown;
    auto registeredListenerCount = std::size_t{0U};
    {
      auto const lock = std::scoped_lock{statePtr->nativeMutex};
      unit = statePtr->unit;
      deviceId = statePtr->deviceId;
      registeredListenerCount = statePtr->registeredDeviceListenerCount;
      statePtr->registeredDeviceListenerCount = 0U;
      statePtr->unit = nullptr;
      statePtr->deviceId = ::kAudioObjectUnknown;
      statePtr->started = false;
      statePtr->stagingBuffer.clear();
      statePtr->bytesPerFrame = 0U;
      statePtr->maximumFramesPerSlice = 0U;
      statePtr->presentationTailFrameCount.store(0U, std::memory_order_release);
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

    {
      auto const callbackLock = std::scoped_lock{statePtr->targetCallbackMutex};
      statePtr->renderTarget = nullptr;
      statePtr->deliveredDrainGeneration = 0U;
      statePtr->deliveredErrorGeneration = 0U;
    }
    statePtr->drainTail.reset();
    if (clearGraph && statePtr->graphRegistryPtr && !routeAnchor.empty())
    {
      statePtr->graphRegistryPtr->clear(routeAnchor);
    }
  }

  Result<> CoreAudioBackend::setProperty(PropertyId const id, PropertyValue const& value)
  {
    auto const statePtr = _implPtr->statePtr;
    {
      auto const lock = std::scoped_lock{statePtr->propertyMutex};
      if (id == PropertyId::Volume)
      {
        auto const* requestedPtr = std::get_if<float>(&value);
        if (requestedPtr == nullptr || !std::isfinite(*requestedPtr))
        {
          return makeError(Error::Code::InvalidInput, "Core Audio volume must be a finite number");
        }
        auto const requested = std::clamp(*requestedPtr, 0.0F, 1.0F);
        {
          auto const nativeLock = std::scoped_lock{statePtr->nativeMutex};
          if (statePtr->unit != nullptr)
          {
            if (auto const status = ::AudioUnitSetParameter(statePtr->unit,
                                                            ::kHALOutputParam_Volume,
                                                            ::kAudioUnitScope_Global,
                                                            0U,
                                                            statePtr->cachedMuted ? 0.0F : requested,
                                                            0U);
                status != ::noErr)
            {
              return detail::makeCoreAudioError(status, "set Core Audio output volume", Error::Code::IoError);
            }
          }
        }
        statePtr->cachedVolume = requested;
      }
      else if (id == PropertyId::Muted)
      {
        auto const* requestedPtr = std::get_if<bool>(&value);
        if (requestedPtr == nullptr)
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
                                                            *requestedPtr ? 0.0F : statePtr->cachedVolume,
                                                            0U);
                status != ::noErr)
            {
              return detail::makeCoreAudioError(status, "set Core Audio output mute", Error::Code::IoError);
            }
          }
        }
        statePtr->cachedMuted = *requestedPtr;
      }
      else
      {
        return makeError(Error::Code::NotSupported);
      }
    }

    statePtr->publishGraph();
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
