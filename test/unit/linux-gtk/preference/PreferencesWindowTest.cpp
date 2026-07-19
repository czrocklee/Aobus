// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "preference/PreferencesWindow.h"

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/audio/BackendIds.h>
#include <ao/rt/AppPrefsState.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/layout/action/LayoutActionCapabilities.h>
#include <ao/uimodel/layout/action/LayoutActionCatalog.h>
#include <ao/uimodel/preference/PreferencesEditorModel.h>
#include <ao/uimodel/preference/ThemePreset.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/listbox.h>
#include <gtkmm/scrolledwindow.h>

#include <cstdint>
#include <optional>

namespace ao::gtk::test
{
  namespace
  {
    uimodel::LayoutActionCatalog makeCatalog()
    {
      auto catalog = uimodel::LayoutActionCatalog{};
      catalog.registerActionDescriptor({.id = "playback.playPause",
                                        .label = "Play/Pause",
                                        .category = "Playback",
                                        .capabilities = uimodel::LayoutActionCapability::None});
      return catalog;
    }

    Gtk::ListBox* outputSelectorListBox(PreferencesWindow& window)
    {
      auto* const selector = window.outputSelector();

      if (selector == nullptr)
      {
        return nullptr;
      }

      auto* const scrolled = dynamic_cast<Gtk::ScrolledWindow*>(selector->get_child());

      if (scrolled == nullptr)
      {
        return nullptr;
      }

      auto* const viewport = scrolled->get_child();

      if (viewport == nullptr)
      {
        return nullptr;
      }

      return dynamic_cast<Gtk::ListBox*>(viewport->get_first_child());
    }
  } // namespace

  TEST_CASE("PreferencesWindow - builds first-cut pages and hosts shortcut editor", "[gtk][unit][preferences]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto window = PreferencesWindow{{}};

    CHECK(window.hasPage("general"));
    CHECK(window.hasPage("appearance"));
    CHECK(window.hasPage("playback"));
    CHECK(window.hasPage("layout"));
    CHECK(window.hasPage("keyboard"));

    auto catalog = makeCatalog();
    window.refreshKeyboardPage(catalog, uimodel::KeymapModel{uimodel::defaultKeymap()}, {});

    CHECK(findLabelByText(window, "Play/Pause") != nullptr);
    CHECK(findLabelByText(window, "Ctrl+P") != nullptr);
  }

  TEST_CASE("PreferencesWindow - layout page dispatches commands", "[gtk][unit][preferences]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    std::int32_t editCount = 0;
    std::int32_t resetCount = 0;
    std::int32_t savePanelsCount = 0;

    auto window = PreferencesWindow{PreferencesWindow::Callbacks{
      .onEditLayout = [&editCount] { ++editCount; },
      .onResetRuntimeLayoutState = [&resetCount] { ++resetCount; },
      .onSaveCurrentPanelSizesAsLayoutDefaults = [&savePanelsCount] { ++savePanelsCount; },
    }};

    auto* const editButton = findButtonByLabel(window, "Edit Layout...");
    auto* const saveButton = findButtonByLabel(window, "Save Current Panel Sizes as Layout Defaults");
    auto* const resetButton = findButtonByLabel(window, "Reset Runtime Layout State");
    REQUIRE(editButton != nullptr);
    REQUIRE(saveButton != nullptr);
    REQUIRE(resetButton != nullptr);

    emitClicked(*editButton);
    emitClicked(*saveButton);
    emitClicked(*resetButton);

    CHECK(editCount == 1);
    CHECK(savePanelsCount == 1);
    CHECK(resetCount == 1);
  }

  TEST_CASE("PreferencesWindow - appearance page persists and applies selected theme", "[gtk][unit][preferences]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto optPersisted = std::optional<rt::AppPrefsState>{};
    auto optTheme = std::optional<uimodel::ThemePreset>{};
    auto window = PreferencesWindow{PreferencesWindow::Callbacks{
      .onPersistPreferences = [&](rt::AppPrefsState const& prefs, uimodel::PreferencesChange) { optPersisted = prefs; },
      .onApplyTheme = [&](uimodel::ThemePreset const theme) { optTheme = theme; },
    }};

    auto prefs = rt::AppPrefsState{};
    prefs.lastThemePreset = "classic";
    prefs.lastOutputBackendId = "existing-backend";
    window.refreshPreferences(prefs, nullptr);
    CHECK(window.selectedThemeId() == "classic");
    CHECK_FALSE(optPersisted);

    window.setSelectedThemeId("modern");

    REQUIRE(optPersisted);
    CHECK(optPersisted->lastThemePreset == "modern");
    CHECK(optPersisted->lastOutputBackendId == "existing-backend");
    REQUIRE(optTheme);
    CHECK(*optTheme == uimodel::ThemePreset::Modern);
  }

