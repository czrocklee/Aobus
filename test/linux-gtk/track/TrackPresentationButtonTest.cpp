// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackPresentationButton.h"

#include "app/AppDialog.h"
#include "app/ThemeCoordinator.h"
#include "test/MessageCatalogTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/linux-gtk/GtkApplicationTestSupport.h"
#include "test/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/linux-gtk/GtkWidgetTestSupport.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>
#include <ao/uimodel/preference/ThemePreset.h>

#include <catch2/catch_test_macros.hpp>
#include <glibmm/main.h>
#include <glibmm/refptr.h>
#include <gtkmm/dialog.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/popover.h>
#include <gtkmm/window.h>
#include <sigc++/scoped_connection.h>

#include <memory>

namespace ao::gtk::test
{
  namespace
  {
    AppDialog* findCustomViewEditor()
    {
      return findAppDialogByTitle("Edit Custom View");
    }

    void clickSave(AppDialog& editor)
    {
      auto* const saveButton = findButtonByLabel(editor, "Save");
      REQUIRE(saveButton != nullptr);
      emitClicked(*saveButton);
    }

    void sendOkIfEditorStillLive()
    {
      // A response, hide, or rebind may already have destroyed the editor.
      // Only exercise late delivery if GTK still lists a live toplevel.
      if (auto* const editor = findCustomViewEditor(); editor != nullptr)
      {
        editor->response(Gtk::ResponseType::OK);
      }
    }

    struct CustomViewPickerFixture final
    {
      Glib::RefPtr<Gtk::Application> appPtr = ensureGtkApplication();
      GtkRuntimeFixture runtimeFixture;
      rt::AppRuntime& runtime = runtimeFixture.runtime();
      ThemeCoordinator themeCoordinator;
      uimodel::TrackPresentationCatalog catalog{runtime.workspace(), ao::test::englishMessageCatalog()};
      uimodel::ListPresentations listPresentations{catalog, runtime.library().changes()};
      Gtk::Window window;
      std::unique_ptr<TrackPresentationButton> pickerPtr =
        std::make_unique<TrackPresentationButton>(runtime.views(),
                                                  runtime.workspace(),
                                                  ao::test::englishMessageCatalog());
      rt::ViewId const activeViewId =
        ao::test::requireValue(runtime.workspace().navigate({.target = rt::kAllTracksListId}));

      CustomViewPickerFixture()
      {
        pickerPtr->setPresentationServices(&catalog, &listPresentations, &themeCoordinator);
        window.set_child(*pickerPtr);
        window.present();
        drainGtkEvents();
      }

      Gtk::Popover& openMenu() const
      {
        auto* const menuButton = findWidget<Gtk::MenuButton>(*pickerPtr);
        REQUIRE(menuButton != nullptr);
        auto* const popover = menuButton->get_popover();
        REQUIRE(popover != nullptr);
        menuButton->popup();
        drainGtkEvents();
        REQUIRE(popover->get_visible());
        return *popover;
      }

      void hidePicker() const
      {
        REQUIRE(pickerPtr->get_mapped());
        pickerPtr->hide();
        REQUIRE_FALSE(pickerPtr->get_mapped());
        REQUIRE(pickerPtr->get_root() == &window);
        REQUIRE(window.get_mapped());
      }

      AppDialog& openEditor() { return openEditor(openMenu()); }

      AppDialog& openEditor(Gtk::Popover& popover)
      {
        auto* const createButton = findButtonByLabel(popover, "Create Custom View...");
        REQUIRE(createButton != nullptr);

        bool enteredNestedLoop = false;
        // Escape the former modal loop if the click starts dispatching idles.
        auto nestedLoopGuard = sigc::scoped_connection{Glib::signal_idle().connect(
          [&]
          {
            enteredNestedLoop = true;

            if (auto* const editor = findCustomViewEditor(); editor != nullptr)
            {
              editor->response(Gtk::ResponseType::CANCEL);
            }

            return false;
          })};
        emitClicked(*createButton);
        nestedLoopGuard.disconnect();
        REQUIRE_FALSE(enteredNestedLoop);

        auto* const editor = findCustomViewEditor();
        REQUIRE(editor != nullptr);
        REQUIRE(editor->get_transient_for() == &window);
        REQUIRE(editor->get_visible());
        return *editor;
      }
    };
  } // namespace

