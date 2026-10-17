// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CoreAudioBackend.h"

#include "detail/AudioBackendDrainTail.h"
#include "detail/AudioBackendRenderBuffer.h"
#include "detail/BackendGraphRegistry.h"
#include "detail/CallbackFence.h"
#include "detail/CoreAudioChannelLayout.h"
#include "detail/CoreAudioControlMailbox.h"
#include "detail/CoreAudioDeviceDiscovery.h"
#include "detail/CoreAudioError.h"
#include "detail/CoreAudioFormat.h"
#include "detail/CoreAudioGraph.h"
#include "detail/CoreAudioLatency.h"
#include "detail/CoreAudioRenderBuffer.h"
#include "detail/CoreAudioRunControl.h"
#include "detail/DecoderOutput.h"
#include "detail/SpeakerSet.h"
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/ChannelDeliveryEvidence.h>
#include <ao/audio/Device.h>
#include <ao/audio/OpenedPcmMode.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/Property.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
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
                                              .mElement = ::kAudioObjectPropertyElementMain},
                 ::AudioObjectPropertyAddress{.mSelector = ::kAudioDevicePropertyPreferredChannelLayout,
                                              .mScope = ::kAudioObjectPropertyScopeOutput,
                                              .mElement = ::kAudioObjectPropertyElementMain}};
    constexpr auto kUnitChannelProperties = std::to_array<::AudioUnitPropertyID>(
      {::kAudioUnitProperty_AudioChannelLayout, ::kAudioOutputUnitProperty_ChannelMap});

    bool canAdmitSourceFormat(SignalFormat const& sourceFormat) noexcept
    {
      return sourceFormat.sampleRate != 0U && sourceFormat.channels != 0U &&
             detail::hasSupportedSourceSpeakerSet(sourceFormat) &&
             ::ao::audio::detail::preferredLosslessPcmEncoding(sourceFormat).has_value();
    }

    BackendOpenFailure sourceFormatFailure(SignalFormat const& sourceFormat)
    {
      if (sourceFormat.sampleRate == 0U || sourceFormat.channels == 0U)
      {
        return {
          .error =
            makeError(Error::Code::InvalidInput, "Core Audio PCM formats require a sample rate and channels").error(),
          .cause = BackendOpenFailure::Cause::OutputFailure};
      }

      if (!detail::hasSupportedSourceSpeakerSet(sourceFormat))
      {
        return {
          .error = makeError(Error::Code::FormatRejected, "Core Audio cannot identify the source channel set").error(),
          .cause = BackendOpenFailure::Cause::TrackRouteIncompatible};
      }

      return {
        .error =
          makeError(Error::Code::FormatRejected, "No lossless Core Audio client PCM encoding is available").error(),
        .cause = BackendOpenFailure::Cause::TrackRouteIncompatible};
    }

    using detail::CoreAudioFault;

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
        case CoreAudioFault::ChannelRouteChanged:
          return "Core Audio channel relationship changed or could not be refreshed";
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
      std::array<bool, kDevicePropertyAddresses.size()> registeredDeviceListeners{};
      std::array<bool, kUnitChannelProperties.size()> registeredUnitListeners{};

      detail::CallbackFence renderFence{};
      detail::CallbackFence deviceListenerFence{};
      RenderTarget* renderTarget = nullptr;
      std::recursive_mutex targetCallbackMutex{};

      std::vector<std::byte> stagingBuffer{};
      std::vector<std::byte> nativeBuffer{};
      detail::CoreAudioChannelPlan channelPlan{};
      std::size_t sampleBytes = 0U;
      std::size_t bytesPerFrame = 0U;
      ::UInt32 maximumFramesPerSlice = 0U;
      std::atomic<std::uint64_t> presentationTailFrameCount{0U};
      detail::AudioBackendDrainTail drainTail{};

      mutable std::recursive_mutex graphPublicationMutex{};
      mutable std::mutex graphStateMutex{};
      // Canonical render-target contract stays separate from native graph storage.
      std::optional<PcmFormat> optClientFormat{};
      std::optional<PcmFormat> optNativeClientFormat{};
      std::optional<SignalFormat> optDeviceFormat{};
      ChannelDeliveryEvidence channelDelivery{};
      ChannelDeliveryEvidence outputChannelDelivery{};
      bool graphPublished = false;

      mutable std::mutex propertyMutex{};
      float cachedVolume = 1.0F;
      bool cachedMuted = false;

      // Public controls acquire targetCallbackMutex before the run gate.
      // Workers release the gate before callback delivery or graph publication.
      detail::CoreAudioRunControl runControl{};
      std::atomic<bool> runActive{false};
      std::atomic<bool> renderAllowed{false};
      std::atomic<std::uint64_t> pendingDrainGeneration{0U};
      std::atomic<bool> pendingGraphPublication{false};
      detail::CoreAudioControlMailbox controlMailbox;
      std::atomic<bool> controlStop{false};
      std::counting_semaphore<> controlSignal{0};
      std::uint64_t deliveredDrainGeneration = 0U;
      std::uint64_t deliveredErrorGeneration = 0U;

      void signalDrainReady(std::uint64_t drainGeneration) noexcept;
      void signalDeviceChange() noexcept;
      void signalFault(CoreAudioFault fault, std::uint64_t faultGeneration = 0U) noexcept;
      void requestGraphPublication() noexcept;
      void controlLoop();
      void handleDrain(std::uint64_t drainGeneration);
      // Execution mode proves transport only; the worker owns full diagnostics.
      // A supplied flag defers publication and records whether a snapshot was updated.
      bool tryRefreshDevice(std::uint64_t deviceGeneration,
                            detail::CoreAudioChannelRefreshMode mode = detail::CoreAudioChannelRefreshMode::Observation,
                            bool* publicationDeferred = nullptr);
      bool tryFinishDeviceRefresh(std::uint64_t deviceGeneration, CoreAudioFault fault, bool* publicationDeferred);
      void handleFault(detail::CoreAudioControlFault const& packet);
      void publishGraph() const;
      void reportError(std::uint64_t errorGeneration, std::string_view message);
    };

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
      std::ignore = controlMailbox.tryPublishDeviceChange(runControl.current());
      controlSignal.release();
    }

    void CoreAudioRuntimeState::signalFault(CoreAudioFault const fault, std::uint64_t const faultGeneration) noexcept
    {
      auto const currentGeneration = faultGeneration == 0U ? runControl.current() : faultGeneration;

      if (!runControl.isCurrent(currentGeneration))
      {
        return;
      }

      if (controlMailbox.tryPublishFault(currentGeneration, fault))
      {
        controlSignal.release();
      }
    }

    void CoreAudioRuntimeState::requestGraphPublication() noexcept
    {
      pendingGraphPublication.store(true, std::memory_order_release);
      controlSignal.release();
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

        if (auto const deviceGeneration = controlMailbox.takeDeviceChange(); deviceGeneration != 0U)
        {
          bool publicationDeferred = false;
          {
            // A current-generation notification may arrive during start/resume's
            // minimal proof. Native-mutex gaps do not admit full observation while
            // those controls still hold their run scope and callback lock.
            auto const optRunScope = runControl.tryCurrent(deviceGeneration);

            if (optRunScope)
            {
              tryRefreshDevice(
                deviceGeneration, detail::CoreAudioChannelRefreshMode::Observation, &publicationDeferred);
            }
          }

          // Observers may close or reenter controls; release the run gate first.
          if (publicationDeferred)
          {
            publishGraph();
          }
        }

        if (auto const optFault = controlMailbox.takeFault(); optFault)
        {
          handleFault(*optFault);
        }

        if (auto const drainGeneration = pendingDrainGeneration.exchange(0U, std::memory_order_acq_rel);
            drainGeneration != 0U)
        {
          handleDrain(drainGeneration);
        }

        // Property changes may originate inside target callbacks. Publish only
        // after their callback locks have unwound, using the current graph state.
        if (pendingGraphPublication.exchange(false, std::memory_order_acq_rel))
        {
          publishGraph();
        }
      }
    }

    void CoreAudioRuntimeState::handleDrain(std::uint64_t const drainGeneration)
    {
      {
        auto const optRunScope = runControl.tryCurrent(drainGeneration);

        if (!optRunScope || !runActive.exchange(false, std::memory_order_acq_rel))
        {
          return;
        }

        renderFence.closeAndWait();
        auto const lock = std::scoped_lock{nativeMutex};

        if (unit != nullptr && started)
        {
          if (::AudioOutputUnitStop(unit) != ::noErr)
          {
            signalFault(CoreAudioFault::DeviceLost, drainGeneration);
            return;
          }

          started = false;
        }
      }

      auto const callbackLock = std::scoped_lock{targetCallbackMutex};

      if (runControl.isCurrent(drainGeneration) && renderTarget != nullptr &&
          deliveredDrainGeneration != drainGeneration)
      {
        deliveredDrainGeneration = drainGeneration;
        renderTarget->handleDrainComplete();
      }
    }

    bool CoreAudioRuntimeState::tryRefreshDevice(std::uint64_t const deviceGeneration,
                                                 detail::CoreAudioChannelRefreshMode const mode,
                                                 bool* const publicationDeferred)
    {
      if (publicationDeferred != nullptr)
      {
        *publicationDeferred = false;
      }

      if (!runControl.isCurrent(deviceGeneration))
      {
        return false;
      }

      ::AudioObjectID device = ::kAudioObjectUnknown;
      {
        auto const lock = std::scoped_lock{nativeMutex};

        if (!runControl.isCurrent(deviceGeneration) || unit == nullptr)
        {
          return false;
        }

        device = deviceId;
      }

      auto aliveAddress = kGlobalMain;
      aliveAddress.mSelector = ::kAudioDevicePropertyDeviceIsAlive;
      ::UInt32 alive = 0U;
      auto const aliveStatus = readAudioObjectValue(device, aliveAddress, alive);

      if (aliveStatus != ::noErr || alive == 0U)
      {
        signalFault(CoreAudioFault::DeviceLost, deviceGeneration);
        return false;
      }

      auto optClient = std::optional<PcmFormat>{};
      {
        auto const lock = std::scoped_lock{graphStateMutex};
        optClient = optClientFormat;
      }

      if (!optClient)
      {
        return false;
      }

      auto fault = CoreAudioFault::None;
      {
        auto const lock = std::scoped_lock{nativeMutex};

        if (!runControl.isCurrent(deviceGeneration) || unit == nullptr)
        {
          return false;
        }

        auto const refreshRes =
          detail::refreshCoreAudioChannels(detail::coreAudioChannelOperations(unit, device),
                                           *optClient,
                                           channelPlan,
                                           mode,
                                           [&] { return runControl.isCurrent(deviceGeneration) && unit != nullptr; });

        // Borrow from refreshRes only for this native-locked refresh.
        detail::CoreAudioChannelRefresh const* refreshed = nullptr;

        if (refreshRes)
        {
          auto const& optRefresh = *refreshRes;

          if (!optRefresh)
          {
            return false;
          }

          refreshed = std::addressof(*optRefresh);
        }

        auto optTailFrames = std::optional<std::uint64_t>{};
        auto optOutputSignal = std::optional<SignalFormat>{};

        if (!refreshRes)
        {
          fault = refreshRes.error().error.code == Error::Code::DeviceNotFound ? CoreAudioFault::DeviceLost
                                                                               : CoreAudioFault::ChannelRouteChanged;
        }
        else if (auto const& optOutput = refreshed->optOutputFormat; optOutput)
        {
          auto const signalRes = detail::coreAudioSignalFormat(*optOutput);

          if (signalRes)
          {
            optOutputSignal = *signalRes;
          }

          // The initial open-time tail remains provisioned until the worker
          // observes a replacement. Start/resume never enumerate stream latency.
          if (mode == detail::CoreAudioChannelRefreshMode::Observation)
          {
            if (auto const tailFramesRes = presentationTailFrames(unit, device, *optOutput, optClient->sampleRate);
                tailFramesRes)
            {
              optTailFrames = *tailFramesRes;
            }
          }
        }

        // Retain nativeMutex through snapshot assignment so overlapping refreshes
        // cannot publish an older read after a newer read of the same generation.
        auto const graphLock = std::scoped_lock{graphStateMutex};

        if (!runControl.isCurrent(deviceGeneration))
        {
          return false;
        }

        // No cached/synthetic output numerics survive an unavailable observation.
        optDeviceFormat = optOutputSignal;

        if (!refreshRes)
        {
          optNativeClientFormat.reset();
          // Failed observation removes position certainty, not known conversion.
          channelDelivery = {.hasSharedConversion = channelDelivery.hasSharedConversion};
          outputChannelDelivery = {.hasSharedConversion = outputChannelDelivery.hasSharedConversion};
        }
        else
        {
          auto const& plan = refreshed->plan;
          optNativeClientFormat = plan.nativeClientFormat;
          channelDelivery = plan.delivery;
          outputChannelDelivery = plan.outputDelivery;
        }

        if (optTailFrames)
        {
          presentationTailFrameCount.store(*optTailFrames, std::memory_order_release);
        }
      }

      return tryFinishDeviceRefresh(deviceGeneration, fault, publicationDeferred);
    }

    bool CoreAudioRuntimeState::tryFinishDeviceRefresh(std::uint64_t const deviceGeneration,
                                                       CoreAudioFault const fault,
                                                       bool* const publicationDeferred)
    {
      // Only a completed snapshot reaches this publication boundary, after
      // nativeMutex and graphStateMutex have been released.
      if (publicationDeferred != nullptr)
      {
        *publicationDeferred = true;
      }
      else
      {
        publishGraph();
      }

      if (fault != CoreAudioFault::None && runControl.isCurrent(deviceGeneration))
      {
        signalFault(fault, deviceGeneration);
      }

      return fault == CoreAudioFault::None && runControl.isCurrent(deviceGeneration);
    }

    void CoreAudioRuntimeState::handleFault(detail::CoreAudioControlFault const& packet)
    {
      auto const faultGeneration = packet.generation;
      auto const fault = packet.kind;

      {
        auto const optRunScope = runControl.tryCurrent(faultGeneration);

        if (!optRunScope)
        {
          return;
        }

        runActive.store(false, std::memory_order_release);
        renderAllowed.store(false, std::memory_order_release);
        renderFence.closeAndWait();
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
          state.optClientFormat = optNativeClientFormat;
          state.optDeviceFormat = optDeviceFormat;
          state.channelDelivery = channelDelivery;
          state.outputChannelDelivery = outputChannelDelivery;
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

      if (runControl.isCurrent(errorGeneration) && renderTarget != nullptr &&
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
      // Identity rendering can use validated AU storage directly, including
      // binding null AU storage to staging. Only a true permutation needs two buffers.
      auto const hasIdentityOrder = state.channelPlan.hasIdentityOrder;
      auto const fallbackBuffer = hasIdentityOrder ? std::span{state.stagingBuffer} : std::span{state.nativeBuffer};
      auto const outputBuffer = detail::bindCoreAudioRenderBuffer(
        outputData, fallbackBuffer, byteCount, state.channelPlan.nativeClientFormat.channels);

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
          state.signalDrainReady(state.runControl.current());
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

      auto const renderBuffer =
        hasIdentityOrder ? outputBuffer.output : std::span{state.stagingBuffer}.first(byteCount);
      auto const renderResult = target->renderPcm(renderBuffer);
      auto const prepared = detail::prepareAudioBackendRenderBuffer(renderBuffer, state.bytesPerFrame, renderResult);

      if (!hasIdentityOrder)
      {
        detail::copyCoreAudioChannels(
          outputBuffer.output, renderBuffer, state.sampleBytes, state.channelPlan.sourceSlots);
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
          state.signalDrainReady(state.runControl.current());
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

    void coreAudioUnitChannelsChanged(void* const context,
                                      ::AudioUnit /*unit*/,
                                      ::AudioUnitPropertyID /*property*/,
                                      ::AudioUnitScope /*scope*/,
                                      ::AudioUnitElement /*element*/) noexcept
    {
      auto& state = *static_cast<CoreAudioRuntimeState*>(context);

      if (!state.deviceListenerFence.tryEnter())
      {
        return;
      }

      auto const callbackLeave = CallbackLeave{state.deviceListenerFence};
      state.signalDeviceChange();
    }

    Result<> installDeviceListeners(::AudioObjectID const deviceId, CoreAudioRuntimeState& state)
    {
      state.deviceListenerFence.open();
      auto const lock = std::scoped_lock{state.nativeMutex};

      for (std::size_t index = 0U; index < kDevicePropertyAddresses.size(); ++index)
      {
        auto const& address = kDevicePropertyAddresses[index];

        if (index >= 3U && ::AudioObjectHasProperty(deviceId, &address) == 0U)
        {
          continue;
        }

        if (auto const status = ::AudioObjectAddPropertyListener(deviceId, &address, &coreAudioDeviceChanged, &state);
            status != ::noErr)
        {
          return detail::makeCoreAudioError(status, "monitor Core Audio output device", Error::Code::InitFailed);
        }

        state.registeredDeviceListeners[index] = true;
      }

      for (std::size_t index = 0U; index < kUnitChannelProperties.size(); ++index)
      {
        auto const status = ::AudioUnitAddPropertyListener(
          state.unit, kUnitChannelProperties[index], &coreAudioUnitChannelsChanged, &state);

        if (status == ::kAudioUnitErr_InvalidProperty || status == ::kAudioUnitErr_PropertyNotInUse)
        {
          continue;
        }

        if (status != ::noErr)
        {
          return detail::makeCoreAudioError(status, "monitor Core Audio channel relationship", Error::Code::InitFailed);
        }

        state.registeredUnitListeners[index] = true;
      }

      return {};
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
    return canAdmitSourceFormat(sourceFormat) ? Backend::prewarmFormatHint(sourceFormat) : std::nullopt;
  }

  BackendOpenResult CoreAudioBackend::open(SignalFormat const& sourceFormat, RenderTarget& target)
  {
    // Reject deterministic source refusals before this backend's own close().
    // Engine closes its previous playback before calling open().
    if (!canAdmitSourceFormat(sourceFormat))
    {
      return std::unexpected{sourceFormatFailure(sourceFormat)};
    }

    close();
    auto const statePtr = _implPtr->statePtr;
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

    auto initializedClient = ::AudioStreamBasicDescription{};
    auto const requestedClientRes = detail::coreAudioFormat(selectedFormat);
    auto const clientStatus =
      readAudioUnitProperty(rawUnit, ::kAudioUnitProperty_StreamFormat, ::kAudioUnitScope_Input, 0U, initializedClient);

    if (clientStatus != ::noErr)
    {
      return std::unexpected{BackendOpenFailure{
        .error =
          detail::makeCoreAudioError(clientStatus, "read Core Audio initialized client format", Error::Code::InitFailed)
            .error(),
        .cause = BackendOpenFailure::Cause::OutputFailure}};
    }

    if (!requestedClientRes || !detail::isSameCoreAudioPcmFormat(*requestedClientRes, initializedClient))
    {
      return std::unexpected{BackendOpenFailure{
        .error = makeError(Error::Code::InitFailed, "Core Audio initialized a different client PCM mode").error(),
        .cause = BackendOpenFailure::Cause::OutputFailure}};
    }

    auto channelPlanRes = detail::configureCoreAudioChannels(
      detail::coreAudioChannelOperations(rawUnit, deviceId), selectedFormat, outputDescription);

    if (!channelPlanRes)
    {
      return std::unexpected{channelPlanRes.error()};
    }

    auto nativeBuffer = std::vector<std::byte>(channelPlanRes->hasIdentityOrder ? 0U : stagingBuffer.size());
    auto const openedMode = detail::coreAudioOpenedPcmMode(selectedFormat, *channelPlanRes);
    auto const channelDelivery = openedMode.channelDelivery;
    auto const outputChannelDelivery = channelPlanRes->outputDelivery;
    auto const nativeClientFormat = channelPlanRes->nativeClientFormat;
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

    {
      auto const runScope = statePtr->runControl.advance();
      statePtr->pendingDrainGeneration.store(0U, std::memory_order_release);
      statePtr->runActive.store(false, std::memory_order_release);
      statePtr->renderAllowed.store(false, std::memory_order_release);
      auto const lock = std::scoped_lock{statePtr->nativeMutex};
      statePtr->unit = unitOwner.release();
      statePtr->deviceId = deviceId;
      statePtr->started = false;
      statePtr->bytesPerFrame = bytesPerFrame;
      statePtr->maximumFramesPerSlice = maximumFrames;
      statePtr->presentationTailFrameCount.store(*tailFramesRes, std::memory_order_release);
      statePtr->stagingBuffer = std::move(stagingBuffer);
      statePtr->nativeBuffer = std::move(nativeBuffer);
      statePtr->sampleBytes = bytesPerSample(selectedFormat.encoding);
      statePtr->channelPlan = std::move(*channelPlanRes);
    }

    {
      auto const lock = std::scoped_lock{statePtr->graphStateMutex};
      auto const signalRes = detail::coreAudioSignalFormat(outputDescription);
      statePtr->optClientFormat = selectedFormat;
      statePtr->optNativeClientFormat = nativeClientFormat;
      statePtr->optDeviceFormat = signalRes ? std::optional{*signalRes} : std::nullopt;
      statePtr->channelDelivery = channelDelivery;
      statePtr->outputChannelDelivery = outputChannelDelivery;
    }

    {
      auto const callbackLock = std::scoped_lock{statePtr->targetCallbackMutex};
      statePtr->renderTarget = &target;
      statePtr->deliveredDrainGeneration = 0U;
      statePtr->deliveredErrorGeneration = 0U;
    }

    auto const listenersRes = installDeviceListeners(deviceId, *statePtr);

    if (!listenersRes)
    {
      close();
      return std::unexpected{
        BackendOpenFailure{.error = listenersRes.error(), .cause = BackendOpenFailure::Cause::OutputFailure}};
    }

    {
      auto const lock = std::scoped_lock{statePtr->graphStateMutex};
      statePtr->graphPublished = true;
    }

    target.handleRouteReady(statePtr->deviceUid);
    target.handleFormatChanged(selectedFormat);
    statePtr->publishGraph();
    return openedMode;
  }

  void CoreAudioBackend::start()
  {
    auto const statePtr = _implPtr->statePtr;
    std::uint64_t runGeneration = 0U;
    ::OSStatus status = ::noErr;
    bool publicationDeferred = false;
    bool shouldRefreshObservation = false;
    {
      auto const callbackLock = std::scoped_lock{statePtr->targetCallbackMutex};
      auto const runScope = statePtr->runControl.advance();
      runGeneration = runScope.generation();
      statePtr->pendingDrainGeneration.store(0U, std::memory_order_release);
      statePtr->drainTail.reset();
      // Keep delivery markers: the preceding run's generation naturally differs.
      // Advancing retires queued open-time notifications. Prove current execution
      // before rendering, then requeue optional observation for this generation.
      // Publication waits until both locks are released; observers may reenter.
      if (statePtr->tryRefreshDevice(
            runGeneration, detail::CoreAudioChannelRefreshMode::Execution, &publicationDeferred))
      {
        shouldRefreshObservation = true;
        statePtr->renderAllowed.store(true, std::memory_order_release);
        statePtr->runActive.store(true, std::memory_order_release);
        statePtr->renderFence.open();

        bool hasUnit = false;
        {
          auto const lock = std::scoped_lock{statePtr->nativeMutex};
          hasUnit = statePtr->unit != nullptr;

          if (hasUnit)
          {
            status = ::AudioOutputUnitStart(statePtr->unit);
            statePtr->started = status == ::noErr;
          }
        }

        if (!hasUnit || status != ::noErr)
        {
          statePtr->renderFence.closeAndWait();
          statePtr->runActive.store(false, std::memory_order_release);
          statePtr->renderAllowed.store(false, std::memory_order_release);
        }
      }
    }

    if (shouldRefreshObservation && statePtr->runControl.isCurrent(runGeneration))
    {
      statePtr->signalDeviceChange();
    }

    if (publicationDeferred)
    {
      statePtr->publishGraph();
    }

    if (status != ::noErr)
    {
      auto const error = detail::makeCoreAudioError(status, "start Core Audio output", Error::Code::IoError).error();
      statePtr->reportError(runGeneration, error.message);
    }
  }

  void CoreAudioBackend::pause()
  {
    auto const statePtr = _implPtr->statePtr;
    auto const callbackLock = std::scoped_lock{statePtr->targetCallbackMutex};
    auto const runScope = statePtr->runControl.lock();
    statePtr->renderFence.closeAndWait();
    auto const lock = std::scoped_lock{statePtr->nativeMutex};

    if (statePtr->unit != nullptr && statePtr->started)
    {
      if (auto const status = ::AudioOutputUnitStop(statePtr->unit); status != ::noErr)
      {
        statePtr->signalFault(CoreAudioFault::DeviceLost, runScope.generation());
      }

      statePtr->started = false;
    }
  }

  void CoreAudioBackend::resume()
  {
    auto const statePtr = _implPtr->statePtr;
    std::uint64_t runGeneration = 0U;
    bool publicationDeferred = false;
    bool shouldRefreshObservation = false;
    {
      auto const callbackLock = std::scoped_lock{statePtr->targetCallbackMutex};
      auto const runScope = statePtr->runControl.lock();
      runGeneration = runScope.generation();

      if (statePtr->runActive.load(std::memory_order_acquire))
      {
        if (statePtr->tryRefreshDevice(
              runGeneration, detail::CoreAudioChannelRefreshMode::Execution, &publicationDeferred))
        {
          shouldRefreshObservation = true;
          statePtr->renderFence.open();
          bool startFailed = false;
          {
            auto const lock = std::scoped_lock{statePtr->nativeMutex};

            if (statePtr->unit == nullptr)
            {
              startFailed = true;
            }
            else if (!statePtr->started)
            {
              if (auto const status = ::AudioOutputUnitStart(statePtr->unit); status != ::noErr)
              {
                startFailed = true;
                statePtr->signalFault(CoreAudioFault::DeviceLost, runScope.generation());
              }
              else
              {
                statePtr->started = true;
              }
            }
          }

          if (startFailed)
          {
            statePtr->renderFence.closeAndWait();
            statePtr->runActive.store(false, std::memory_order_release);
          }
        }
      }
    }

    if (shouldRefreshObservation && statePtr->runControl.isCurrent(runGeneration))
    {
      statePtr->signalDeviceChange();
    }

    if (publicationDeferred)
    {
      statePtr->publishGraph();
    }
  }

  void CoreAudioBackend::flush()
  {
    stop();
    auto const statePtr = _implPtr->statePtr;
    std::uint64_t runGeneration = 0U;
    ::OSStatus status = ::noErr;
    {
      auto const callbackLock = std::scoped_lock{statePtr->targetCallbackMutex};
      auto const runScope = statePtr->runControl.lock();
      runGeneration = runScope.generation();
      auto const lock = std::scoped_lock{statePtr->nativeMutex};

      if (statePtr->unit != nullptr)
      {
        status = ::AudioUnitReset(statePtr->unit, ::kAudioUnitScope_Global, 0U);
      }
    }

    if (status != ::noErr)
    {
      auto const error = detail::makeCoreAudioError(status, "reset Core Audio output", Error::Code::IoError).error();
      statePtr->reportError(runGeneration, error.message);
    }
  }

  void CoreAudioBackend::stop()
  {
    auto const statePtr = _implPtr->statePtr;
    auto const callbackLock = std::scoped_lock{statePtr->targetCallbackMutex};
    auto const runScope = statePtr->runControl.advance();
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
    // Retire the control-side capability before callback/native retirement.
    auto const statePtr = _implPtr->statePtr;
    ::AudioUnit unit = nullptr;
    ::AudioObjectID deviceId = ::kAudioObjectUnknown;
    auto registeredDeviceListeners = std::array<bool, kDevicePropertyAddresses.size()>{};
    auto registeredUnitListeners = std::array<bool, kUnitChannelProperties.size()>{};
    {
      auto const callbackLock = std::scoped_lock{statePtr->targetCallbackMutex};
      auto const runScope = statePtr->runControl.advance();
      statePtr->runActive.store(false, std::memory_order_release);
      statePtr->renderAllowed.store(false, std::memory_order_release);
      statePtr->renderFence.closeAndWait();
      statePtr->deviceListenerFence.close();
      {
        auto const lock = std::scoped_lock{statePtr->nativeMutex};
        unit = statePtr->unit;
        deviceId = statePtr->deviceId;
        registeredDeviceListeners = std::exchange(statePtr->registeredDeviceListeners, {});
        registeredUnitListeners = std::exchange(statePtr->registeredUnitListeners, {});
        statePtr->unit = nullptr;
        statePtr->deviceId = ::kAudioObjectUnknown;
        statePtr->started = false;
        statePtr->stagingBuffer.clear();
        statePtr->nativeBuffer.clear();
        statePtr->channelPlan = {};
        statePtr->sampleBytes = 0U;
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
        statePtr->optNativeClientFormat.reset();
        statePtr->optDeviceFormat.reset();
        statePtr->channelDelivery = {};
        statePtr->outputChannelDelivery = {};
      }

      if (clearGraph && statePtr->graphRegistryPtr && !routeAnchor.empty())
      {
        pendingDelivery = statePtr->graphRegistryPtr->prepareClear(routeAnchor);
      }
    }

    for (std::size_t index = 0U; index < registeredDeviceListeners.size(); ++index)
    {
      if (registeredDeviceListeners[index])
      {
        auto const& address = kDevicePropertyAddresses[index];
        ::AudioObjectRemovePropertyListener(deviceId, &address, &coreAudioDeviceChanged, statePtr.get());
      }
    }

    for (std::size_t index = 0U; index < registeredUnitListeners.size(); ++index)
    {
      if (registeredUnitListeners[index])
      {
        ::AudioUnitRemovePropertyListenerWithUserData(
          unit, kUnitChannelProperties[index], &coreAudioUnitChannelsChanged, statePtr.get());
      }
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
