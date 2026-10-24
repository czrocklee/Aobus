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
    // success here. Nested-graph
    // scenarios deliberately violate the gate-deferral precondition to reach
    // the lifecycle diagnostic; they do not promise safe synchronous graph
    // composition from a target callback.
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
} // namespace ao::audio::backend::test
