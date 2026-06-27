// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/AppDialog.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/headerbar.h>
#include <gtkmm/label.h>

#include <cstdint>
#include <vector>

namespace ao::gtk::test
{
  TEST_CASE("AppDialog - configures a modal window with custom title buttons", "[gtk][unit][app][dialog]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto dialog = AppDialog{};

    auto* const titlebar = dialog.get_titlebar();
    REQUIRE(titlebar != nullptr);

    auto* const headerBar = dynamic_cast<Gtk::HeaderBar*>(titlebar);
    REQUIRE(headerBar != nullptr);

    CHECK_FALSE(headerBar->get_show_title_buttons());
    CHECK(dialog.get_modal());
  }

  TEST_CASE("AppDialog - action buttons emit configured response ids", "[gtk][unit][app][dialog]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto dialog = AppDialog{};
    auto responses = std::vector<std::int32_t>{};
    dialog.signal_response().connect([&](std::int32_t const id) { responses.push_back(id); });

    auto* const cancelButton = dialog.addCancelAction("Cancel", -6);
    auto* const primaryButton = dialog.addPrimaryAction("Save", -3);
    REQUIRE(cancelButton != nullptr);
    REQUIRE(primaryButton != nullptr);

    CHECK(primaryButton->has_css_class("suggested-action"));
    CHECK_FALSE(cancelButton->has_css_class("suggested-action"));

    emitClicked(*primaryButton);
    emitClicked(*cancelButton);

    REQUIRE(responses.size() == 2U);
    CHECK(responses[0] == -3);
    CHECK(responses[1] == -6);
  }

  TEST_CASE("AppDialog - content replacement detaches previous widget", "[gtk][unit][app][dialog]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto dialog = AppDialog{};
    auto first = Gtk::Label{"First content"};
    auto second = Gtk::Label{"Second content"};

    dialog.setContentWidget(first);
    REQUIRE(first.get_parent() != nullptr);
    CHECK(first.get_hexpand());
    CHECK(first.get_vexpand());
    CHECK(findLabelByText(dialog, "First content") == &first);

    dialog.setContentWidget(second);

    CHECK(first.get_parent() == nullptr);
    REQUIRE(second.get_parent() != nullptr);
    CHECK(second.get_hexpand());
    CHECK(second.get_vexpand());
    CHECK(findLabelByText(dialog, "First content") == nullptr);
    CHECK(findLabelByText(dialog, "Second content") == &second);
  }
} // namespace ao::gtk::test