  TEST_CASE("PreferencesWindow - layout page persists default preset for next launch", "[gtk][unit][preferences]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto optPersisted = std::optional<rt::AppPrefsState>{};
    auto window = PreferencesWindow{PreferencesWindow::Callbacks{
      .onPersistPreferences = [&](rt::AppPrefsState const& prefs, uimodel::PreferencesChange) { optPersisted = prefs; },
      .onApplyTheme = [](uimodel::ThemePreset) { FAIL("Layout preset changes must not apply theme changes"); },
    }};

    auto prefs = rt::AppPrefsState{};
    prefs.lastThemePreset = "classic";
    prefs.lastLayoutPreset = "classic";
    prefs.lastOutputBackendId = "existing-backend";
    window.refreshPreferences(prefs, nullptr);
    CHECK(window.selectedLayoutPresetId() == "classic");
    CHECK_FALSE(optPersisted);

    window.setSelectedLayoutPresetId("modern");

    REQUIRE(optPersisted);
    CHECK(optPersisted->lastLayoutPreset == "modern");
    CHECK(optPersisted->lastThemePreset == "classic");
    CHECK(optPersisted->lastOutputBackendId == "existing-backend");
  }

  TEST_CASE("PreferencesWindow - playback output selection persists the confirmed device", "[gtk][unit][preferences]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto fixture = GtkRuntimeFixture{};
    rt::test::addReadyAudioProvider(fixture.runtime());

    auto optPersisted = std::optional<rt::AppPrefsState>{};
    auto window = PreferencesWindow{PreferencesWindow::Callbacks{
      .onPersistPreferences =
        [&](rt::AppPrefsState const& prefs, uimodel::PreferencesChange const change)
      {
        CHECK(change == uimodel::PreferencesChange::OutputDevice);
        optPersisted = prefs;
      },
    }};

    auto prefs = rt::AppPrefsState{};
    prefs.lastThemePreset = "modern";
    prefs.lastLayoutPreset = "classic";
    window.refreshPreferences(prefs, &fixture.runtime().playback());
    REQUIRE(window.outputSelector() != nullptr);

    emitShow(*window.outputSelector());
    drainGtkEvents();

    auto* const listBox = outputSelectorListBox(window);
    REQUIRE(listBox != nullptr);
    auto* const deviceRow = listBox->get_row_at_index(1);
    REQUIRE(deviceRow != nullptr);

    emitRowActivated(*listBox, *deviceRow);

    REQUIRE(optPersisted);
    CHECK(optPersisted->lastThemePreset == "modern");
    CHECK(optPersisted->lastLayoutPreset == "classic");
    CHECK(optPersisted->lastOutputBackendId == "test_backend");
    CHECK(optPersisted->lastOutputDeviceId == "test_device");
    CHECK(optPersisted->lastOutputProfileId == audio::kProfileShared.raw());
  }

  TEST_CASE("PreferencesWindow - target hide clears window-scoped output selector", "[gtk][unit][preferences]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto fixture = GtkRuntimeFixture{};
    rt::test::addReadyAudioProvider(fixture.runtime());

    auto target = Gtk::Window{};
    auto window = PreferencesWindow{{}};
    auto prefs = rt::AppPrefsState{};

    window.refreshPreferences(prefs, &fixture.runtime().playback(), &target);
    REQUIRE(window.hasOutputSelector());
    CHECK(window.outputDeviceLabelText() != "Unavailable");

    ::g_signal_emit_by_name(target.gobj(), "hide");
    drainGtkEvents();

    CHECK_FALSE(window.hasOutputSelector());
    CHECK(window.outputDeviceLabelText() == "Unavailable");
  }

  TEST_CASE("PreferencesWindow - unknown persisted ids fall back to visible defaults", "[gtk][unit][preferences]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto window = PreferencesWindow{{}};
    auto prefs = rt::AppPrefsState{};
    prefs.lastThemePreset = "future-theme";
    prefs.lastLayoutPreset = "future-layout";

    window.refreshPreferences(prefs, nullptr);

    CHECK(window.selectedThemeId() == "classic");
    CHECK(window.selectedLayoutPresetId() == "classic");
  }
} // namespace ao::gtk::test
