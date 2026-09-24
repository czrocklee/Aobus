// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/SelectionInfoLabel.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryCommands.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/label.h>

#include <chrono>

namespace ao::gtk::test
{
  TEST_CASE("SelectionInfoLabel - binds selection changes to summary text", "[gtk][unit][track][selection]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto const reply = ao::test::requireValue(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
    auto label = SelectionInfoLabel{runtime.views(), runtime.workspace(), ao::test::englishMessageCatalog()};
    auto const& text = dynamic_cast<Gtk::Label const&>(label.widget());

    CHECK(text.has_css_class("dim-label"));

    auto const emptyText = text.get_text();

    REQUIRE(runtime.views().setSelection(reply, {TrackId{1}, TrackId{2}}));
    auto const selectedText = text.get_text();
    CHECK(selectedText != emptyText);

    REQUIRE(runtime.views().setSelection(reply, {}));
    CHECK(text.get_text() == emptyText);
  }

  TEST_CASE("SelectionInfoLabel - follows only the active runtime view", "[gtk][unit][track][selection]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto firstTrackId = kInvalidTrackId;
    auto secondTrackId = kInvalidTrackId;
    auto fixture =
      GtkRuntimeFixture{[&](library::MusicLibrary& musicLibrary)
                        {
                          firstTrackId = library::test::addTrackWithUniqueFixtureUri(
                            musicLibrary, {.title = "First selection", .duration = std::chrono::minutes{2}});
                          secondTrackId = library::test::addTrackWithUniqueFixtureUri(
                            musicLibrary, {.title = "Second selection", .duration = std::chrono::minutes{3}});
                        }};
    auto& runtime = fixture.runtime();
    REQUIRE(firstTrackId != kInvalidTrackId);
    REQUIRE(secondTrackId != kInvalidTrackId);
    auto const listId = ao::test::requireValue(
      runGtkTask(runtime, runtime.library().commands().createListAsync(rt::ListDraft{.name = "Second view"})));
    auto const firstViewId = ao::test::requireValue(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
    auto const secondViewId = ao::test::requireValue(runtime.workspace().navigate({.target = listId}));
    REQUIRE(firstViewId != secondViewId);

    auto label = SelectionInfoLabel{runtime.views(), runtime.workspace(), ao::test::englishMessageCatalog()};
    auto const& text = dynamic_cast<Gtk::Label const&>(label.widget());
    CHECK(text.get_text().empty());

    REQUIRE(runtime.views().setSelection(secondViewId, {secondTrackId}));
    CHECK(text.get_text() == "1 item selected (3:00)");

    {
      auto initialLabel = SelectionInfoLabel{runtime.views(), runtime.workspace(), ao::test::englishMessageCatalog()};
      auto const& initialText = dynamic_cast<Gtk::Label const&>(initialLabel.widget());
      CHECK(initialText.get_text() == "1 item selected (3:00)");
    }

    REQUIRE(runtime.workspace().focusView(firstViewId));
    drainGtkEvents();
    CHECK(text.get_text().empty());

    REQUIRE(runtime.views().setSelection(firstViewId, {firstTrackId}));
    CHECK(text.get_text() == "1 item selected (2:00)");

    // A background view may change without replacing the active view's summary.
    REQUIRE(runtime.views().setSelection(secondViewId, {firstTrackId, secondTrackId}));
    CHECK(text.get_text() == "1 item selected (2:00)");

    REQUIRE(runtime.workspace().focusView(secondViewId));
    drainGtkEvents();
    CHECK(text.get_text() == "2 items selected (5:00)");
  }
} // namespace ao::gtk::test
