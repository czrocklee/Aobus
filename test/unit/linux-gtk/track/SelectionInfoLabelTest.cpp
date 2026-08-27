// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/SelectionInfoLabel.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/label.h>

namespace ao::gtk::test
{
  TEST_CASE("SelectionInfoLabel - binds selection changes to summary text", "[gtk][unit][track][selection]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto const reply = ao::test::requireValue(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
    auto label = SelectionInfoLabel{runtime.views(), ao::test::englishMessageCatalog()};
    auto const& text = dynamic_cast<Gtk::Label const&>(label.widget());

    CHECK(text.has_css_class("dim-label"));

    auto const emptyText = text.get_text();

    REQUIRE(runtime.views().setSelection(reply, {TrackId{1}, TrackId{2}}));
    auto const selectedText = text.get_text();
    CHECK(selectedText != emptyText);

    REQUIRE(runtime.views().setSelection(reply, {}));
    CHECK(text.get_text() == emptyText);
  }
} // namespace ao::gtk::test
