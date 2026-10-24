// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/fatal/AudioFatalProbeProtocol.h"
#include "test/fatal/ProbeProcess.h"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace ao::audio::backend::test
{
  TEST_CASE("CoreAudioBackend - reentry from its own RenderTarget callback stacks aborts in the fatal probes",
            "[audio][integration][coreaudio][backend][concurrency]")
  {
    // Each native scenario opens a real Core Audio output device, the exact
    // configured UID when AOBUS_TEST_COREAUDIO_DEVICE_UID names one or the
    // first existing output when it is unset, and reenters a control from a
    // real backend target callback in a child process. The ten open/close
    // scenarios remain, plus all five run controls from PCM, underrun,
    // position, and ordered drain-complete origins: 30 children in this one
    // Catch registration. Drain completion is part of the render domain even
    // though it runs on the control worker, not the native render thread.
    // These origins cross threads, so the case carries [concurrency]. The
    // existing probe oracle bounds each child: the diagnostic fires inside
    // the triggered callback, and a missing diagnostic can only surface as a
    // nonfatal child exit or the intrinsic probe timeout, never as a false
    // success here.
    // Nested-graph scenarios deliberately violate the gate-deferral
    // precondition to reach the lifecycle diagnostic; they do not promise
    // safe synchronous graph composition from a target callback.
    constexpr auto kProbeTimeout = std::chrono::seconds{15};
    auto const executablePath = ao::test::siblingProbeExecutablePath("ao_audio_fatal_probe");
    REQUIRE_FALSE(executablePath.empty());

    for (auto const& expectation : audio::test::coreAudioFatalProbeExpectations())
    {
      INFO("probe: " << expectation.scenario);
      auto const result = ao::test::runProbeProcess(executablePath, expectation.scenario, kProbeTimeout);

      REQUIRE(result.started);
      CHECK(result.launchError.empty());
      CHECK_FALSE(result.timedOut);
      CHECK(result.hasFatalTermination());
      CHECK(result.hasPlatformAbort());
      CHECK(result.standardError.contains("AOBUS_FATAL"));
      CHECK(result.standardError.contains(expectation.category));
      CHECK(result.standardError.contains(expectation.context));
      CHECK(result.standardError.contains("source="));
      CHECK(result.standardError.contains("CoreAudioBackend.cpp"));
    }
  }

  TEST_CASE("CoreAudioRenderAdmission - open preconditions abort the fatal probe child",
            "[audio][integration][coreaudio]")
  {
    // These two children call production CoreAudioRenderAdmission::open() in
    // the fatal probe process. They do not open a Core Audio device, resolve
    // a device UID, or look up a native component. Each child uses the same
    // intrinsic 15s probe oracle as the RenderTarget cases.
    constexpr auto kProbeTimeout = std::chrono::seconds{15};
    auto const executablePath = ao::test::siblingProbeExecutablePath("ao_audio_fatal_probe");
    REQUIRE_FALSE(executablePath.empty());

    for (auto const& expectation : audio::test::coreAudioRenderAdmissionFatalProbeExpectations())
    {
      INFO("probe: " << expectation.scenario);
      auto const result = ao::test::runProbeProcess(executablePath, expectation.scenario, kProbeTimeout);

      REQUIRE(result.started);
      CHECK(result.launchError.empty());
      CHECK_FALSE(result.timedOut);
      CHECK(result.hasFatalTermination());
      CHECK(result.hasPlatformAbort());
      CHECK(result.standardError.contains("AOBUS_FATAL"));
      CHECK(result.standardError.contains(expectation.category));
      CHECK(result.standardError.contains(expectation.context));
      CHECK(result.standardError.contains("source="));
      CHECK(result.standardError.contains("CoreAudioRenderAdmission.cpp:"));
    }
  }

  TEST_CASE("CoreAudioUnitRun - start while a native stop is unconfirmed aborts the fatal probe child",
            "[audio][integration][coreaudio]")
  {
    // This device-free child constructs the production CoreAudioUnitRun with
    // both constructor-injected operations overridden: start reports noErr,
    // stop reports a plain nonzero status, and no AudioToolbox call, device,
    // or component lookup ever happens. The child records started, fails the
    // stop, and calls start again, which must abort at the canStart() expects
    // precondition before the bound start operation runs. The precondition is
    // pure single-threaded state, so the case does not carry [concurrency]. If
    // the production guard is removed, the child returns a nonfatal failure
    // exit without a destructor abort and this case fails through the same
    // 15s probe oracle as the other native registrations.
    constexpr auto kProbeTimeout = std::chrono::seconds{15};
    auto const executablePath = ao::test::siblingProbeExecutablePath("ao_audio_fatal_probe");
    REQUIRE_FALSE(executablePath.empty());

    for (auto const& expectation : audio::test::coreAudioUnitRunFatalProbeExpectations())
    {
      INFO("probe: " << expectation.scenario);
      auto const result = ao::test::runProbeProcess(executablePath, expectation.scenario, kProbeTimeout);

      REQUIRE(result.started);
      CHECK(result.launchError.empty());
      CHECK_FALSE(result.timedOut);
      CHECK(result.hasFatalTermination());
      CHECK(result.hasPlatformAbort());
      CHECK(result.standardError.contains("AOBUS_FATAL"));
      CHECK(result.standardError.contains(expectation.category));
      CHECK(result.standardError.contains(expectation.context));
      CHECK(result.standardError.contains("source="));
      CHECK(result.standardError.contains("CoreAudioUnitRun.cpp"));
    }
  }

  TEST_CASE("CoreAudioUnitRun - empty constructor bindings abort the fatal probe child",
            "[audio][integration][coreaudio]")
  {
    // Two device-free children construct the production CoreAudioUnitRun
    // directly, each with exactly one of the two constructor-injected
    // operations empty; the other binding keeps its SDK default and is never
    // invoked. No AudioToolbox call, device open, or component lookup ever
    // happens, and the precondition is pure single-threaded construction
    // state, so the case does not carry [concurrency]. If the production guard
    // is removed, each child returns a nonfatal failure exit without invoking
    // the empty function, and this case fails through the same 15s probe
    // oracle as the other native registrations.
    constexpr auto kProbeTimeout = std::chrono::seconds{15};
    auto const executablePath = ao::test::siblingProbeExecutablePath("ao_audio_fatal_probe");
    REQUIRE_FALSE(executablePath.empty());

    for (auto const& expectation : audio::test::coreAudioUnitRunConstructorFatalProbeExpectations())
    {
      INFO("probe: " << expectation.scenario);
      auto const result = ao::test::runProbeProcess(executablePath, expectation.scenario, kProbeTimeout);

      REQUIRE(result.started);
      CHECK(result.launchError.empty());
      CHECK_FALSE(result.timedOut);
      CHECK(result.hasFatalTermination());
      CHECK(result.hasPlatformAbort());
      CHECK(result.standardError.contains("AOBUS_FATAL"));
      CHECK(result.standardError.contains(expectation.category));
      CHECK(result.standardError.contains(expectation.context));
      CHECK(result.standardError.contains("source="));
      CHECK(result.standardError.contains("CoreAudioUnitRun.cpp:"));
    }
  }

  TEST_CASE("CoreAudioBackend - empty native unit bindings abort the fatal probe child",
            "[audio][integration][coreaudio]")
  {
    // Two device-free children construct the backend through its existing
    // four-argument constructor with a value-only device identity and one
    // empty unit operation; the run bindings keep their SDK defaults and are
    // never invoked. The runtime-state constructor must abort at its
    // unit-binding expects contract before Impl creates its control thread,
    // so no device query, component lookup, open, or start ever happens. If
    // the production guard is removed, the child returns a nonfatal failure
    // exit while the ordinary no-unit destructor and control-thread join
    // settle normally, and this case fails through the same 15s probe oracle
    // as the other native registrations.
    constexpr auto kProbeTimeout = std::chrono::seconds{15};
    auto const executablePath = ao::test::siblingProbeExecutablePath("ao_audio_fatal_probe");
    REQUIRE_FALSE(executablePath.empty());

    for (auto const& expectation : audio::test::coreAudioBackendConstructorFatalProbeExpectations())
    {
      INFO("probe: " << expectation.scenario);
      auto const result = ao::test::runProbeProcess(executablePath, expectation.scenario, kProbeTimeout);

      REQUIRE(result.started);
      CHECK(result.launchError.empty());
      CHECK_FALSE(result.timedOut);
      CHECK(result.hasFatalTermination());
      CHECK(result.hasPlatformAbort());
      CHECK(result.standardError.contains("AOBUS_FATAL"));
      CHECK(result.standardError.contains(expectation.category));
      CHECK(result.standardError.contains(expectation.context));
      CHECK(result.standardError.contains("source="));
      CHECK(result.standardError.contains("CoreAudioBackend.cpp:"));
    }
  }
} // namespace ao::audio::backend::test