  // Catalog tests own menu contents; the widget owns binding and apply-before-persist.
  TEST_CASE("TrackPresentationButton - binds the active title and applies a menu selection")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto themeCoordinator = ThemeCoordinator{};
    auto catalog = uimodel::TrackPresentationCatalog{runtime.workspace(), ao::test::englishMessageCatalog()};
    auto listPresentations = uimodel::ListPresentations{catalog, runtime.library().changes()};

    auto window = Gtk::Window{};
    auto button = TrackPresentationButton{runtime.views(), runtime.workspace(), ao::test::englishMessageCatalog()};
    button.setPresentationServices(&catalog, &listPresentations, &themeCoordinator);
    window.set_child(button);
    auto* const menuButton = findWidget<Gtk::MenuButton>(button);
    REQUIRE(menuButton != nullptr);
    CHECK_FALSE(menuButton->get_sensitive());
    CHECK(menuButton->get_label() == "Presentation");

    REQUIRE(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
    drainGtkEvents();

    CHECK(menuButton->get_sensitive());
    CHECK(menuButton->get_label() == "Library");
    CHECK(button.get_valign() == Gtk::Align::CENTER);
    CHECK(menuButton->get_valign() == Gtk::Align::CENTER);
    CHECK(hasCssClass(*menuButton, "ao-presentation-trigger"));

    auto* const popover = menuButton->get_popover();
    REQUIRE(popover != nullptr);
    window.present();
    drainGtkEvents();
    menuButton->popup();
    drainGtkEvents();
    REQUIRE(popover->get_visible());

    auto* const albumsButton = findButtonByLabel(*popover, "Albums");
    REQUIRE(albumsButton != nullptr);
    CHECK(hasCssClass(*albumsButton, "ao-presentation-menu-item"));
    CHECK_FALSE(hasCssClass(*albumsButton, "ao-presentation-trigger"));

    // The adapter owns the apply-then-persist order: the runtime must accept the
    // change before the list preference records it.
    emitClicked(*albumsButton);
    drainGtkEvents();
    CHECK_FALSE(popover->get_visible());
    CHECK(menuButton->get_label() == "Albums");

    auto const activeViewId = runtime.workspace().snapshot().activeViewId;
    REQUIRE(activeViewId != rt::kInvalidViewId);
    CHECK(runtime.views().trackListState(activeViewId).presentation.isPreset(rt::BuiltinPreset::Albums));

    auto const optStored = listPresentations.presentationIdForList(rt::kAllTracksListId);
    REQUIRE(optStored);
    CHECK(*optStored == rt::TrackPresetId{rt::BuiltinPreset::Albums});

    auto const sort = rt::TrackColumnSort{.column = rt::TrackField::Title, .ascending = false};
    REQUIRE(runtime.views().setColumnSort(activeViewId, sort));
    drainGtkEvents();
    CHECK(menuButton->get_label() == "Albums");
    CHECK(listPresentations.presentationIdForList(rt::kAllTracksListId) == optStored);

    menuButton->popup();
    drainGtkEvents();
    auto* const resetButton = findButtonByLabel(*popover, "Albums");
    REQUIRE(resetButton != nullptr);
    REQUIRE(resetButton->get_sensitive());
    emitClicked(*resetButton);
    drainGtkEvents();
    CHECK(runtime.views().trackListState(activeViewId).presentation ==
          rt::builtinTrackPresentationPreset(rt::BuiltinPreset::Albums).spec);
    CHECK(listPresentations.presentationIdForList(rt::kAllTracksListId) == optStored);
    window.close();
    drainGtkEvents();
  }

