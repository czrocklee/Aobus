// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "app/FormBuilder.h"

#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkLayoutTestSupport.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/entry.h>
#include <gtkmm/label.h>

#include <string>

namespace ao::gtk::test
{
  TEST_CASE("FormBoxedList - long labels yield constrained width to the form control",
            "[gtk][regression][form][geometry]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto const longLabel = std::string{"Fréquence d'échantillonnage exceptionnellement longue"};
    auto entry = Gtk::Entry{};
    auto list = FormBoxedList{};
    list.addEntryRow(longLabel, entry);

    auto host = AllocationHost{list};
    host.allocateChild(260, 60);
    drainGtkEvents();

    auto* const label = findLabelByText(list, longLabel);
    REQUIRE(label != nullptr);
    auto const layoutPtr = label->get_layout();
    REQUIRE(layoutPtr != nullptr);
    CHECK(layoutPtr->is_ellipsized());
    CHECK(label->get_tooltip_text() == longLabel);
  }
} // namespace ao::gtk::test
