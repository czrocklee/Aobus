// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/preference/PreferencesEditorModel.h>

#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/rt/AppState.h>
#include <ao/uimodel/preference/ThemePreset.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace ao::uimodel::test
{
  namespace
  {
    struct PreferenceWrite final
    {
      rt::AppPrefsState preferences;
      PreferencesChange change;
    };

    void checkPreferences(rt::AppPrefsState const& actual, rt::AppPrefsState const& expected)
    {
      CHECK(actual.lastThemePreset == expected.lastThemePreset);
      CHECK(actual.lastLayoutPreset == expected.lastLayoutPreset);
      CHECK(actual.preferredOutputSelection == expected.preferredOutputSelection);
    }
  } // namespace

  TEST_CASE("PreferencesEditorModel - theme changes persist and apply selected theme", "[uimodel][unit][preference]")
  {
    auto persisted = std::vector<PreferenceWrite>{};
    auto appliedThemes = std::vector<ThemePreset>{};
    auto appliedOutputs = std::vector<audio::OutputDeviceSelection>{};
    auto initial = rt::AppPrefsState{};
    initial.lastThemePreset = "classic";

    SECTION("backend-only initial preferences")
    {
      initial.preferredOutputSelection.backendId = audio::BackendId{"existing-backend"};
    }

    SECTION("unrelated populated preferences remain unchanged")
    {
      initial.lastLayoutPreset = "classic";
      initial.preferredOutputSelection = {.backendId = audio::BackendId{"existing-backend"},
                                          .deviceId = audio::DeviceId{"existing-device"},
                                          .profileId = audio::kProfileExclusive};
    }

    auto model = PreferencesEditorModel{initial,
                                        [&persisted](rt::AppPrefsState const& prefs, PreferencesChange const change)
                                        { persisted.push_back({prefs, change}); },
                                        [&appliedThemes](ThemePreset const theme) { appliedThemes.push_back(theme); },
                                        [&appliedOutputs](audio::OutputDeviceSelection const& selection)
                                        { appliedOutputs.push_back(selection); }};

    model.setTheme(ThemePreset::Modern);

    auto expected = initial;
    expected.lastThemePreset = "modern";
    REQUIRE(persisted.size() == 1U);
    CHECK(persisted.front().change == PreferencesChange::Theme);
    checkPreferences(persisted.front().preferences, expected);
    checkPreferences(model.preferences(), expected);
    CHECK(appliedThemes == std::vector<ThemePreset>{ThemePreset::Modern});
    CHECK(appliedOutputs.empty());
  }

  TEST_CASE("PreferencesEditorModel - output changes persist the requested preference", "[uimodel][unit][preference]")
  {
    auto persisted = std::vector<PreferenceWrite>{};
    auto appliedThemes = std::vector<ThemePreset>{};
    auto appliedOutputs = std::vector<audio::OutputDeviceSelection>{};
    auto initial = rt::AppPrefsState{};
    initial.lastThemePreset = "modern";

    SECTION("first output preference")
    {
      initial.preferredOutputSelection = {};
    }

    SECTION("replacement preserves unrelated populated preferences")
    {
      initial.lastLayoutPreset = "classic";
      initial.preferredOutputSelection = {.backendId = audio::BackendId{"existing-backend"},
                                          .deviceId = audio::DeviceId{"existing-device"},
                                          .profileId = audio::kProfileExclusive};
    }

    auto model = PreferencesEditorModel{initial,
                                        [&persisted](rt::AppPrefsState const& prefs, PreferencesChange const change)
                                        { persisted.push_back({prefs, change}); },
                                        [&appliedThemes](ThemePreset const theme) { appliedThemes.push_back(theme); },
                                        [&appliedOutputs](audio::OutputDeviceSelection const& selection)
                                        { appliedOutputs.push_back(selection); }};

    auto const requested = audio::OutputDeviceSelection{
      .backendId = audio::BackendId{"pipewire"},
      .deviceId = audio::DeviceId{"system-default"},
      .profileId = audio::kProfileShared,
    };
    model.setPreferredOutputDevice(requested);

    auto expected = initial;
    expected.preferredOutputSelection = requested;
    REQUIRE(persisted.size() == 1U);
    CHECK(persisted.front().change == PreferencesChange::OutputDevice);
    checkPreferences(persisted.front().preferences, expected);
    checkPreferences(model.preferences(), expected);
    CHECK(appliedOutputs == std::vector<audio::OutputDeviceSelection>{requested});
    CHECK(appliedThemes.empty());
  }

  TEST_CASE("PreferencesEditorModel - layout preset changes persist for the next layout load",
            "[uimodel][unit][preference]")
  {
    auto persisted = std::vector<PreferenceWrite>{};
    auto appliedThemes = std::vector<ThemePreset>{};
    auto appliedOutputs = std::vector<audio::OutputDeviceSelection>{};
    auto initial = rt::AppPrefsState{};
    initial.preferredOutputSelection.backendId = audio::BackendId{"existing-backend"};
    initial.lastLayoutPreset = "classic";

    SECTION("modern theme with backend-only output remains unchanged")
    {
      initial.lastThemePreset = "modern";
    }

    SECTION("classic theme and populated output remain unchanged")
    {
      initial.lastThemePreset = "classic";
      initial.preferredOutputSelection = {.backendId = audio::BackendId{"existing-backend"},
                                          .deviceId = audio::DeviceId{"existing-device"},
                                          .profileId = audio::kProfileExclusive};
    }

    auto model = PreferencesEditorModel{initial,
                                        [&persisted](rt::AppPrefsState const& prefs, PreferencesChange const change)
                                        { persisted.push_back({prefs, change}); },
                                        [&appliedThemes](ThemePreset const theme) { appliedThemes.push_back(theme); },
                                        [&appliedOutputs](audio::OutputDeviceSelection const& selection)
                                        { appliedOutputs.push_back(selection); }};

    model.setLayoutPreset("modern");

    auto expected = initial;
    expected.lastLayoutPreset = "modern";
    REQUIRE(persisted.size() == 1U);
    CHECK(persisted.front().change == PreferencesChange::LayoutPreset);
    checkPreferences(persisted.front().preferences, expected);
    checkPreferences(model.preferences(), expected);
    CHECK(appliedThemes.empty());
    CHECK(appliedOutputs.empty());
  }

  TEST_CASE("mergePreferenceChange preserves unrelated current preferences", "[uimodel][unit][preference]")
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
