// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/AppDialog.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/headerbar.h>

namespace ao::gtk::test
{
  TEST_CASE("AppDialog - titlebar buttons", "[gtk][app][dialog]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto dialog = AppDialog{};

    auto* const titlebar = dialog.get_titlebar();
    REQUIRE(titlebar != nullptr);

    auto* const headerBar = dynamic_cast<Gtk::HeaderBar*>(titlebar);
    REQUIRE(headerBar != nullptr);

    CHECK_FALSE(headerBar->get_show_title_buttons());
  }
} // namespace ao::gtk::test
