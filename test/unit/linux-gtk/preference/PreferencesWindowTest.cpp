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
#include <ao/audio/BackendProvider.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/rt/AppState.h>
#include <ao/rt/playback/PlaybackEvents.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/preference/PreferencesEditorModel.h>
#include <ao/uimodel/preference/ThemePreset.h>

#include <catch2/catch_test_macros.hpp>
#include <glib-object.h>
#include <glib.h>
#include <gtkmm/dialog.h>
#include <gtkmm/listbox.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/stack.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
    uimodel::LayoutSchema makeSchema()
    {
      auto schema = uimodel::LayoutSchema{};
      schema.tryAddAction(
        {.id = "playback.playPause", .label = "Play/Pause", .category = "Playback", .capabilities = 0});
      return schema;
    }

    auto const kPendingCloseMessage =
      std::string{"The last shortcut change was not saved. Retry the save, discard the change, or keep editing."};

    /// The live pending-shortcut close prompt: a separate, showing toplevel carrying that message.
    /// A retired prompt may remain in the toplevel list hidden until its parent goes away.
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

    /// Finds the exact retained prompt wrapper after it is hidden. This is an arrange guard for
    /// stale-response coverage, not a requirement that every implementation retain hidden prompts.
    AppDialog* shortcutClosePromptIncludingHidden(Gtk::Window& parent, std::uintptr_t const identity)
    {
      for (auto* const topLevel : Gtk::Window::list_toplevels())
      {
        if (auto* const dialog = dynamic_cast<AppDialog*>(topLevel);
            dialog != nullptr && dialog->get_transient_for() == &parent &&
            reinterpret_cast<std::uintptr_t>(dialog) == identity &&
            findLabelByText(*dialog, kPendingCloseMessage) != nullptr)
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

    void activateOutputDeviceRow(PreferencesWindow& window, int const index)
    {
      auto* const selector = window.outputSelector();
      REQUIRE(selector != nullptr);
      emitShow(*selector);
      drainGtkEvents();

      auto* const listBox = outputSelectorListBox(window);
      REQUIRE(listBox != nullptr);
      auto* const row = listBox->get_row_at_index(index);
      REQUIRE(row != nullptr);
      emitRowActivated(*listBox, *row);
      // Activation may replace or retire the selector synchronously.
      drainGtkEvents();
    }

    audio::BackendProvider::Status makeAlsaOutputStatus()
    {
      auto status = rt::test::makePipeWireOutputStatus();
      status.descriptor.id = audio::BackendId{"alsa"};
      status.devices[0].backendId = audio::BackendId{"alsa"};
      return status;
    }
  } // namespace

  TEST_CASE("PreferencesWindow - builds first-cut pages and hosts shortcut editor", "[gtk][unit][preference]")
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

  TEST_CASE("PreferencesWindow - renders locale-selected page and action copy", "[gtk][unit][preference][localization]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto const textCatalog = ao::test::messageCatalog("de-DE");
    auto window = PreferencesWindow{textCatalog, {}};

    CHECK(window.get_title() == "Einstellungen");
    CHECK(findLabelByText(window, "Design") != nullptr);
    CHECK(findLabelByText(window, "Aktionen") != nullptr);
    CHECK(findButtonByLabel(window, "Layout bearbeiten...") != nullptr);
  }

  TEST_CASE("PreferencesWindow - layout page dispatches commands", "[gtk][unit][preference]")
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

  TEST_CASE("PreferencesWindow - appearance page persists and applies selected theme", "[gtk][unit][preference]")
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

  TEST_CASE("PreferencesWindow - layout page persists default preset for next launch", "[gtk][unit][preference]")
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

  TEST_CASE("PreferencesWindow - playback output selection persists the requested device", "[gtk][unit][preference]")
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

  TEST_CASE(
    "PreferencesWindow - repeated output requests persist exact selections while summary follows the active route",
    "[gtk][unit][preference]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    rt::test::addReadyAudioProvider(fixture.runtime());
    rt::test::addReadyAudioProvider(fixture.runtime(), rt::test::makePipeWireOutputStatus());
    drainGtkEvents();

    auto persisted = std::vector<rt::AppPrefsState>{};
    auto window =
      PreferencesWindow{ao::test::englishMessageCatalog(),
                        PreferencesWindow::Callbacks{
                          .onPersistPreferences = [&](rt::AppPrefsState const& prefs, uimodel::PreferencesChange)
                          { persisted.push_back(prefs); },
                        }};
    auto prefs = rt::AppPrefsState{};
    prefs.lastThemePreset = "modern";
    prefs.lastLayoutPreset = "classic";
    prefs.preferredOutputSelection.backendId = audio::BackendId{"previous-request"};
    window.refreshPreferences(prefs, &fixture.runtime().playback());
    CHECK(window.outputDeviceLabelText() == "test_backend");
    CHECK(persisted.empty());

    activateOutputDeviceRow(window, 3); // PipeWire shared, after the ready backend's header and row.
    REQUIRE(persisted.size() == 1);
    auto const pipewireSelection = audio::OutputDeviceSelection{
      .backendId = audio::BackendId{"pipewire"},
      .deviceId = audio::DeviceId{"device1"},
      .profileId = audio::kProfileShared,
    };
    CHECK(persisted[0].preferredOutputSelection == pipewireSelection);
    CHECK(persisted[0].lastThemePreset == "modern");
    CHECK(persisted[0].lastLayoutPreset == "classic");
    CHECK(window.outputDeviceLabelText() == "PW");

    activateOutputDeviceRow(window, 1); // Ready backend's shared device.
    REQUIRE(persisted.size() == 2);
    auto const readySelection = audio::OutputDeviceSelection{
      .backendId = audio::BackendId{"test_backend"},
      .deviceId = audio::DeviceId{"test_device"},
      .profileId = audio::kProfileShared,
    };
    CHECK(persisted[1].preferredOutputSelection == readySelection);
    CHECK(persisted[1].lastThemePreset == "modern");
    CHECK(persisted[1].lastLayoutPreset == "classic");
    CHECK(window.outputDeviceLabelText() == "test_backend");
  }

  TEST_CASE("PreferencesWindow - removing playback retires the selector and leaves output unavailable",
            "[gtk][unit][preference][async]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    rt::test::addReadyAudioProvider(fixture.runtime(), rt::test::makePipeWireOutputStatus());
    drainGtkEvents();

    bool selectorFinalized = false;
    auto window = PreferencesWindow{ao::test::englishMessageCatalog(), {}};
    window.refreshPreferences(rt::AppPrefsState{}, &fixture.runtime().playback());
    REQUIRE(window.hasOutputSelector());
    REQUIRE(window.outputDeviceLabelText() == "PW");
    ::g_object_weak_ref(
      G_OBJECT(window.outputSelector()->gobj()),
      +[](void* data, ::GObject*) { *static_cast<bool*>(data) = true; },
      &selectorFinalized);

    window.refreshPreferences(rt::AppPrefsState{}, nullptr);

    CHECK_FALSE(window.hasOutputSelector());
    CHECK(window.outputDeviceLabelText() == "Unavailable");
    CHECK_FALSE(selectorFinalized);
    drainGtkEvents();
    CHECK(selectorFinalized);

    rt::test::addReadyAudioProvider(fixture.runtime(), makeAlsaOutputStatus());
    fixture.runtime().playback().commands().setOutputDevice(
      audio::BackendId{"alsa"}, audio::DeviceId{"device1"}, audio::kProfileShared);
    drainGtkEvents();
    CHECK_FALSE(window.hasOutputSelector());
    CHECK(window.outputDeviceLabelText() == "Unavailable");
  }

  TEST_CASE("PreferencesWindow - consecutive rebinds finalize every retired selector on idle",
            "[gtk][unit][preference][async]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    rt::test::addReadyAudioProvider(fixture.runtime(), rt::test::makePipeWireOutputStatus());
    drainGtkEvents();

    auto retiredSelectorsFinalized = std::array<bool, 3>{};
    bool currentSelectorFinalized = false;
    auto window = PreferencesWindow{ao::test::englishMessageCatalog(), {}};
    window.refreshPreferences(rt::AppPrefsState{}, &fixture.runtime().playback());

    for (auto& finalized : retiredSelectorsFinalized)
    {
      REQUIRE(window.hasOutputSelector());
      ::g_object_weak_ref(
        G_OBJECT(window.outputSelector()->gobj()),
        +[](void* data, ::GObject*) { *static_cast<bool*>(data) = true; },
        &finalized);
      window.refreshPreferences(rt::AppPrefsState{}, &fixture.runtime().playback());
    }

    REQUIRE(window.hasOutputSelector());
    ::g_object_weak_ref(
      G_OBJECT(window.outputSelector()->gobj()),
      +[](void* data, ::GObject*) { *static_cast<bool*>(data) = true; },
      &currentSelectorFinalized);

    for (bool const finalized : retiredSelectorsFinalized)
    {
      CHECK_FALSE(finalized);
    }

    drainGtkEvents();

    for (bool const finalized : retiredSelectorsFinalized)
    {
      CHECK(finalized);
    }

    CHECK_FALSE(currentSelectorFinalized);
    CHECK(window.hasOutputSelector());
    CHECK(window.outputDeviceLabelText() == "PW");
  }

  TEST_CASE("PreferencesWindow - destruction with pending selector retirement finalizes every selector once",
            "[gtk][unit][preference][async]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    rt::test::addReadyAudioProvider(fixture.runtime(), rt::test::makePipeWireOutputStatus());
    drainGtkEvents();

    // Three retired selectors followed by the current one.
    auto selectorFinalizations = std::array<int, 4>{};

    {
      auto window = PreferencesWindow{ao::test::englishMessageCatalog(), {}};

      for (int& finalizations : selectorFinalizations)
      {
        window.refreshPreferences(rt::AppPrefsState{}, &fixture.runtime().playback());
        REQUIRE(window.hasOutputSelector());
        ::g_object_weak_ref(
          G_OBJECT(window.outputSelector()->gobj()),
          +[](void* data, ::GObject*) { ++*static_cast<int*>(data); },
          &finalizations);
      }

      for (int const finalizations : selectorFinalizations)
      {
        REQUIRE(finalizations == 0);
      }
    }

    // The window disconnects its retirement idle before releasing the selectors
    // it captures, so draining never re-enters the destroyed owner.
    drainGtkEvents();

    for (int const finalizations : selectorFinalizations)
    {
      CHECK(finalizations == 1);
    }
  }

  TEST_CASE("PreferencesWindow - rebinding during a selection's snapshot publication drops the stale request",
            "[gtk][unit][preference][async]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto oldFixture = GtkRuntimeFixture{};
    auto newFixture = GtkRuntimeFixture{};
    rt::test::addReadyAudioProvider(oldFixture.runtime(), rt::test::makePipeWireOutputStatus());
    rt::test::addReadyAudioProvider(newFixture.runtime(), makeAlsaOutputStatus());
    drainGtkEvents();

    auto persisted = std::vector<rt::AppPrefsState>{};
    auto window =
      PreferencesWindow{ao::test::englishMessageCatalog(),
                        PreferencesWindow::Callbacks{
                          .onPersistPreferences = [&](rt::AppPrefsState const& prefs, uimodel::PreferencesChange)
                          { persisted.push_back(prefs); },
                        }};
    window.refreshPreferences(rt::AppPrefsState{}, &oldFixture.runtime().playback());
    REQUIRE(window.outputDeviceLabelText() == "PW");

    bool rebound = false;
    auto const rebindSub = oldFixture.runtime().playback().events().onSnapshot(
      [&](rt::PlaybackSnapshot const& snapshot)
      {
        if (!rebound && snapshot.transport.output.selectedDevice.profileId == audio::kProfileExclusive)
        {
          rebound = true;
          window.refreshPreferences(rt::AppPrefsState{}, &newFixture.runtime().playback());
        }
      });

    // The command publishes its snapshot before the old selector reaches its recorder.
    activateOutputDeviceRow(window, 2); // PipeWire exclusive.
    REQUIRE(rebound);
    CHECK(persisted.empty());
    CHECK(window.outputDeviceLabelText() == "ALSA");

    activateOutputDeviceRow(window, 1); // ALSA shared on the replacement binding.
    REQUIRE(persisted.size() == 1);
    CHECK(persisted[0].preferredOutputSelection == audio::OutputDeviceSelection{
                                                     .backendId = audio::BackendId{"alsa"},
                                                     .deviceId = audio::DeviceId{"device1"},
                                                     .profileId = audio::kProfileShared,
                                                   });
    CHECK(window.outputDeviceLabelText() == "ALSA");
  }

  TEST_CASE("PreferencesWindow - target hide and ordinary close retire an open output selector",
            "[gtk][unit][preference][async]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    rt::test::addReadyAudioProvider(fixture.runtime(), rt::test::makePipeWireOutputStatus());
    drainGtkEvents();

    auto target = Gtk::Window{};
    bool selectorFinalized = false;
    auto window = PreferencesWindow{ao::test::englishMessageCatalog(), {}};
    window.refreshPreferences(rt::AppPrefsState{}, &fixture.runtime().playback(), &target);
    REQUIRE(window.hasOutputSelector());
    REQUIRE(window.outputDeviceLabelText() == "PW");
    auto* const stack = findWidget<Gtk::Stack>(window);
    REQUIRE(stack != nullptr);
    stack->set_visible_child("playback");
    target.present();
    window.set_transient_for(target);
    window.present();
    REQUIRE(tryPumpGtkEventsUntil([&window] { return window.get_mapped(); }));
    auto* const selector = window.outputSelector();
    selector->popup();
    REQUIRE(tryPumpGtkEventsUntil([selector] { return selector->get_mapped(); }));
    ::g_object_weak_ref(
      G_OBJECT(selector->gobj()),
      +[](void* data, ::GObject*) { *static_cast<bool*>(data) = true; },
      &selectorFinalized);

    SECTION("target hide")
    {
      target.set_visible(false);
    }

    SECTION("ordinary close")
    {
      window.close();
    }

    CHECK_FALSE(window.get_visible());
    CHECK_FALSE(window.hasOutputSelector());
    CHECK(window.outputDeviceLabelText() == "Unavailable");
    CHECK_FALSE(selectorFinalized);
    drainGtkEvents();
    CHECK(selectorFinalized);

    rt::test::addReadyAudioProvider(fixture.runtime(), makeAlsaOutputStatus());
    fixture.runtime().playback().commands().setOutputDevice(
      audio::BackendId{"alsa"}, audio::DeviceId{"device1"}, audio::kProfileShared);
    drainGtkEvents();
    CHECK_FALSE(window.hasOutputSelector());
    CHECK(window.outputDeviceLabelText() == "Unavailable");
  }

  TEST_CASE("PreferencesWindow - output summary follows a replacement playback service, not retired events",
            "[gtk][unit][preference]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto oldFixture = GtkRuntimeFixture{};
    auto newFixture = GtkRuntimeFixture{};
    rt::test::addReadyAudioProvider(newFixture.runtime(), rt::test::makePipeWireOutputStatus());
    drainGtkEvents();

    auto window = PreferencesWindow{ao::test::englishMessageCatalog(), {}};
    window.refreshPreferences(rt::AppPrefsState{}, &oldFixture.runtime().playback());
    REQUIRE(window.outputDeviceLabelText() == "--");

    window.refreshPreferences(rt::AppPrefsState{}, &newFixture.runtime().playback());
    REQUIRE(window.hasOutputSelector());
    REQUIRE(window.outputDeviceLabelText() == "PW");

    rt::test::addReadyAudioProvider(newFixture.runtime(), makeAlsaOutputStatus());
    newFixture.runtime().playback().commands().setOutputDevice(
      audio::BackendId{"alsa"}, audio::DeviceId{"device1"}, audio::kProfileShared);
    drainGtkEvents();
    REQUIRE(window.outputDeviceLabelText() == "ALSA");

    rt::test::addReadyAudioProvider(oldFixture.runtime());
    drainGtkEvents();
    CHECK(window.outputDeviceLabelText() == "ALSA");
    CHECK(window.hasOutputSelector());
  }

  TEST_CASE("PreferencesWindow - unknown persisted ids fall back to visible defaults", "[gtk][unit][preference]")
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

  TEST_CASE("PreferencesWindow - ordinary close keeps a failed shortcut candidate", "[gtk][unit][preference][shortcut]")
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
            "[gtk][unit][preference][shortcut]")
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
            "[gtk][unit][preference][shortcut]")
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
            "[gtk][unit][preference][shortcut]")
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
            "[gtk][unit][preference][shortcut]")
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
            "[gtk][unit][preference][shortcut][async]")
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
    auto* const retiredPrompt = pendingShortcutClosePrompt();
    REQUIRE(retiredPrompt != nullptr);
    auto const retiredPromptIdentity = reinterpret_cast<std::uintptr_t>(retiredPrompt);

    // This implementation retains the hidden managed prompt after target dismissal. The exact
    // retained wrapper is required below solely to make stale response delivery deterministic.
    ::g_signal_emit_by_name(target.gobj(), "hide");
    drainGtkEvents();

    CHECK(pendingShortcutClosePrompt() == nullptr);
    CHECK(pendingShortcutClosePromptCount() == 0);
    REQUIRE(shortcutClosePromptIncludingHidden(window, retiredPromptIdentity) != nullptr);

    SECTION("retained response leaves a fresh clean session unchanged")
    {
      std::int32_t freshPersistCount = 0;
      window.refreshKeyboardPage(makeSchema(),
                                 uimodel::KeymapModel{uimodel::defaultKeymap()},
                                 [&freshPersistCount](uimodel::KeymapModel const&) -> Result<>
                                 {
                                   ++freshPersistCount;
                                   return {};
                                 });
      window.set_visible(true);
      drainGtkEvents();

      REQUIRE(window.get_visible());
      REQUIRE(findLabelByText(window, "Ctrl+P") != nullptr);
      REQUIRE(findButtonByLabel(window, "✕") != nullptr);
      REQUIRE(findLabelByText(window, "Could not save shortcuts: disk full") == nullptr);
      auto* const liveRetiredPrompt = shortcutClosePromptIncludingHidden(window, retiredPromptIdentity);
      REQUIRE(liveRetiredPrompt != nullptr);

      liveRetiredPrompt->response(Gtk::ResponseType::REJECT);
      drainGtkEvents();

      CHECK(freshPersistCount == 0);
      CHECK(window.get_visible());
      CHECK(findLabelByText(window, "Ctrl+P") != nullptr);
      CHECK(findButtonByLabel(window, "✕") != nullptr);
      CHECK(findLabelByText(window, "Could not save shortcuts: disk full") == nullptr);
      CHECK(pendingShortcutClosePrompt() == nullptr);
    }

    SECTION("retained response cannot steer a fresh failed draft or its prompt")
    {
      std::int32_t freshPersistCount = 0;
      window.refreshKeyboardPage(makeSchema(),
                                 uimodel::KeymapModel{uimodel::defaultKeymap()},
                                 [&freshPersistCount](uimodel::KeymapModel const&) -> Result<>
                                 {
                                   ++freshPersistCount;
                                   return makeError(Error::Code::IoError, "fresh disk full");
                                 });
      window.set_visible(true);
      drainGtkEvents();

      auto* const freshRemoveButton = findButtonByLabel(window, "✕");
      REQUIRE(freshRemoveButton != nullptr);
      emitClicked(*freshRemoveButton);
      drainGtkEvents();

      REQUIRE(freshPersistCount == 1);
      REQUIRE(window.get_visible());
      REQUIRE(findLabelByText(window, "Ctrl+P") == nullptr);
      REQUIRE(findLabelByText(window, "Media:Play") != nullptr);
      REQUIRE(findLabelByText(window, "Could not save shortcuts: fresh disk full") != nullptr);
      REQUIRE(findButtonByLabel(window, "Retry") != nullptr);
      REQUIRE(findButtonByLabel(window, "Discard") != nullptr);

      ::g_signal_emit_by_name(window.gobj(), "close-request", &handled);
      drainGtkEvents();
      auto* const freshPrompt = pendingShortcutClosePrompt();
      REQUIRE(freshPrompt != nullptr);
      REQUIRE(reinterpret_cast<std::uintptr_t>(freshPrompt) != retiredPromptIdentity);
      auto* const liveRetiredPrompt = shortcutClosePromptIncludingHidden(window, retiredPromptIdentity);
      REQUIRE(liveRetiredPrompt != nullptr);
      REQUIRE(liveRetiredPrompt != freshPrompt);

      liveRetiredPrompt->response(Gtk::ResponseType::REJECT);
      drainGtkEvents();

      CHECK(freshPersistCount == 1);
      CHECK(window.get_visible());
      CHECK(findLabelByText(window, "Ctrl+P") == nullptr);
      CHECK(findLabelByText(window, "Media:Play") != nullptr);
      CHECK(findLabelByText(window, "Could not save shortcuts: fresh disk full") != nullptr);
      CHECK(findButtonByLabel(window, "Retry") != nullptr);
      CHECK(findButtonByLabel(window, "Discard") != nullptr);
      auto* const remainingPrompt = pendingShortcutClosePrompt();
      REQUIRE(remainingPrompt != nullptr);
      CHECK(remainingPrompt == freshPrompt);
      CHECK(remainingPrompt->get_visible());
    }
  }
} // namespace ao::gtk::test
