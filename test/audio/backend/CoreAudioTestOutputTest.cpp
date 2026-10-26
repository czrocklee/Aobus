// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/fatal/ProbeProcess.h"
#include "test/audio/backend/CoreAudioTestSupport.h"
#include <ao/Error.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <format>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::audio::backend::test
{
  namespace
  {
    constexpr auto kProbeTimeout = std::chrono::seconds{15};

    // The environment boundary is owned by the ao_coreaudio_output_probe child,
    // which reshapes its own process environment before calling the wrapper.
    ao::test::ProbeProcessResult runCoreAudioOutputProbe(std::string_view const scenario)
    {
      auto const executablePath = ao::test::siblingProbeExecutablePath("ao_coreaudio_output_probe");

      if (executablePath.empty())
      {
        return {};
      }

      return ao::test::runProbeProcess(executablePath, scenario, kProbeTimeout);
    }

    void requireSuccessfulProbe(ao::test::ProbeProcessResult const& result)
    {
      INFO("launch error: " << result.launchError);
      INFO("standard error: " << result.standardError);
      REQUIRE(result.started);
      REQUIRE_FALSE(result.timedOut);
      REQUIRE(result.hasSuccessfulExit());
      CHECK(result.standardError.empty());
    }
  } // namespace

  TEST_CASE("resolveCoreAudioTestOutput returns the owning non-default exact Core Audio device",
            "[audio][unit][coreaudio]")
  {
    auto devices = std::vector<Device>{{.id = DeviceId{"builtin-uid"},
                                        .displayName = "Built-in Output",
                                        .description = "Apple",
                                        .isDefault = true,
                                        .backendId = kBackendCoreAudio},
                                       {.id = DeviceId{"BlackHole2ch_UID"},
                                        .displayName = "BlackHole 2ch",
                                        .description = "PipeWire virtual",
                                        .backendId = kBackendPipeWire},
                                       {.id = DeviceId{"BlackHole2ch_UID"},
                                        .displayName = "BlackHole 2ch",
                                        .description = "Existential Audio",
                                        .isDefault = false,
                                        .backendId = kBackendCoreAudio}};

    auto const selectedRes = audio::test::resolveCoreAudioTestOutput(devices, "BlackHole2ch_UID");

    REQUIRE(selectedRes);
    devices[2] = Device{.id = DeviceId{"replacement-uid"},
                        .displayName = "Replacement",
                        .description = "Changed source",
                        .isDefault = true,
                        .backendId = kBackendPipeWire};
    CHECK(selectedRes->id == "BlackHole2ch_UID");
    CHECK(selectedRes->displayName == "BlackHole 2ch");
    CHECK(selectedRes->description == "Existential Audio");
    CHECK_FALSE(selectedRes->isDefault);
    CHECK(selectedRes->backendId == kBackendCoreAudio);
  }

  TEST_CASE("resolveCoreAudioTestOutput rejects an empty UID without selecting a device", "[audio][unit][coreaudio]")
  {
    auto const devices = std::vector<Device>{{.id = DeviceId{"builtin-uid"},
                                              .displayName = "Built-in Output",
                                              .description = "Apple",
                                              .isDefault = true,
                                              .backendId = kBackendCoreAudio}};

    auto const selectedRes = audio::test::resolveCoreAudioTestOutput(devices, "");

    REQUIRE_FALSE(selectedRes);
    CHECK(selectedRes.error().code == Error::Code::InvalidInput);
  }

  TEST_CASE("resolveCoreAudioTestOutput does not fall back from a non-exact UID", "[audio][unit][coreaudio]")
  {
    auto const devices = std::vector<Device>{{.id = DeviceId{"builtin-uid"},
                                              .displayName = "Built-in Output",
                                              .description = "Apple",
                                              .isDefault = true,
                                              .backendId = kBackendCoreAudio},
                                             {.id = DeviceId{"BlackHole2ch_UID"},
                                              .displayName = "BlackHole 2ch",
                                              .description = "Existential Audio",
                                              .isDefault = false,
                                              .backendId = kBackendCoreAudio}};
    auto requestedUid = std::string_view{};

    SECTION("missing UID")
    {
      requestedUid = "missing-uid";
    }

    SECTION("display name")
    {
      requestedUid = "BlackHole 2ch";
    }

    SECTION("different case")
    {
      requestedUid = "blackhole2ch_uid";
    }

    SECTION("leading whitespace")
    {
      requestedUid = " BlackHole2ch_UID";
    }

    SECTION("trailing whitespace")
    {
      requestedUid = "BlackHole2ch_UID ";
    }

    SECTION("whitespace only")
    {
      requestedUid = " ";
    }

    auto const selectedRes = audio::test::resolveCoreAudioTestOutput(devices, requestedUid);

    REQUIRE_FALSE(selectedRes);
    CHECK(selectedRes.error().code == Error::Code::DeviceNotFound);
  }

  TEST_CASE("resolveCoreAudioTestOutput rejects a duplicate Core Audio UID instead of selecting the first",
            "[audio][unit][coreaudio]")
  {
    auto const devices = std::vector<Device>{{.id = DeviceId{"BlackHole2ch_UID"},
                                              .displayName = "BlackHole 2ch",
                                              .description = "first",
                                              .isDefault = true,
                                              .backendId = kBackendCoreAudio},
                                             {.id = DeviceId{"BlackHole2ch_UID"},
                                              .displayName = "BlackHole 2ch copy",
                                              .description = "second",
                                              .isDefault = false,
                                              .backendId = kBackendCoreAudio},
                                             {.id = DeviceId{"builtin-uid"},
                                              .displayName = "Built-in Output",
                                              .description = "Apple",
                                              .isDefault = false,
                                              .backendId = kBackendCoreAudio}};

    auto const selectedRes = audio::test::resolveCoreAudioTestOutput(devices, "BlackHole2ch_UID");

    REQUIRE_FALSE(selectedRes);
    CHECK(selectedRes.error().code == Error::Code::InvalidState);
  }

  TEST_CASE("resolveCoreAudioTestOutput does not accept another backend's matching UID", "[audio][unit][coreaudio]")
  {
    auto const devices = std::vector<Device>{{.id = DeviceId{"builtin-uid"},
                                              .displayName = "Built-in Output",
                                              .description = "Apple",
                                              .isDefault = true,
                                              .backendId = kBackendCoreAudio},
                                             {.id = DeviceId{"BlackHole2ch_UID"},
                                              .displayName = "BlackHole 2ch",
                                              .description = "PipeWire virtual",
                                              .isDefault = false,
                                              .backendId = kBackendPipeWire}};

    auto const selectedRes = audio::test::resolveCoreAudioTestOutput(devices, "BlackHole2ch_UID");

    REQUIRE_FALSE(selectedRes);
    CHECK(selectedRes.error().code == Error::Code::DeviceNotFound);
  }

  TEST_CASE("requestedCoreAudioTestOutput returns an empty selection when the UID environment is unset",
            "[audio][integration][coreaudio]")
  {
    auto const result = runCoreAudioOutputProbe("unset");

    requireSuccessfulProbe(result);
    CHECK(result.standardOutput == "unset: absent\n");
  }

  TEST_CASE("requestedCoreAudioTestOutput rejects an empty UID environment value as invalid input",
            "[audio][integration][coreaudio]")
  {
    auto const result = runCoreAudioOutputProbe("empty");

    requireSuccessfulProbe(result);
    CHECK(result.standardOutput ==
          std::format("empty: error code={}\n", std::to_underlying(Error::Code::InvalidInput)));
  }

  TEST_CASE("requestedCoreAudioTestOutput rejects an unmatched UID environment value as device not found",
            "[audio][integration][coreaudio]")
  {
    auto const result = runCoreAudioOutputProbe("missing");

    requireSuccessfulProbe(result);
    CHECK(result.standardOutput ==
          std::format("missing: error code={}\n", std::to_underlying(Error::Code::DeviceNotFound)));
  }
} // namespace ao::audio::backend::test
