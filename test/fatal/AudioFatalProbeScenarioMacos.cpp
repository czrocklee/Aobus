// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

// Native Core Audio fatal-probe scenarios. The thirty RenderTarget reentry
// scenarios open the exact Core Audio output device named by
// AOBUS_TEST_COREAUDIO_DEVICE_UID when one is configured, or the first
// existing real Core Audio output when the boundary is unset, through a real
// backend and reenter open(), close(), or a run control from one of the
// backend's own RenderTarget callback stacks. Two admission precondition
// children call production CoreAudioRenderAdmission::open() only and do not
// open a device or look up a native component. One device-free unit-run child
// drives production CoreAudioUnitRun through its constructor-injected
// operations with both SDK calls overridden, so it also never opens a
// device, looks up a component, or calls the AudioToolbox SDK. Four further
// device-free constructor children reject empty native bindings at
// construction: two construct CoreAudioUnitRun directly with one of the two
// constructor-injected run bindings empty, and two construct CoreAudioBackend
// through its four-argument constructor with a value-only device identity and
// one empty unit operation. In every constructor child the remaining bindings
// keep their SDK defaults and are never invoked, and no device query,
// component lookup, open, or start ever happens; if construction unexpectedly
// survives, the child reports a probe failure and exits nonfatally without
// calling the empty function. The production
// diagnostic must abort the child process. A missing diagnostic may only surface as an
// orderly nonfatal failure exit or through the intrinsic ProbeProcess
// timeout; these scenarios never sleep, never fabricate callback-stack
// membership, and never invoke private or manually constructed backend hooks.

#include "lib/audio/backend/CoreAudioBackend.h"
#include "lib/audio/backend/detail/BackendGraphRegistry.h"
#include "lib/audio/backend/detail/CoreAudioRenderAdmission.h"
#include "lib/audio/backend/detail/CoreAudioUnitRun.h"
#include "test/fatal/AudioFatalProbeScenario.h"
#include "test/unit/audio/backend/CoreAudioTestSupport.h"
#include <ao/Error.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/Property.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/flow/Graph.h>
#include <ao/utility/ScopedRegistration.h>

#include <MacTypes.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <print>
#include <semaphore>
#include <span>
#include <string_view>
#include <utility>

namespace ao::audio::test
{
  namespace
  {
    constexpr auto kProbeSourceFormat =
      SignalFormat{.sampleRate = 44100, .channels = 2, .precisionBits = 16, .sampleKind = SampleKind::Integer};

    // Dedicated probe route anchor, deliberately unrelated to the native
    // device anchor that normal Core Audio route and device publications
    // use. Native graph publications on the real route can never reach this
    // subscriber, so only the synchronous target-origin publication from
    // inside a target callback can claim the armed probe reentry; a worker
    // publishing the native route can no longer steal the claim on a thread
    // without the target callback scope.
    constexpr auto kProbeGraphRouteAnchor = std::string_view{"ao-coreaudio-reentry-probe-route"};

    enum class ProbeTrigger : std::uint8_t
    {
      RouteReady,
      FormatChanged,
      RenderPcm,
      Underrun,
      PositionAdvanced,
      DrainComplete,
    };

    enum class ProbeReentry : std::uint8_t
    {
      Open,
      Close,
      Start,
      Pause,
      Resume,
      Flush,
      Stop,
      GraphOpen,
      GraphClose,
    };

    struct ProbeScenarioSpec final
    {
      std::string_view name;
      ProbeTrigger trigger;
      ProbeReentry reentry;
      bool requiresStart;
      bool requiresGraphRegistry;
    };

