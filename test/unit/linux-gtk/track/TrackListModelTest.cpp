// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackListModel.h"

#include "test/unit/PresentationTextCatalogTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include "track/TrackRowCache.h"
#include "track/TrackRowObject.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/TrackSourceDelta.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <catch2/catch_test_macros.hpp>
#include <giomm/listmodel.h>
#include <glibmm/refptr.h>
#include <gtk/gtk.h>
#include <gtkmm/application.h>
#include <gtkmm/multiselection.h>
#include <sigc++/functors/mem_fun.h>
#include <sigc++/scoped_connection.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
    library::test::TrackSpec makeTrackSpec(std::string_view title,
                                           std::string_view artist,
                                           std::string_view album,
                                           std::uint16_t year = 2020)
    {
      auto spec = library::test::TrackSpec{};
      spec.title = title;
      spec.artist = artist;
      spec.album = album;
      spec.albumArtist = "AlbumArtist";
      spec.genre = "Genre";
      spec.year = year;
      spec.duration = std::chrono::minutes{3};
      return spec;
    }

    std::pair<::guint, ::guint> sectionRangeAt(GtkSectionModel* model, ::guint position)
    {
      ::guint start = 0;
      ::guint end = 0;
      ::gtk_section_model_get_section(model, position, &start, &end);
      return {start, end};
    }

    bool sectionRangesAreValid(GtkSectionModel* model)
    {
      auto const size = ::g_list_model_get_n_items(G_LIST_MODEL(model));

      for (::guint position = 0; position < size; ++position)
      {
        auto const [start, end] = sectionRangeAt(model, position);

        if (start > position || end <= position || end > size)
        {
          return false;
        }
      }

      return true;
    }

    struct SpyTrackListModelEvents final
    {
      struct Event
      {
        ::guint position;
        ::guint removed;
        ::guint added;
        ::guint sizeDuringEvent;
        bool projectionAttachedDuringEvent;
      };

      std::vector<Event> events;
      Glib::RefPtr<TrackListModel> modelPtr;

      void handleItemsChanged(::guint position, ::guint removed, ::guint added)
      {
        events.push_back({position, removed, added, modelPtr->get_n_items(), modelPtr->projection() != nullptr});
      }
    };
  } // namespace

  TEST_CASE("TrackListModel - exposes projection rows and emits playing-track updates", "[gtk][unit][track][adapter]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.list_model_test");
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const& musicLibrary = runtime.musicLibrary();

    auto const id1 = addRuntimeTrack(runtime, makeTrackSpec("Song A", "Artist A", "Album A", 2020));
    auto const id2 = addRuntimeTrack(runtime, makeTrackSpec("Song B", "Artist B", "Album B", 2021));

    auto sourcePtr = std::make_shared<rt::test::MutableTrackSource>();
    sourcePtr->addInitial(id1);
    sourcePtr->addInitial(id2);

    auto rowCache = TrackRowCache{runtime.library(), ao::test::englishPresentationTextCatalog()};
    auto const projectionPtr =
      std::make_shared<rt::TrackListProjection>(rt::ViewId{1}, rt::TrackSourceLease{sourcePtr}, musicLibrary);

    auto const modelPtr = TrackListModel::create(rowCache);
    modelPtr->bindProjection(projectionPtr);

    auto spy = SpyTrackListModelEvents{};
    spy.modelPtr = modelPtr;
    modelPtr->signal_items_changed().connect(sigc::mem_fun(spy, &SpyTrackListModelEvents::handleItemsChanged));

    SECTION("Basic properties and size")
    {
      CHECK(modelPtr->projection() == projectionPtr.get());
      CHECK(modelPtr->indexOf(id1) == 0);
      CHECK(modelPtr->indexOf(id2) == 1);
      CHECK(modelPtr->get_n_items() == 2);
      CHECK(modelPtr->get_item_type() != G_TYPE_INVALID);

      auto const itemPtr = modelPtr->get_object(0);
      REQUIRE(itemPtr != nullptr);
      auto const castRowPtr = std::dynamic_pointer_cast<TrackRowObject>(itemPtr);
      REQUIRE(castRowPtr);
      CHECK(castRowPtr->trackId() == id1);
      CHECK(castRowPtr->fieldText(rt::TrackField::Artist) == "Artist A");
    }

    SECTION("Setting the playing track emits the playing-changed signal, not items_changed")
    {
      std::int32_t playingChangedCount = 0;
      modelPtr->signalPlayingChanged().connect([&] { ++playingChangedCount; });

      CHECK(spy.events.empty());
      modelPtr->setPlayingTrackId(id1);

      // The shared, cached row objects make items_changed a no-op (GTK dedups the
      // rebind), so the highlight is driven by the dedicated signal instead.
      CHECK(spy.events.empty());
      CHECK(playingChangedCount == 1);
      CHECK(modelPtr->playingTrackId() == id1);

      // get_item_vfunc still stamps isPlaying() on the object it hands back, so a
      // freshly bound (scrolled-in) row reflects the current playing track.
      auto const playingRowPtr = std::dynamic_pointer_cast<TrackRowObject>(modelPtr->get_object(0));
      REQUIRE(playingRowPtr);
      CHECK(playingRowPtr->isPlaying());
    }

    SECTION("Setting the same playing track twice is a no-op")
    {
      std::int32_t playingChangedCount = 0;
      modelPtr->signalPlayingChanged().connect([&] { ++playingChangedCount; });

      modelPtr->setPlayingTrackId(id1);
      modelPtr->setPlayingTrackId(id1);

      CHECK(playingChangedCount == 1);
    }

    SECTION("Setting playing track outside the projection emits only the playing signal")
    {
      std::int32_t playingChangedCount = 0;
      modelPtr->signalPlayingChanged().connect([&] { ++playingChangedCount; });

      modelPtr->setPlayingTrackId(TrackId{987654});

      CHECK(playingChangedCount == 1);
      CHECK(spy.events.empty());
      CHECK(modelPtr->get_n_items() == 2);
      CHECK(modelPtr->playingTrackId() == TrackId{987654});
    }

    SECTION("Setting playing track before binding a projection records state and emits the signal")
    {
      auto emptyModelPtr = TrackListModel::create(rowCache);
      std::int32_t playingChangedCount = 0;
      emptyModelPtr->signalPlayingChanged().connect([&] { ++playingChangedCount; });

      emptyModelPtr->setPlayingTrackId(id1);

      CHECK(playingChangedCount == 1);
      CHECK(emptyModelPtr->playingTrackId() == id1);
      CHECK(emptyModelPtr->get_n_items() == 0);
    }

    SECTION("Switching the playing track re-emits the signal and restamps both rows")
    {
      std::int32_t playingChangedCount = 0;
      modelPtr->signalPlayingChanged().connect([&] { ++playingChangedCount; });

      modelPtr->setPlayingTrackId(id1);
      modelPtr->setPlayingTrackId(id2);

      CHECK(playingChangedCount == 2);
      CHECK(spy.events.empty());
      CHECK(modelPtr->playingTrackId() == id2);

      auto const oldRowPtr = std::dynamic_pointer_cast<TrackRowObject>(modelPtr->get_object(0));
      auto const newRowPtr = std::dynamic_pointer_cast<TrackRowObject>(modelPtr->get_object(1));
      REQUIRE(oldRowPtr);
      REQUIRE(newRowPtr);
      CHECK_FALSE(oldRowPtr->isPlaying());
      CHECK(newRowPtr->isPlaying());
    }

    SECTION("Delta batch notifications - Insert")
    {
      auto const id3 = addRuntimeTrack(runtime, makeTrackSpec("Song C", "Artist C", "Album C", 2022));
      sourcePtr->insert(id3, 0);

      REQUIRE(spy.events.size() == 1);
      CHECK(spy.events[0].position == 0);
      CHECK(spy.events[0].removed == 0);
      CHECK(spy.events[0].added == 1);
      CHECK(spy.events[0].sizeDuringEvent == 3);
      CHECK(modelPtr->get_n_items() == 3);
      CHECK(modelPtr->indexOf(id3) == 0);
      CHECK(modelPtr->indexOf(id1) == 1);
      CHECK(modelPtr->indexOf(id2) == 2);
    }

    SECTION("Delta batch notifications - Remove")
    {
      sourcePtr->remove(id1);

      REQUIRE(spy.events.size() == 1);
      CHECK(spy.events[0].position == 0);
      CHECK(spy.events[0].removed == 1);
      CHECK(spy.events[0].added == 0);
      CHECK(spy.events[0].sizeDuringEvent == 1);
      CHECK(modelPtr->get_n_items() == 1);
    }

    SECTION("Delta batch notifications - Update")
    {
      sourcePtr->update(id2);

      REQUIRE(spy.events.size() == 1);
      CHECK(spy.events[0].position == 1);
      CHECK(spy.events[0].removed == 1);
      CHECK(spy.events[0].added == 1);
      CHECK(spy.events[0].sizeDuringEvent == 2);
    }

    SECTION("Delta batch notifications - Reset")
    {
      sourcePtr->emitReset();

      REQUIRE(spy.events.size() == 1);
      CHECK(spy.events[0].position == 0);
      CHECK(spy.events[0].removed == 2);
      CHECK(spy.events[0].added == 2);
      CHECK(spy.events[0].sizeDuringEvent == 2);
    }

    SECTION("Clearing and unbinding projection")
    {
      modelPtr->clearProjection();
      CHECK(modelPtr->projection() == nullptr);
      CHECK(modelPtr->get_n_items() == 0);
    }
  }

  TEST_CASE("TrackListModel - section ranges propagate without materializing rows", "[gtk][regression][track-model]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const& library = runtime.musicLibrary();

    auto const albumA1 = addRuntimeTrack(runtime, makeTrackSpec("A1", "Artist", "Album A"));
    auto const albumA2 = addRuntimeTrack(runtime, makeTrackSpec("A2", "Artist", "Album A"));
    auto const albumB = addRuntimeTrack(runtime, makeTrackSpec("B1", "Artist", "Album B"));
    auto sourcePtr = rt::test::makeMutableTrackSource({albumA1, albumA2, albumB});
    auto projectionPtr =
      std::make_shared<rt::TrackListProjection>(rt::ViewId{1}, rt::TrackSourceLease{sourcePtr}, library);
    auto rowCache = TrackRowCache{runtime.library(), ao::test::englishPresentationTextCatalog()};
    auto modelPtr = TrackListModel::create(rowCache);
    modelPtr->bindProjection(projectionPtr);

    CHECK(modelPtr->get_item_type() == TrackRowObject::objectType());
    CHECK(rowCache.cachedRowCount() == 0);
    CHECK(modelPtr->get_section(0) == std::make_pair(::guint{0}, ::guint{3}));
    CHECK(modelPtr->get_section(3) == std::make_pair(::guint{3}, ::guint{G_MAXUINT}));

    projectionPtr->setPresentation(rt::TrackPresentationSpec{
      .groupBy = rt::TrackGroupKey::Album,
      .sortBy = {{.field = rt::TrackSortField::Album}},
    });
    auto selectionPtr = Gtk::MultiSelection::create(modelPtr);
    auto* const sectionModel = GTK_SECTION_MODEL(selectionPtr->gobj());
    REQUIRE(GTK_IS_SECTION_MODEL(sectionModel));

    CHECK(sectionRangeAt(sectionModel, 0) == std::make_pair(::guint{0}, ::guint{2}));
    CHECK(sectionRangeAt(sectionModel, 1) == std::make_pair(::guint{0}, ::guint{2}));
    CHECK(sectionRangeAt(sectionModel, 2) == std::make_pair(::guint{2}, ::guint{3}));

    auto const albumA3 = addRuntimeTrack(runtime, makeTrackSpec("A3", "Artist", "Album A"));
    sourcePtr->append(albumA3);
    CHECK(sectionRangeAt(sectionModel, 0) == std::make_pair(::guint{0}, ::guint{3}));
    CHECK(sectionRangeAt(sectionModel, 3) == std::make_pair(::guint{3}, ::guint{4}));

    sourcePtr->update(albumA3);
    CHECK(sectionRangeAt(sectionModel, 2) == std::make_pair(::guint{0}, ::guint{3}));

    sourcePtr->remove(albumA3);
    CHECK(sectionRangeAt(sectionModel, 0) == std::make_pair(::guint{0}, ::guint{2}));
    CHECK(sectionRangeAt(sectionModel, 2) == std::make_pair(::guint{2}, ::guint{3}));

    auto const albumC = addRuntimeTrack(runtime, makeTrackSpec("C1", "Artist", "Album C"));
    sourcePtr->append(albumC);
    CHECK(sectionRangeAt(sectionModel, 3) == std::make_pair(::guint{3}, ::guint{4}));

    sourcePtr->remove(albumC);
    CHECK(sectionRangeAt(sectionModel, 2) == std::make_pair(::guint{2}, ::guint{3}));
    CHECK(rowCache.cachedRowCount() == 0);
  }

  TEST_CASE("TrackListModel - source invalidation clears rows and detaches the projection",
            "[gtk][regression][track-model]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const firstTrackId = addRuntimeTrack(runtime, makeTrackSpec("First", "Artist", "Album"));
    auto const secondTrackId = addRuntimeTrack(runtime, makeTrackSpec("Second", "Artist", "Album"));
    auto sourcePtr = rt::test::makeMutableTrackSource({firstTrackId, secondTrackId});
    auto rowCache = TrackRowCache{runtime.library(), ao::test::englishPresentationTextCatalog()};
    auto projectionPtr =
      std::make_shared<rt::TrackListProjection>(rt::ViewId{1}, rt::TrackSourceLease{sourcePtr}, runtime.musicLibrary());
    auto const modelPtr = TrackListModel::create(rowCache);
    modelPtr->bindProjection(projectionPtr);

    REQUIRE(modelPtr->get_n_items() == 2);

    auto spy = SpyTrackListModelEvents{};
    spy.modelPtr = modelPtr;
    modelPtr->signal_items_changed().connect(sigc::mem_fun(spy, &SpyTrackListModelEvents::handleItemsChanged));

    sourcePtr->invalidate();

    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events[0].position == 0);
    CHECK(spy.events[0].removed == 2);
    CHECK(spy.events[0].added == 0);
    CHECK(spy.events[0].sizeDuringEvent == 0);
    CHECK_FALSE(spy.events[0].projectionAttachedDuringEvent);
    CHECK(modelPtr->get_n_items() == 0);
    CHECK(modelPtr->projection() == nullptr);
    CHECK_FALSE(modelPtr->indexOf(firstTrackId));
    CHECK(modelPtr->get_object(0) == nullptr);

    sourcePtr->invalidate();
    CHECK(spy.events.size() == 1);

    auto const weakProjectionPtr = std::weak_ptr<rt::TrackListProjection>{projectionPtr};
    projectionPtr.reset();

    CHECK_FALSE(weakProjectionPtr.expired());
    drainGtkEvents();
    CHECK(weakProjectionPtr.expired());
  }

  TEST_CASE("TrackListModel - multi-delta batches reset atomically and preserve section invariants",
            "[gtk][regression][track-model]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const& library = runtime.musicLibrary();

    auto const trackA = addRuntimeTrack(runtime, makeTrackSpec("Track 1", "Artist A", "Album A"));
    auto const trackB = addRuntimeTrack(runtime, makeTrackSpec("Track 2", "Artist B", "Album B"));
    auto sourcePtr = rt::test::makeMutableTrackSource({trackA, trackB});
    auto projectionPtr =
      std::make_shared<rt::TrackListProjection>(rt::ViewId{1}, rt::TrackSourceLease{sourcePtr}, library);
    projectionPtr->setPresentation(rt::TrackPresentationSpec{
      .groupBy = rt::TrackGroupKey::Album,
      .sortBy = {{.field = rt::TrackSortField::Album}},
    });

    auto rowCache = TrackRowCache{runtime.library(), ao::test::englishPresentationTextCatalog()};
    auto modelPtr = TrackListModel::create(rowCache);
    modelPtr->bindProjection(projectionPtr);

    auto selectionPtr = Gtk::MultiSelection::create(modelPtr);
    auto* const sectionModel = GTK_SECTION_MODEL(selectionPtr->gobj());
    REQUIRE(GTK_IS_SECTION_MODEL(sectionModel));

    auto spy = SpyTrackListModelEvents{};
    spy.modelPtr = modelPtr;
    auto spyConnection = sigc::scoped_connection{
      modelPtr->signal_items_changed().connect(sigc::mem_fun(spy, &SpyTrackListModelEvents::handleItemsChanged))};

    auto projectionBatches = std::vector<rt::TrackListProjectionDeltaBatch>{};
    auto projectionSubscription = projectionPtr->subscribe(
      [&projectionBatches](rt::TrackListProjectionDeltaBatch const& batch) { projectionBatches.push_back(batch); });
    projectionBatches.clear();

    auto sectionValidityDuringEvents = std::vector<bool>{};
    auto sectionConnection = sigc::scoped_connection{modelPtr->signal_items_changed().connect(
      [&sectionValidityDuringEvents, sectionModel](::guint, ::guint, ::guint)
      { sectionValidityDuringEvents.push_back(sectionRangesAreValid(sectionModel)); })};

    // Simulate multi-track addition during library scan (which causes multiple edit ranges in sorted projection)
    auto const newTrackA = addRuntimeTrack(runtime, makeTrackSpec("Track 0", "Artist A", "Album A"));
    auto const newTrackB = addRuntimeTrack(runtime, makeTrackSpec("Track 3", "Artist B", "Album B"));

    sourcePtr->batchInsert(std::array{newTrackA, newTrackB});

    REQUIRE(projectionBatches.size() == 1);
    REQUIRE(projectionBatches.front().deltas.size() == 2);
    REQUIRE(std::holds_alternative<rt::ProjectionInsertRange>(projectionBatches.front().deltas[0]));
    REQUIRE(std::holds_alternative<rt::ProjectionInsertRange>(projectionBatches.front().deltas[1]));
    auto const& firstInsert = std::get<rt::ProjectionInsertRange>(projectionBatches.front().deltas[0]);
    auto const& secondInsert = std::get<rt::ProjectionInsertRange>(projectionBatches.front().deltas[1]);
    CHECK(firstInsert.range.start == 1);
    CHECK(firstInsert.range.count == 1);
    CHECK(secondInsert.range.start == 3);
    CHECK(secondInsert.range.count == 1);

    CHECK(modelPtr->get_n_items() == 4);
    REQUIRE(spy.events.size() == 1);
    CHECK(spy.events.front().position == 0);
    CHECK(spy.events.front().removed == 2);
    CHECK(spy.events.front().added == 4);
    CHECK(spy.events.front().sizeDuringEvent == 4);
    CHECK(spy.events.front().projectionAttachedDuringEvent);

    REQUIRE(sectionValidityDuringEvents.size() == 1);
    CHECK(sectionValidityDuringEvents.front());

    // Verify all section ranges remain valid
    for (::guint i = 0; i < 4; ++i)
    {
      auto const range = sectionRangeAt(sectionModel, i);
      CHECK(range.first <= i);
      CHECK(range.second > i);
      CHECK(range.second <= 4);
    }
  }

  TEST_CASE("TrackListModel - multi-delta updates refresh cached rows across notification strategies",
            "[gtk][regression][track-model]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto const trackA = addRuntimeTrack(runtime, makeTrackSpec("Track A", "Artist", "Album"));
    auto const trackB = addRuntimeTrack(runtime, makeTrackSpec("Track B", "Artist", "Album"));
    auto const trackC = addRuntimeTrack(runtime, makeTrackSpec("Track C", "Artist", "Album"));
    auto sourcePtr = rt::test::makeMutableTrackSource({trackA, trackB, trackC});
    auto projectionPtr =
      std::make_shared<rt::TrackListProjection>(rt::ViewId{1}, rt::TrackSourceLease{sourcePtr}, runtime.musicLibrary());
    auto rowCache = TrackRowCache{runtime.library(), ao::test::englishPresentationTextCatalog()};
    auto modelPtr = TrackListModel::create(rowCache);
    modelPtr->bindProjection(projectionPtr);

    auto const cachedTrackCPtr = std::dynamic_pointer_cast<TrackRowObject>(modelPtr->get_object(2));
    REQUIRE(cachedTrackCPtr);
    CHECK(cachedTrackCPtr->fieldText(rt::TrackField::Title) == "Track C");

    updateRuntimeTrack(runtime, trackC, [](library::test::TrackSpec& spec) { spec.title = "Updated Track C"; });
    CHECK(cachedTrackCPtr->fieldText(rt::TrackField::Title) == "Track C");

    auto projectionBatches = std::vector<rt::TrackListProjectionDeltaBatch>{};
    auto projectionSubscription = projectionPtr->subscribe(
      [&projectionBatches](rt::TrackListProjectionDeltaBatch const& batch) { projectionBatches.push_back(batch); });
    projectionBatches.clear();

    auto spy = SpyTrackListModelEvents{};
    spy.modelPtr = modelPtr;
    auto spyConnection = sigc::scoped_connection{
      modelPtr->signal_items_changed().connect(sigc::mem_fun(spy, &SpyTrackListModelEvents::handleItemsChanged))};

    SECTION("Update-only batches retain targeted notifications")
    {
      sourcePtr->batchUpdate(std::array{trackA, trackC});

      REQUIRE(projectionBatches.size() == 1);
      REQUIRE(projectionBatches.front().deltas.size() == 2);
      CHECK(std::holds_alternative<rt::ProjectionUpdateRange>(projectionBatches.front().deltas[0]));
      CHECK(std::holds_alternative<rt::ProjectionUpdateRange>(projectionBatches.front().deltas[1]));

      REQUIRE(spy.events.size() == 2);
      CHECK(spy.events[0].position == 0);
      CHECK(spy.events[0].removed == 1);
      CHECK(spy.events[0].added == 1);
      CHECK(spy.events[0].sizeDuringEvent == 3);
      CHECK(spy.events[1].position == 2);
      CHECK(spy.events[1].removed == 1);
      CHECK(spy.events[1].added == 1);
      CHECK(spy.events[1].sizeDuringEvent == 3);

      auto const refreshedTrackCPtr = std::dynamic_pointer_cast<TrackRowObject>(modelPtr->get_object(2));
      REQUIRE(refreshedTrackCPtr);
      CHECK(refreshedTrackCPtr != cachedTrackCPtr);
      CHECK(refreshedTrackCPtr->fieldText(rt::TrackField::Title) == "Updated Track C");
    }

    SECTION("Structural batches reset after invalidating final-coordinate updates")
    {
      auto const insertedTrack = addRuntimeTrack(runtime, makeTrackSpec("Inserted", "Artist", "Album"));
      auto const finalTrackIds = std::array{trackA, insertedTrack, trackB, trackC};
      sourcePtr->replaceWithBatch(finalTrackIds,
                                  rt::TrackSourceDelta{rt::delta::RegularTrackEditScript{
                                    .edits =
                                      {
                                        rt::delta::InsertRange{.start = 1, .trackIds = {insertedTrack}},
                                        rt::delta::UpdateRange{.start = 3, .trackIds = {trackC}},
                                      },
                                  }});

      REQUIRE(projectionBatches.size() == 1);
      REQUIRE(projectionBatches.front().deltas.size() == 2);
      CHECK(std::holds_alternative<rt::ProjectionInsertRange>(projectionBatches.front().deltas[0]));
      CHECK(std::holds_alternative<rt::ProjectionUpdateRange>(projectionBatches.front().deltas[1]));

      REQUIRE(spy.events.size() == 1);
      CHECK(spy.events.front().position == 0);
      CHECK(spy.events.front().removed == 3);
      CHECK(spy.events.front().added == 4);
      CHECK(spy.events.front().sizeDuringEvent == 4);

      auto const refreshedTrackCPtr = std::dynamic_pointer_cast<TrackRowObject>(modelPtr->get_object(3));
      REQUIRE(refreshedTrackCPtr);
      CHECK(refreshedTrackCPtr != cachedTrackCPtr);
      CHECK(refreshedTrackCPtr->fieldText(rt::TrackField::Title) == "Updated Track C");
    }
  }
} // namespace ao::gtk::test
