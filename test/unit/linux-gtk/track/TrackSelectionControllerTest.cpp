// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackSelectionController.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "track/TrackListModel.h"
#include "track/TrackRowCache.h"
#include <ao/Type.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/lmdb/Transaction.h>
#include <ao/rt/TrackListProjection.h>
#include <ao/rt/TrackSource.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/columnview.h>
#include <gtkmm/multiselection.h>
#include <gtkmm/selectionmodel.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <optional>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
    class MutableTrackSource final : public rt::TrackSource
    {
    public:
      void addInitial(TrackId id) { _ids.push_back(id); }
      std::size_t size() const override { return _ids.size(); }
      TrackId trackIdAt(std::size_t index) const override { return _ids.at(index); }
      std::optional<std::size_t> indexOf(TrackId id) const override
      {
        auto it = std::ranges::find(_ids, id);

        if (it == _ids.end())
        {
          return std::nullopt;
        }

        return static_cast<std::size_t>(std::ranges::distance(_ids.begin(), it));
      }

    private:
      std::vector<TrackId> _ids;
    };
  } // namespace

  TEST_CASE("TrackSelectionController - selection management", "[gtk][track][selection]")
  {
    [[maybe_unused]] auto const app = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& library = fixture.runtime().musicLibrary();
    auto cache = TrackRowCache{library};

    auto model = TrackListModel::create(cache);
    auto selectionModel = Gtk::MultiSelection::create(model);

    auto trackId1 = TrackId{kInvalidTrackId};
    auto trackId2 = TrackId{kInvalidTrackId};

    // 1. Add tracks
    {
      auto txn = library.writeTransaction();
      auto writer = library.tracks().writer(txn);

      auto builder1 = library::TrackBuilder::createNew();
      builder1.metadata().title("Track 1");
      auto const [hot1, cold1] = builder1.serialize(txn, library.dictionary(), library.resources());
      trackId1 = writer.createHotCold(hot1, cold1).first;

      auto builder2 = library::TrackBuilder::createNew();
      builder2.metadata().title("Track 2");
      auto const [hot2, cold2] = builder2.serialize(txn, library.dictionary(), library.resources());
      trackId2 = writer.createHotCold(hot2, cold2).first;

      txn.commit();
    }

    // 2. Bind model to a projection
    auto source = std::make_shared<MutableTrackSource>();
    source->addInitial(trackId1);
    source->addInitial(trackId2);
    auto projection = std::make_shared<rt::TrackListProjection>(rt::ViewId{1}, *source, library);
    model->bindProjection(projection);
    drainGtkEvents();

    {
      auto columnView = Gtk::ColumnView{};
      columnView.set_model(selectionModel);

      auto controller = TrackSelectionController{columnView, model, selectionModel};

      SECTION("selection updates")
      {
        CHECK(controller.selectedTrackCount() == 0);

        // Select first track
        selectionModel->select_item(0, true);
        drainGtkEvents();

        CHECK(controller.selectedTrackCount() == 1);
        CHECK(controller.primarySelectedTrackId() == trackId1);
        auto const ids = controller.selectedTrackIds();
        REQUIRE(ids.size() == 1);
        CHECK(ids[0] == trackId1);
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
        auto changed = false;
        controller.signalSelectionChanged().connect([&] { changed = true; });

        selectionModel->select_item(1, true);
        drainGtkEvents();

        CHECK(changed == true);
      }

      columnView.set_model(Glib::RefPtr<Gtk::SelectionModel>{});
      drainGtkEvents();
    }

    drainGtkEvents();
  }
} // namespace ao::gtk::test