    constexpr auto kProbeScenarioSpecs = std::array{
      ProbeScenarioSpec{"coreaudio-open-from-route-ready", ProbeTrigger::RouteReady, ProbeReentry::Open, false, false},
      ProbeScenarioSpec{"coreaudio-close-from-route-ready",
                        ProbeTrigger::RouteReady,
                        ProbeReentry::Close,
                        false,
                        false},
      ProbeScenarioSpec{"coreaudio-open-from-format-changed",
                        ProbeTrigger::FormatChanged,
                        ProbeReentry::Open,
                        false,
                        false},
      ProbeScenarioSpec{"coreaudio-close-from-format-changed",
                        ProbeTrigger::FormatChanged,
                        ProbeReentry::Close,
                        false,
                        false},
      ProbeScenarioSpec{"coreaudio-open-from-graph-callback",
                        ProbeTrigger::RouteReady,
                        ProbeReentry::GraphOpen,
                        false,
                        true},
      ProbeScenarioSpec{"coreaudio-close-from-graph-callback",
                        ProbeTrigger::RouteReady,
                        ProbeReentry::GraphClose,
                        false,
                        true},
      ProbeScenarioSpec{"coreaudio-open-from-render-pcm", ProbeTrigger::RenderPcm, ProbeReentry::Open, true, false},
      ProbeScenarioSpec{"coreaudio-close-from-render-pcm", ProbeTrigger::RenderPcm, ProbeReentry::Close, true, false},
      ProbeScenarioSpec{"coreaudio-open-from-drain-complete",
                        ProbeTrigger::DrainComplete,
                        ProbeReentry::Open,
                        true,
                        false},
      ProbeScenarioSpec{"coreaudio-close-from-drain-complete",
                        ProbeTrigger::DrainComplete,
                        ProbeReentry::Close,
                        true,
                        false},
      ProbeScenarioSpec{"coreaudio-start-from-render-pcm", ProbeTrigger::RenderPcm, ProbeReentry::Start, true, false},
      ProbeScenarioSpec{"coreaudio-pause-from-render-pcm", ProbeTrigger::RenderPcm, ProbeReentry::Pause, true, false},
      ProbeScenarioSpec{"coreaudio-resume-from-render-pcm", ProbeTrigger::RenderPcm, ProbeReentry::Resume, true, false},
      ProbeScenarioSpec{"coreaudio-flush-from-render-pcm", ProbeTrigger::RenderPcm, ProbeReentry::Flush, true, false},
      ProbeScenarioSpec{"coreaudio-stop-from-render-pcm", ProbeTrigger::RenderPcm, ProbeReentry::Stop, true, false},
      ProbeScenarioSpec{"coreaudio-start-from-underrun", ProbeTrigger::Underrun, ProbeReentry::Start, true, false},
      ProbeScenarioSpec{"coreaudio-pause-from-underrun", ProbeTrigger::Underrun, ProbeReentry::Pause, true, false},
      ProbeScenarioSpec{"coreaudio-resume-from-underrun", ProbeTrigger::Underrun, ProbeReentry::Resume, true, false},
      ProbeScenarioSpec{"coreaudio-flush-from-underrun", ProbeTrigger::Underrun, ProbeReentry::Flush, true, false},
      ProbeScenarioSpec{"coreaudio-stop-from-underrun", ProbeTrigger::Underrun, ProbeReentry::Stop, true, false},
      ProbeScenarioSpec{"coreaudio-start-from-position-advanced",
                        ProbeTrigger::PositionAdvanced,
                        ProbeReentry::Start,
                        true,
                        false},
      ProbeScenarioSpec{"coreaudio-pause-from-position-advanced",
                        ProbeTrigger::PositionAdvanced,
                        ProbeReentry::Pause,
                        true,
                        false},
      ProbeScenarioSpec{"coreaudio-resume-from-position-advanced",
                        ProbeTrigger::PositionAdvanced,
                        ProbeReentry::Resume,
                        true,
                        false},
      ProbeScenarioSpec{"coreaudio-flush-from-position-advanced",
                        ProbeTrigger::PositionAdvanced,
                        ProbeReentry::Flush,
                        true,
                        false},
      ProbeScenarioSpec{"coreaudio-stop-from-position-advanced",
                        ProbeTrigger::PositionAdvanced,
                        ProbeReentry::Stop,
                        true,
                        false},
      ProbeScenarioSpec{"coreaudio-start-from-drain-complete",
                        ProbeTrigger::DrainComplete,
                        ProbeReentry::Start,
                        true,
                        false},
      ProbeScenarioSpec{"coreaudio-pause-from-drain-complete",
                        ProbeTrigger::DrainComplete,
                        ProbeReentry::Pause,
                        true,
                        false},
      ProbeScenarioSpec{"coreaudio-resume-from-drain-complete",
                        ProbeTrigger::DrainComplete,
                        ProbeReentry::Resume,
                        true,
                        false},
      ProbeScenarioSpec{"coreaudio-flush-from-drain-complete",
                        ProbeTrigger::DrainComplete,
                        ProbeReentry::Flush,
                        true,
                        false},
      ProbeScenarioSpec{"coreaudio-stop-from-drain-complete",
                        ProbeTrigger::DrainComplete,
                        ProbeReentry::Stop,
                        true,
                        false},
    };

    ProbeScenarioSpec const* findProbeScenarioSpec(std::string_view const scenario) noexcept
    {
      for (auto const& spec : kProbeScenarioSpecs)
      {
        if (spec.name == scenario)
        {
          return &spec;
        }
      }

      return nullptr;
    }