  TEST_CASE("TrackPresentationButton - saves one custom draft and defers applying it")
  {
    auto fixture = CustomViewPickerFixture{};
    auto& editor = fixture.openEditor();
    auto* const nameEntry = findWidget<Gtk::Entry>(editor);
    REQUIRE(nameEntry != nullptr);
    nameEntry->set_text("Release Review");

    clickSave(editor);
    REQUIRE(findCustomViewEditor() == &editor);
    nameEntry->set_text("Late Edit");
    sendOkIfEditorStillLive();

    CHECK(fixture.catalog.customPresentations().empty());
    CHECK(
      fixture.runtime.views().trackListState(fixture.activeViewId).presentation.isPreset(rt::BuiltinPreset::Library));
    CHECK_FALSE(fixture.listPresentations.presentationIdForList(rt::kAllTracksListId).has_value());
    drainGtkEvents();

    CHECK(findCustomViewEditor() == nullptr);
    auto const presets = fixture.catalog.customPresentations();
    REQUIRE(presets.size() == 1);
    CHECK(presets.front().label == "Release Review");
    auto const presetId = presets.front().presetId();
    CHECK(fixture.runtime.views().trackListState(fixture.activeViewId).presentation.isPreset(presetId));
    CHECK(fixture.listPresentations.presentationIdForList(rt::kAllTracksListId) == rt::TrackPresetId{presetId});
  }

  TEST_CASE("TrackPresentationButton - preserves a custom draft when only the picker is hidden")
  {
    auto fixture = CustomViewPickerFixture{};
    auto& editor = fixture.openEditor();
    auto* const nameEntry = findWidget<Gtk::Entry>(editor);
    REQUIRE(nameEntry != nullptr);
    nameEntry->set_text("Visibility Draft");

    SECTION("the editor and typed draft survive hiding and showing the picker")
    {
      fixture.hidePicker();
      drainGtkEvents();
      REQUIRE(findCustomViewEditor() == &editor);
      CHECK(editor.get_mapped());

      fixture.pickerPtr->show();
      REQUIRE(tryPumpGtkEventsUntil([&] { return fixture.pickerPtr->get_mapped(); }));
      REQUIRE(findCustomViewEditor() == &editor);
      CHECK(nameEntry->get_text() == "Visibility Draft");
      clickSave(editor);
    }

    SECTION("a queued Save can commit while the picker is still hidden")
    {
      clickSave(editor);
      fixture.hidePicker();
    }

    CHECK(fixture.catalog.customPresentations().empty());
    drainGtkEvents();

    CHECK(findCustomViewEditor() == nullptr);
    auto const presets = fixture.catalog.customPresentations();
    REQUIRE(presets.size() == 1);
    CHECK(presets.front().label == "Visibility Draft");
    auto const presetId = presets.front().presetId();
    CHECK(fixture.runtime.views().trackListState(fixture.activeViewId).presentation.isPreset(presetId));
    CHECK(fixture.listPresentations.presentationIdForList(rt::kAllTracksListId) == rt::TrackPresetId{presetId});
  }

  TEST_CASE("TrackPresentationButton - seeds a custom draft from the live presentation before the label refreshes")
  {
    auto fixture = CustomViewPickerFixture{};
    auto& popover = fixture.openMenu();
    auto* const menuButton = findWidget<Gtk::MenuButton>(*fixture.pickerPtr);
    REQUIRE(menuButton != nullptr);
    auto const renderedSpec = fixture.runtime.views().trackListState(fixture.activeViewId).presentation;
    auto const& albums = rt::builtinTrackPresentationPreset(rt::BuiltinPreset::Albums).spec;

    SECTION("named preset with live values that differ from the catalog")
    {
      auto albumsSpec = albums;
      REQUIRE_FALSE(albumsSpec.sortBy.empty());
      albumsSpec.sortBy.front().ascending = !albumsSpec.sortBy.front().ascending;
      REQUIRE(fixture.runtime.workspace().setActivePresentation(albumsSpec));
    }

    SECTION("a column override contributes to the copied order without renaming the source preset")
    {
      REQUIRE(fixture.runtime.workspace().setActivePresentation(albums));
      auto const sort = rt::TrackColumnSort{.column = rt::TrackField::Title, .ascending = false};
      REQUIRE(fixture.runtime.views().setColumnSort(fixture.activeViewId, sort));
    }

    auto const liveSpec = fixture.runtime.views().trackListState(fixture.activeViewId).presentation;
    REQUIRE(liveSpec.effectiveSortBy() != renderedSpec.effectiveSortBy());
    REQUIRE(fixture.runtime.workspace().snapshot().activeViewId == fixture.activeViewId);
    REQUIRE(menuButton->get_label() == "Library");

    // Click the already-open menu before dispatching the presentation notification.
    auto& editor = fixture.openEditor(popover);
    clickSave(editor);
    drainGtkEvents();

    auto const presets = fixture.catalog.customPresentations();
    REQUIRE(presets.size() == 1);
    CHECK(presets.front().label == "Albums Copy");
    CHECK(presets.front().optBasePresetId == liveSpec.presetId);
    CHECK(presets.front().spec.groupBy == liveSpec.groupBy);
    CHECK(presets.front().spec.sortBy == liveSpec.effectiveSortBy());
    CHECK_FALSE(presets.front().spec.optColumnSort);
    CHECK(presets.front().spec.visibleFields == liveSpec.visibleFields);
  }

