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
#include <gtkmm/gestureclick.h>
#include <gtkmm/label.h>
#include <gtkmm/listview.h>
#include <gtkmm/popover.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <cstddef>
#include <cstdint>
#include <memory>
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

    bool hasCaptureKeyController(Gtk::Widget& widget)
    {
      auto const controllersPtr = widget.observe_controllers();
      REQUIRE(controllersPtr);

      auto const count = controllersPtr->get_n_items();

      for (auto i = 0U; i < count; ++i)
      {
        auto const keyControllerPtr = std::dynamic_pointer_cast<Gtk::EventControllerKey>(controllersPtr->get_object(i));

        if (keyControllerPtr && keyControllerPtr->get_propagation_phase() == Gtk::PropagationPhase::CAPTURE)
        {
          return true;
        }
      }

      return false;
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

    Gtk::Label* findLabelByClass(Gtk::Widget& widget, Glib::ustring const& cssClass)
    {
      if (auto* const label = dynamic_cast<Gtk::Label*>(&widget); label != nullptr && label->has_css_class(cssClass))
      {
        return label;
      }

      for (auto* child = widget.get_first_child(); child != nullptr; child = child->get_next_sibling())
      {
        if (auto* const label = findLabelByClass(*child, cssClass); label != nullptr)
        {
          return label;
        }
      }

      return nullptr;
    }

    void emitFocusLeave(Gtk::Entry& entry)
    {
      auto const controllersPtr = entry.observe_controllers();
      REQUIRE(controllersPtr);

      auto const count = controllersPtr->get_n_items();

      for (auto i = 0U; i < count; ++i)
      {
        auto const focusControllerPtr =
          std::dynamic_pointer_cast<Gtk::EventControllerFocus>(controllersPtr->get_object(i));

        if (focusControllerPtr)
        {
          ::g_signal_emit_by_name(focusControllerPtr->gobj(), "leave");
          return;
        }
      }

      FAIL("EntryCompletionController did not install a focus controller");
    }

    void emitDismissPress(Gtk::Window& window)
    {
      auto const controllersPtr = window.observe_controllers();
      REQUIRE(controllersPtr);

      auto const count = controllersPtr->get_n_items();

      for (auto i = 0U; i < count; ++i)
      {
        auto const gesturePtr = std::dynamic_pointer_cast<Gtk::GestureClick>(controllersPtr->get_object(i));

        if (gesturePtr && gesturePtr->get_propagation_phase() == Gtk::PropagationPhase::CAPTURE)
        {
          ::g_signal_emit_by_name(gesturePtr->gobj(), "pressed", 1, -100.0, -100.0);
          return;
        }
      }

      FAIL("EntryCompletionController did not install an outside click watch");
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

  TEST_CASE("EntryCompletionController - removes its entry controllers on destruction", "[gtk][completion]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto window = Gtk::Window{};
    auto entry = Gtk::Entry{};
    window.set_child(entry);

    auto const controllersPtr = entry.observe_controllers();
    auto const baseline = controllersPtr->get_n_items();

    {
      auto controller = EntryCompletionController{
        entry, [](std::string_view, std::size_t) -> std::optional<rt::CompletionResult> { return std::nullopt; }};

      // The controller installs exactly its key, click, and focus controllers on the borrowed entry.
      CHECK(controllersPtr->get_n_items() == baseline + 3);
    }

    // After destruction the entry is back to baseline: a host may now install a fresh controller
    // (e.g. DetailFieldEditor::setCompletionProvider) on this same entry without the stale lambdas
    // of the destroyed controller firing into freed memory.
    CHECK(controllersPtr->get_n_items() == baseline);
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

  TEST_CASE("EntryCompletionController - intercepts completion keys before widget defaults", "[gtk][completion]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto window = Gtk::Window{};
    auto entry = Gtk::Entry{};
    window.set_child(entry);

    auto controller = EntryCompletionController{
      entry, [](std::string_view, std::size_t) -> std::optional<rt::CompletionResult> { return std::nullopt; }};

    CHECK(hasCaptureKeyController(entry));

    auto* const popover = findCompletionPopover(entry);
    REQUIRE(popover != nullptr);
    CHECK(hasCaptureKeyController(*popover));
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

    auto* const title = findLabelByClass(*popover, "ao-query-completion-row-title");
    REQUIRE(title != nullptr);
    CHECK(title->get_text() == "$album");

    auto* const detail = findLabelByClass(*popover, "ao-query-completion-row-detail");
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

  TEST_CASE("EntryCompletionController - lets popup height follow natural row content", "[gtk][completion]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto window = Gtk::Window{};
    auto entry = Gtk::Entry{};
    window.set_child(entry);
    entry.set_text("$");
    entry.set_position(charCount("$"));

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

                                                  return rt::CompletionResult{
                                                    .replaceBegin = 0,
                                                    .replaceEnd = 1,
                                                    .items = std::move(items),
                                                  };
                                                }};

    auto* const popover = findCompletionPopover(entry);
    REQUIRE(popover != nullptr);

    auto* const scrolledWindow = dynamic_cast<Gtk::ScrolledWindow*>(popover->get_child());
    REQUIRE(scrolledWindow != nullptr);
    CHECK(scrolledWindow->get_min_content_height() == 0);
    CHECK(scrolledWindow->get_max_content_height() == 180);
    CHECK(scrolledWindow->get_propagate_natural_height());
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
    emitFocusLeave(entry);
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

    emitDismissPress(window);

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
    ::g_signal_emit_by_name(popover->gobj(), "closed");
    controller.applySelected();

    CHECK(entry.get_text() == "$al");
  }
} // namespace ao::gtk::test