    /**
     * @brief The single target fixture for the native reentry scenarios.
     *
     * The designated trigger callback performs exactly one control attempt
     * against the backend that invoked it. The one-attempt guard is
     * what keeps a missing production diagnostic from recursing through
     * nested opens; without it a reentered open() would reenter its own
     * notifications until the stack overflowed. All cross-thread state is an
     * atomic claim plus a semaphore release, so no scenario depends on timing.
     */
    class ReentryProbeTarget final : public RenderTarget
    {
    public:
      ReentryProbeTarget(ProbeTrigger const trigger, ProbeReentry const reentry) noexcept
        : _trigger{trigger}, _reentry{reentry}
      {
      }

      // The target is declared before the backend, so backend destruction
      // settles its borrowed target while the target is still alive. The raw
      // pointer is therefore attached late, once the backend exists.
      void attachBackend(backend::CoreAudioBackend& backendRef) noexcept { _backend = &backendRef; }

      void attachGraphRegistry(std::shared_ptr<backend::detail::BackendGraphRegistry> registryPtr)
      {
        _registryPtr = std::move(registryPtr);
      }

      // Claims the single graph-observer reentry armed by the nested registry
      // publication performed from the trigger callback.
      bool tryClaimGraphReentry() noexcept { return _graphArmed.exchange(false, std::memory_order_acq_rel); }

      // Returns only when the reentry attempt returned control, which is the
      // nonfatal failure path; the expected abort never returns here.
      void waitUntilReentryReturned() noexcept { _reentryReturned.acquire(); }

      RenderPcmResult renderPcm(std::span<std::byte> output) noexcept override
      {
        std::ranges::fill(output, std::byte{0});

        if (_trigger == ProbeTrigger::RenderPcm)
        {
          attemptReentry();
        }

        if (_trigger == ProbeTrigger::Underrun)
        {
          // A short, non-drained render asks the backend to emit a real underrun.
          return RenderPcmResult{};
        }

        if (_trigger == ProbeTrigger::DrainComplete)
        {
          // Normal drained progress: the backend completes the presentation
          // tail and reports drain completion through its control thread.
          return RenderPcmResult{.drained = true};
        }

        return RenderPcmResult{
          .bytesWritten = output.size(),
          .positionFrames = _trigger == ProbeTrigger::PositionAdvanced && !output.empty() ? 1U : 0U};
      }

      void handleUnderrun() noexcept override
      {
        if (_trigger == ProbeTrigger::Underrun)
        {
          attemptReentry();
        }
      }

      void handlePositionAdvanced(std::uint32_t /*frames*/) noexcept override
      {
        if (_trigger == ProbeTrigger::PositionAdvanced)
        {
          attemptReentry();
        }
      }

      void handleDrainComplete() noexcept override
      {
        if (_trigger == ProbeTrigger::DrainComplete)
        {
          attemptReentry();
        }
      }

      void handleRouteReady(std::string_view /*routeAnchor*/) noexcept override
      {
        if (_trigger == ProbeTrigger::RouteReady)
        {
          attemptReentry();
        }
      }

      void handleFormatChanged(PcmFormat const& /*format*/) noexcept override
      {
        if (_trigger == ProbeTrigger::FormatChanged)
        {
          attemptReentry();
        }
      }

      void handlePropertyChanged(PropertySnapshot /*snapshot*/) noexcept override {}
      void handleBackendError(std::string_view /*message*/) noexcept override {}

    private:
      void attemptReentry() noexcept
      {
        if (_reentryAttempted.exchange(true, std::memory_order_acq_rel))
        {
          return;
        }

        performReentry();
        _reentryReturned.release();
      }

      void performReentry() noexcept
      {
        switch (_reentry)
        {
          case ProbeReentry::Open:
          {
            [[maybe_unused]] auto const reopenedRes = _backend->open(kProbeSourceFormat, *this);
            return;
          }
          case ProbeReentry::Close:
          {
            _backend->close();
            return;
          }
          case ProbeReentry::Start:
          {
            _backend->start();
            return;
          }
          case ProbeReentry::Pause:
          {
            _backend->pause();
            return;
          }
          case ProbeReentry::Resume:
          {
            _backend->resume();
            return;
          }
          case ProbeReentry::Flush:
          {
            _backend->flush();
            return;
          }
          case ProbeReentry::Stop:
          {
            _backend->stop();
            return;
          }
          case ProbeReentry::GraphOpen:
          case ProbeReentry::GraphClose:
          {
            // Deliver a real registry publication from inside the target
            // callback, on the dedicated probe route anchor. The armed
            // observer, nested on this target stack, then reenters the backend
            // while the target invocation is still active, which is the
            // graph-callback contract under test. Native publications on the
            // device route cannot reach this subscriber, so the claim can
            // only be taken synchronously on this target-origin stack.
            _graphArmed.store(true, std::memory_order_release);
            auto graph = flow::Graph{};
            graph.nodes.push_back(flow::Node{.id = "ao-coreaudio-reentry-probe"});
            _registryPtr->publish(kProbeGraphRouteAnchor, std::move(graph));
            return;
          }
        }
      }

