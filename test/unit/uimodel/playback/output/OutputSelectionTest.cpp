// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/playback/output/OutputSelection.h>

#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/rt/PlaybackState.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::uimodel::test
{
  namespace
  {
    rt::OutputState outputCatalog()
    {
      return {
        .availableBackends =
          {
            {
              .id = audio::BackendId{"pipewire"},
              .supportedProfiles = {{.id = audio::kProfileShared}, {.id = audio::kProfileExclusive}},
              .devices =
                {
                  {
                    .id = audio::DeviceId{},
                    .displayName = "System Default",
                    .isDefault = true,
                    .backendId = audio::BackendId{"pipewire"},
                  },
                  {
                    .id = audio::DeviceId{"device1"},
                    .displayName = "Built-in Audio",
                    .backendId = audio::BackendId{"pipewire"},
                  },
                },
            },
          },
      };
    }
  } // namespace

  TEST_CASE("OutputSelection - validates intent against the output catalog", "[uimodel][unit][playback][output]")
  {
    auto const output = outputCatalog();

    SECTION("backend and profile are required")
    {
      CHECK_FALSE(canRestoreOutputDeviceSelection({}, output));
      CHECK_FALSE(canRestoreOutputDeviceSelection({.backendId = audio::BackendId{"pipewire"}}, output));
      CHECK_FALSE(canRestoreOutputDeviceSelection({.profileId = audio::kProfileShared}, output));
    }

    SECTION("an unpublished named backend stays pending while a known unsupported profile is rejected")
    {
      CHECK(canRestoreOutputDeviceSelection({.backendId = audio::BackendId{"missing"},
                                             .deviceId = audio::DeviceId{"device1"},
                                             .profileId = audio::kProfileShared},
                                            output));
      CHECK_FALSE(canRestoreOutputDeviceSelection({.backendId = audio::BackendId{"pipewire"},
                                                   .deviceId = audio::DeviceId{"device1"},
                                                   .profileId = audio::ProfileId{"unsupported"}},
                                                  output));
    }

    SECTION("a named device remains pending intent before the catalog is published")
    {
      CHECK(canRestoreOutputDeviceSelection({.backendId = audio::BackendId{"pipewire"},
                                             .deviceId = audio::DeviceId{"device1"},
                                             .profileId = audio::kProfileShared},
                                            {}));
    }

    SECTION("an unavailable named device remains restorable intent")
    {
      CHECK(canRestoreOutputDeviceSelection({.backendId = audio::BackendId{"pipewire"},
                                             .deviceId = audio::DeviceId{"temporarily-unavailable"},
                                             .profileId = audio::kProfileExclusive},
                                            output));
    }

    SECTION("an empty device is valid only for an advertised compatible default")
    {
      CHECK(canRestoreOutputDeviceSelection(
        {.backendId = audio::BackendId{"pipewire"}, .deviceId = audio::DeviceId{}, .profileId = audio::kProfileShared},
        output));
      CHECK_FALSE(canRestoreOutputDeviceSelection({.backendId = audio::BackendId{"pipewire"},
                                                   .deviceId = audio::DeviceId{},
                                                   .profileId = audio::kProfileExclusive},
                                                  output));
      CHECK_FALSE(canRestoreOutputDeviceSelection(
        {.backendId = audio::kBackendPipeWire, .deviceId = audio::DeviceId{}, .profileId = audio::kProfileShared}, {}));
      CHECK_FALSE(canRestoreOutputDeviceSelection(
        {.backendId = audio::kBackendWasapi, .deviceId = audio::DeviceId{}, .profileId = audio::kProfileShared}, {}));

      auto nonDefaultOutput = output;
      nonDefaultOutput.availableBackends.front().devices.front().isDefault = false;
      CHECK_FALSE(canRestoreOutputDeviceSelection(
        {.backendId = audio::BackendId{"pipewire"}, .deviceId = audio::DeviceId{}, .profileId = audio::kProfileShared},
        nonDefaultOutput));
    }
  }

  TEST_CASE("OutputSelection - resolves preferred intent before session fallback", "[uimodel][unit][playback][output]")
  {
    auto const output = outputCatalog();
    auto const fallback = audio::OutputDeviceSelection{
      .backendId = audio::BackendId{"pipewire"},
      .deviceId = audio::DeviceId{"device1"},
      .profileId = audio::kProfileShared,
    };

    SECTION("valid preferred intent wins even when its named device is unavailable")
    {
      auto const preferred = audio::OutputDeviceSelection{
        .backendId = audio::BackendId{"pipewire"},
        .deviceId = audio::DeviceId{"temporarily-unavailable"},
        .profileId = audio::kProfileExclusive,
      };

      auto const optSelection = resolveOutputDeviceSelectionToRestore(preferred, fallback, output);

      REQUIRE(optSelection);
      CHECK(*optSelection == preferred);
    }

    SECTION("incomplete preferred intent falls back to the last active selection")
    {
      auto const optSelection =
        resolveOutputDeviceSelectionToRestore({.backendId = audio::BackendId{"pipewire"}}, fallback, output);

      REQUIRE(optSelection);
      CHECK(*optSelection == fallback);
    }

    SECTION("an incomplete fallback is ignored")
    {
      auto incompleteFallback = fallback;
      incompleteFallback.profileId = {};

      CHECK_FALSE(resolveOutputDeviceSelectionToRestore({}, incompleteFallback, output));
    }

    SECTION("an empty-device fallback without a published backend is ignored")
    {
      auto invalidFallback = fallback;
      invalidFallback.backendId = audio::BackendId{"missing"};
      invalidFallback.deviceId = {};

      CHECK_FALSE(resolveOutputDeviceSelectionToRestore({}, invalidFallback, output));
    }
  }
} // namespace ao::uimodel::test