  TEST_CASE("TrackPresentationButton - revokes a queued custom Save when the editor session ends")
  {
    auto fixture = CustomViewPickerFixture{};
    auto const initialSpec = fixture.runtime.views().trackListState(fixture.activeViewId).presentation;
    clickSave(fixture.openEditor());
    auto* const pendingEditor = findCustomViewEditor();
    REQUIRE(pendingEditor != nullptr);

    SECTION("Cancel action")
    {
      auto* const cancelButton = findButtonByLabel(*pendingEditor, "Cancel");
      REQUIRE(cancelButton != nullptr);
      emitClicked(*cancelButton);
    }

    SECTION("native close request")
    {
      pendingEditor->close();
    }

    SECTION("picker removal followed by reattachment to the same window before the idle")
    {
      auto& picker = *fixture.pickerPtr;
      REQUIRE(picker.get_mapped());
      fixture.window.unset_child();
      REQUIRE(picker.get_root() == nullptr);
      fixture.window.set_child(picker);
      REQUIRE(picker.get_root() == &fixture.window);
    }

    SECTION("parent window hide even if the picker is already hidden")
    {
      fixture.hidePicker();
      fixture.window.hide();
    }

    sendOkIfEditorStillLive();
    drainGtkEvents();

    CHECK(findCustomViewEditor() == nullptr);
    CHECK(fixture.catalog.customPresentations().empty());
    CHECK(fixture.runtime.views().trackListState(fixture.activeViewId).presentation == initialSpec);
    CHECK_FALSE(fixture.listPresentations.presentationIdForList(rt::kAllTracksListId).has_value());
  }

  TEST_CASE("TrackPresentationButton - abandons the custom draft when the editor session ends")
  {
    auto fixture = CustomViewPickerFixture{};
    auto& editor = fixture.openEditor();
    auto* const nameEntry = findWidget<Gtk::Entry>(editor);
    REQUIRE(nameEntry != nullptr);
    nameEntry->set_text("Unsubmitted Review");

    SECTION("Cancel action")
    {
      auto* const cancelButton = findButtonByLabel(editor, "Cancel");
      REQUIRE(cancelButton != nullptr);
      emitClicked(*cancelButton);
    }

    SECTION("native close request")
    {
      editor.close();
    }

    SECTION("editor hide revokes a late OK")
    {
      editor.hide();
      sendOkIfEditorStillLive();
    }

    SECTION("picker destruction")
    {
      fixture.window.unset_child();
      fixture.pickerPtr.reset();
    }

    drainGtkEvents();
    CHECK(findCustomViewEditor() == nullptr);
    CHECK(fixture.catalog.customPresentations().empty());
    CHECK(
      fixture.runtime.views().trackListState(fixture.activeViewId).presentation.isPreset(rt::BuiltinPreset::Library));
  }