      ProbeTrigger _trigger;
      ProbeReentry _reentry;
      backend::CoreAudioBackend* _backend = nullptr;
      std::shared_ptr<backend::detail::BackendGraphRegistry> _registryPtr{};
      std::atomic_bool _reentryAttempted{false};
      std::atomic_bool _graphArmed{false};
      std::binary_semaphore _reentryReturned{0};
    };

    void reportProbeFailure(std::string_view const scenario, std::string_view const reason)
    {
      std::println(stderr, "coreaudio fatal probe '{}': {}", scenario, reason);
    }

    std::int32_t runRenderAdmissionFatalProbe(std::string_view const scenario)
    {
      // Production open() preconditions only. No device, no component lookup,
      // and no withheld callback scope: an open fence is the active interval,
      // and the second open() aborts before any token write.
      if (auto admission = backend::detail::CoreAudioRenderAdmission{};
          scenario == "coreaudio-admission-zero-generation")
      {
        admission.open(std::uint64_t{0});
      }
      else if (scenario == "coreaudio-admission-active-interval")
      {
        admission.open(std::uint64_t{7});
        admission.open(std::uint64_t{8});
      }

      reportProbeFailure(scenario, "admission open returned without the expected abort");
      return 3;
    }

    std::int32_t runCoreAudioUnitRunFatalProbe(std::string_view const scenario)
    {
      // Device-free pure-state precondition: both constructor-injected
      // operations are overridden, so no AudioToolbox call ever happens. The
      // stop status is a plain nonzero value, not a real SDK result, and the
      // unit pointer stays opaque because the bound operations never inspect
      // it.
      constexpr ::OSStatus kProbeStopStatus = 1;
      auto* const probeUnit = static_cast<::AudioUnit>(nullptr);

      std::int32_t startOperationCalls = 0;
      auto run = backend::detail::CoreAudioUnitRun{backend::detail::CoreAudioUnitRunOperations{
        .start =
          [&](::AudioUnit)
        {
          ++startOperationCalls;
          return ::noErr;
        },
        .stop = [](::AudioUnit) { return kProbeStopStatus; },
      }};

      run.start(probeUnit);
      run.stop(probeUnit);

      // The second start must abort at the canStart() precondition before
      // the bound start operation runs.
      run.start(probeUnit);

      reportProbeFailure(scenario,
                         startOperationCalls > 1 ? "start reached the bound operation without the expected abort"
                                                 : "start returned without the expected abort");
      return 3;
    }

    std::int32_t runCoreAudioUnitRunConstructorFatalProbe(std::string_view const scenario)
    {
      // Construction contract only: exactly one of the two constructor-injected
      // operations is empty; the other keeps its SDK default, and construction
      // never invokes either. No AudioToolbox call, device, or component
      // lookup happens in this child.
      auto operations = backend::detail::CoreAudioUnitRunOperations{};

      if (scenario == "coreaudio-unit-run-unbound-start")
      {
        operations.start = nullptr;
      }
      else
      {
        operations.stop = nullptr;
      }

      // The constructor must abort at the empty-binding expects contract
      // before the record exists.
      [[maybe_unused]] auto const run = backend::detail::CoreAudioUnitRun{std::move(operations)};

      reportProbeFailure(scenario, "construction returned without the expected abort");
      return 3;
    }

    std::int32_t runCoreAudioBackendConstructorFatalProbe(std::string_view const scenario)
    {
      // The four-argument constructor reaches the runtime-state constructor,
      // which must abort at the empty unit-binding expects contract before Impl
      // creates its control thread. The device is a value-only identity
      // string, the run bindings keep their SDK defaults, and no open, start,
      // device query, or component lookup ever happens. If construction
      // unexpectedly survives, the ordinary no-unit destructor and control
      // thread join settle without invoking the empty function.
      auto operations = backend::CoreAudioBackend::NativeOperations{};

      if (scenario == "coreaudio-backend-unbound-reset")
      {
        operations.unit.reset = nullptr;
      }
      else
      {
        operations.unit.getMaximumFramesPerSlice = nullptr;
      }

      [[maybe_unused]] auto const probeBackend = backend::CoreAudioBackend{
        Device{.id = DeviceId{"ao-coreaudio-constructor-probe"}, .backendId = kBackendCoreAudio},
        kProfileShared,
        std::shared_ptr<backend::detail::BackendGraphRegistry>{},
        std::move(operations)};

      reportProbeFailure(scenario, "construction returned without the expected abort");
      return 3;
    }
  } // namespace

