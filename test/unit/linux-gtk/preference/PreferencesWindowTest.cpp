// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "preference/PreferencesWindow.h"

#include "app/AppDialog.h"
#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include <ao/Error.h>
#include <ao/audio/BackendIds.h>
#include <ao/rt/AppState.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/preference/PreferencesEditorModel.h>
#include <ao/uimodel/preference/ThemePreset.h>

#include <catch2/catch_test_macros.hpp>
#include <glib.h>
#include <gtkmm/dialog.h>
#include <gtkmm/listbox.h>
#include <gtkmm/scrolledwindow.h>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace ao::gtk::test
{
  namespace
  {
    uimodel::LayoutSchema makeSchema()
    {
      auto schema = uimodel::LayoutSchema{};
      schema.addAction({.id = "playback.playPause", .label = "Play/Pause", .category = "Playback", .capabilities = 0});
      return schema;
    }

    auto const kPendingCloseMessage =
      std::string{"The last shortcut change was not saved. Retry the save, discard the change, or keep editing."};

    /// The live pending-shortcut close prompt: a separate, showing toplevel carrying that message.
    /// A retired prompt stays in the toplevel list hidden until its parent goes away.
    AppDialog* pendingShortcutClosePrompt()
    {
      for (auto* const topLevel : Gtk::Window::list_toplevels())
      {
        if (!topLevel->get_visible() || findLabelByText(*topLevel, kPendingCloseMessage) == nullptr)
        {
          continue;
        }

        if (auto* const dialog = dynamic_cast<AppDialog*>(topLevel); dialog != nullptr)
        {
          return dialog;
        }
      }

      return nullptr;
    }

    /// Counts pending-shortcut prompts across all toplevels. The toplevel list is process-wide, so
    /// this matches the prompt's own message rather than counting every AppDialog in the suite.
    std::size_t pendingShortcutClosePromptCount()
    {
      std::size_t count = 0;

      for (auto* const topLevel : Gtk::Window::list_toplevels())
      {
        if (dynamic_cast<AppDialog*>(topLevel) != nullptr && topLevel->get_visible() &&
            findLabelByText(*topLevel, kPendingCloseMessage) != nullptr)
        {
          ++count;
        }
      }

      return count;
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

    auto window = PreferencesWindow{ao::test::englishMessageCatalog(), {}};

    CHECK(window.hasPage("general"));
    CHECK(window.hasPage("appearance"));
    CHECK(window.hasPage("playback"));
    CHECK(window.hasPage("layout"));
    CHECK(window.hasPage("keyboard"));

    auto schema = makeSchema();
    window.refreshKeyboardPage(schema, uimodel::KeymapModel{uimodel::defaultKeymap()}, {});

    CHECK(findLabelByText(window, "Play/Pause") != nullptr);
    CHECK(findLabelByText(window, "Ctrl+P") != nullptr);
  }

  TEST_CASE("PreferencesWindow - renders locale-selected page and action copy", "[gtk][unit][localization]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto const textCatalog = ao::test::messageCatalog("de-DE");
    auto window = PreferencesWindow{textCatalog, {}};

    CHECK(window.get_title() == "Einstellungen");
    CHECK(findLabelByText(window, "Design") != nullptr);
    CHECK(findLabelByText(window, "Aktionen") != nullptr);
    CHECK(findButtonByLabel(window, "Layout bearbeiten...") != nullptr);
  }

  TEST_CASE("PreferencesWindow - layout page dispatches commands", "[gtk][unit][preferences]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    std::int32_t editCount = 0;
    std::int32_t resetCount = 0;
    std::int32_t savePanelsCount = 0;

    auto window =
      PreferencesWindow{ao::test::englishMessageCatalog(),
                        PreferencesWindow::Callbacks{
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
    auto window = PreferencesWindow{ao::test::englishMessageCatalog(),
                                    PreferencesWindow::Callbacks{
                                      .onPersistPreferences = [&](rt::AppPrefsState const& prefs,
                                                                  uimodel::PreferencesChange) { optPersisted = prefs; },
                                      .onApplyTheme = [&](uimodel::ThemePreset const theme) { optTheme = theme; },
                                    }};

    auto prefs = rt::AppPrefsState{};
    prefs.lastThemePreset = "classic";
    prefs.preferredOutputSelection.backendId = audio::BackendId{"existing-backend"};
    window.refreshPreferences(prefs, nullptr);
    CHECK(window.selectedThemeId() == "classic");
    CHECK_FALSE(optPersisted);

    window.setSelectedThemeId("modern");

    REQUIRE(optPersisted);
    CHECK(optPersisted->lastThemePreset == "modern");
    CHECK(optPersisted->preferredOutputSelection.backendId == "existing-backend");
    REQUIRE(optTheme);
    CHECK(*optTheme == uimodel::ThemePreset::Modern);
  }

  TEST_CASE("PreferencesWindow - layout page persists default preset for next launch", "[gtk][unit][preferences]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto optPersisted = std::optional<rt::AppPrefsState>{};
    auto window = PreferencesWindow{
      ao::test::englishMessageCatalog(),
      PreferencesWindow::Callbacks{
        .onPersistPreferences = [&](rt::AppPrefsState const& prefs, uimodel::PreferencesChange)
        { optPersisted = prefs; },
        .onApplyTheme = [](uimodel::ThemePreset) { FAIL("Layout preset changes must not apply theme changes"); },
      }};

    auto prefs = rt::AppPrefsState{};
    prefs.lastThemePreset = "classic";
    prefs.lastLayoutPreset = "classic";
    prefs.preferredOutputSelection.backendId = audio::BackendId{"existing-backend"};
    window.refreshPreferences(prefs, nullptr);
    CHECK(window.selectedLayoutPresetId() == "classic");
    CHECK_FALSE(optPersisted);

    window.setSelectedLayoutPresetId("modern");

    REQUIRE(optPersisted);
    CHECK(optPersisted->lastLayoutPreset == "modern");
    CHECK(optPersisted->lastThemePreset == "classic");
    CHECK(optPersisted->preferredOutputSelection.backendId == "existing-backend");
  }

  TEST_CASE("PreferencesWindow - playback output selection persists the requested device", "[gtk][unit][preferences]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto fixture = GtkRuntimeFixture{};
    rt::test::addReadyAudioProvider(fixture.runtime());

    auto optPersisted = std::optional<rt::AppPrefsState>{};
    auto window = PreferencesWindow{ao::test::englishMessageCatalog(),
                                    PreferencesWindow::Callbacks{
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
    CHECK(optPersisted->preferredOutputSelection.backendId == "test_backend");
    CHECK(optPersisted->preferredOutputSelection.deviceId == "test_device");
    CHECK(optPersisted->preferredOutputSelection.profileId == audio::kProfileShared.raw());
  }

  TEST_CASE("PreferencesWindow - target hide clears window-scoped output selector", "[gtk][unit][preferences]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto fixture = GtkRuntimeFixture{};
    rt::test::addReadyAudioProvider(fixture.runtime());

    auto target = Gtk::Window{};
    auto window = PreferencesWindow{ao::test::englishMessageCatalog(), {}};
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

    auto window = PreferencesWindow{ao::test::englishMessageCatalog(), {}};
    auto prefs = rt::AppPrefsState{};
    prefs.lastThemePreset = "future-theme";
    prefs.lastLayoutPreset = "future-layout";

    window.refreshPreferences(prefs, nullptr);

    CHECK(window.selectedThemeId() == "classic");
    CHECK(window.selectedLayoutPresetId() == "classic");
  }

  TEST_CASE("PreferencesWindow - ordinary close keeps a failed shortcut candidate",
            "[gtk][unit][preferences][shortcut]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto window = PreferencesWindow{ao::test::englishMessageCatalog(), {}};
    window.refreshKeyboardPage(makeSchema(),
                               uimodel::KeymapModel{uimodel::defaultKeymap()},
                               [](uimodel::KeymapModel const&) -> Result<>
                               { return makeError(Error::Code::IoError, "disk full"); });
    drainGtkEvents();

    auto* const removeButton = findButtonByLabel(window, "✕");
    REQUIRE(removeButton != nullptr);
    emitClicked(*removeButton);
    drainGtkEvents();
    REQUIRE(findLabelByText(window, "Could not save shortcuts: disk full") != nullptr);

    gboolean handled = FALSE;
    ::g_signal_emit_by_name(window.gobj(), "close-request", &handled);
    drainGtkEvents();

    CHECK(findLabelByText(window, "Could not save shortcuts: disk full") != nullptr);

    auto* const prompt = pendingShortcutClosePrompt();
    REQUIRE(prompt != nullptr);
    prompt->response(Gtk::ResponseType::CANCEL);
    drainGtkEvents();
  }

  TEST_CASE("PreferencesWindow - refreshing the keyboard page keeps a failed shortcut candidate",
            "[gtk][unit][preferences][shortcut]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto const failPersist = [](uimodel::KeymapModel const&) -> Result<>
    { return makeError(Error::Code::IoError, "disk full"); };
    auto window = PreferencesWindow{ao::test::englishMessageCatalog(), {}};
    window.refreshKeyboardPage(makeSchema(), uimodel::KeymapModel{uimodel::defaultKeymap()}, failPersist);
    drainGtkEvents();

    auto* const removeButton = findButtonByLabel(window, "✕");
    REQUIRE(removeButton != nullptr);
    emitClicked(*removeButton);
    drainGtkEvents();
    REQUIRE(findLabelByText(window, "Could not save shortcuts: disk full") != nullptr);

    window.refreshKeyboardPage(makeSchema(), uimodel::KeymapModel{uimodel::defaultKeymap()}, failPersist);
    drainGtkEvents();

    CHECK(findLabelByText(window, "Could not save shortcuts: disk full") != nullptr);
    CHECK(findLabelByText(window, "Ctrl+P") == nullptr);
  }

  TEST_CASE("PreferencesWindow - target hide discards a failed shortcut candidate without a new prompt",
            "[gtk][unit][preferences][shortcut]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto fixture = GtkRuntimeFixture{};
    rt::test::addReadyAudioProvider(fixture.runtime());

    auto target = Gtk::Window{};
    auto window = PreferencesWindow{ao::test::englishMessageCatalog(), {}};
    auto prefs = rt::AppPrefsState{};
    window.refreshPreferences(prefs, &fixture.runtime().playback(), &target);
    window.refreshKeyboardPage(makeSchema(),
                               uimodel::KeymapModel{uimodel::defaultKeymap()},
                               [](uimodel::KeymapModel const&) -> Result<>
                               { return makeError(Error::Code::IoError, "disk full"); });
    drainGtkEvents();

    auto* const removeButton = findButtonByLabel(window, "✕");
    REQUIRE(removeButton != nullptr);
    emitClicked(*removeButton);
    drainGtkEvents();
    REQUIRE(findLabelByText(window, "Could not save shortcuts: disk full") != nullptr);

    ::g_signal_emit_by_name(target.gobj(), "hide");
    drainGtkEvents();

    CHECK(findButtonByLabel(window, "Retry") == nullptr);
    CHECK(pendingShortcutClosePrompt() == nullptr);
  }

  TEST_CASE("PreferencesWindow - pending-shortcut close prompt applies the chosen response",
            "[gtk][unit][preferences][shortcut]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    bool persistShouldFail = true;
    std::int32_t persistCount = 0;
    auto window = PreferencesWindow{ao::test::englishMessageCatalog(), {}};
    window.refreshKeyboardPage(makeSchema(),
                               uimodel::KeymapModel{uimodel::defaultKeymap()},
                               [&](uimodel::KeymapModel const&) -> Result<>
                               {
                                 ++persistCount;

                                 if (persistShouldFail)
                                 {
                                   return makeError(Error::Code::IoError, "disk full");
                                 }

                                 return {};
                               });
    drainGtkEvents();

    auto* const removeButton = findButtonByLabel(window, "✕");
    REQUIRE(removeButton != nullptr);
    emitClicked(*removeButton);
    drainGtkEvents();
    REQUIRE(persistCount == 1);
    REQUIRE(findLabelByText(window, "Could not save shortcuts: disk full") != nullptr);

    gboolean handled = FALSE;
    ::g_signal_emit_by_name(window.gobj(), "close-request", &handled);
    drainGtkEvents();

    auto* const prompt = pendingShortcutClosePrompt();
    REQUIRE(prompt != nullptr);

    SECTION("Cancel keeps the candidate and the editor")
    {
      prompt->response(Gtk::ResponseType::CANCEL);
      drainGtkEvents();

      CHECK(persistCount == 1);
      CHECK(findLabelByText(window, "Could not save shortcuts: disk full") != nullptr);
      CHECK(findButtonByLabel(window, "✕") != nullptr);
      CHECK(pendingShortcutClosePrompt() == nullptr);
    }

    SECTION("Closing the prompt itself keeps the candidate rather than discarding it")
    {
      // Cancel is listed before Discard, so it is the prompt's close response: answering the
      // prompt from the window manager must never be the destructive choice.
      gboolean promptHandled = FALSE;
      ::g_signal_emit_by_name(prompt->gobj(), "close-request", &promptHandled);
      drainGtkEvents();

      CHECK(persistCount == 1);
      CHECK(findLabelByText(window, "Could not save shortcuts: disk full") != nullptr);
      CHECK(findButtonByLabel(window, "✕") != nullptr);
    }

    SECTION("Discard dismisses without another save attempt")
    {
      prompt->response(Gtk::ResponseType::REJECT);
      drainGtkEvents();

      CHECK(persistCount == 1);
      CHECK(findLabelByText(window, "Could not save shortcuts: disk full") == nullptr);
      CHECK(findButtonByLabel(window, "✕") == nullptr);
    }

    SECTION("Retry that fails again keeps the candidate and the editor")
    {
      prompt->response(Gtk::ResponseType::OK);
      drainGtkEvents();

      CHECK(persistCount == 2);
      CHECK(findLabelByText(window, "Could not save shortcuts: disk full") != nullptr);
      CHECK(findButtonByLabel(window, "✕") != nullptr);
    }

    SECTION("Retry that succeeds dismisses the window")
    {
      persistShouldFail = false;
      prompt->response(Gtk::ResponseType::OK);
      drainGtkEvents();

      CHECK(persistCount == 2);
      CHECK(findLabelByText(window, "Could not save shortcuts: disk full") == nullptr);
      CHECK(findButtonByLabel(window, "✕") == nullptr);
    }
  }

  TEST_CASE("PreferencesWindow - repeated close requests reuse one pending-shortcut prompt",
            "[gtk][unit][preferences][shortcut]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto window = PreferencesWindow{ao::test::englishMessageCatalog(), {}};
    window.refreshKeyboardPage(makeSchema(),
                               uimodel::KeymapModel{uimodel::defaultKeymap()},
                               [](uimodel::KeymapModel const&) -> Result<>
                               { return makeError(Error::Code::IoError, "disk full"); });
    drainGtkEvents();

    auto* const removeButton = findButtonByLabel(window, "✕");
    REQUIRE(removeButton != nullptr);
    emitClicked(*removeButton);
    drainGtkEvents();

    gboolean handled = FALSE;
    ::g_signal_emit_by_name(window.gobj(), "close-request", &handled);
    drainGtkEvents();
    auto* const firstPrompt = pendingShortcutClosePrompt();
    REQUIRE(firstPrompt != nullptr);

    ::g_signal_emit_by_name(window.gobj(), "close-request", &handled);
    drainGtkEvents();

    CHECK(pendingShortcutClosePromptCount() == 1);
    CHECK(pendingShortcutClosePrompt() == firstPrompt);

    firstPrompt->response(Gtk::ResponseType::CANCEL);
    drainGtkEvents();
    CHECK(pendingShortcutClosePrompt() == nullptr);
  }

  TEST_CASE("PreferencesWindow - dismissal retires a live pending-shortcut prompt",
            "[gtk][unit][preferences][shortcut]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto fixture = GtkRuntimeFixture{};
    rt::test::addReadyAudioProvider(fixture.runtime());

    auto target = Gtk::Window{};
    auto window = PreferencesWindow{ao::test::englishMessageCatalog(), {}};
    auto prefs = rt::AppPrefsState{};
    window.refreshPreferences(prefs, &fixture.runtime().playback(), &target);
    window.refreshKeyboardPage(makeSchema(),
                               uimodel::KeymapModel{uimodel::defaultKeymap()},
                               [](uimodel::KeymapModel const&) -> Result<>
                               { return makeError(Error::Code::IoError, "disk full"); });
    drainGtkEvents();

    auto* const removeButton = findButtonByLabel(window, "✕");
    REQUIRE(removeButton != nullptr);
    emitClicked(*removeButton);
    drainGtkEvents();

    gboolean handled = FALSE;
    ::g_signal_emit_by_name(window.gobj(), "close-request", &handled);
    drainGtkEvents();
    REQUIRE(pendingShortcutClosePrompt() != nullptr);

    // The target window disappearing dismisses Preferences; the prompt must not outlive it and
    // reach a later editing session.
    ::g_signal_emit_by_name(target.gobj(), "hide");
    drainGtkEvents();

    CHECK(pendingShortcutClosePrompt() == nullptr);
    CHECK(pendingShortcutClosePromptCount() == 0);

    std::int32_t freshPersistCount = 0;
    window.refreshKeyboardPage(makeSchema(),
                               uimodel::KeymapModel{uimodel::defaultKeymap()},
                               [&freshPersistCount](uimodel::KeymapModel const&) -> Result<>
                               {
                                 ++freshPersistCount;
                                 return {};
                               });
    drainGtkEvents();

    CHECK(findButtonByLabel(window, "✕") != nullptr);
  }
} // namespace ao::gtk::test
