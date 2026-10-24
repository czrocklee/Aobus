// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/CoreAudioBackend.h"
#include "lib/audio/backend/detail/BackendGraphRegistry.h"
#include "test/unit/audio/backend/CoreAudioTestSupport.h"
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/Property.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/flow/Graph.h>

#include <AudioToolbox/AUComponent.h>
#include <AudioToolbox/AudioOutputUnit.h>
#include <AudioToolbox/AudioUnit.h>
#include <AudioToolbox/AudioUnitProperties.h>
#include <CoreAudio/AudioHardwareBase.h>
#include <MacTypes.h>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <semaphore>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::audio::backend::test
{
  namespace
  {
    constexpr auto kProgressTimeout = std::chrono::seconds{5};
    constexpr ::OSStatus kStopFailure = -123456;
    constexpr ::OSStatus kFaultStopFailure = -123457;
    constexpr ::OSStatus kResumeStartFailure = -123458;
    constexpr ::OSStatus kNotCalled = -123459;
    constexpr ::UInt32 kReportedMaximumFrames = 1U;
    constexpr auto kSourceFormat =
      SignalFormat{.sampleRate = 44100, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::Integer};

    Device requireRunStateTestOutput()
    {
      auto const selectedRes = audio::test::selectCoreAudioTestOutput();

      if (!selectedRes)
      {
        FAIL("AOBUS_TEST_COREAUDIO_DEVICE_UID: " << selectedRes.error().message);
      }

      if (!*selectedRes)
      {
        SKIP("macOS host has no live Core Audio output device");
      }

      return **selectedRes;
    }

    // A nonzero configured status is a controlled result without an SDK call.
    // It does not claim that the SDK itself failed to start or stop the unit.
    struct NativeRunState final
    {
      std::atomic<::OSStatus> startStatus{::noErr};
      std::atomic<::OSStatus> stopStatus{::noErr};
      std::atomic<std::size_t> startCalls{0U};
      std::atomic<std::size_t> stopCalls{0U};
      std::atomic<std::size_t> sdkStartCalls{0U};
      std::atomic<std::size_t> sdkStopCalls{0U};
      std::atomic<std::size_t> resetCalls{0U};
      std::atomic<std::size_t> badArguments{0U};
      std::atomic<::OSStatus> lastSdkStartStatus{kNotCalled};
      std::atomic<::OSStatus> lastSdkStopStatus{kNotCalled};
      std::atomic<::OSStatus> lastResetStatus{kNotCalled};
      // Identity observation only; never dereferenced or used after detachment.
      std::atomic<::AudioUnit> lastStartedUnit{nullptr};
    };

    CoreAudioBackend::NativeOperations runStateOperations(NativeRunState& state)
    {
      return {.run = {.start = [&state](::AudioUnit const unit) -> ::OSStatus
                      {
                        state.startCalls.fetch_add(1U);
                        state.lastStartedUnit.store(unit);

                        if (unit == nullptr)
                        {
                          state.badArguments.fetch_add(1U);
                          return ::kAudioUnitErr_InvalidParameter;
                        }

                        if (auto const status = state.startStatus.load(); status != ::noErr)
                        {
                          return status;
                        }

                        state.sdkStartCalls.fetch_add(1U);
                        auto const sdkStatus = ::AudioOutputUnitStart(unit);
                        state.lastSdkStartStatus.store(sdkStatus);
                        return sdkStatus;
                      },
                      .stop = [&state](::AudioUnit const unit) -> ::OSStatus
                      {
                        state.stopCalls.fetch_add(1U);

                        if (unit == nullptr || unit != state.lastStartedUnit.load())
                        {
                          state.badArguments.fetch_add(1U);
                          return ::kAudioUnitErr_InvalidParameter;
                        }

                        if (auto const status = state.stopStatus.load(); status != ::noErr)
                        {
                          return status;
                        }

                        state.sdkStopCalls.fetch_add(1U);
                        auto const sdkStatus = ::AudioOutputUnitStop(unit);
                        state.lastSdkStopStatus.store(sdkStatus);
                        return sdkStatus;
                      }},
              .unit = {.reset = [&state](::AudioUnit const unit,
                                         ::AudioUnitScope const scope,
                                         ::AudioUnitElement const element) -> ::OSStatus
                       {
                         state.resetCalls.fetch_add(1U);

                         if (unit == nullptr || unit != state.lastStartedUnit.load() ||
                             scope != ::kAudioUnitScope_Global || element != 0U)
                         {
                           state.badArguments.fetch_add(1U);
                           return ::kAudioUnitErr_InvalidParameter;
                         }

                         auto const status = ::AudioUnitReset(unit, scope, element);
                         state.lastResetStatus.store(status);
                         return status;
                       }}};
    }

    class RunStateRenderTarget final : public RenderTarget
    {
    public:
      explicit RunStateRenderTarget(bool const drains = false)
        : _drains{drains}
      {
      }

      RenderPcmResult renderPcm(std::span<std::byte> const output) noexcept override
      {
        _renderCalls.fetch_add(1U, std::memory_order_release);
        _renderEntered.release();

        if (_drains)
        {
          return {.drained = true};
        }

        std::ranges::fill(output, std::byte{0});
        return {.bytesWritten = output.size()};
      }

      void handleUnderrun() noexcept override {}
      void handlePositionAdvanced(std::uint32_t /*frames*/) noexcept override {}
      void handleDrainComplete() noexcept override { _drainCompletions.fetch_add(1U, std::memory_order_release); }
      void handleRouteReady(std::string_view /*routeAnchor*/) noexcept override {}
      void handleFormatChanged(PcmFormat const& /*format*/) noexcept override {}
      void handlePropertyChanged(PropertySnapshot /*snapshot*/) noexcept override {}
      void handleBackendError(std::string_view const message) noexcept override
      {
        {
          auto const lock = std::scoped_lock{_errorMutex};
          _errors.emplace_back(message);
        }

        _errorReported.release();
      }

      bool tryWaitForRenderAfter(std::size_t const previousCalls)
      {
        auto const deadline = std::chrono::steady_clock::now() + kProgressTimeout;

        while (renderCalls() <= previousCalls)
        {
          if (!_renderEntered.try_acquire_until(deadline))
          {
            return false;
          }
        }

        return true;
      }

      bool tryWaitForError() { return _errorReported.try_acquire_for(kProgressTimeout); }
      std::size_t renderCalls() const noexcept { return _renderCalls.load(std::memory_order_acquire); }
      std::size_t drainCompletions() const noexcept { return _drainCompletions.load(std::memory_order_acquire); }
      std::vector<std::string> errors() const
      {
        auto const lock = std::scoped_lock{_errorMutex};
        return _errors;
      }

    private:
      bool const _drains;
      std::atomic<std::size_t> _renderCalls{0U};
      std::atomic<std::size_t> _drainCompletions{0U};
      std::counting_semaphore<> _renderEntered{0};
      std::counting_semaphore<> _errorReported{0};
      mutable std::mutex _errorMutex{};
      std::vector<std::string> _errors{};
    };

    void requireRunError(RunStateRenderTarget const& target,
                         std::size_t const expectedCount,
                         std::string_view const expectedMessage)
    {
      auto const errors = target.errors();
      REQUIRE(errors.size() == expectedCount);
      CHECK(errors.back() == expectedMessage);
    }

    void requireControlWorkerProgress(CoreAudioBackend& backend,
                                      detail::BackendGraphRegistry& registry,
                                      std::string_view const routeAnchor)
    {
      auto quarterPublished = std::counting_semaphore{0};
      auto threeQuartersPublished = std::counting_semaphore{0};
      // A repeated call may initially observe the previous 0.75 snapshot;
      // only a 0.75 publication following this call's 0.25 counts as progress.
      auto quarterObserved = std::atomic_bool{false};
      auto sub =
        registry.subscribe(routeAnchor,
                           [&](flow::Graph const& graph)
                           {
                             auto const sink = std::ranges::find(graph.nodes, routeAnchor, &flow::Node::id);

                             if (sink == graph.nodes.end())
                             {
                               return;
                             }

                             if (sink->maxSoftwareGain == 0.25F)
                             {
                               quarterObserved.store(true, std::memory_order_release);
                               quarterPublished.release();
                             }
                             else if (sink->maxSoftwareGain == 0.75F && quarterObserved.load(std::memory_order_acquire))
                             {
                               threeQuartersPublished.release();
                             }
                           });
      REQUIRE(sub);
      REQUIRE(backend.set(props::kVolume, 0.25F));
      REQUIRE(quarterPublished.try_acquire_for(kProgressTimeout));
      REQUIRE(backend.set(props::kVolume, 0.75F));
      REQUIRE(threeQuartersPublished.try_acquire_for(kProgressTimeout));
      // Cancellation fences the observer before either semaphore is destroyed.
      sub.reset();
      auto const volumeRes = backend.get(props::kVolume);
      REQUIRE(volumeRes);
      CHECK(*volumeRes == 0.75F);
    }

    struct MaximumFramesRead final
    {
      ::AudioUnit unit = nullptr;
      ::AudioUnitPropertyID property = 0U;
      ::AudioUnitScope scope = 0U;
      ::AudioUnitElement element = 0U;
      ::UInt32 inputSize = 0U;
      ::UInt32 sdkSize = 0U;
      ::UInt32 sdkMaximumFrames = 0U;
      ::OSStatus sdkStatus = kNotCalled;
    };

    struct MaximumFramesObservation final
    {
      std::atomic<std::size_t> calls{0U};
      std::atomic<std::size_t> badArguments{0U};
      std::array<MaximumFramesRead, 2U> reads{};
    };
  } // namespace

  TEST_CASE("CoreAudioBackend - failed pause withholds activation and reset until an explicit stop succeeds",
            "[audio][integration][coreaudio][backend][coreaudio-run-state][concurrency]")
  {
    auto const device = requireRunStateTestOutput();
    CAPTURE(device.id.raw());
    // Captures and target precede the backend: destruction closes the native
    // owner and joins its worker before any borrowed state is destroyed.
    auto native = NativeRunState{};
    native.stopStatus.store(kStopFailure);
    auto target = RunStateRenderTarget{};
    auto graphRegistryPtr = std::make_shared<detail::BackendGraphRegistry>();
    auto backend = CoreAudioBackend{device, kProfileShared, graphRegistryPtr, runStateOperations(native)};
    REQUIRE(backend.open(kSourceFormat, target));

    backend.start();
    REQUIRE(target.tryWaitForRenderAfter(0U));
    REQUIRE(native.startCalls.load() == 1U);
    REQUIRE(native.sdkStartCalls.load() == 1U);
    REQUIRE(native.lastSdkStartStatus.load() == ::noErr);
    REQUIRE(target.errors().empty());

    backend.pause();
    auto const callsAtPause = target.renderCalls();
    CHECK(callsAtPause > 0U);
    CHECK(native.stopCalls.load() == 1U);
    CHECK(native.sdkStopCalls.load() == 0U);
    requireRunError(target, 1U, "pause Core Audio output failed (OSStatus -123456)");

    backend.resume();
    CHECK(native.startCalls.load() == 1U);
    CHECK(target.errors().size() == 1U);
    requireControlWorkerProgress(backend, *graphRegistryPtr, device.id.raw());
    CHECK(target.renderCalls() == callsAtPause);
    CHECK(native.stopCalls.load() == 1U);

    backend.start();
    requireRunError(
      target, 2U, "start Core Audio output withheld: previous native stop remains unconfirmed (OSStatus -123456)");
    CHECK(native.startCalls.load() == 1U);

    backend.flush();
    requireRunError(target, 3U, "stop Core Audio output failed (OSStatus -123456)");
    CHECK(native.stopCalls.load() == 2U);
    CHECK(native.sdkStopCalls.load() == 0U);
    CHECK(native.resetCalls.load() == 0U);
    requireControlWorkerProgress(backend, *graphRegistryPtr, device.id.raw());
    CHECK(target.renderCalls() == callsAtPause);
    CHECK(native.startCalls.load() == 1U);
    CHECK(native.stopCalls.load() == 2U);

    // noErr switches the binding back to the real SDK; no fake success can
    // clear the retained stop failure or establish reset eligibility here.
    native.stopStatus.store(::noErr);
    backend.stop();
    REQUIRE(native.stopCalls.load() == 3U);
    REQUIRE(native.sdkStopCalls.load() == 1U);
    REQUIRE(native.lastSdkStopStatus.load() == ::noErr);
    backend.flush();
    CHECK(native.stopCalls.load() == 3U);
    CHECK(native.resetCalls.load() == 1U);
    CHECK(native.lastResetStatus.load() == ::noErr);

    backend.start();
    REQUIRE(target.tryWaitForRenderAfter(callsAtPause));
    CHECK(native.startCalls.load() == 2U);
    CHECK(native.sdkStartCalls.load() == 2U);
    CHECK(native.lastSdkStartStatus.load() == ::noErr);
    backend.close();
    CHECK(native.stopCalls.load() == 3U);
    CHECK(native.sdkStopCalls.load() == 1U);
    CHECK(native.resetCalls.load() == 1U);
    CHECK(native.badArguments.load() == 0U);
    CHECK(target.errors().size() == 3U);
    CHECK(target.drainCompletions() == 0U);
  }

  TEST_CASE("CoreAudioBackend - a successful pause retry permits a new start without reviving the errored run",
            "[audio][integration][coreaudio][backend][coreaudio-run-state][concurrency]")
  {
    auto const device = requireRunStateTestOutput();
    CAPTURE(device.id.raw());
    auto native = NativeRunState{};
    native.stopStatus.store(kStopFailure);
    auto target = RunStateRenderTarget{};
    auto graphRegistryPtr = std::make_shared<detail::BackendGraphRegistry>();
    auto backend = CoreAudioBackend{device, kProfileShared, graphRegistryPtr, runStateOperations(native)};
    REQUIRE(backend.open(kSourceFormat, target));
    backend.start();
    REQUIRE(target.tryWaitForRenderAfter(0U));
    REQUIRE(native.lastSdkStartStatus.load() == ::noErr);

    backend.pause();
    auto const callsAtPause = target.renderCalls();
    REQUIRE(native.stopCalls.load() == 1U);
    REQUIRE(native.sdkStopCalls.load() == 0U);
    requireRunError(target, 1U, "pause Core Audio output failed (OSStatus -123456)");

    // Confirming the native stop repairs eligibility, not the terminally
    // errored logical run. Same-run resume must not manufacture recovery.
    native.stopStatus.store(::noErr);
    backend.pause();
    REQUIRE(native.stopCalls.load() == 2U);
    REQUIRE(native.sdkStopCalls.load() == 1U);
    REQUIRE(native.lastSdkStopStatus.load() == ::noErr);
    backend.resume();
    requireControlWorkerProgress(backend, *graphRegistryPtr, device.id.raw());
    CHECK(native.startCalls.load() == 1U);
    CHECK(native.sdkStartCalls.load() == 1U);
    CHECK(native.stopCalls.load() == 2U);
    CHECK(native.resetCalls.load() == 0U);
    CHECK(target.renderCalls() == callsAtPause);
    requireRunError(target, 1U, "pause Core Audio output failed (OSStatus -123456)");

    backend.start();
    REQUIRE(target.tryWaitForRenderAfter(callsAtPause));
    CHECK(native.startCalls.load() == 2U);
    CHECK(native.sdkStartCalls.load() == 2U);
    CHECK(native.lastSdkStartStatus.load() == ::noErr);
    backend.close();
    CHECK(native.badArguments.load() == 0U);
    CHECK(target.errors().size() == 1U);
    CHECK(target.drainCompletions() == 0U);
  }

  TEST_CASE("CoreAudioBackend - flush after a confirmed explicit stop resets without a second native stop",
            "[audio][integration][coreaudio][backend][coreaudio-run-state][concurrency]")
  {
    auto const device = requireRunStateTestOutput();
    CAPTURE(device.id.raw());
    auto native = NativeRunState{};
    auto target = RunStateRenderTarget{};
    auto graphRegistryPtr = std::make_shared<detail::BackendGraphRegistry>();
    auto backend = CoreAudioBackend{device, kProfileShared, graphRegistryPtr, runStateOperations(native)};
    REQUIRE(backend.open(kSourceFormat, target));
    backend.start();
    REQUIRE(target.tryWaitForRenderAfter(0U));
    REQUIRE(native.sdkStartCalls.load() == 1U);
    REQUIRE(native.lastSdkStartStatus.load() == ::noErr);

    backend.stop();
    auto const callsAtStop = target.renderCalls();
    REQUIRE(native.stopCalls.load() == 1U);
    REQUIRE(native.sdkStopCalls.load() == 1U);
    REQUIRE(native.lastSdkStopStatus.load() == ::noErr);
    backend.flush();
    CHECK(native.stopCalls.load() == 1U);
    CHECK(native.resetCalls.load() == 1U);
    CHECK(native.lastResetStatus.load() == ::noErr);
    requireControlWorkerProgress(backend, *graphRegistryPtr, device.id.raw());
    CHECK(target.renderCalls() == callsAtStop);
    backend.close();
    // close owns direct SDK cleanup, outside the injected run-operation counts.
    CHECK(native.stopCalls.load() == 1U);
    CHECK(native.sdkStopCalls.load() == 1U);
    CHECK(native.resetCalls.load() == 1U);
    CHECK(native.badArguments.load() == 0U);
    CHECK(target.errors().empty());
    CHECK(target.drainCompletions() == 0U);
  }

  TEST_CASE("CoreAudioBackend - a failed presentation-tail stop reports its drain context without completing drain",
            "[audio][integration][coreaudio][backend][coreaudio-run-state][concurrency]")
  {
    auto const device = requireRunStateTestOutput();
    CAPTURE(device.id.raw());
    auto native = NativeRunState{};
    native.stopStatus.store(kStopFailure);
    auto target = RunStateRenderTarget{true};
    auto graphRegistryPtr = std::make_shared<detail::BackendGraphRegistry>();
    auto backend = CoreAudioBackend{device, kProfileShared, graphRegistryPtr, runStateOperations(native)};
    REQUIRE(backend.open(kSourceFormat, target));
    backend.start();
    REQUIRE(target.tryWaitForError());
    REQUIRE(native.sdkStartCalls.load() == 1U);
    REQUIRE(native.lastSdkStartStatus.load() == ::noErr);
    CHECK(target.renderCalls() == 1U);
    requireRunError(target, 1U, "stop Core Audio output after drain failed (OSStatus -123456)");

    // The error proves the worker attempted stop. A device publication can
    // precede that error, so require two distinct subsequent property graphs
    // before checking the bounded no-automatic-retry observation.
    requireControlWorkerProgress(backend, *graphRegistryPtr, device.id.raw());
    CHECK(native.stopCalls.load() == 1U);
    CHECK(native.sdkStopCalls.load() == 0U);
    CHECK(target.drainCompletions() == 0U);
    CHECK(target.errors().size() == 1U);

    backend.resume();
    CHECK(native.startCalls.load() == 1U);
    CHECK(native.stopCalls.load() == 1U);
    CHECK(target.errors().size() == 1U);
    backend.start();
    requireRunError(
      target, 2U, "start Core Audio output withheld: previous native stop remains unconfirmed (OSStatus -123456)");
    CHECK(native.startCalls.load() == 1U);
    CHECK(target.renderCalls() == 1U);

    // flush is an explicit retry, unlike a queued control-worker followup.
    backend.flush();
    requireRunError(target, 3U, "stop Core Audio output failed (OSStatus -123456)");
    CHECK(native.stopCalls.load() == 2U);
    CHECK(native.resetCalls.load() == 0U);
    requireControlWorkerProgress(backend, *graphRegistryPtr, device.id.raw());
    backend.close();
    CHECK(native.stopCalls.load() == 2U);
    CHECK(native.sdkStopCalls.load() == 0U);
    CHECK(native.resetCalls.load() == 0U);
    CHECK(native.badArguments.load() == 0U);
    CHECK(target.renderCalls() == 1U);
    CHECK(target.drainCompletions() == 0U);
    CHECK(target.errors().size() == 3U);
  }

  TEST_CASE("CoreAudioBackend - an actual oversized render reports frame-limit and failed fault-stop context once",
            "[audio][integration][coreaudio][backend][coreaudio-run-state][concurrency]")
  {
    auto const device = requireRunStateTestOutput();
    CAPTURE(device.id.raw());
    auto native = NativeRunState{};
    native.stopStatus.store(kFaultStopFailure);
    auto maximumFrames = MaximumFramesObservation{};
    auto operations = runStateOperations(native);
    operations.unit.getMaximumFramesPerSlice = [&maximumFrames](::AudioUnit const unit,
                                                                ::AudioUnitPropertyID const property,
                                                                ::AudioUnitScope const scope,
                                                                ::AudioUnitElement const element,
                                                                void* const data,
                                                                ::UInt32* const size) -> ::OSStatus
    {
      auto const index = maximumFrames.calls.fetch_add(1U);

      if (index >= maximumFrames.reads.size())
      {
        maximumFrames.badArguments.fetch_add(1U);
        return ::kAudioUnitErr_InvalidParameter;
      }

      auto& read = maximumFrames.reads[index];
      read.unit = unit;
      read.property = property;
      read.scope = scope;
      read.element = element;
      read.inputSize = size == nullptr ? 0U : *size;

      if (unit == nullptr || property != ::kAudioUnitProperty_MaximumFramesPerSlice ||
          scope != ::kAudioUnitScope_Global || element != 0U || data == nullptr || size == nullptr ||
          read.inputSize != sizeof(::UInt32))
      {
        maximumFrames.badArguments.fetch_add(1U);
        return ::kAudioUnitErr_InvalidParameter;
      }

      // Read the actual provisioned unit before introducing a controlled
      // observation inconsistency. No native buffer size or global device
      // setting is changed, and this is not a malformed-SDK-response claim.
      read.sdkStatus = ::AudioUnitGetProperty(unit, property, scope, element, data, size);
      read.sdkSize = *size;

      if (read.sdkStatus != ::noErr)
      {
        return read.sdkStatus;
      }

      if (read.sdkSize != sizeof(::UInt32))
      {
        maximumFrames.badArguments.fetch_add(1U);
        return ::kAudioHardwareBadPropertySizeError;
      }

      std::memcpy(&read.sdkMaximumFrames, data, sizeof(read.sdkMaximumFrames));

      if (read.sdkMaximumFrames <= kReportedMaximumFrames)
      {
        maximumFrames.badArguments.fetch_add(1U);
        return ::kAudioUnitErr_InvalidParameter;
      }

      auto const reportedMaximumFrames = kReportedMaximumFrames;
      std::memcpy(data, &reportedMaximumFrames, sizeof(reportedMaximumFrames));
      return ::noErr;
    };
    auto target = RunStateRenderTarget{};
    auto graphRegistryPtr = std::make_shared<detail::BackendGraphRegistry>();
    auto backend = CoreAudioBackend{device, kProfileShared, graphRegistryPtr, std::move(operations)};
    REQUIRE(backend.open(kSourceFormat, target));
    REQUIRE(maximumFrames.calls.load() == 1U);
    REQUIRE(maximumFrames.badArguments.load() == 0U);
    auto const& read = maximumFrames.reads.front();
    CHECK(read.unit != nullptr);
    CHECK(read.property == ::kAudioUnitProperty_MaximumFramesPerSlice);
    CHECK(read.scope == ::kAudioUnitScope_Global);
    CHECK(read.element == 0U);
    CHECK(read.inputSize == sizeof(::UInt32));
    CHECK(read.sdkSize == sizeof(::UInt32));
    CHECK(read.sdkStatus == ::noErr);
    CHECK(read.sdkMaximumFrames > kReportedMaximumFrames);

    backend.start();
    // Only the positive frame-limit error proves the native callback had more
    // than one frame. A route that never exercises it fails this progress guard.
    REQUIRE(target.tryWaitForError());
    REQUIRE(native.sdkStartCalls.load() == 1U);
    REQUIRE(native.lastSdkStartStatus.load() == ::noErr);
    CHECK(read.unit == native.lastStartedUnit.load());
    requireRunError(target,
                    1U,
                    "Core Audio exceeded the configured render frame limit; "
                    "stop Core Audio output after fault failed (OSStatus -123457)");
    CHECK(target.renderCalls() == 0U);
    backend.resume();
    CHECK(native.startCalls.load() == 1U);
    CHECK(target.errors().size() == 1U);
    requireControlWorkerProgress(backend, *graphRegistryPtr, device.id.raw());
    CHECK(native.stopCalls.load() == 1U);
    CHECK(native.sdkStopCalls.load() == 0U);
    CHECK(target.drainCompletions() == 0U);

    // This explicit same-generation stop attempt must not duplicate the error
    // already reported for the worker fault.
    backend.pause();
    CHECK(native.stopCalls.load() == 2U);
    CHECK(target.errors().size() == 1U);
    backend.flush();
    requireRunError(target, 2U, "stop Core Audio output failed (OSStatus -123457)");
    CHECK(native.stopCalls.load() == 3U);
    CHECK(native.resetCalls.load() == 0U);
    requireControlWorkerProgress(backend, *graphRegistryPtr, device.id.raw());
    backend.close();
    CHECK(native.stopCalls.load() == 3U);
    CHECK(native.sdkStopCalls.load() == 0U);
    CHECK(native.resetCalls.load() == 0U);
    CHECK(native.badArguments.load() == 0U);
    CHECK(maximumFrames.calls.load() == 1U);
    CHECK(maximumFrames.badArguments.load() == 0U);
    CHECK(target.renderCalls() == 0U);
    CHECK(target.drainCompletions() == 0U);
    CHECK(target.errors().size() == 2U);
  }

  TEST_CASE("CoreAudioBackend - failed resume start becomes inactive while a confirmed stopped unit remains resettable",
            "[audio][integration][coreaudio][backend][coreaudio-run-state][concurrency]")
  {
    auto const device = requireRunStateTestOutput();
    CAPTURE(device.id.raw());
    auto native = NativeRunState{};
    auto target = RunStateRenderTarget{};
    auto graphRegistryPtr = std::make_shared<detail::BackendGraphRegistry>();
    auto backend = CoreAudioBackend{device, kProfileShared, graphRegistryPtr, runStateOperations(native)};
    REQUIRE(backend.open(kSourceFormat, target));
    backend.start();
    REQUIRE(target.tryWaitForRenderAfter(0U));
    REQUIRE(native.startCalls.load() == 1U);
    REQUIRE(native.sdkStartCalls.load() == 1U);
    REQUIRE(native.lastSdkStartStatus.load() == ::noErr);
    backend.pause();
    auto const callsAtPause = target.renderCalls();
    REQUIRE(native.stopCalls.load() == 1U);
    REQUIRE(native.sdkStopCalls.load() == 1U);
    REQUIRE(native.lastSdkStopStatus.load() == ::noErr);
    REQUIRE(target.errors().empty());

    native.startStatus.store(kResumeStartFailure);
    backend.resume();
    requireRunError(target, 1U, "resume Core Audio output failed (OSStatus -123458)");
    CHECK(native.startCalls.load() == 2U);
    CHECK(native.sdkStartCalls.load() == 1U);
    backend.resume();
    CHECK(native.startCalls.load() == 2U);
    CHECK(native.sdkStartCalls.load() == 1U);
    CHECK(target.errors().size() == 1U);
    requireControlWorkerProgress(backend, *graphRegistryPtr, device.id.raw());
    CHECK(target.renderCalls() == callsAtPause);

    backend.flush();
    CHECK(native.stopCalls.load() == 1U);
    CHECK(native.sdkStopCalls.load() == 1U);
    CHECK(native.resetCalls.load() == 1U);
    CHECK(native.lastResetStatus.load() == ::noErr);
    backend.close();
    CHECK(native.startCalls.load() == 2U);
    CHECK(native.stopCalls.load() == 1U);
    CHECK(native.resetCalls.load() == 1U);
    CHECK(native.badArguments.load() == 0U);
    CHECK(target.renderCalls() == callsAtPause);
    CHECK(target.drainCompletions() == 0U);
    CHECK(target.errors().size() == 1U);
  }
} // namespace ao::audio::backend::test
