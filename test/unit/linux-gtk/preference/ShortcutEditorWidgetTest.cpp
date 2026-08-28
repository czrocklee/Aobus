// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "preference/ShortcutEditorWidget.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkLayoutTestSupport.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include <ao/uimodel/input/KeyChord.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>

#include <catch2/catch_test_macros.hpp>
#include <gdk/gdkkeysyms.h>
#include <gdk/gdktypes.h>
#include <gdkmm/enums.h>
#include <gtkmm/button.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/label.h>

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
    using uimodel::ActionCapability;

    uimodel::KeyChord chord(std::string const& text)
    {
      auto const optChord = uimodel::KeyChord::parse(text);
      REQUIRE(optChord);
      return *optChord;
    }

    uimodel::LayoutSchema makeSchema()
    {
      auto schema = uimodel::LayoutSchema{};
      schema.addAction({.id = "playback.playPause", .label = "Play/Pause", .category = "Playback", .capabilities = 0});
      schema.addAction({.id = "playback.next", .label = "Next", .category = "Playback", .capabilities = 0});
      // Requires a widget anchor and presents a menu: not drivable by a global accelerator.
      schema.addAction({.id = "track.editTags",
                        .label = "Edit Tags",
                        .category = "Tracks",
                        .capabilities = ActionCapability::RequiresAnchor | ActionCapability::PresentsMenu});
      return schema;
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
      auto* const captureWindow = editor.captureWindow();
      REQUIRE(captureWindow != nullptr);

      auto const keyControllerPtr = findController<Gtk::EventControllerKey>(*captureWindow);
      REQUIRE(keyControllerPtr);

      gboolean handled = FALSE;
      ::g_signal_emit_by_name(
        keyControllerPtr->gobj(), "key-pressed", keyval, 0U, static_cast<GdkModifierType>(modifiers), &handled);
      CHECK(handled == TRUE);
    }
  } // namespace

  TEST_CASE("ShortcutEditorWidget - lists only shortcut-eligible actions", "[gtk][unit][preferences][shortcut]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto host = Gtk::Window{};
    auto editor = ShortcutEditorWidget{
      ao::test::englishMessageCatalog(), makeSchema(), uimodel::KeymapModel{uimodel::defaultKeymap()}, {}, host};
    drainGtkEvents();

    auto const& ids = editor.editableActionIds();
    CHECK(contains(ids, "playback.playPause"));
    CHECK(contains(ids, "playback.next"));
    CHECK_FALSE(contains(ids, "track.editTags"));
  }

  TEST_CASE("ShortcutEditorWidget - renders the effective chords for each action", "[gtk][unit][preferences][shortcut]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto host = Gtk::Window{};
    auto editor = ShortcutEditorWidget{
      ao::test::englishMessageCatalog(), makeSchema(), uimodel::KeymapModel{uimodel::defaultKeymap()}, {}, host};
    drainGtkEvents();

    CHECK(findLabelByText(editor, "Ctrl+P") != nullptr);
    CHECK(findLabelByText(editor, "Media:Play") != nullptr);
    CHECK(findLabelByText(editor, "Play/Pause") != nullptr);
  }

  TEST_CASE("ShortcutEditorWidget - renders locale-selected editor chrome", "[gtk][unit][localization]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto const textCatalog = ao::test::messageCatalog("de-DE");
    auto host = Gtk::Window{};
    auto editor =
      ShortcutEditorWidget{textCatalog, makeSchema(), uimodel::KeymapModel{uimodel::defaultKeymap()}, {}, host};
    drainGtkEvents();

    CHECK(findLabelByText(editor, "Tastaturkürzel anpassen") != nullptr);
    CHECK(findButtonByLabel(editor, "Alle zurücksetzen") != nullptr);
    CHECK(findButtonByLabel(editor, "Hinzufügen…") != nullptr);
  }

  TEST_CASE("ShortcutEditorWidget - long action names preserve shortcut controls at constrained width",
            "[gtk][regression][preferences][geometry]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto const longActionLabel = std::string{"Activar/desactivar reproducción aleatoria excepcionalmente larga"};
    auto schema = uimodel::LayoutSchema{};
    schema.addAction(
      {.id = "playback.toggleShuffle", .label = longActionLabel, .category = "Playback", .capabilities = 0});
    auto hostWindow = Gtk::Window{};
    auto editor = ShortcutEditorWidget{
      ao::test::englishMessageCatalog(), schema, uimodel::KeymapModel{uimodel::defaultKeymap()}, {}, hostWindow};

    auto allocationHost = AllocationHost{editor};
    allocationHost.allocateChild(420, 480);
    drainGtkEvents();

    auto* const actionLabel = findLabelByText(editor, longActionLabel);
    REQUIRE(actionLabel != nullptr);
    auto const layoutPtr = actionLabel->get_layout();
    REQUIRE(layoutPtr != nullptr);
    CHECK(layoutPtr->is_ellipsized());
    CHECK(actionLabel->get_tooltip_text() == longActionLabel);
    CHECK(findButtonByLabel(editor, "Add…") != nullptr);
    CHECK(findButtonByLabel(editor, "Reset") != nullptr);
  }

  TEST_CASE("ShortcutEditorWidget - routes shortcut button events to keymap changes",
            "[gtk][unit][preferences][shortcut]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    std::int32_t changeCount = 0;
    auto optLastModel = std::optional<uimodel::KeymapModel>{};
    auto host = Gtk::Window{};
    auto editor = ShortcutEditorWidget{ao::test::englishMessageCatalog(),
                                       makeSchema(),
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
      REQUIRE(optLastModel);
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
      REQUIRE(optLastModel);
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
      CHECK_FALSE(optLastModel);
      CHECK(findLabelByText(editor, "Ctrl+P") != nullptr);
    }

    SECTION("remove button removes a chord and refreshes the row")
    {
      clickButtonByLabel(editor, "✕");
      drainGtkEvents();

      CHECK(changeCount == 1);
      REQUIRE(optLastModel);
      CHECK_FALSE(hasChord(optLastModel->chordsFor("playback.playPause"), chord("Ctrl+P")));
      CHECK(findLabelByText(editor, "Ctrl+P") == nullptr);
    }

    SECTION("per-action Reset restores that action's default")
    {
      auto editedKeymap = uimodel::KeymapModel{uimodel::defaultKeymap()};
      editedKeymap.applyOverrides(uimodel::KeymapOverrides{{"playback.next", {"Ctrl+N"}}});
      auto editedEditor = ShortcutEditorWidget{ao::test::englishMessageCatalog(),
                                               makeSchema(),
                                               std::move(editedKeymap),
                                               [&](uimodel::KeymapModel const& model)
                                               {
                                                 ++changeCount;
                                                 optLastModel = model;
                                               },
                                               host};

      clickButtonByLabel(editedEditor, "Reset", 1);
      drainGtkEvents();

      REQUIRE(optLastModel);
      CHECK_FALSE(hasChord(optLastModel->chordsFor("playback.next"), chord("Ctrl+N")));
      CHECK(hasChord(optLastModel->chordsFor("playback.next"), chord("Ctrl+Right")));
    }

    SECTION("Reset All restores every edited action")
    {
      auto editedKeymap = uimodel::KeymapModel{uimodel::defaultKeymap()};
      editedKeymap.applyOverrides(uimodel::KeymapOverrides{{"playback.next", {"Ctrl+N"}}, {"playback.playPause", {}}});
      auto editedEditor = ShortcutEditorWidget{ao::test::englishMessageCatalog(),
                                               makeSchema(),
                                               std::move(editedKeymap),
                                               [&](uimodel::KeymapModel const& model)
                                               {
                                                 ++changeCount;
                                                 optLastModel = model;
                                               },
                                               host};

      clickButtonByLabel(editedEditor, "Reset All");
      drainGtkEvents();

      REQUIRE(optLastModel);
      CHECK(hasChord(optLastModel->chordsFor("playback.playPause"), chord("Ctrl+P")));
      CHECK_FALSE(hasChord(optLastModel->chordsFor("playback.next"), chord("Ctrl+N")));
    }
  }

  TEST_CASE("ShortcutEditorWidget - ignores conflict responses after destruction",
            "[gtk][regression][shortcut][concurrency]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    std::int32_t changeCount = 0;
    auto respond = std::function<void(bool)>{};
    auto host = Gtk::Window{};
    auto editorPtr = std::make_unique<ShortcutEditorWidget>(
      ao::test::englishMessageCatalog(),
      makeSchema(),
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

  TEST_CASE("ShortcutEditorWidget - parents capture popups to the injected host", "[gtk][unit][preferences][shortcut]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto host = Gtk::Window{};
    auto editor = ShortcutEditorWidget{
      ao::test::englishMessageCatalog(), makeSchema(), uimodel::KeymapModel{uimodel::defaultKeymap()}, {}, host};

    clickButtonByLabel(editor, "Add…", 1);

    REQUIRE(editor.captureWindow() != nullptr);
    CHECK(editor.captureWindow()->get_transient_for() == &host);
    CHECK(editor.captureWindow()->get_modal());

    emitShortcutCapture(editor, GDK_KEY_Escape);
    drainGtkEvents();
    CHECK(editor.captureWindow() == nullptr);
  }

  TEST_CASE("ShortcutEditorWidget - old capture teardown does not close a replacement popup",
            "[gtk][unit][preferences][shortcut]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto host = Gtk::Window{};
    auto editor = ShortcutEditorWidget{
      ao::test::englishMessageCatalog(), makeSchema(), uimodel::KeymapModel{uimodel::defaultKeymap()}, {}, host};

    clickButtonByLabel(editor, "Add…", 1);
    auto* const firstCapture = editor.captureWindow();
    REQUIRE(firstCapture != nullptr);

    clickButtonByLabel(editor, "Add…", 0);
    auto* const secondCapture = editor.captureWindow();
    REQUIRE(secondCapture != nullptr);
    REQUIRE(secondCapture != firstCapture);

    drainGtkEvents();

    CHECK(editor.captureWindow() == secondCapture);
  }
} // namespace ao::gtk::test
