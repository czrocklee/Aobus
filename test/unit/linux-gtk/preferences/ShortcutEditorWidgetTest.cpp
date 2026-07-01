// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "preferences/ShortcutEditorWidget.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/uimodel/input/KeyChord.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/layout/action/LayoutActionCatalog.h>
#include <ao/uimodel/layout/action/LayoutActionTypes.h>

#include <catch2/catch_test_macros.hpp>
#include <gdk/gdkkeysyms.h>
#include <gdk/gdktypes.h>
#include <gdkmm/enums.h>
#include <gtkmm/button.h>
#include <gtkmm/eventcontrollerkey.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
    using uimodel::LayoutActionCapability;

    uimodel::KeyChord chord(std::string const& text)
    {
      auto const optChord = uimodel::KeyChord::parse(text);
      REQUIRE(optChord.has_value());
      return *optChord;
    }

    uimodel::LayoutActionCatalog makeCatalog()
    {
      auto catalog = uimodel::LayoutActionCatalog{};
      catalog.registerActionDescriptor({.id = "playback.playPause",
                                        .label = "Play/Pause",
                                        .category = "Playback",
                                        .capabilities = LayoutActionCapability::None});
      catalog.registerActionDescriptor(
        {.id = "playback.next", .label = "Next", .category = "Playback", .capabilities = LayoutActionCapability::None});
      // Requires a widget anchor and presents a menu: not drivable by a global accelerator.
      catalog.registerActionDescriptor(
        {.id = "track.editTags",
         .label = "Edit Tags",
         .category = "Tracks",
         .capabilities = LayoutActionCapability::RequiresAnchor | LayoutActionCapability::PresentsMenu});
      return catalog;
    }

    bool contains(std::vector<std::string> const& ids, std::string const& id)
    {
      return std::ranges::contains(ids, id);
    }

    bool hasChord(std::vector<uimodel::KeyChord> const& chords, uimodel::KeyChord const& c)
    {
      return std::ranges::contains(chords, c);
    }

    std::vector<Gtk::Button*> buttonsByLabel(Gtk::Widget& root, std::string const& labelText)
    {
      auto result = std::vector<Gtk::Button*>{};

      for (auto* const button : collectAll<Gtk::Button>(root))
      {
        if (button->get_label() == labelText)
        {
          result.push_back(button);
        }
      }

      return result;
    }

    void clickButtonByLabel(Gtk::Widget& root, std::string const& labelText)
    {
      auto* const button = findButtonByLabel(root, labelText);
      REQUIRE(button != nullptr);
      emitClicked(*button);
    }

    void clickButtonByLabel(Gtk::Widget& root, std::string const& labelText, std::size_t const index)
    {
      auto const buttons = buttonsByLabel(root, labelText);
      REQUIRE(buttons.size() > index);
      emitClicked(*buttons[index]);
    }

    void emitShortcutCapture(ShortcutEditorWidget& editor,
                             guint const keyval,
                             Gdk::ModifierType const modifiers = Gdk::ModifierType{})
    {
      auto* const captureWindow = editor.captureWindowForTest();
      REQUIRE(captureWindow != nullptr);

      auto const keyControllerPtr = findController<Gtk::EventControllerKey>(*captureWindow);
      REQUIRE(keyControllerPtr);

      gboolean handled = FALSE;
      ::g_signal_emit_by_name(
        keyControllerPtr->gobj(), "key-pressed", keyval, 0U, static_cast<GdkModifierType>(modifiers), &handled);
      CHECK(handled == TRUE);
    }
  } // namespace

  TEST_CASE("ShortcutEditorWidget lists only shortcut-eligible actions", "[gtk][unit][preferences][shortcut]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto host = Gtk::Window{};
    auto editor = ShortcutEditorWidget{makeCatalog(), uimodel::KeymapModel{uimodel::defaultKeymap()}, {}, host};
    drainGtkEvents();

    auto const& ids = editor.editableActionIds();
    CHECK(contains(ids, "playback.playPause"));
    CHECK(contains(ids, "playback.next"));
    CHECK_FALSE(contains(ids, "track.editTags"));
  }

  TEST_CASE("ShortcutEditorWidget renders the effective chords for each action", "[gtk][unit][preferences][shortcut]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto host = Gtk::Window{};
    auto editor = ShortcutEditorWidget{makeCatalog(), uimodel::KeymapModel{uimodel::defaultKeymap()}, {}, host};
    drainGtkEvents();

    CHECK(findLabelByText(editor, "Ctrl+P") != nullptr);
    CHECK(findLabelByText(editor, "Media:Play") != nullptr);
    CHECK(findLabelByText(editor, "Play/Pause") != nullptr);
  }

  TEST_CASE("ShortcutEditorWidget routes shortcut button events to keymap changes",
            "[gtk][unit][preferences][shortcut]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    std::int32_t changeCount = 0;
    auto optLastModel = std::optional<uimodel::KeymapModel>{};
    auto host = Gtk::Window{};
    auto editor = ShortcutEditorWidget{makeCatalog(),
                                       uimodel::KeymapModel{uimodel::defaultKeymap()},
                                       [&](uimodel::KeymapModel const& model)
                                       {
                                         ++changeCount;
                                         optLastModel = model;
                                       },
                                       host};
    drainGtkEvents();

    SECTION("Add captures a free chord, notifies, and shows it")
    {
      clickButtonByLabel(editor, "Add…", 1);
      emitShortcutCapture(editor, GDK_KEY_N, Gdk::ModifierType::CONTROL_MASK);
      drainGtkEvents();

      CHECK(changeCount == 1);
      REQUIRE(optLastModel.has_value());
      CHECK(optLastModel->actionFor(chord("Ctrl+N")) == std::optional<std::string>{"playback.next"});
      CHECK(findLabelByText(editor, "Ctrl+N") != nullptr);
    }

    SECTION("Add confirms and transfers an in-use chord")
    {
      auto ownerLabel = std::string{};
      auto chordText = std::string{};
      editor.setConflictConfirmer(
        [&](std::string const& owner, std::string const& text, std::function<void(bool)> respond)
        {
          ownerLabel = owner;
          chordText = text;
          respond(true);
        });

      clickButtonByLabel(editor, "Add…", 1);
      emitShortcutCapture(editor, GDK_KEY_P, Gdk::ModifierType::CONTROL_MASK);
      drainGtkEvents();

      CHECK(ownerLabel == "Play/Pause");
      CHECK(chordText == "Ctrl+P");
      REQUIRE(optLastModel.has_value());
      CHECK(hasChord(optLastModel->chordsFor("playback.next"), chord("Ctrl+P")));
      CHECK_FALSE(hasChord(optLastModel->chordsFor("playback.playPause"), chord("Ctrl+P")));
      CHECK(optLastModel->conflicts().empty());
    }

    SECTION("Add leaves the keymap untouched when reassignment is declined")
    {
      editor.setConflictConfirmer([](std::string const&, std::string const&, std::function<void(bool)> respond)
                                  { respond(false); });

      clickButtonByLabel(editor, "Add…", 1);
      emitShortcutCapture(editor, GDK_KEY_P, Gdk::ModifierType::CONTROL_MASK);
      drainGtkEvents();

      CHECK(changeCount == 0);
      CHECK_FALSE(optLastModel.has_value());
      CHECK(findLabelByText(editor, "Ctrl+P") != nullptr);
    }

    SECTION("remove button removes a chord and refreshes the row")
    {
      clickButtonByLabel(editor, "✕");
      drainGtkEvents();

      CHECK(changeCount == 1);
      REQUIRE(optLastModel.has_value());
      CHECK_FALSE(hasChord(optLastModel->chordsFor("playback.playPause"), chord("Ctrl+P")));
      CHECK(findLabelByText(editor, "Ctrl+P") == nullptr);
    }

    SECTION("per-action Reset restores that action's default")
    {
      auto editedKeymap = uimodel::KeymapModel{uimodel::defaultKeymap()};
      editedKeymap.applyOverrides(uimodel::KeymapOverrides{{"playback.next", {"Ctrl+N"}}});
      auto editedEditor = ShortcutEditorWidget{makeCatalog(),
                                               std::move(editedKeymap),
                                               [&](uimodel::KeymapModel const& model)
                                               {
                                                 ++changeCount;
                                                 optLastModel = model;
                                               },
                                               host};

      clickButtonByLabel(editedEditor, "Reset", 1);
      drainGtkEvents();

      REQUIRE(optLastModel.has_value());
      CHECK_FALSE(hasChord(optLastModel->chordsFor("playback.next"), chord("Ctrl+N")));
      CHECK(hasChord(optLastModel->chordsFor("playback.next"), chord("Ctrl+Right")));
    }

    SECTION("Reset All restores every edited action")
    {
      auto editedKeymap = uimodel::KeymapModel{uimodel::defaultKeymap()};
      editedKeymap.applyOverrides(uimodel::KeymapOverrides{{"playback.next", {"Ctrl+N"}}, {"playback.playPause", {}}});
      auto editedEditor = ShortcutEditorWidget{makeCatalog(),
                                               std::move(editedKeymap),
                                               [&](uimodel::KeymapModel const& model)
                                               {
                                                 ++changeCount;
                                                 optLastModel = model;
                                               },
                                               host};

      clickButtonByLabel(editedEditor, "Reset All");
      drainGtkEvents();

      REQUIRE(optLastModel.has_value());
      CHECK(hasChord(optLastModel->chordsFor("playback.playPause"), chord("Ctrl+P")));
      CHECK_FALSE(hasChord(optLastModel->chordsFor("playback.next"), chord("Ctrl+N")));
    }
  }

  TEST_CASE("ShortcutEditorWidget ignores conflict responses after destruction", "[gtk][unit][preferences][shortcut]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    std::int32_t changeCount = 0;
    auto respond = std::function<void(bool)>{};
    auto host = Gtk::Window{};
    auto editorPtr = std::make_unique<ShortcutEditorWidget>(
      makeCatalog(),
      uimodel::KeymapModel{uimodel::defaultKeymap()},
      [&](uimodel::KeymapModel const&) { ++changeCount; },
      host);
    editorPtr->setConflictConfirmer([&](std::string const&, std::string const&, std::function<void(bool)> callback)
                                    { respond = std::move(callback); });

    clickButtonByLabel(*editorPtr, "Add…", 1);
    emitShortcutCapture(*editorPtr, GDK_KEY_P, Gdk::ModifierType::CONTROL_MASK);
    REQUIRE(respond);

    editorPtr.reset();
    respond(true);

    CHECK(changeCount == 0);
  }

  TEST_CASE("ShortcutEditorWidget parents capture popups to the injected host", "[gtk][unit][preferences][shortcut]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto host = Gtk::Window{};
    auto editor = ShortcutEditorWidget{makeCatalog(), uimodel::KeymapModel{uimodel::defaultKeymap()}, {}, host};

    clickButtonByLabel(editor, "Add…", 1);

    REQUIRE(editor.captureWindowForTest() != nullptr);
    CHECK(editor.captureWindowForTest()->get_transient_for() == &host);
    CHECK(editor.captureWindowForTest()->get_modal());

    emitShortcutCapture(editor, GDK_KEY_Escape);
    drainGtkEvents();
    CHECK(editor.captureWindowForTest() == nullptr);
  }

  TEST_CASE("ShortcutEditorWidget old capture teardown does not close a replacement popup",
            "[gtk][unit][preferences][shortcut]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto host = Gtk::Window{};
    auto editor = ShortcutEditorWidget{makeCatalog(), uimodel::KeymapModel{uimodel::defaultKeymap()}, {}, host};

    clickButtonByLabel(editor, "Add…", 1);
    auto* const firstCapture = editor.captureWindowForTest();
    REQUIRE(firstCapture != nullptr);

    clickButtonByLabel(editor, "Add…", 0);
    auto* const secondCapture = editor.captureWindowForTest();
    REQUIRE(secondCapture != nullptr);
    REQUIRE(secondCapture != firstCapture);

    drainGtkEvents();

    CHECK(editor.captureWindowForTest() == secondCapture);
  }
} // namespace ao::gtk::test
