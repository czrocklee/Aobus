// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/rt/AppPrefsState.h>
#include <ao/rt/PlaybackState.h>
#include <ao/uimodel/preferences/PreferencesEditorModel.h>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("PreferencesEditorModel - theme changes persist and apply selected theme", "[uimodel][unit][preferences]")
  {
    auto persisted = std::vector<rt::AppPrefsState>{};
    auto applied = std::vector<rt::ThemePresetId>{};
    auto initial = rt::AppPrefsState{};
    initial.lastThemePreset = "classic";
    initial.lastOutputBackendId = "existing-backend";

    auto model = PreferencesEditorModel{initial,
                                        [&](rt::AppPrefsState const& prefs, PreferencesChange const change)
                                        {
                                          CHECK(change == PreferencesChange::Theme);
                                          persisted.push_back(prefs);
                                        },
                                        [&](rt::ThemePresetId const theme) { applied.push_back(theme); },
                                        {}};

    model.setTheme(rt::ThemePresetId::Modern);

    REQUIRE(persisted.size() == 1U);
    CHECK(persisted.front().lastThemePreset == "modern");
    CHECK(persisted.front().lastOutputBackendId == "existing-backend");
    REQUIRE(applied.size() == 1U);
    CHECK(applied.front() == rt::ThemePresetId::Modern);
    CHECK(model.preferences().lastThemePreset == "modern");
  }

  TEST_CASE("PreferencesEditorModel - output changes persist engine-confirmed selection",
            "[uimodel][unit][preferences]")
  {
    auto optPersisted = std::optional<rt::AppPrefsState>{};
    auto optApplied = std::optional<rt::OutputDeviceSelection>{};
    auto initial = rt::AppPrefsState{};
    initial.lastThemePreset = "modern";

    auto model = PreferencesEditorModel{initial,
                                        [&](rt::AppPrefsState const& prefs, PreferencesChange const change)
                                        {
                                          CHECK(change == PreferencesChange::OutputDevice);
                                          optPersisted = prefs;
                                        },
                                        {},
                                        [&](rt::OutputDeviceSelection const& selection) { optApplied = selection; }};

    auto const confirmed = rt::OutputDeviceSelection{
      .backendId = audio::BackendId{"pipewire"},
      .deviceId = audio::DeviceId{"system-default"},
      .profileId = audio::kProfileShared,
    };
    model.setOutputDeviceConfirmed(confirmed);

    REQUIRE(optPersisted);
    CHECK(optPersisted->lastThemePreset == "modern");
    CHECK(optPersisted->lastOutputBackendId == "pipewire");
    CHECK(optPersisted->lastOutputDeviceId == "system-default");
    CHECK(optPersisted->lastOutputProfileId == audio::kProfileShared.raw());
    REQUIRE(optApplied);
    CHECK(*optApplied == confirmed);
  }

  TEST_CASE("PreferencesEditorModel - layout preset changes persist for the next layout load",
            "[uimodel][unit][preferences]")
  {
    auto optPersisted = std::optional<rt::AppPrefsState>{};
    auto initial = rt::AppPrefsState{};
    initial.lastThemePreset = "modern";
    initial.lastOutputBackendId = "existing-backend";
    initial.lastLayoutPreset = "classic";

    auto model = PreferencesEditorModel{
      initial,
      [&](rt::AppPrefsState const& prefs, PreferencesChange const change)
      {
        CHECK(change == PreferencesChange::LayoutPreset);
        optPersisted = prefs;
      },
      [](rt::ThemePresetId) { FAIL("Layout preset changes must not apply theme changes"); },
      [](rt::OutputDeviceSelection const&) { FAIL("Layout preset changes must not apply output changes"); }};

    model.setLayoutPreset("modern");

    REQUIRE(optPersisted);
    CHECK(optPersisted->lastLayoutPreset == "modern");
    CHECK(optPersisted->lastThemePreset == "modern");
    CHECK(optPersisted->lastOutputBackendId == "existing-backend");
    CHECK(model.preferences().lastLayoutPreset == "modern");
  }

  TEST_CASE("mergePreferenceChange preserves unrelated current preferences", "[uimodel][unit][preferences]")
  {
    auto current = rt::AppPrefsState{};
    current.lastThemePreset = "classic";
    current.lastLayoutPreset = "modern";
    current.lastOutputBackendId = "pipewire";
    current.lastOutputDeviceId = "current-device";
    current.lastOutputProfileId = audio::kProfileShared.raw();

    auto requested = rt::AppPrefsState{};
    requested.lastThemePreset = "modern";
    requested.lastLayoutPreset = "classic";
    requested.lastOutputBackendId = "alsa";
    requested.lastOutputDeviceId = "requested-device";
    requested.lastOutputProfileId = audio::kProfileExclusive.raw();

    SECTION("theme updates only the theme")
    {
      auto merged = mergePreferenceChange(current, requested, PreferencesChange::Theme);

      CHECK(merged.lastThemePreset == "modern");
      CHECK(merged.lastLayoutPreset == "modern");
      CHECK(merged.lastOutputBackendId == "pipewire");
      CHECK(merged.lastOutputDeviceId == "current-device");
      CHECK(merged.lastOutputProfileId == audio::kProfileShared.raw());
    }

    SECTION("layout preset updates only the layout preset")
    {
      auto merged = mergePreferenceChange(current, requested, PreferencesChange::LayoutPreset);

      CHECK(merged.lastThemePreset == "classic");
      CHECK(merged.lastLayoutPreset == "classic");
      CHECK(merged.lastOutputBackendId == "pipewire");
      CHECK(merged.lastOutputDeviceId == "current-device");
      CHECK(merged.lastOutputProfileId == audio::kProfileShared.raw());
    }

    SECTION("output updates only the output tuple")
    {
      auto merged = mergePreferenceChange(current, requested, PreferencesChange::OutputDevice);

      CHECK(merged.lastThemePreset == "classic");
      CHECK(merged.lastLayoutPreset == "modern");
      CHECK(merged.lastOutputBackendId == "alsa");
      CHECK(merged.lastOutputDeviceId == "requested-device");
      CHECK(merged.lastOutputProfileId == audio::kProfileExclusive.raw());
    }
  }
} // namespace ao::uimodel::test
