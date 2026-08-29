// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/app/DesktopOutputSelection.h>

#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/rt/PlaybackState.h>
#include <ao/winui/DesktopSettingsYamlSchema.h>
#include <ao/winui/layout/ShellState.h>
#include <ao/yaml/RymlAdapter.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::winui::test
{
  TEST_CASE("DesktopOutputSelection - resolves the stored Windows preference at startup", "[winui][unit][app][output]")
  {
    auto settings = DesktopSettings{};
    settings.preferredOutputSelection = {
      .backendId = audio::kBackendWasapi,
      .deviceId = audio::DeviceId{"studio-dac"},
      .profileId = audio::kProfileExclusive,
    };
    auto const output = rt::OutputState{
      .availableBackends =
        {
          {
            .id = audio::kBackendWasapi,
            .supportedProfiles = {{.id = audio::kProfileShared}, {.id = audio::kProfileExclusive}},
            .devices =
              {
                {
                  .id = audio::DeviceId{"studio-dac"},
                  .displayName = "Studio DAC",
                  .isDefault = true,
                  .backendId = audio::kBackendWasapi,
                },
              },
          },
        },
    };

    auto const optSelection = resolveDesktopOutputSelectionToRestore(settings, output);

    REQUIRE(optSelection);
    CHECK(*optSelection == settings.preferredOutputSelection);
  }

  TEST_CASE("DesktopOutputSelection - remembers the exact request for the next settings checkpoint",
            "[winui][unit][app][output]")
  {
    auto settings = DesktopSettings{};
    settings.window = {.x = 41, .y = 42, .width = 1400, .height = 900, .maximized = true};
    settings.shellMode = ShellMode::Classic;
    settings.lastLibraryPath = "C:/Music";
    settings.navigationPaneWidth = 275.0;
    settings.inspectorPaneWidth = 375.0;
    auto const selection = audio::OutputDeviceSelection{
      .backendId = audio::kBackendWasapi,
      .deviceId = audio::DeviceId{"headphones"},
      .profileId = audio::kProfileShared,
    };
    auto expected = settings;
    expected.preferredOutputSelection = selection;

    CHECK(rememberDesktopOutputSelection(settings, selection));
    CHECK(settings == expected);
    CHECK_FALSE(rememberDesktopOutputSelection(settings, selection));

    auto tree = ryml::Tree{yaml::callbacks()};
    REQUIRE(DesktopSettingsYamlSchema{}.serialize(tree.rootref(), settings));
    auto const decodedRes = DesktopSettingsYamlSchema{}.deserialize(tree.rootref(), DesktopSettings{});

    REQUIRE(decodedRes);
    CHECK(*decodedRes == settings);
  }
} // namespace ao::winui::test
