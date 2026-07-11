// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/SelectionInfoLabel.h"

#include "test/unit/TestUtils.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/label.h>

namespace ao::gtk::test
{
  TEST_CASE("SelectionInfoLabel - binds selection changes to summary text", "[gtk][unit][track][selection]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto const reply = ao::test::requireValue(runtime.views().createView({.listId = rt::kAllTracksListId}));
    auto label = SelectionInfoLabel{runtime.views()};
    auto const& text = dynamic_cast<Gtk::Label const&>(label.widget());

    CHECK(text.has_css_class("dim-label"));

    auto const emptyText = text.get_text();

    REQUIRE(runtime.views().setSelection(reply.viewId, {TrackId{1}, TrackId{2}}));
    auto const selectedText = text.get_text();
    CHECK(selectedText != emptyText);

    REQUIRE(runtime.views().setSelection(reply.viewId, {}));
    CHECK(text.get_text() == emptyText);
  }
} // namespace ao::gtk::test