  std::int32_t runCoreAudioFatalProbeScenario(std::string_view const scenario)
  {
    // Admission preconditions abort before device or component lookup. These
    // children do not open a Core Audio device.
    if (scenario == "coreaudio-admission-zero-generation" || scenario == "coreaudio-admission-active-interval")
    {
      return runRenderAdmissionFatalProbe(scenario);
    }

    // The unit-run precondition aborts before device or component lookup too;
    // this child does not open a Core Audio device.
    if (scenario == "coreaudio-unit-run-unconfirmed-start")
    {
      return runCoreAudioUnitRunFatalProbe(scenario);
    }

    // Constructor binding preconditions abort before any device or component
    // lookup; these children do not open a Core Audio device either.
    if (scenario == "coreaudio-unit-run-unbound-start" || scenario == "coreaudio-unit-run-unbound-stop")
    {
      return runCoreAudioUnitRunConstructorFatalProbe(scenario);
    }

    if (scenario == "coreaudio-backend-unbound-reset" || scenario == "coreaudio-backend-unbound-frames-readback")
    {
      return runCoreAudioBackendConstructorFatalProbe(scenario);
    }

    auto const* const spec = findProbeScenarioSpec(scenario);

    if (spec == nullptr)
    {
      reportProbeFailure(scenario, "unknown native scenario");
      return 2;
    }

    auto const selectedRes = selectCoreAudioTestOutput();

    if (!selectedRes)
    {
      // A configured but unresolvable UID is a setup error, not a skip and
      // never a substitute device.
      reportProbeFailure(scenario, selectedRes.error().message);
      return 3;
    }

    if (!*selectedRes)
    {
      // An unset UID on a host with no live Core Audio output is the genuine
      // absence boundary. This child has no Catch2 skip to delegate to, so the
      // absence is reported as an orderly nonfatal failure exit; there is
      // never a fallback and never an invented device.
      reportProbeFailure(scenario, "macOS host has no live Core Audio output device");
      return 3;
    }

    auto const& device = **selectedRes;

    auto target = ReentryProbeTarget{spec->trigger, spec->reentry};
    auto graphRegistryPtr = spec->requiresGraphRegistry ? std::make_shared<backend::detail::BackendGraphRegistry>()
                                                        : std::shared_ptr<backend::detail::BackendGraphRegistry>{};
    auto backend = backend::CoreAudioBackend{device, kProfileShared, graphRegistryPtr};

    target.attachBackend(backend);

    auto subscription = utility::ScopedRegistration{};

    if (spec->requiresGraphRegistry)
    {
      target.attachGraphRegistry(graphRegistryPtr);

      subscription = graphRegistryPtr->subscribe(kProbeGraphRouteAnchor,
                                                 [&backend, &target, spec](flow::Graph const&)
                                                 {
                                                   if (!target.tryClaimGraphReentry())
                                                   {
                                                     return;
                                                   }

                                                   if (spec->reentry == ProbeReentry::GraphOpen)
                                                   {
                                                     [[maybe_unused]] auto const reopenedRes =
                                                       backend.open(kProbeSourceFormat, target);
                                                     return;
                                                   }

                                                   backend.close();
                                                 });
    }

    auto const openedRes = backend.open(kProbeSourceFormat, target);

    if (!openedRes)
    {
      reportProbeFailure(scenario, openedRes.error().error.message);
      return 3;
    }

    if (spec->requiresStart)
    {
      backend.start();
    }

    // The expected abort happens inside the triggered callback and never
    // returns here. This wait only completes on the nonfatal failure path
    // where the production diagnostic is missing and control comes back; the
    // child is then torn down and reports an unexpected-control failure. A
    // reentry that deadlocks inside the backend instead surfaces only through
    // the existing intrinsic ProbeProcess timeout.
    target.waitUntilReentryReturned();

    reportProbeFailure(scenario, "reentry returned control without the expected abort");
    backend.stop();
    backend.close();

    return 3;
  }
} // namespace ao::audio::test
