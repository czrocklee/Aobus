// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "completion/EntryCompletionController.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/rt/CompletionItem.h>
#include <ao/rt/CompletionResult.h>

#include <catch2/catch_test_macros.hpp>
#include <giomm/listmodel.h>
#include <glib-object.h>
#include <glibmm/refptr.h>
#include <glibmm/ustring.h>
#include <gtkmm/entry.h>
#include <gtkmm/eventcontroller.h>
#include <gtkmm/eventcontrollerfocus.h>
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
      for (auto* child = entry.get_first_child(); child != nullptr; child = child->get_next_sibling())
      {
        if (auto* const popover = dynamic_cast<Gtk::Popover*>(child); popover != nullptr)
        {
          return popover;
        }
      }

      return nullptr;
    }
  }

  TEST_CASE("EntryCompletionController - uses UTF-8 byte spans with GTK character cursors", "[gtk][completion]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto window = Gtk::Window{};
    auto entry = Gtk::Entry{};
    window.set_child(entry);
    entry.set_text("你 $al");
    entry.set_position(charCount("你 $al"));

    auto providerCalled = false;
    auto controller = EntryCompletionController{
      entry,
      [&providerCalled](std::string_view text, std::size_t cursor) -> std::optional<rt::CompletionResult>
      {
        providerCalled = true;
        CHECK(text == std::string_view{"你 $al"});
        CHECK(cursor == std::string{"你 $al"}.size());

        auto items = std::vector<rt::CompletionItem>{};
        items.push_back(rt::CompletionItem{
          .displayText = "$album",
          .insertText = "$album",
          .detail = "",
          .rank = 0,
        });
        return rt::CompletionResult{
          .replaceBegin = std::string{"你 "}.size(),
          .replaceEnd = std::string{"你 $al"}.size(),
          .items = std::move(items),
        };
      }};

    controller.update();
    controller.applySelected();

    CHECK(providerCalled);
    CHECK(entry.get_text() == "你 $album");
    CHECK(entry.get_position() == charCount("你 $album"));
  }

  TEST_CASE("EntryCompletionController - programmatic text changes do not call provider", "[gtk][completion]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto window = Gtk::Window{};
    auto entry = Gtk::Entry{};
    window.set_child(entry);
    entry.set_text("$al");
    entry.set_position(charCount("$al"));

    auto providerCalls = 0;
    auto controller = EntryCompletionController{
      entry,
      [&providerCalls](std::string_view, std::size_t) -> std::optional<rt::CompletionResult>
      {
        ++providerCalls;

        auto items = std::vector<rt::CompletionItem>{};
        items.push_back(rt::CompletionItem{
          .displayText = "$album",
          .insertText = "$album",
          .detail = "",
          .rank = 0,
        });
        return rt::CompletionResult{
          .replaceBegin = 0, .replaceEnd = std::string{"$al"}.size(), .items = std::move(items)};
      }};

    controller.update();
    REQUIRE(providerCalls == 1);

    controller.setTextProgrammatically("$artist");

    CHECK(providerCalls == 1);
    controller.applySelected();
    CHECK(entry.get_text() == "$artist");
  }

  TEST_CASE("EntryCompletionController - shows completion popup without modal keyboard grab", "[gtk][completion]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto window = Gtk::Window{};
    auto entry = Gtk::Entry{};
    window.set_child(entry);
    entry.set_text("$al");
    entry.set_position(charCount("$al"));

    auto controller = EntryCompletionController{entry,
                                                [](std::string_view, std::size_t) -> std::optional<rt::CompletionResult>
                                                {
                                                  auto items = std::vector<rt::CompletionItem>{};
                                                  items.push_back(rt::CompletionItem{
                                                    .displayText = "$album",
                                                    .insertText = "$album",
                                                    .detail = "",
                                                    .rank = 0,
                                                  });
                                                  return rt::CompletionResult{.replaceBegin = 0,
                                                                              .replaceEnd = std::string{"$al"}.size(),
                                                                              .items = std::move(items)};
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

  TEST_CASE("EntryCompletionController - renders completion item detail", "[gtk][completion]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto window = Gtk::Window{};
    auto entry = Gtk::Entry{};
    window.set_child(entry);
    entry.set_text("$al");
    entry.set_position(charCount("$al"));

    auto controller = EntryCompletionController{entry,
                                                [](std::string_view, std::size_t) -> std::optional<rt::CompletionResult>
                                                {
                                                  auto items = std::vector<rt::CompletionItem>{};
                                                  items.push_back(rt::CompletionItem{
                                                    .displayText = "$album",
                                                    .insertText = "$album",
                                                    .detail = "field",
                                                    .rank = 0,
                                                  });
                                                  return rt::CompletionResult{.replaceBegin = 0,
                                                                              .replaceEnd = std::string{"$al"}.size(),
                                                                              .items = std::move(items)};
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

  TEST_CASE("EntryCompletionController - applies refreshed completion rows", "[gtk][completion]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto window = Gtk::Window{};
    auto entry = Gtk::Entry{};
    window.set_child(entry);
    entry.set_text("$");
    entry.set_position(charCount("$"));

    auto providerCalls = 0;
    auto controller = EntryCompletionController{
      entry,
      [&providerCalls](std::string_view, std::size_t) -> std::optional<rt::CompletionResult>
      {
        ++providerCalls;

        auto items = std::vector<rt::CompletionItem>{};

        if (providerCalls == 1)
        {
          items.push_back(
            rt::CompletionItem{.displayText = "$artist", .insertText = "$artist", .detail = "", .rank = 0});
          items.push_back(rt::CompletionItem{.displayText = "$album", .insertText = "$album", .detail = "", .rank = 0});
        }
        else
        {
          items.push_back(
            rt::CompletionItem{.displayText = "$composer", .insertText = "$composer", .detail = "", .rank = 0});
        }

        return rt::CompletionResult{
          .replaceBegin = 0, .replaceEnd = std::string{"$"}.size(), .items = std::move(items)};
      }};

    controller.update();
    REQUIRE(controller.moveSelection(1));

    controller.update();
    controller.applySelected();

    CHECK(providerCalls == 2);
    CHECK(entry.get_text() == "$composer");
  }

  TEST_CASE("EntryCompletionController - clears completion state when entry focus leaves", "[gtk][completion]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto window = Gtk::Window{};
    auto entry = Gtk::Entry{};
    window.set_child(entry);
    entry.set_text("$al");
    entry.set_position(charCount("$al"));

    auto controller = EntryCompletionController{entry,
                                                [](std::string_view, std::size_t) -> std::optional<rt::CompletionResult>
                                                {
                                                  auto items = std::vector<rt::CompletionItem>{};
                                                  items.push_back(rt::CompletionItem{
                                                    .displayText = "$album",
                                                    .insertText = "$album",
                                                    .detail = "",
                                                    .rank = 0,
                                                  });
                                                  return rt::CompletionResult{.replaceBegin = 0,
                                                                              .replaceEnd = std::string{"$al"}.size(),
                                                                              .items = std::move(items)};
                                                }};

    controller.update();
    REQUIRE(emitFocusLeave(entry));
    controller.applySelected();

    CHECK(entry.get_text() == "$al");
  }

  TEST_CASE("EntryCompletionController - dismisses popup on outside press without modal autohide", "[gtk][completion]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto window = Gtk::Window{};
    auto entry = Gtk::Entry{};
    window.set_child(entry);
    entry.set_text("$al");
    entry.set_position(charCount("$al"));

    auto controller = EntryCompletionController{entry,
                                                [](std::string_view, std::size_t) -> std::optional<rt::CompletionResult>
                                                {
                                                  auto items = std::vector<rt::CompletionItem>{};
                                                  items.push_back(rt::CompletionItem{
                                                    .displayText = "$album",
                                                    .insertText = "$album",
                                                    .detail = "",
                                                    .rank = 0,
                                                  });
                                                  return rt::CompletionResult{.replaceBegin = 0,
                                                                              .replaceEnd = std::string{"$al"}.size(),
                                                                              .items = std::move(items)};
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

  TEST_CASE("EntryCompletionController - clears completion state when popover closes", "[gtk][completion]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto window = Gtk::Window{};
    auto entry = Gtk::Entry{};
    window.set_child(entry);
    entry.set_text("$al");
    entry.set_position(charCount("$al"));

    auto controller = EntryCompletionController{entry,
                                                [](std::string_view, std::size_t) -> std::optional<rt::CompletionResult>
                                                {
                                                  auto items = std::vector<rt::CompletionItem>{};
                                                  items.push_back(rt::CompletionItem{
                                                    .displayText = "$album",
                                                    .insertText = "$album",
                                                    .detail = "",
                                                    .rank = 0,
                                                  });
                                                  return rt::CompletionResult{.replaceBegin = 0,
                                                                              .replaceEnd = std::string{"$al"}.size(),
                                                                              .items = std::move(items)};
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
