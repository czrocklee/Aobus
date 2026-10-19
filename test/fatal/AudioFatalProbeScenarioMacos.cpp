// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

// Native Core Audio fatal-probe scenarios. Each scenario opens the exact
// Core Audio output device named by AOBUS_TEST_COREAUDIO_DEVICE_UID when one
// is configured, or the first existing real Core Audio output when the
// boundary is unset, through a real backend and reenters open() or close()
// from one of the backend's own RenderTarget callback stacks; the
// production CallbackStackScope diagnostic must abort the child process. A
// missing diagnostic may only surface as an orderly nonfatal failure exit or
// through the intrinsic ProbeProcess timeout; these scenarios never sleep,
// never fabricate callback-stack membership, and never invoke private or
// manually constructed backend hooks.

#include "lib/audio/backend/CoreAudioBackend.h"
#include "lib/audio/backend/detail/BackendGraphRegistry.h"
#include "lib/audio/backend/detail/CoreAudioDeviceDiscovery.h"
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
      DrainComplete,
    };

    enum class ProbeReentry : std::uint8_t
    {
      Open,
      Close,
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
     * The designated trigger callback performs exactly one open or close
     * attempt against the backend that invoked it. The one-attempt guard is
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

        if (_trigger == ProbeTrigger::DrainComplete)
        {
          // Normal drained progress: the backend completes the presentation
          // tail and reports drain completion through its control thread.
          return RenderPcmResult{.drained = true};
        }

        return RenderPcmResult{.bytesWritten = output.size()};
      }

      void handleUnderrun() noexcept override {}
      void handlePositionAdvanced(std::uint32_t /*frames*/) noexcept override {}

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
  } // namespace

  std::int32_t runCoreAudioFatalProbeScenario(std::string_view const scenario)
  {
    auto const* const spec = findProbeScenarioSpec(scenario);

    if (spec == nullptr)
    {
      reportProbeFailure(scenario, "unknown native scenario");
      return 2;
    }

    auto const requestedRes = requestedCoreAudioTestOutput();

    if (!requestedRes)
    {
      // A configured but unresolvable UID is a setup error, not a skip and
      // never a substitute device.
      reportProbeFailure(scenario, requestedRes.error().message);
      return 3;
    }

    // An unset UID is the legitimate unconfigured boundary and keeps the
    // owning native test policy: use the first existing real Core Audio
    // output. A present but invalid, empty, or ambiguous UID stays a hard
    // failure above; there is never a fallback and never an invented device.
    auto device = Device{};

    if (auto const& optDevice = *requestedRes; optDevice)
    {
      device = *optDevice;
    }
    else
    {
      auto const devices = backend::detail::enumerateCoreAudioOutputDevices();

      if (devices.empty())
      {
        reportProbeFailure(scenario, "macOS host has no live Core Audio output device");
        return 3;
      }

      device = devices.front();
    }

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