  TEST_CASE("TrackPresentationButton - rebinding services revokes the custom editor and queued Save")
  {
    // Use independent workspace storage so a draft crossing the service
    // boundary is observable in the replacement catalog as well as the old one.
    auto replacementRuntimeFixture = GtkRuntimeFixture{};
    auto& replacementRuntime = replacementRuntimeFixture.runtime();
    auto replacementCatalog =
      uimodel::TrackPresentationCatalog{replacementRuntime.workspace(), ao::test::englishMessageCatalog()};
    auto replacementPreferences =
      uimodel::ListPresentations{replacementCatalog, replacementRuntime.library().changes()};
    auto fixture = CustomViewPickerFixture{};
    auto& editor = fixture.openEditor();
    auto* const nameEntry = findWidget<Gtk::Entry>(editor);
    REQUIRE(nameEntry != nullptr);
    nameEntry->set_text("Outgoing Draft");

    auto const rebind = [&]
    {
      fixture.pickerPtr->setPresentationServices(
        &replacementCatalog, &replacementPreferences, &fixture.themeCoordinator);
    };

    SECTION("while the editor awaits a response")
    {
      rebind();
      sendOkIfEditorStillLive();
    }

    SECTION("after Save is clicked and before the idle runs")
    {
      clickSave(editor);
      rebind();
    }

    drainGtkEvents();
    CHECK(findCustomViewEditor() == nullptr);
    CHECK(fixture.catalog.customPresentations().empty());
    CHECK(replacementCatalog.customPresentations().empty());
  }

  TEST_CASE("TrackPresentationButton - drops a queued custom Save when focus changes")
  {
    auto fixture = CustomViewPickerFixture{};
    auto& runtime = fixture.runtime;
    auto const secondListId = ao::test::requireValue(
      runGtkTask(runtime, runtime.library().commands().createListAsync(rt::ListDraft{.name = "Other"})));
    auto& editor = fixture.openEditor();
    clickSave(editor);

    // Move focus after Save queues the draft but before the idle can commit it.
    auto const secondViewId = ao::test::requireValue(runtime.workspace().navigate({.target = secondListId}));
    REQUIRE(secondViewId != fixture.activeViewId);
    auto const secondPresentation = runtime.views().trackListState(secondViewId).presentation;
    drainGtkEvents();

    CHECK(findCustomViewEditor() == nullptr);
    CHECK(fixture.catalog.customPresentations().empty());
    CHECK(runtime.views().trackListState(fixture.activeViewId).presentation.isPreset(rt::BuiltinPreset::Library));
    CHECK(runtime.views().trackListState(secondViewId).presentation == secondPresentation);
    CHECK_FALSE(fixture.listPresentations.presentationIdForList(rt::kAllTracksListId).has_value());
    CHECK_FALSE(fixture.listPresentations.presentationIdForList(secondListId).has_value());
  }

  TEST_CASE("TrackPresentationButton - rebinding services drops the pending apply")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto themeCoordinator = ThemeCoordinator{};
    auto catalog = uimodel::TrackPresentationCatalog{runtime.workspace(), ao::test::englishMessageCatalog()};
    auto listPresentations = uimodel::ListPresentations{catalog, runtime.library().changes()};
    auto replacementPreferences = uimodel::ListPresentations{catalog, runtime.library().changes()};
    auto window = Gtk::Window{};

    auto const activeViewId = ao::test::requireValue(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
    drainGtkEvents();

    auto button = TrackPresentationButton{runtime.views(), runtime.workspace(), ao::test::englishMessageCatalog()};
    button.setPresentationServices(&catalog, &listPresentations, &themeCoordinator);
    window.set_child(button);
    drainGtkEvents();

    auto* const menuButton = findWidget<Gtk::MenuButton>(button);
    REQUIRE(menuButton != nullptr);
    auto* const popover = menuButton->get_popover();
    REQUIRE(popover != nullptr);
    auto* const albumsButton = findButtonByLabel(*popover, "Albums");
    REQUIRE(albumsButton != nullptr);

    // The queued apply belongs to the outgoing session. Rebinding must cancel
    // it, or it lands in the runtime with the incoming store recording it.
    emitClicked(*albumsButton);
    button.setPresentationServices(&catalog, &replacementPreferences, &themeCoordinator);
    drainGtkEvents();

    CHECK(runtime.views().trackListState(activeViewId).presentation.isPreset(rt::BuiltinPreset::Library));
    CHECK_FALSE(listPresentations.presentationIdForList(rt::kAllTracksListId).has_value());
    CHECK_FALSE(replacementPreferences.presentationIdForList(rt::kAllTracksListId).has_value());
  }

