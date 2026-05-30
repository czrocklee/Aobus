// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "list/ListNavigationController.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "track/TrackRowCache.h"
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/lmdb/Transaction.h>
#include <ao/rt/TrackSource.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/window.h>

#include <utility>

namespace ao::gtk::test
{
  TEST_CASE("ListNavigationController - basic interactions", "[gtk][list][controller]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto window = Gtk::Window{};
    auto cache = TrackRowCache{fixture.runtime().musicLibrary()};

    auto selectedId = ListId{999};
    auto callbacks =
      ListNavigationController::Callbacks{.onListSelected = [&](ListId id) { selectedId = id; },
                                          .getListMembership = [&](ListId) -> rt::TrackSource* { return nullptr; },
                                          .onListPresentationSaved = {},
                                          .getListPresentation = {}};

    auto controller = ListNavigationController{window, fixture.runtime(), std::move(callbacks)};
    window.set_child(controller.widget());

    SECTION("rebuildTree populates the navigation panel")
    {
      {
        auto txn = fixture.runtime().musicLibrary().writeTransaction();
        auto writer = fixture.runtime().musicLibrary().lists().writer(txn);
        auto builder = library::ListBuilder::createNew();
        builder.name("Test List");
        writer.create(builder.serialize());
        txn.commit();
      }

      auto txn = fixture.runtime().musicLibrary().readTransaction();
      controller.rebuildTree(cache, txn);
      drainGtkEvents();

      // Navigation panel should contain "All Tracks" and "Test List"
    }

    SECTION("select triggers callback")
    {
      auto testListId = ListId{0};
      {
        auto txn = fixture.runtime().musicLibrary().writeTransaction();
        auto writer = fixture.runtime().musicLibrary().lists().writer(txn);
        auto builder = library::ListBuilder::createNew();
        builder.name("Select Target");
        auto [id, _] = writer.create(builder.serialize());
        testListId = id;
        txn.commit();
      }

      auto txn = fixture.runtime().musicLibrary().readTransaction();
      controller.rebuildTree(cache, txn);
      drainGtkEvents();

      controller.select(testListId);
      drainGtkEvents();

      CHECK(selectedId == testListId);
    }
  }
} // namespace ao::gtk::test
