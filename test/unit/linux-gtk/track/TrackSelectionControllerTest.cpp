// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackSelectionController.h"

#include "../../TestUtils.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "track/TrackListModel.h"
#include "track/TrackRowCache.h"
#include <ao/Type.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/lmdb/Transaction.h>
#include <ao/rt/projection/TrackListProjection.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/columnview.h>
#include <gtkmm/multiselection.h>
#include <gtkmm/selectionmodel.h>

#include <chrono>
#include <memory>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
  } // namespace

  TEST_CASE("TrackSelectionController - selection management", "[gtk][track][selection]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& library = fixture.runtime().musicLibrary();
    auto cache = TrackRowCache{fixture.runtime().library()};

    auto modelPtr = TrackListModel::create(cache);
    auto selectionModelPtr = Gtk::MultiSelection::create(modelPtr);

    auto trackId1 = TrackId{kInvalidTrackId};
    auto trackId2 = TrackId{kInvalidTrackId};

    // 1. Add tracks
    {
      auto txn = library.writeTransaction();
      auto writer = library.tracks().writer(txn);

      auto builder1 = library::TrackBuilder::createNew();
      builder1.metadata().title("Track 1");
      builder1.property().duration(std::chrono::minutes{2});
      auto serializeResult1 = builder1.serialize(txn, library.dictionary(), library.resources());
      REQUIRE(serializeResult1);
      auto const [hot1, cold1] = *serializeResult1;
      trackId1 = ao::test::requireValue(writer.createHotCold(hot1, cold1)).first;

      auto builder2 = library::TrackBuilder::createNew();
      builder2.metadata().title("Track 2");
      builder2.property().duration(std::chrono::minutes{3});
      auto serializeResult2 = builder2.serialize(txn, library.dictionary(), library.resources());
      REQUIRE(serializeResult2);
      auto const [hot2, cold2] = *serializeResult2;
      trackId2 = ao::test::requireValue(writer.createHotCold(hot2, cold2)).first;

      txn.commit();
    }

    // 2. Bind model to a projection
    auto sourcePtr = std::make_shared<MutableTrackSource>();
    sourcePtr->addInitial(trackId1);
    sourcePtr->addInitial(trackId2);
    auto projectionPtr = std::make_shared<rt::TrackListProjection>(rt::ViewId{1}, *sourcePtr, library);
    modelPtr->bindProjection(projectionPtr);
    drainGtkEvents();

    {
      auto columnView = Gtk::ColumnView{};
      columnView.set_model(selectionModelPtr);

      auto controller = TrackSelectionController{columnView, modelPtr, selectionModelPtr};

      SECTION("selection updates")
      {
        CHECK(controller.selectedTrackCount() == 0);

        // Select first track
        selectionModelPtr->select_item(0, true);
        drainGtkEvents();

        CHECK(controller.selectedTrackCount() == 1);
        CHECK(controller.primarySelectedTrackId() == trackId1);
        auto const ids = controller.selectedTrackIds();
        REQUIRE(ids.size() == 1);
        CHECK(ids[0] == trackId1);
      }

      SECTION("selected row helpers return rows and aggregate duration")
      {
        selectionModelPtr->select_item(0, false);
        selectionModelPtr->select_item(1, false);
        drainGtkEvents();

        auto const rows = controller.selectedRows();
        REQUIRE(rows.size() == 2);
        CHECK(rows[0]->trackId() == trackId1);
        CHECK(rows[1]->trackId() == trackId2);
        CHECK(controller.selectedTracksDuration() == std::chrono::minutes{5});

        auto const visibleIds = controller.visibleTrackIds();
        CHECK(visibleIds == std::vector<TrackId>{trackId1, trackId2});
      }

      SECTION("selectTrack helper")
      {
        controller.selectTrack(trackId2);
        drainGtkEvents();

        CHECK(controller.selectedTrackCount() == 1);
        CHECK(controller.primarySelectedTrackId() == trackId2);
      }

      SECTION("signal propagation")
      {
        bool changed = false;
        controller.signalSelectionChanged().connect([&] { changed = true; });

        selectionModelPtr->select_item(1, true);
        drainGtkEvents();

        CHECK(changed == true);
      }

      columnView.set_model(Glib::RefPtr<Gtk::SelectionModel>{});
      drainGtkEvents();
    }

    drainGtkEvents();
  }
} // namespace ao::gtk::test
