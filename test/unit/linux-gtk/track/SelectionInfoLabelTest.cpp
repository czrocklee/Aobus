// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/SelectionInfoLabel.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/ViewService.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/label.h>

namespace ao::gtk::test
{
  // SelectionInfoLabel self-subscribes to ViewService selection changes and renders the count.
  // The observable contract is the label text: empty when nothing is selected, singular vs plural
  // noun otherwise. We drive real selection changes and read the rendered text back.
  TEST_CASE("SelectionInfoLabel - renders selection count text", "[gtk][track][selection]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto const reply = runtime.views().createView({.listId = rt::kAllTracksListId});
    auto label = SelectionInfoLabel{runtime.views()};
    auto const& text = dynamic_cast<Gtk::Label const&>(label.widget());

    SECTION("starts empty with no selection")
    {
      CHECK(text.get_text().empty());
    }

    SECTION("single selection uses the singular noun")
    {
      runtime.views().setSelection(reply.viewId, {TrackId{1}});
      CHECK(text.get_text() == "1 item selected");
    }

    SECTION("multiple selection uses the plural noun")
    {
      runtime.views().setSelection(reply.viewId, {TrackId{1}, TrackId{2}, TrackId{3}});
      CHECK(text.get_text() == "3 items selected");
    }

    SECTION("clearing the selection restores empty text")
    {
      runtime.views().setSelection(reply.viewId, {TrackId{1}, TrackId{2}});
      REQUIRE(text.get_text() == "2 items selected");

      runtime.views().setSelection(reply.viewId, {});
      CHECK(text.get_text().empty());
    }
  }
} // namespace ao::gtk::test
