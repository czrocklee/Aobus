// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackColumnFactoryBuilder.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "track/TrackFieldUi.h"
#include "track/TrackListModel.h"
#include "track/TrackRowCache.h"
#include "track/TrackRowObject.h"
#include <ao/Type.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/lmdb/Transaction.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackListProjection.h>
#include <ao/rt/TrackSource.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/columnview.h>
#include <gtkmm/columnviewcolumn.h>
#include <gtkmm/selectionmodel.h>
#include <gtkmm/singleselection.h>
#include <gtkmm/window.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
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

  TEST_CASE("TrackColumnFactoryBuilder - factory lifecycle", "[gtk][track][column]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& library = fixture.runtime().musicLibrary();
    auto cache = TrackRowCache{library};

    {
      auto window = Gtk::Window{};
      auto columnView = Gtk::ColumnView{};
      window.set_child(columnView);

      auto trackId = TrackId{kInvalidTrackId};
      {
        auto txn = library.writeTransaction();
        auto writer = library.tracks().writer(txn);
        auto builder = library::TrackBuilder::createNew();
        builder.metadata().title("Test Title").artist("Test Artist");
        builder.property().durationMs(120000);
        auto const [hot, cold] = builder.serialize(txn, library.dictionary(), library.resources());
        trackId = writer.createHotCold(hot, cold).first;
        txn.commit();
      }

      auto sourcePtr = std::make_shared<MutableTrackSource>();
      sourcePtr->addInitial(trackId);
      auto projectionPtr = std::make_shared<rt::TrackListProjection>(rt::ViewId{1}, *sourcePtr, library);
      auto modelPtr = TrackListModel::create(cache);
      modelPtr->bindProjection(projectionPtr);

      auto selectionPtr = Gtk::SingleSelection::create(modelPtr);
      columnView.set_model(selectionPtr);

      SECTION("static column (e.g. Duration)")
      {
        auto factoryPtr = buildColumnFactory(rt::TrackField::Duration, [](auto, auto, auto) {});
        auto columnPtr = Gtk::ColumnViewColumn::create("Duration", factoryPtr);
        columnView.append_column(columnPtr);

        drainGtkEvents();

        columnView.set_model(Glib::RefPtr<Gtk::SelectionModel>{});
        drainGtkEvents();
      }

      SECTION("editable column (e.g. Title)")
      {
        auto committedValue = std::string{};
        auto factoryPtr =
          buildColumnFactory(rt::TrackField::Title, [&](auto, auto, auto val) { committedValue = val; });
        auto columnPtr = Gtk::ColumnViewColumn::create("Title", factoryPtr);
        columnView.append_column(columnPtr);

        drainGtkEvents();

        auto rowPtr = cache.trackRow(trackId);
        REQUIRE(rowPtr);

        rowPtr->setPlaying(true);
        drainGtkEvents();

        rowPtr->setPlaying(false);
        drainGtkEvents();

        columnView.set_model(Glib::RefPtr<Gtk::SelectionModel>{});
        drainGtkEvents();
      }

      SECTION("bind all fields")
      {
        for (auto const& def : rt::trackFieldDefinitions())
        {
          auto factoryPtr = buildColumnFactory(def.field, [](auto, auto, auto) {});
          auto columnPtr = Gtk::ColumnViewColumn::create(std::string{def.label}, factoryPtr);
          columnView.append_column(columnPtr);
        }

        drainGtkEvents();

        columnView.set_model(Glib::RefPtr<Gtk::SelectionModel>{});
        drainGtkEvents();
      }

      window.unset_child();
      drainGtkEvents();
    }

    drainGtkEvents();
  }
} // namespace ao::gtk::test
