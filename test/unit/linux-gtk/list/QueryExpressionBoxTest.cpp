// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "list/QueryExpressionBox.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/window.h>

namespace ao::gtk::test
{
  // Wiring smoke only. Completion routing (text change -> provider -> popup) is
  // covered by EntryCompletionControllerTest; the completer logic by
  // CompletionServiceTest.
  TEST_CASE("QueryExpressionBox - wires the query entry into the box", "[gtk][unit][list][query]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};

    auto window = Gtk::Window{};
    auto box = QueryExpressionBox{fixture.runtime().completion()};
    window.set_child(box);
    drainGtkEvents();

    auto& entry = box.entry();
    CHECK(entry.get_parent() == &box);
    CHECK(entry.get_hexpand());
    CHECK(hasCssClass(entry, "ao-query-expression-entry"));
  }
} // namespace ao::gtk::test
