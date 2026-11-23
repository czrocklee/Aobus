// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackListModel.h"

#include "test/MessageCatalogTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/linux-gtk/GtkApplicationTestSupport.h"
#include "test/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/runtime/source/TrackSourceTestSupport.h"
#include "track/TrackListItem.h"
#include "track/TrackRowCache.h"
#include "track/TrackRowObject.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/TrackSourceDelta.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <giomm/listmodel.h>
#include <glibmm/refptr.h>
#include <gtk/gtk.h>
#include <gtkmm/multiselection.h>
#include <sigc++/functors/mem_fun.h>
#include <sigc++/scoped_connection.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <format>
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

    bool hasValidSectionRanges(GtkSectionModel* model)
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

    Glib::RefPtr<TrackRowObject> trackRowAt(TrackListModel& model, ::guint position)
    {
      auto const itemPtr = std::dynamic_pointer_cast<TrackListItem>(model.get_object(position));
      REQUIRE(itemPtr);
      auto const rowPtr = model.trackRow(itemPtr->trackId());
      REQUIRE(rowPtr);
      return rowPtr;
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

    struct TrackListModelFixture final
    {
      TrackListModelFixture()
        : id1{addRuntimeTrack(runtime, makeTrackSpec("Song A", "Artist A", "Album A", 2020))}
        , id2{addRuntimeTrack(runtime, makeTrackSpec("Song B", "Artist B", "Album B", 2021))}
        , sourcePtr{rt::test::makeMutableTrackSource({id1, id2})}
        , rowCache{runtime.library(), ao::test::englishMessageCatalog()}
        , projectionPtr{runtime.views().createTransientTrackListProjection(rt::TrackSourceLease{sourcePtr})}
        , modelPtr{TrackListModel::create(rowCache)}
      {
        modelPtr->bindProjection(projectionPtr);
        spy.modelPtr = modelPtr;
        spyConnection =
          modelPtr->signal_items_changed().connect(sigc::mem_fun(spy, &SpyTrackListModelEvents::handleItemsChanged));
      }

      GtkRuntimeFixture runtimeFixture{};
      rt::AppRuntime& runtime = runtimeFixture.runtime();
      TrackId id1;
      TrackId id2;
      std::shared_ptr<rt::test::MutableTrackSource> sourcePtr;
      TrackRowCache rowCache;
      std::shared_ptr<rt::TrackListProjection> projectionPtr;
      Glib::RefPtr<TrackListModel> modelPtr;
      SpyTrackListModelEvents spy;
      sigc::scoped_connection spyConnection;
    };
  } // namespace

  TEST_CASE("TrackListModel - exposes projection identity and cached row fields")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = TrackListModelFixture{};

    CHECK(fixture.modelPtr->projection() == fixture.projectionPtr.get());
    CHECK(fixture.modelPtr->indexOf(fixture.id1) == 0);
    CHECK(fixture.modelPtr->indexOf(fixture.id2) == 1);
    CHECK(fixture.modelPtr->get_n_items() == 2);
    CHECK(fixture.modelPtr->get_item_type() == TrackListItem::objectType());

    auto const rowPtr = trackRowAt(*fixture.modelPtr, 0);
    CHECK(rowPtr->trackId() == fixture.id1);
    CHECK(rowPtr->fieldText(rt::TrackField::Artist) == "Artist A");
  }

  TEST_CASE("TrackListModel - reports playing state without structural notifications")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = TrackListModelFixture{};

    SECTION("Setting the same playing track twice emits only one playing-changed signal")
    {
      std::int32_t playingChangedCount = 0;
      auto connection =
        sigc::scoped_connection{fixture.modelPtr->signalPlayingChanged().connect([&] { ++playingChangedCount; })};

      fixture.modelPtr->setPlayingTrackId(fixture.id1);
      CHECK(playingChangedCount == 1);
      CHECK(fixture.spy.events.empty());
      CHECK(fixture.modelPtr->playingTrackId() == fixture.id1);

      fixture.modelPtr->setPlayingTrackId(fixture.id1);
      CHECK(playingChangedCount == 1);
      CHECK(fixture.spy.events.empty());
      CHECK(fixture.modelPtr->playingTrackId() == fixture.id1);
    }

    SECTION("Setting playing track outside the projection emits only the playing signal")
    {
      std::int32_t playingChangedCount = 0;
      fixture.modelPtr->signalPlayingChanged().connect([&] { ++playingChangedCount; });

      fixture.modelPtr->setPlayingTrackId(TrackId{987654});

      CHECK(playingChangedCount == 1);
      CHECK(fixture.spy.events.empty());
      CHECK(fixture.modelPtr->get_n_items() == 2);
      CHECK(fixture.modelPtr->playingTrackId() == TrackId{987654});
    }

    SECTION("Setting playing track before binding a projection records state and emits the signal")
    {
      auto emptyModelPtr = TrackListModel::create(fixture.rowCache);
      std::int32_t playingChangedCount = 0;
      emptyModelPtr->signalPlayingChanged().connect([&] { ++playingChangedCount; });

      emptyModelPtr->setPlayingTrackId(fixture.id1);

      CHECK(playingChangedCount == 1);
      CHECK(emptyModelPtr->playingTrackId() == fixture.id1);
      CHECK(emptyModelPtr->get_n_items() == 0);
    }

    SECTION("Switching the playing track re-emits the signal without replacing the cached row")
    {
      std::int32_t playingChangedCount = 0;
      auto connection =
        sigc::scoped_connection{fixture.modelPtr->signalPlayingChanged().connect([&] { ++playingChangedCount; })};

      auto const retainedRowPtr = trackRowAt(*fixture.modelPtr, 0);
      fixture.modelPtr->setPlayingTrackId(fixture.id1);
      CHECK(playingChangedCount == 1);
      CHECK(trackRowAt(*fixture.modelPtr, 0) == retainedRowPtr);

      fixture.modelPtr->setPlayingTrackId(fixture.id2);
      CHECK(playingChangedCount == 2);
      CHECK(fixture.spy.events.empty());
      CHECK(fixture.modelPtr->playingTrackId() == fixture.id2);
      CHECK(trackRowAt(*fixture.modelPtr, 0) == retainedRowPtr);
    }
  }

  TEST_CASE("TrackListModel - translates projection changes into list-model notifications")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = TrackListModelFixture{};

    SECTION("Delta batch notifications - Insert")
    {
      auto const id3 = addRuntimeTrack(fixture.runtime, makeTrackSpec("Song C", "Artist C", "Album C", 2022));
      fixture.sourcePtr->insert(id3, 0);

      REQUIRE(fixture.spy.events.size() == 1);
      CHECK(fixture.spy.events[0].position == 0);
      CHECK(fixture.spy.events[0].removed == 0);
      CHECK(fixture.spy.events[0].added == 1);
      CHECK(fixture.spy.events[0].sizeDuringEvent == 3);
      CHECK(fixture.modelPtr->get_n_items() == 3);
      CHECK(fixture.modelPtr->indexOf(id3) == 0);
      CHECK(fixture.modelPtr->indexOf(fixture.id1) == 1);
      CHECK(fixture.modelPtr->indexOf(fixture.id2) == 2);
    }

    SECTION("Removing a selected track drops it but keeps the surviving selection")
    {
      auto const selectionPtr = Gtk::MultiSelection::create(fixture.modelPtr);
      REQUIRE(selectionPtr->select_all());
      fixture.sourcePtr->remove(fixture.id1);

      REQUIRE(fixture.spy.events.size() == 1);
      CHECK(fixture.spy.events[0].position == 0);
      CHECK(fixture.spy.events[0].removed == 1);
      CHECK(fixture.spy.events[0].added == 0);
      CHECK(fixture.spy.events[0].sizeDuringEvent == 1);
      CHECK(fixture.modelPtr->get_n_items() == 1);
      CHECK(selectionPtr->get_selection()->get_size() == 1);
      CHECK(fixture.modelPtr->indexOf(fixture.id2) == 0);
      CHECK(selectionPtr->is_selected(0));

      fixture.sourcePtr->insert(fixture.id1, 0);

      CHECK(selectionPtr->get_selection()->get_size() == 1);
      CHECK_FALSE(selectionPtr->is_selected(0));
      CHECK(selectionPtr->is_selected(1));
    }

    SECTION("Delta batch notifications - Update")
    {
      fixture.sourcePtr->update(fixture.id2);

      REQUIRE(fixture.spy.events.size() == 1);
      CHECK(fixture.spy.events[0].position == 1);
      CHECK(fixture.spy.events[0].removed == 1);
      CHECK(fixture.spy.events[0].added == 1);
      CHECK(fixture.spy.events[0].sizeDuringEvent == 2);
    }

    SECTION("Delta batch notifications - Reset")
    {
      fixture.sourcePtr->emitReset();

      REQUIRE(fixture.spy.events.size() == 1);
      CHECK(fixture.spy.events[0].position == 0);
      CHECK(fixture.spy.events[0].removed == 2);
      CHECK(fixture.spy.events[0].added == 2);
      CHECK(fixture.spy.events[0].sizeDuringEvent == 2);
    }

    SECTION("Clearing and unbinding projection")
    {
      auto selectionPtr = Gtk::MultiSelection::create(fixture.modelPtr);
      CHECK(::g_list_model_get_n_items(G_LIST_MODEL(selectionPtr->gobj())) == 2);

      fixture.modelPtr->clearProjection();

      CHECK(fixture.modelPtr->projection() == nullptr);
      CHECK(fixture.modelPtr->get_n_items() == 0);
      CHECK(::g_list_model_get_n_items(G_LIST_MODEL(selectionPtr->gobj())) == 0);
      REQUIRE(fixture.spy.events.size() == 1);
      CHECK(fixture.spy.events.front().position == 0);
      CHECK(fixture.spy.events.front().removed == 2);
      CHECK(fixture.spy.events.front().added == 0);
      CHECK(fixture.spy.events.front().sizeDuringEvent == 0);
      CHECK_FALSE(fixture.spy.events.front().projectionAttachedDuringEvent);
    }
  }

  TEST_CASE("TrackListModel - keeps selection and fresh row text during a cold title reorder")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    constexpr ::guint kTrackCount = 32;
    constexpr ::guint kLastPosition = kTrackCount - 1U;
    constexpr ::guint kMiddlePosition = 16;
    auto const startsAtFront = GENERATE(true, false);
    ::guint const oldPosition = startsAtFront ? 0U : kLastPosition;
    ::guint const newPosition = startsAtFront ? kLastPosition : 0U;
    ::guint const newOtherPosition = startsAtFront ? kMiddlePosition - 1U : kMiddlePosition + 1U;
    auto const newTitle = std::string_view{startsAtFront ? "Track 99" : "Track 00"};
    CAPTURE(oldPosition, newPosition);

    // Each direction starts with a separate runtime, projection, and empty row cache.
    auto trackIds = std::array<TrackId, kTrackCount>{};
    auto fixture = GtkRuntimeFixture{[&trackIds](library::MusicLibrary& storage)
                                     {
                                       for (::guint position = 0; position < kTrackCount; ++position)
                                       {
                                         trackIds[position] =
                                           library::test::addTrack(storage,
                                                                   {.title = std::format("Track {:02}", position + 1),
                                                                    .uri = std::format("track-{}.flac", position + 1)});
                                       }
                                     }};
    auto& runtime = fixture.runtime();
    auto const editedTrackId = trackIds[oldPosition];
    auto const otherTrackId = trackIds[kMiddlePosition];
    auto const viewId = ao::test::requireValue(runtime.workspace().navigate(rt::NavigationRequest{
      .target = rt::GlobalViewKind::AllTracks,
      .optPresentation = rt::NavigationPresentation{.spec = {.sortBy = {{.field = rt::TrackSortField::Title}}}},
    }));
    auto const projectionPtr = ao::test::requireValue(runtime.views().findTrackListProjection(viewId));
    auto rowCache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    auto const modelPtr = TrackListModel::create(rowCache);
    modelPtr->bindProjection(projectionPtr);

    REQUIRE(modelPtr->get_n_items() == kTrackCount);
    auto const selectionPtr = Gtk::MultiSelection::create(modelPtr);
    auto const oldItemPtr = std::dynamic_pointer_cast<TrackListItem>(selectionPtr->get_object(oldPosition));
    auto const otherItemPtr = std::dynamic_pointer_cast<TrackListItem>(selectionPtr->get_object(kMiddlePosition));
    REQUIRE(oldItemPtr);
    REQUIRE(otherItemPtr);
    REQUIRE(oldItemPtr->trackId() == editedTrackId);
    REQUIRE(otherItemPtr->trackId() == otherTrackId);
    REQUIRE(selectionPtr->select_item(oldPosition, true));
    REQUIRE(selectionPtr->select_item(kMiddlePosition, false));
    REQUIRE(selectionPtr->is_selected(oldPosition));
    REQUIRE(selectionPtr->is_selected(kMiddlePosition));
    CHECK(rowCache.rowLoadCount() == 0);

    auto const oldRowPtr = modelPtr->trackRow(oldItemPtr->trackId());
    auto const otherRowPtr = modelPtr->trackRow(otherItemPtr->trackId());
    REQUIRE(oldRowPtr);
    REQUIRE(otherRowPtr);
    REQUIRE(oldRowPtr->fieldText(rt::TrackField::Title).raw() == (startsAtFront ? "Track 01" : "Track 32"));
    REQUIRE(otherRowPtr->fieldText(rt::TrackField::Title) == "Track 17");
    auto const cachedRowsBefore = rowCache.cachedRowCount();
    REQUIRE(cachedRowsBefore == 2);
    auto const rowLoadsBefore = rowCache.rowLoadCount();
    REQUIRE(rowLoadsBefore == 2);

    auto movedItemPtr = Glib::RefPtr<TrackListItem>{};
    auto movedTitle = std::string{};
    auto connection = sigc::scoped_connection{selectionPtr->signal_items_changed().connect(
      [&](::guint position, ::guint, ::guint added)
      {
        if (newPosition >= position && newPosition - position < added)
        {
          auto const itemPtr = std::dynamic_pointer_cast<TrackListItem>(selectionPtr->get_object(newPosition));

          if (!movedItemPtr && itemPtr && itemPtr->trackId() == editedTrackId)
          {
            // GTK has already followed the selection; no field observer has asked for data yet.
            CHECK(rowCache.rowLoadCount() == rowLoadsBefore);
            movedItemPtr = itemPtr;

            if (auto const rowPtr = modelPtr->trackRow(itemPtr->trackId()); rowPtr)
            {
              movedTitle = rowPtr->fieldText(rt::TrackField::Title).raw();
            }
          }
        }
      })};

    updateRuntimeTrack(runtime, editedTrackId, [newTitle](library::test::TrackSpec& spec) { spec.title = newTitle; });

    CHECK(movedItemPtr == oldItemPtr);
    CHECK(movedTitle == newTitle);
    CHECK(selectionPtr->get_selection()->get_size() == 2);
    CHECK(selectionPtr->is_selected(newPosition));
    CHECK(selectionPtr->is_selected(newOtherPosition));
    CHECK(selectionPtr->get_object(newPosition) == oldItemPtr);
    CHECK(selectionPtr->get_object(newOtherPosition) == otherItemPtr);
    CHECK(rowCache.cachedRowCount() == cachedRowsBefore);
    // Only the edited row's explicit request may load a payload, regardless of GTK's identity lookups.
    CHECK(rowCache.rowLoadCount() - rowLoadsBefore <= 1);
  }

  TEST_CASE("TrackListModel - section ranges propagate without materializing rows")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto const albumA1 = addRuntimeTrack(runtime, makeTrackSpec("A1", "Artist", "Album A"));
    auto const albumA2 = addRuntimeTrack(runtime, makeTrackSpec("A2", "Artist", "Album A"));
    auto const albumB = addRuntimeTrack(runtime, makeTrackSpec("B1", "Artist", "Album B"));
    auto sourcePtr = rt::test::makeMutableTrackSource({albumA1, albumA2, albumB});
    auto projectionPtr = std::shared_ptr<rt::TrackListProjection>{
      runtime.views().createTransientTrackListProjection(rt::TrackSourceLease{sourcePtr})};
    auto rowCache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    auto modelPtr = TrackListModel::create(rowCache);
    modelPtr->bindProjection(projectionPtr);

    CHECK(modelPtr->get_item_type() == TrackListItem::objectType());
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

  TEST_CASE("TrackListModel - source invalidation clears rows and detaches the projection")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const firstTrackId = addRuntimeTrack(runtime, makeTrackSpec("First", "Artist", "Album"));
    auto const secondTrackId = addRuntimeTrack(runtime, makeTrackSpec("Second", "Artist", "Album"));
    auto sourcePtr = rt::test::makeMutableTrackSource({firstTrackId, secondTrackId});
    auto rowCache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    auto projectionPtr = std::shared_ptr<rt::TrackListProjection>{
      runtime.views().createTransientTrackListProjection(rt::TrackSourceLease{sourcePtr})};
    auto const modelPtr = TrackListModel::create(rowCache);
    modelPtr->bindProjection(projectionPtr);

    REQUIRE(modelPtr->get_n_items() == 2);

    auto spy = SpyTrackListModelEvents{};
    spy.modelPtr = modelPtr;
    modelPtr->signal_items_changed().connect(sigc::mem_fun(spy, &SpyTrackListModelEvents::handleItemsChanged));

    rt::test::TrackSourceAccess::invalidate(*sourcePtr);

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

    rt::test::TrackSourceAccess::invalidate(*sourcePtr);
    CHECK(spy.events.size() == 1);

    auto const weakProjectionPtr = std::weak_ptr<rt::TrackListProjection>{projectionPtr};
    projectionPtr.reset();

    CHECK_FALSE(weakProjectionPtr.expired());
    drainGtkEvents();
    CHECK(weakProjectionPtr.expired());
  }

  TEST_CASE("TrackListModel - multi-delta batches reset atomically and preserve section invariants")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto const trackA = addRuntimeTrack(runtime, makeTrackSpec("Track 1", "Artist A", "Album A"));
    auto const trackB = addRuntimeTrack(runtime, makeTrackSpec("Track 2", "Artist B", "Album B"));
    auto sourcePtr = rt::test::makeMutableTrackSource({trackA, trackB});
    auto projectionPtr = std::shared_ptr<rt::TrackListProjection>{
      runtime.views().createTransientTrackListProjection(rt::TrackSourceLease{sourcePtr})};
    projectionPtr->setPresentation(rt::TrackPresentationSpec{
      .groupBy = rt::TrackGroupKey::Album,
      .sortBy = {{.field = rt::TrackSortField::Album}},
    });

    auto rowCache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
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
      { sectionValidityDuringEvents.push_back(hasValidSectionRanges(sectionModel)); })};

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

  TEST_CASE("TrackListModel - multi-delta updates refresh cached rows across notification strategies")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto const trackA = addRuntimeTrack(runtime, makeTrackSpec("Track A", "Artist", "Album"));
    auto const trackB = addRuntimeTrack(runtime, makeTrackSpec("Track B", "Artist", "Album"));
    auto const trackC = addRuntimeTrack(runtime, makeTrackSpec("Track C", "Artist", "Album"));
    auto sourcePtr = rt::test::makeMutableTrackSource({trackA, trackB, trackC});
    auto projectionPtr = std::shared_ptr<rt::TrackListProjection>{
      runtime.views().createTransientTrackListProjection(rt::TrackSourceLease{sourcePtr})};
    auto rowCache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    auto modelPtr = TrackListModel::create(rowCache);
    modelPtr->bindProjection(projectionPtr);

    auto const cachedTrackCPtr = trackRowAt(*modelPtr, 2);
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

      auto const refreshedTrackCPtr = trackRowAt(*modelPtr, 2);
      CHECK(refreshedTrackCPtr == cachedTrackCPtr);
      CHECK(cachedTrackCPtr->fieldText(rt::TrackField::Title) == "Updated Track C");
    }

    SECTION("Structural batches reset after refreshing final-coordinate updates")
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

      auto const refreshedTrackCPtr = trackRowAt(*modelPtr, 3);
      CHECK(refreshedTrackCPtr == cachedTrackCPtr);
      CHECK(cachedTrackCPtr->fieldText(rt::TrackField::Title) == "Updated Track C");
    }
  }
} // namespace ao::gtk::test