  TEST_CASE("TrackPresentationButton - cancels pending presentation apply when destroyed")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto themeCoordinator = ThemeCoordinator{};
    auto catalog = uimodel::TrackPresentationCatalog{runtime.workspace(), ao::test::englishMessageCatalog()};
    auto listPresentations = uimodel::ListPresentations{catalog, runtime.library().changes()};
    auto window = Gtk::Window{};

    REQUIRE(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
    drainGtkEvents();
    auto const activeViewId = runtime.workspace().snapshot().activeViewId;
    REQUIRE(activeViewId != rt::kInvalidViewId);
    REQUIRE(runtime.views().trackListState(activeViewId).presentation.isPreset(rt::BuiltinPreset::Library));

    auto buttonPtr = std::make_unique<TrackPresentationButton>(
      runtime.views(), runtime.workspace(), ao::test::englishMessageCatalog());
    buttonPtr->setPresentationServices(&catalog, &listPresentations, &themeCoordinator);
    window.set_child(*buttonPtr);
    drainGtkEvents();

    auto* const menuButton = findWidget<Gtk::MenuButton>(*buttonPtr);
    REQUIRE(menuButton != nullptr);
    auto* const popover = menuButton->get_popover();
    REQUIRE(popover != nullptr);
    auto* const albumsButton = findButtonByLabel(*popover, "Albums");
    REQUIRE(albumsButton != nullptr);

    emitClicked(*albumsButton);
    window.unset_child();
    buttonPtr.reset();
    drainGtkEvents();

    CHECK(runtime.views().trackListState(activeViewId).presentation.isPreset(rt::BuiltinPreset::Library));
  }

  TEST_CASE("TrackPresentationButton - refused deferred applies do not persist a preference")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto themeCoordinator = ThemeCoordinator{};
    themeCoordinator.setTheme(uimodel::ThemePreset::Modern);
    auto catalog = uimodel::TrackPresentationCatalog{runtime.workspace(), ao::test::englishMessageCatalog()};
    auto listPresentations = uimodel::ListPresentations{catalog, runtime.library().changes()};
    auto window = Gtk::Window{};

    auto const firstViewId = ao::test::requireValue(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
    drainGtkEvents();
    auto button = TrackPresentationButton{runtime.views(), runtime.workspace(), ao::test::englishMessageCatalog()};
    button.setPresentationServices(&catalog, &listPresentations, &themeCoordinator);
    window.set_child(button);
    window.present();
    drainGtkEvents();

    auto* const menuButton = findWidget<Gtk::MenuButton>(button);
    REQUIRE(menuButton != nullptr);
    auto* const popover = menuButton->get_popover();
    REQUIRE(popover != nullptr);
    auto* const albumsButton = findButtonByLabel(*popover, "Albums");
    REQUIRE(albumsButton != nullptr);

    auto const secondListId = ao::test::requireValue(
      runGtkTask(runtime, runtime.library().commands().createListAsync(rt::ListDraft{.name = "Other"})));

    emitClicked(*albumsButton);

    auto const secondViewId = ao::test::requireValue(runtime.workspace().navigate({.target = secondListId}));
    auto const secondPresentation = runtime.views().trackListState(secondViewId).presentation;
    drainGtkEvents();

    CHECK(runtime.views().trackListState(firstViewId).presentation.isPreset(rt::BuiltinPreset::Library));
    CHECK(runtime.views().trackListState(secondViewId).presentation == secondPresentation);
    CHECK_FALSE(listPresentations.presentationIdForList(secondListId).has_value());

    CHECK_FALSE(listPresentations.presentationIdForList(rt::kAllTracksListId).has_value());

    auto* const errorDialog = findAppDialogByTitle("Unable to Change Track View");
    REQUIRE(errorDialog != nullptr);
    CHECK(errorDialog->get_transient_for() == &window);
    CHECK(errorDialog->has_css_class("ao-theme-modern"));
    themeCoordinator.setTheme(uimodel::ThemePreset::Classic);
    CHECK_FALSE(errorDialog->has_css_class("ao-theme-modern"));
    CHECK(errorDialog->has_css_class("ao-theme-classic"));
    errorDialog->response(Gtk::ResponseType::CLOSE);
    window.close();
    drainGtkEvents();
  }
} // namespace ao::gtk::test
