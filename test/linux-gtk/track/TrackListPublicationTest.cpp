// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/MessageCatalogTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/linux-gtk/GtkApplicationTestSupport.h"
#include "test/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/linux-gtk/GtkWidgetTestSupport.h"
#include "test/runtime/source/TrackSourceTestSupport.h"
#include "track/TrackColumnFactoryBuilder.h"
#include "track/TrackListItem.h"
#include "track/TrackListModel.h"
#include "track/TrackRowCache.h"
#include "track/TrackRowObject.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibraryJobs.h>
#include <ao/rt/library/LibraryTransfer.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <glib.h>
#include <glibmm/refptr.h>
#include <gtkmm/columnview.h>
#include <gtkmm/columnviewcolumn.h>
#include <gtkmm/label.h>
#include <gtkmm/noselection.h>
#include <gtkmm/window.h>
#include <sigc++/scoped_connection.h>

#include <array>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
    Glib::RefPtr<TrackListModel> openTrackList(rt::AppRuntime& runtime,
                                               TrackRowCache const& rowCache,
                                               rt::NavigationRequest const& request)
    {
      auto const viewId = ao::test::requireValue(runtime.workspace().navigate(request));
      auto const modelPtr = TrackListModel::create(rowCache);
      modelPtr->bindProjection(ao::test::requireValue(runtime.views().findTrackListProjection(viewId)));
      return modelPtr;
    }

    void editTitle(rt::AppRuntimeOwner& owner, std::span<TrackId const> trackIds, std::string_view title)
    {
      auto& runtime = owner.runtime();
      auto const targets = ao::test::requireValue(runtime.library().bindTrackTargets(trackIds));
      auto const editRes = runGtkTask(
        runtime,
        owner.libraryCommands().updateMetadataAsync(targets, rt::MetadataPatch{.optTitle = std::string{title}}));
      REQUIRE(editRes);
      REQUIRE(editRes->status == rt::AuthoringStatus::Applied);
    }

    Glib::RefPtr<TrackListItem> itemAt(TrackListModel& model, ::guint position)
    {
      return std::dynamic_pointer_cast<TrackListItem>(model.get_object(position));
    }

    std::string readTitle(TrackRowObject const& row)
    {
      return row.fieldText(rt::TrackField::Title).raw();
    }

    struct PublishedRow final
    {
      TrackId id = kInvalidTrackId;
      std::string title;
      std::string boundTitle;
    };

    sigc::scoped_connection observePublishedRows(TrackListModel& model,
                                                 TrackId trackId,
                                                 std::vector<PublishedRow>& rows,
                                                 std::string const& boundTitle)
    {
      return sigc::scoped_connection{model.signal_items_changed().connect(
        [&model, trackId, &rows, &boundTitle](::guint position, ::guint, ::guint added)
        {
          auto const optIndex = model.indexOf(trackId);

          if (!optIndex || *optIndex < position || *optIndex - position >= added)
          {
            return;
          }

          auto& published = rows.emplace_back();
          // A metadata lookup can refresh the row itself. Sample the existing binding first.
          published.boundTitle = boundTitle;

          auto const itemPtr = itemAt(model, static_cast<::guint>(*optIndex));

          if (!itemPtr)
          {
            return;
          }

          published.id = itemPtr->trackId();

          if (auto const rowPtr = model.trackRow(published.id); rowPtr)
          {
            published.title = readTitle(*rowPtr);
          }
        })};
    }
  } // namespace

  TEST_CASE("TrackListModel - publishes fresh title when a grouped reorder changes the heading")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const editedId = addRuntimeTrack(
      fixture.owner(), library::test::TrackSpec{.title = "Bravo", .artist = "Aimer", .album = "Mixtape"});
    auto const observedId = addRuntimeTrack(
      fixture.owner(), library::test::TrackSpec{.title = "Charlie", .artist = "AIMER", .album = "Mixtape"});
    auto const unobservedId = addRuntimeTrack(
      fixture.owner(), library::test::TrackSpec{.title = "Delta", .artist = "AIMER", .album = "Mixtape"});

    auto rowCache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    auto const modelPtr =
      openTrackList(runtime,
                    rowCache,
                    {.target = rt::GlobalViewKind::AllTracks,
                     .optPresentation = rt::NavigationPresentation{
                       .spec = rt::TrackPresentationSpec{
                         .groupBy = rt::TrackGroupKey::Album, .sortBy = {{.field = rt::TrackSortField::Title}}}}});
    auto const& projection = *modelPtr->projection();
    REQUIRE(modelPtr->get_n_items() == 3);
    REQUIRE(projection.groupCount() == 1);
    REQUIRE(projection.groupAt(0).heading.secondary == rt::TrackGroupHeadingValue{"Aimer"});
    auto const originalItemPtr = itemAt(*modelPtr, 0);
    REQUIRE(originalItemPtr);
    REQUIRE(originalItemPtr->trackId() == editedId);
    auto const cachedRowPtr = modelPtr->trackRow(originalItemPtr->trackId());
    REQUIRE(cachedRowPtr);
    auto boundTitle = readTitle(*cachedRowPtr);
    REQUIRE(boundTitle == "Bravo");
    auto const fieldsConnection = sigc::scoped_connection{
      cachedRowPtr->signalFieldsChanged().connect([&] { boundTitle = readTitle(*cachedRowPtr); })};

    auto const observedRowPtr = modelPtr->trackRow(observedId);
    REQUIRE(observedRowPtr);
    REQUIRE(readTitle(*observedRowPtr) == "Charlie");
    bool unchangedRowNotified = false;
    auto const observedFieldsConnection = sigc::scoped_connection{
      observedRowPtr->signalFieldsChanged().connect([&unchangedRowNotified] { unchangedRowNotified = true; })};
    {
      auto const rowPtr = modelPtr->trackRow(unobservedId);
      REQUIRE(rowPtr);
      REQUIRE(readTitle(*rowPtr) == "Delta");
    }

    auto projectionBatches = std::vector<rt::TrackListProjectionDeltaBatch>{};
    auto const projectionSub = projection.subscribe([&projectionBatches](rt::TrackListProjectionDeltaBatch const& batch)
                                                    { projectionBatches.push_back(batch); });
    projectionBatches.clear();
    auto publishedRows = std::vector<PublishedRow>{};
    auto const connection = observePublishedRows(*modelPtr, editedId, publishedRows, boundTitle);
    auto const rowLoadsBefore = rowCache.rowLoadCount();

    // Moving the first row changes the heading's artist spelling, not just the track order.
    editTitle(fixture.owner(), std::array{editedId}, "Yankee");

    REQUIRE(projectionBatches.size() == 1);
    REQUIRE(projectionBatches.front().deltas.size() == 1);
    REQUIRE(std::holds_alternative<rt::ProjectionReset>(projectionBatches.front().deltas.front()));
    REQUIRE_FALSE(publishedRows.empty());

    for (auto const& row : publishedRows)
    {
      CHECK(row.id == editedId);
      CHECK(row.boundTitle == "Yankee");
      CHECK(row.title == "Yankee");
    }

    REQUIRE(projection.groupCount() == 1);
    CHECK(projection.groupAt(0).heading.secondary == rt::TrackGroupHeadingValue{"AIMER"});
    auto const movedItemPtr = itemAt(*modelPtr, 2);
    REQUIRE(movedItemPtr);
    CHECK(movedItemPtr == originalItemPtr);
    CHECK(modelPtr->trackRow(movedItemPtr->trackId()) == cachedRowPtr);
    CHECK(readTitle(*cachedRowPtr) == "Yankee");

    // Revisit both unchanged rows so a deferred reload also spends the same read budget.
    CHECK(modelPtr->trackRow(observedId) == observedRowPtr);
    CHECK(readTitle(*observedRowPtr) == "Charlie");
    auto const unobservedRowPtr = modelPtr->trackRow(unobservedId);
    REQUIRE(unobservedRowPtr);
    CHECK(readTitle(*unobservedRowPtr) == "Delta");
    CHECK_FALSE(unchangedRowNotified);
    CHECK(rowCache.rowLoadCount() - rowLoadsBefore <= 1);
  }

  TEST_CASE("TrackListModel - filter insertions refresh another view's bound titles before notifying GTK")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto const twoRanges = GENERATE(false, true);
    CAPTURE(twoRanges);
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const firstId =
      addRuntimeTrack(fixture.owner(), library::test::TrackSpec{.title = "Outside One", .artist = "Aimer"});
    auto const anchorId =
      addRuntimeTrack(fixture.owner(), library::test::TrackSpec{.title = "Match Anchor", .artist = "Celine"});
    auto const lastId =
      addRuntimeTrack(fixture.owner(), library::test::TrackSpec{.title = "Outside Two", .artist = "Daho"});
    auto const editedIds = std::array{firstId, lastId};

    auto rowCache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    auto const filteredModelPtr = openTrackList(
      runtime,
      rowCache,
      {.target = rt::FilteredListTarget{.listId = rt::kAllTracksListId, .filterExpression = "$title ~ \"Match\""},
       .optPresentation = rt::NavigationPresentation{
         .spec = rt::TrackPresentationSpec{.sortBy = {{.field = rt::TrackSortField::Artist}}}}});
    auto const& projection = *filteredModelPtr->projection();
    REQUIRE(projection.size() == 1);
    REQUIRE(projection.trackIdAt(0) == anchorId);

    // Mount the production Title column on a static source for the two rows.
    // It observes bound fields but cannot publish an edit or rebind on its own.
    auto sourcePtr = std::make_shared<rt::test::MutableTrackSource>();
    sourcePtr->addInitial(firstId);
    sourcePtr->addInitial(lastId);
    auto const passiveModelPtr = TrackListModel::create(rowCache);
    passiveModelPtr->bindProjection(
      runtime.views().createTransientTrackListProjection(rt::TrackSourceLease{sourcePtr}));
    auto columnView = Gtk::ColumnView{Gtk::NoSelection::create(passiveModelPtr)};
    auto const titleFactoryPtr = buildColumnFactory(rt::TrackField::Title, {}, {}, *passiveModelPtr);
    columnView.append_column(Gtk::ColumnViewColumn::create("Title", titleFactoryPtr));
    auto window = GtkWindowFixture{};
    window.window().set_default_size(400, 200);
    window.mount(columnView);
    window.present();
    REQUIRE(tryPumpGtkEventsUntil(
      [&] { return findLabelByText(columnView, "Outside One") && findLabelByText(columnView, "Outside Two"); }));
    auto const* const firstLabel = findLabelByText(columnView, "Outside One");
    auto const* const lastLabel = findLabelByText(columnView, "Outside Two");
    REQUIRE(firstLabel != nullptr);
    REQUIRE(lastLabel != nullptr);
    REQUIRE(firstLabel->get_mapped());
    REQUIRE(lastLabel->get_mapped());

    auto changes = std::vector<std::array<::guint, 3>>{};
    auto titles = std::vector<std::array<std::string, 2>>{};
    auto const notification = sigc::scoped_connection{filteredModelPtr->signal_items_changed().connect(
      [&](::guint start, ::guint removed, ::guint added)
      {
        // Do not look up rows here. That could reload stale fields before sampling the existing labels.
        titles.push_back({firstLabel->get_text().raw(), lastLabel->get_text().raw()});
        changes.push_back({start, removed, added});
      })};
    auto batches = std::vector<rt::TrackListProjectionDeltaBatch>{};
    auto const projectionSub =
      projection.subscribe([&batches](rt::TrackListProjectionDeltaBatch const& batch) { batches.push_back(batch); });
    batches.clear();
    editTitle(fixture.owner(), std::span{editedIds}.first(twoRanges ? 2 : 1), "Match New");

    REQUIRE(batches.size() == 1);
    auto const& deltas = batches.front().deltas;
    REQUIRE(deltas.size() == (twoRanges ? 2U : 1U));
    auto const* const firstInsert = std::get_if<rt::ProjectionInsertRange>(&deltas.front());
    REQUIRE(firstInsert != nullptr);
    CHECK(firstInsert->range.start == 0);
    CHECK(firstInsert->range.count == 1);
    CHECK(projection.trackIdAt(0) == firstId);
    CHECK(projection.trackIdAt(1) == anchorId);

    if (twoRanges)
    {
      auto const* const lastInsert = std::get_if<rt::ProjectionInsertRange>(&deltas.back());
      REQUIRE(lastInsert != nullptr);
      CHECK(lastInsert->range.start == 2);
      CHECK(lastInsert->range.count == 1);
      CHECK(projection.trackIdAt(2) == lastId);
      CHECK(changes == std::vector<std::array<::guint, 3>>{{0, 1, 3}});
    }
    else
    {
      CHECK(changes == std::vector<std::array<::guint, 3>>{{0, 0, 1}});
    }

    CHECK(titles == std::vector<std::array<std::string, 2>>{{"Match New", twoRanges ? "Match New" : "Outside Two"}});
  }

  TEST_CASE("TrackListModel - a bound row still receives title edits after a list-only restore")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const trackId = addRuntimeTrack(fixture.owner(), library::test::TrackSpec{.title = "Before"});

    auto rowCache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    auto const modelPtr = openTrackList(runtime, rowCache, {.target = rt::GlobalViewKind::AllTracks});
    auto const itemPtr = itemAt(*modelPtr, 0);
    REQUIRE(itemPtr);
    REQUIRE(itemPtr->trackId() == trackId);
    auto const boundRowPtr = modelPtr->trackRow(itemPtr->trackId());
    REQUIRE(boundRowPtr);
    auto boundTitle = readTitle(*boundRowPtr);
    REQUIRE(boundTitle == "Before");
    auto const fieldsConnection = sigc::scoped_connection{
      boundRowPtr->signalFieldsChanged().connect([&] { boundTitle = readTitle(*boundRowPtr); })};

    auto const path = fixture.tempDir().path() / "lists.yaml";
    {
      auto document = std::ofstream{path};
      document << "version: 1\nexport-mode: list-only\nlibrary:\n  lists:\n    - id: 1\n      name: Restored\n";
    }
    bool sawReset = false;
    auto const changeSub = runtime.library().changes().onChanged([&sawReset](rt::LibraryChangeSet const& change)
                                                                 { sawReset = sawReset || change.libraryReset; });

    // List-only Restore publishes a real library reset without replacing the track identities.
    auto& jobs = fixture.owner().libraryJobs();
    auto plan =
      ao::test::requireValue(runGtkTask(runtime, jobs.prepareLibraryImportAsync(path, rt::ImportMode::Restore)));
    auto const report = ao::test::requireValue(runGtkTask(runtime, jobs.applyLibraryImportPlanAsync(std::move(plan))));
    REQUIRE(report.listsCreated == 1);
    REQUIRE(sawReset);
    REQUIRE(itemAt(*modelPtr, 0) == itemPtr);

    // The binding gets no new metadata lookup just because the model announced a reset.
    editTitle(fixture.owner(), std::array{trackId}, "After");
    CHECK(boundTitle == "After");
  }
} // namespace ao::gtk::test
