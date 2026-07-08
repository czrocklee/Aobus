// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/PlaybackDetailsWidget.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/box.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>

#include <vector>

namespace ao::gtk::test
{
  TEST_CASE("PlaybackDetailsWidget - renders idle stream status", "[gtk][unit][playback]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto widget = PlaybackDetailsWidget{fixture.runtime().playback()};

    auto* const root = dynamic_cast<Gtk::Box*>(&widget.widget());
    REQUIRE(root != nullptr);
    CHECK(root->has_css_class("ao-playback-details"));

    auto const labels = collectAll<Gtk::Label>(*root);
    REQUIRE(labels.size() == 1);
    CHECK_FALSE(labels[0]->get_text().empty());
    CHECK(labels[0]->has_css_class("dim-label"));

    auto const icons = collectAll<Gtk::Image>(*root);
    REQUIRE(icons.size() == 1);
    CHECK_FALSE(icons[0]->get_icon_name().empty());
    CHECK_FALSE(icons[0]->get_visible());
  }
} // namespace ao::gtk::test
