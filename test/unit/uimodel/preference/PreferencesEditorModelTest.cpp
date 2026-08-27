// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/preference/PreferencesEditorModel.h>

#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/rt/AppState.h>
#include <ao/uimodel/preference/ThemePreset.h>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("PreferencesEditorModel - theme changes persist and apply selected theme", "[uimodel][unit][preferences]")
  {
    auto persisted = std::vector<rt::AppPrefsState>{};
    auto applied = std::vector<ThemePreset>{};
    auto initial = rt::AppPrefsState{};
    initial.lastThemePreset = "classic";
    initial.preferredOutputSelection.backendId = audio::BackendId{"existing-backend"};

    auto model = PreferencesEditorModel{initial,
                                        [&](rt::AppPrefsState const& prefs, PreferencesChange const change)
                                        {
                                          CHECK(change == PreferencesChange::Theme);
                                          persisted.push_back(prefs);
                                        },
                                        [&](ThemePreset const theme) { applied.push_back(theme); },
                                        {}};

    model.setTheme(ThemePreset::Modern);

    REQUIRE(persisted.size() == 1U);
    CHECK(persisted.front().lastThemePreset == "modern");
    CHECK(persisted.front().preferredOutputSelection.backendId == "existing-backend");
    REQUIRE(applied.size() == 1U);
    CHECK(applied.front() == ThemePreset::Modern);
    CHECK(model.preferences().lastThemePreset == "modern");
  }

  TEST_CASE("PreferencesEditorModel - output changes persist the requested preference", "[uimodel][unit][preferences]")
  {
    auto optPersisted = std::optional<rt::AppPrefsState>{};
    auto optApplied = std::optional<audio::OutputDeviceSelection>{};
    auto initial = rt::AppPrefsState{};
    initial.lastThemePreset = "modern";

    auto model = PreferencesEditorModel{initial,
                                        [&](rt::AppPrefsState const& prefs, PreferencesChange const change)
                                        {
                                          CHECK(change == PreferencesChange::OutputDevice);
                                          optPersisted = prefs;
                                        },
                                        {},
                                        [&](audio::OutputDeviceSelection const& selection) { optApplied = selection; }};

    auto const requested = audio::OutputDeviceSelection{
      .backendId = audio::BackendId{"pipewire"},
      .deviceId = audio::DeviceId{"system-default"},
      .profileId = audio::kProfileShared,
    };
    model.setPreferredOutputDevice(requested);

    REQUIRE(optPersisted);
    CHECK(optPersisted->lastThemePreset == "modern");
    CHECK(optPersisted->preferredOutputSelection.backendId == "pipewire");
    CHECK(optPersisted->preferredOutputSelection.deviceId == "system-default");
    CHECK(optPersisted->preferredOutputSelection.profileId == audio::kProfileShared.raw());
    REQUIRE(optApplied);
    CHECK(*optApplied == requested);
  }

  TEST_CASE("PreferencesEditorModel - layout preset changes persist for the next layout load",
            "[uimodel][unit][preferences]")
  {
    auto optPersisted = std::optional<rt::AppPrefsState>{};
    auto initial = rt::AppPrefsState{};
    initial.lastThemePreset = "modern";
    initial.preferredOutputSelection.backendId = audio::BackendId{"existing-backend"};
    initial.lastLayoutPreset = "classic";

    auto model = PreferencesEditorModel{initial,
                                        [&](rt::AppPrefsState const& prefs, PreferencesChange const change)
                                        {
                                          CHECK(change == PreferencesChange::LayoutPreset);
                                          optPersisted = prefs;
                                        },
                                        [](ThemePreset) { FAIL("Layout preset changes must not apply theme changes"); },
                                        [](audio::OutputDeviceSelection const&)
                                        { FAIL("Layout preset changes must not apply output changes"); }};

    model.setLayoutPreset("modern");

    REQUIRE(optPersisted);
    CHECK(optPersisted->lastLayoutPreset == "modern");
    CHECK(optPersisted->lastThemePreset == "modern");
    CHECK(optPersisted->preferredOutputSelection.backendId == "existing-backend");
    CHECK(model.preferences().lastLayoutPreset == "modern");
  }

  TEST_CASE("mergePreferenceChange preserves unrelated current preferences", "[uimodel][unit][preferences]")
  {
    auto current = rt::AppPrefsState{};
    current.lastThemePreset = "classic";
    current.lastLayoutPreset = "modern";
    current.preferredOutputSelection.backendId = audio::BackendId{"pipewire"};
    current.preferredOutputSelection.deviceId = audio::DeviceId{"current-device"};
    current.preferredOutputSelection.profileId = audio::kProfileShared;

    auto requested = rt::AppPrefsState{};
    requested.lastThemePreset = "modern";
    requested.lastLayoutPreset = "classic";
    requested.preferredOutputSelection.backendId = audio::BackendId{"alsa"};
    requested.preferredOutputSelection.deviceId = audio::DeviceId{"requested-device"};
    requested.preferredOutputSelection.profileId = audio::kProfileExclusive;

    SECTION("theme updates only the theme")
    {
      auto merged = mergePreferenceChange(current, requested, PreferencesChange::Theme);

      CHECK(merged.lastThemePreset == "modern");
      CHECK(merged.lastLayoutPreset == "modern");
      CHECK(merged.preferredOutputSelection.backendId == "pipewire");
      CHECK(merged.preferredOutputSelection.deviceId == "current-device");
      CHECK(merged.preferredOutputSelection.profileId == audio::kProfileShared.raw());
    }

    SECTION("layout preset updates only the layout preset")
    {
      auto merged = mergePreferenceChange(current, requested, PreferencesChange::LayoutPreset);

      CHECK(merged.lastThemePreset == "classic");
      CHECK(merged.lastLayoutPreset == "classic");
      CHECK(merged.preferredOutputSelection.backendId == "pipewire");
      CHECK(merged.preferredOutputSelection.deviceId == "current-device");
      CHECK(merged.preferredOutputSelection.profileId == audio::kProfileShared.raw());
    }

    SECTION("output updates only the output tuple")
    {
      auto merged = mergePreferenceChange(current, requested, PreferencesChange::OutputDevice);

      CHECK(merged.lastThemePreset == "classic");
      CHECK(merged.lastLayoutPreset == "modern");
      CHECK(merged.preferredOutputSelection.backendId == "alsa");
      CHECK(merged.preferredOutputSelection.deviceId == "requested-device");
      CHECK(merged.preferredOutputSelection.profileId == audio::kProfileExclusive.raw());
    }
  }
} // namespace ao::uimodel::test
