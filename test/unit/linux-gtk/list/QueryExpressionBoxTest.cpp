// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "list/QueryExpressionBox.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <giomm/listmodel.h>
#include <gtkmm/window.h>

using namespace ao;
using namespace ao::gtk;
using namespace ao::gtk::test;

TEST_CASE("QueryExpressionBox - smoke test", "[gtk][list][query]")
{
  [[maybe_unused]] auto const app = ensureGtkApplication();
  auto fixture = GtkRuntimeFixture{};
  auto& library = fixture.runtime().musicLibrary();

  auto window = Gtk::Window{};
  auto box = QueryExpressionBox{library};
  window.set_child(box);

  drainGtkEvents();
}
