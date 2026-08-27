// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "completion/EntryCompletionController.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionResult.h>

#include <catch2/catch_test_macros.hpp>
#include <gdk/gdkenums.h>
#include <gdk/gdkkeysyms.h>
#include <gdkmm/enums.h>
#include <giomm/listmodel.h>
#include <glib-object.h>
#include <glibmm/refptr.h>
#include <glibmm/ustring.h>
#include <gtkmm/entry.h>
#include <gtkmm/eventcontroller.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/label.h>
#include <gtkmm/listview.h>
#include <gtkmm/popover.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
    std::int32_t charCount(char const* text)
    {
      return static_cast<std::int32_t>(Glib::ustring{text}.length());
    }

    Gtk::Popover* findCompletionPopover(Gtk::Entry& entry)
    {
      return findWidget<Gtk::Popover>(entry);
    }

    bool emitCompletionKey(Gtk::Entry& entry, guint const keyval)
    {
      auto const keyControllerPtr = findControllerIf<Gtk::EventControllerKey>(
        entry,
        [](Gtk::EventControllerKey const& controller)
        { return controller.get_propagation_phase() == Gtk::PropagationPhase::CAPTURE; });
      REQUIRE(keyControllerPtr);

      gboolean handled = FALSE;
      ::g_signal_emit_by_name(keyControllerPtr->gobj(),
                              "key-pressed",
                              keyval,
                              0U,
                              static_cast<GdkModifierType>(Gdk::ModifierType{}),
                              &handled);
      return handled == TRUE;
    }

    rt::CompletionResult keyboardCompletionResult()
    {
      return rt::CompletionResult{
        .replaceBegin = 0,
        .replaceEnd = 2,
        .items =
          {
            rt::CompletionItem{.displayText = "$artist", .insertText = "$artist", .detail = {}, .rank = 0},
            rt::CompletionItem{.displayText = "$album", .insertText = "$album", .detail = {}, .rank = 1},
          },
      };
    }

    rt::CompletionResult pageCompletionResult()
    {
      auto items = std::vector<rt::CompletionItem>{};

      for (std::size_t index = 0; index < 12; ++index)
      {
        auto text = std::string{"$item"} + std::to_string(index);
        items.push_back(rt::CompletionItem{.displayText = text, .insertText = std::move(text)});
      }

      return rt::CompletionResult{.replaceBegin = 0, .replaceEnd = 2, .items = std::move(items)};
    }
  } // namespace

  TEST_CASE("EntryCompletionController - uses UTF-8 byte spans with GTK character cursors", "[gtk][unit][completion]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto window = Gtk::Window{};
    auto entry = Gtk::Entry{};
    window.set_child(entry);
    entry.set_text("你 $al");
    entry.set_position(charCount("你 $al"));

    bool providerCalled = false;
    auto controller = EntryCompletionController{
      entry,
      ao::test::englishMessageCatalog(),
      [&providerCalled](std::string_view text, std::size_t cursor) -> std::optional<rt::CompletionResult>
      {
        providerCalled = true;
        CHECK(text == std::string_view{"你 $al"});
        CHECK(cursor == std::string_view{"你 $al"}.size());

        auto items = std::vector<rt::CompletionItem>{};
        items.push_back(rt::CompletionItem{
          .displayText = "$album",
          .insertText = "$album",
          .detail = {},
          .rank = 0,
        });
        return rt::CompletionResult{
          .replaceBegin = std::string_view{"你 "}.size(),
          .replaceEnd = std::string_view{"你 $al"}.size(),
          .items = std::move(items),
        };
      }};

    controller.update();
    controller.applySelected();

    CHECK(providerCalled);
    CHECK(entry.get_text() == "你 $album");
    CHECK(entry.get_position() == charCount("你 $album"));
  }

  TEST_CASE("EntryCompletionController - programmatic text changes do not call provider", "[gtk][unit][completion]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto window = Gtk::Window{};
    auto entry = Gtk::Entry{};
    window.set_child(entry);
    entry.set_text("$al");
    entry.set_position(charCount("$al"));

    std::int32_t providerCalls = 0;
    auto controller = EntryCompletionController{
      entry,
      ao::test::englishMessageCatalog(),
      [&providerCalls](std::string_view, std::size_t) -> std::optional<rt::CompletionResult>
      {
        ++providerCalls;

        auto items = std::vector<rt::CompletionItem>{};
        items.push_back(rt::CompletionItem{
          .displayText = "$album",
          .insertText = "$album",
          .detail = {},
          .rank = 0,
        });
        return rt::CompletionResult{
          .replaceBegin = 0, .replaceEnd = std::string_view{"$al"}.size(), .items = std::move(items)};
      }};

    controller.update();
    REQUIRE(providerCalls == 1);

    controller.setTextProgrammatically("$artist");

    CHECK(providerCalls == 1);
    controller.applySelected();
    CHECK(entry.get_text() == "$artist");
  }

  TEST_CASE("EntryCompletionController - shows completion popup without modal keyboard grab", "[gtk][unit][completion]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto window = Gtk::Window{};
    auto entry = Gtk::Entry{};
    window.set_child(entry);
    entry.set_text("$al");
    entry.set_position(charCount("$al"));

    auto controller = EntryCompletionController{
      entry,
      ao::test::englishMessageCatalog(),
      [](std::string_view, std::size_t) -> std::optional<rt::CompletionResult>
      {
        auto items = std::vector<rt::CompletionItem>{};
        items.push_back(rt::CompletionItem{
          .displayText = "$album",
          .insertText = "$album",
          .detail = {},
          .rank = 0,
        });
        return rt::CompletionResult{
          .replaceBegin = 0, .replaceEnd = std::string_view{"$al"}.size(), .items = std::move(items)};
      }};

    auto* const popover = findCompletionPopover(entry);
    REQUIRE(popover != nullptr);
    CHECK_FALSE(popover->get_autohide());

    auto* const scrolledWindow = dynamic_cast<Gtk::ScrolledWindow*>(popover->get_child());
    REQUIRE(scrolledWindow != nullptr);
    CHECK_FALSE(scrolledWindow->get_can_focus());
    CHECK_FALSE(scrolledWindow->get_focusable());

    auto* const listView = dynamic_cast<Gtk::ListView*>(scrolledWindow->get_child());
    REQUIRE(listView != nullptr);
    CHECK_FALSE(listView->get_can_focus());
    CHECK_FALSE(listView->get_focusable());

    controller.update();
    CHECK(popover->get_visible());
  }

  TEST_CASE("EntryCompletionController - renders completion item detail", "[gtk][unit][completion]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto window = Gtk::Window{};
    auto entry = Gtk::Entry{};
    window.set_child(entry);
    entry.set_text("$al");
    entry.set_position(charCount("$al"));

    auto controller = EntryCompletionController{
      entry,
      ao::test::englishMessageCatalog(),
      [](std::string_view, std::size_t) -> std::optional<rt::CompletionResult>
      {
        auto items = std::vector<rt::CompletionItem>{};
        items.push_back(rt::CompletionItem{
          .displayText = "$album",
          .insertText = "$album",
          .detail = {.kind = rt::CompletionDetailKind::Field},
          .rank = 0,
        });
        return rt::CompletionResult{
          .replaceBegin = 0, .replaceEnd = std::string_view{"$al"}.size(), .items = std::move(items)};
      }};

    auto* const popover = findCompletionPopover(entry);
    REQUIRE(popover != nullptr);

    controller.update();

    auto* const title = findWidgetByClass<Gtk::Label>(*popover, "ao-query-completion-row-title");
    REQUIRE(title != nullptr);
    CHECK(title->get_text() == "$album");

    auto* const detail = findWidgetByClass<Gtk::Label>(*popover, "ao-query-completion-row-detail");
    REQUIRE(detail != nullptr);
    CHECK(detail->get_visible());
    CHECK(detail->get_text() == "field");
  }

  TEST_CASE("EntryCompletionController - applies refreshed completion rows", "[gtk][unit][completion]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto window = Gtk::Window{};
    auto entry = Gtk::Entry{};
    window.set_child(entry);
    entry.set_text("$");
    entry.set_position(charCount("$"));

    std::int32_t providerCalls = 0;
    auto controller = EntryCompletionController{
      entry,
      ao::test::englishMessageCatalog(),
      [&providerCalls](std::string_view, std::size_t) -> std::optional<rt::CompletionResult>
      {
        ++providerCalls;

        auto items = std::vector<rt::CompletionItem>{};

        if (providerCalls == 1)
        {
          items.push_back(
            rt::CompletionItem{.displayText = "$artist", .insertText = "$artist", .detail = {}, .rank = 0});
          items.push_back(rt::CompletionItem{.displayText = "$album", .insertText = "$album", .detail = {}, .rank = 0});
        }
        else
        {
          items.push_back(
            rt::CompletionItem{.displayText = "$composer", .insertText = "$composer", .detail = {}, .rank = 0});
        }

        return rt::CompletionResult{
          .replaceBegin = 0, .replaceEnd = std::string_view{"$"}.size(), .items = std::move(items)};
      }};

    controller.update();
    REQUIRE(controller.moveSelection(1));

    controller.update();
    controller.applySelected();

    CHECK(providerCalls == 2);
    CHECK(entry.get_text() == "$composer");
  }

  TEST_CASE("EntryCompletionController - keyboard navigation cycles and only Tab accepts a selection",
            "[gtk][regression][completion]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto window = Gtk::Window{};
    auto entry = Gtk::Entry{};
    window.set_child(entry);
    entry.set_text("$a");
    entry.set_position(charCount("$a"));

    auto controller = EntryCompletionController{entry,
                                                ao::test::englishMessageCatalog(),
                                                [](std::string_view, std::size_t) -> std::optional<rt::CompletionResult>
                                                { return keyboardCompletionResult(); }};
    auto* const popover = findCompletionPopover(entry);
    REQUIRE(popover != nullptr);

    controller.update();
    REQUIRE(popover->get_visible());

    SECTION("Down after the final item returns to the first item")
    {
      CHECK(emitCompletionKey(entry, GDK_KEY_Down));
      CHECK(popover->get_visible());
      CHECK(emitCompletionKey(entry, GDK_KEY_Down));
      CHECK(popover->get_visible());

      controller.applySelected();
      CHECK(entry.get_text() == "$artist");
    }

    SECTION("Up before the first item returns to the final item")
    {
      CHECK(emitCompletionKey(entry, GDK_KEY_Up));
      CHECK(popover->get_visible());

      controller.applySelected();
      CHECK(entry.get_text() == "$album");
    }

    SECTION("Page Down moves forward and stops at the final item")
    {
      CHECK(emitCompletionKey(entry, GDK_KEY_Page_Down));
      CHECK(popover->get_visible());
      CHECK(emitCompletionKey(entry, GDK_KEY_Page_Down));
      CHECK(popover->get_visible());

      controller.applySelected();
      CHECK(entry.get_text() == "$album");
    }

    SECTION("Page Up stops at the first item")
    {
      CHECK(emitCompletionKey(entry, GDK_KEY_Down));
      CHECK(emitCompletionKey(entry, GDK_KEY_Page_Up));
      CHECK(popover->get_visible());

      controller.applySelected();
      CHECK(entry.get_text() == "$artist");
    }

    SECTION("Tab accepts the selected item")
    {
      CHECK(emitCompletionKey(entry, GDK_KEY_Tab));
      CHECK(entry.get_text() == "$artist");
      CHECK_FALSE(popover->get_visible());
    }

    SECTION("Keypad Tab accepts the selected item")
    {
      CHECK(emitCompletionKey(entry, GDK_KEY_KP_Tab));
      CHECK(entry.get_text() == "$artist");
      CHECK_FALSE(popover->get_visible());
    }

    SECTION("Return dismisses without accepting the selected item")
    {
      CHECK_FALSE(emitCompletionKey(entry, GDK_KEY_Return));
      CHECK(entry.get_text() == "$a");
      CHECK_FALSE(popover->get_visible());

      controller.applySelected();
      CHECK(entry.get_text() == "$a");
    }

    SECTION("Keypad Enter dismisses without accepting the selected item")
    {
      CHECK_FALSE(emitCompletionKey(entry, GDK_KEY_KP_Enter));
      CHECK(entry.get_text() == "$a");
      CHECK_FALSE(popover->get_visible());

      controller.applySelected();
      CHECK(entry.get_text() == "$a");
    }
  }

  TEST_CASE("EntryCompletionController - page navigation uses the visible viewport", "[gtk][unit][completion]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto window = Gtk::Window{};
    auto entry = Gtk::Entry{};
    window.set_child(entry);
    entry.set_text("$a");
    entry.set_position(charCount("$a"));

    auto controller = EntryCompletionController{entry,
                                                ao::test::englishMessageCatalog(),
                                                [](std::string_view, std::size_t) -> std::optional<rt::CompletionResult>
                                                { return pageCompletionResult(); }};
    auto* const popover = findCompletionPopover(entry);
    REQUIRE(popover != nullptr);
    auto* const scrolledWindow = dynamic_cast<Gtk::ScrolledWindow*>(popover->get_child());
    REQUIRE(scrolledWindow != nullptr);

    controller.update();
    REQUIRE(popover->get_visible());

    auto const adjustmentPtr = scrolledWindow->get_vadjustment();
    REQUIRE(adjustmentPtr);
    adjustmentPtr->set_upper(120.0);
    adjustmentPtr->set_page_size(30.0);

    CHECK(emitCompletionKey(entry, GDK_KEY_Page_Down));
    CHECK(emitCompletionKey(entry, GDK_KEY_Page_Down));
    CHECK(emitCompletionKey(entry, GDK_KEY_Page_Up));
    CHECK(popover->get_visible());

    controller.applySelected();
    CHECK(entry.get_text() == "$item3");
  }

  TEST_CASE("EntryCompletionController - clears completion state when entry focus leaves", "[gtk][unit][completion]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto window = Gtk::Window{};
    auto entry = Gtk::Entry{};
    window.set_child(entry);
    entry.set_text("$al");
    entry.set_position(charCount("$al"));

    auto controller = EntryCompletionController{
      entry,
      ao::test::englishMessageCatalog(),
      [](std::string_view, std::size_t) -> std::optional<rt::CompletionResult>
      {
        auto items = std::vector<rt::CompletionItem>{};
        items.push_back(rt::CompletionItem{
          .displayText = "$album",
          .insertText = "$album",
          .detail = {},
          .rank = 0,
        });
        return rt::CompletionResult{
          .replaceBegin = 0, .replaceEnd = std::string_view{"$al"}.size(), .items = std::move(items)};
      }};

    controller.update();
    REQUIRE(emitFocusLeave(entry));
    controller.applySelected();

    CHECK(entry.get_text() == "$al");
  }

  TEST_CASE("EntryCompletionController - dismisses popup on outside press without modal autohide",
            "[gtk][unit][completion]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto window = Gtk::Window{};
    auto entry = Gtk::Entry{};
    window.set_child(entry);
    entry.set_text("$al");
    entry.set_position(charCount("$al"));

    auto controller = EntryCompletionController{
      entry,
      ao::test::englishMessageCatalog(),
      [](std::string_view, std::size_t) -> std::optional<rt::CompletionResult>
      {
        auto items = std::vector<rt::CompletionItem>{};
        items.push_back(rt::CompletionItem{
          .displayText = "$album",
          .insertText = "$album",
          .detail = {},
          .rank = 0,
        });
        return rt::CompletionResult{
          .replaceBegin = 0, .replaceEnd = std::string_view{"$al"}.size(), .items = std::move(items)};
      }};

    auto* const popover = findCompletionPopover(entry);
    REQUIRE(popover != nullptr);
    CHECK_FALSE(popover->get_autohide());

    controller.update();
    REQUIRE(popover->get_visible());

    REQUIRE(emitGesturePressed(window, 1, -100.0, -100.0, Gtk::PropagationPhase::CAPTURE));

    CHECK_FALSE(popover->get_visible());
    controller.applySelected();
    CHECK(entry.get_text() == "$al");
  }

  TEST_CASE("EntryCompletionController - clears completion state when popover closes", "[gtk][unit][completion]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto window = Gtk::Window{};
    auto entry = Gtk::Entry{};
    window.set_child(entry);
    entry.set_text("$al");
    entry.set_position(charCount("$al"));

    auto controller = EntryCompletionController{
      entry,
      ao::test::englishMessageCatalog(),
      [](std::string_view, std::size_t) -> std::optional<rt::CompletionResult>
      {
        auto items = std::vector<rt::CompletionItem>{};
        items.push_back(rt::CompletionItem{
          .displayText = "$album",
          .insertText = "$album",
          .detail = {},
          .rank = 0,
        });
        return rt::CompletionResult{
          .replaceBegin = 0, .replaceEnd = std::string_view{"$al"}.size(), .items = std::move(items)};
      }};

    auto* const popover = findCompletionPopover(entry);
    REQUIRE(popover != nullptr);
    CHECK_FALSE(popover->get_autohide());

    controller.update();
    emitClosed(*popover);
    controller.applySelected();

    CHECK(entry.get_text() == "$al");
  }
} // namespace ao::gtk::test
