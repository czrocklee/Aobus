// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/async/Runtime.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackFieldValue.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/projection/TrackDetailProjection.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    using F = TrackField;

    std::string aggregateString(AggregateValue<TrackFieldRawValue> const& agg)
    {
      if (!agg.optValue)
      {
        return {};
      }

      return std::get<std::string>(*agg.optValue);
    }

    struct TrackDetailProjectionFixture final
    {
      MusicLibraryFixture libraryFixture;
      InlineExecutor executor;
      async::Runtime runtime;
      LibraryChanges changes;
      LibraryWriterFixture writerFixture;
      TrackSourceCache sources;
      ViewService views;
      WorkspaceService workspace;

      TrackDetailProjectionFixture()
        : libraryFixture{}
        , runtime{executor}
        , changes{}
        , writerFixture{libraryFixture.library(), changes}
        , sources{libraryFixture.library(), changes}
        , views{executor, libraryFixture.library(), sources}
        , workspace{executor, views, changes}
      {
      }

      LibraryWriter& writer() { return writerFixture.writer(); }
    };
  } // namespace

  TEST_CASE("TrackDetailProjection - refreshes selected fields after intersecting TracksMutated",
            "[runtime][unit][projection][detail]")
  {
    auto env = TrackDetailProjectionFixture{};

    auto const id1 =
      env.libraryFixture.addTrack(library::test::TrackSpec{.title = "Before", .artist = "ArtistA", .album = "AlbumX"});

    auto const reply = ao::test::requireValue(env.views.createView(TrackListViewConfig{.listId = kAllTracksListId}));
    REQUIRE(env.views.setSelection(reply, {id1}));

    auto projPtr = env.views.detailProjection(ExplicitViewTarget{reply}, env.workspace, env.changes);

    auto snap = projPtr->snapshot();
    CHECK(snap.selectionKind == SelectionKind::Single);
    auto const& titleAgg = snap.fields[static_cast<std::size_t>(F::Title)];
    REQUIRE(titleAgg.optValue);
    CHECK(aggregateString(titleAgg) == "Before");

    // Mutate the track in the library using the service
    {
      auto const patch = MetadataPatch{.optTitle = "After"};
      auto const targetIds = std::array{id1};
      REQUIRE(env.writerFixture.updateMetadata(targetIds, patch));
    }

    // Mutation service already published the signal

    snap = projPtr->snapshot();
    CHECK(aggregateString(snap.fields[static_cast<std::size_t>(F::Title)]) == "After");
  }

  TEST_CASE("TrackDetailProjection - ignores non-intersecting TracksMutated", "[runtime][unit][projection][detail]")
  {
    auto env = TrackDetailProjectionFixture{};

    auto const id1 = env.libraryFixture.addTrack("Selected");
    auto const id2 = env.libraryFixture.addTrack("Other");

    auto const reply = ao::test::requireValue(env.views.createView(TrackListViewConfig{.listId = kAllTracksListId}));
    REQUIRE(env.views.setSelection(reply, {id1}));

    auto projPtr = env.views.detailProjection(ExplicitViewTarget{reply}, env.workspace, env.changes);
    std::int32_t publicationCount = 0;
    [[maybe_unused]] auto subscription = projPtr->subscribe([&](TrackDetailSnapshot const&) { ++publicationCount; });
    CHECK(publicationCount == 1);

    // Mutate a track not in the selection
    auto const otherIds = std::array{id2};
    REQUIRE(env.writerFixture.updateMetadata(otherIds, MetadataPatch{.optTitle = "Something Else"}));

    CHECK(publicationCount == 1);
    CHECK(projPtr->snapshot().trackIds == std::vector{id1});
  }

  TEST_CASE("TrackDetailProjection - aggregates common and mixed metadata for multi-select",
            "[runtime][unit][projection][detail]")
  {
    auto env = TrackDetailProjectionFixture{};

    auto const id1 =
      env.libraryFixture.addTrack(library::test::TrackSpec{.title = "Song A", .artist = "Same", .album = "AlbumX"});
    auto const id2 =
      env.libraryFixture.addTrack(library::test::TrackSpec{.title = "Song B", .artist = "Same", .album = "AlbumY"});

    auto const reply = ao::test::requireValue(env.views.createView(TrackListViewConfig{.listId = kAllTracksListId}));
    REQUIRE(env.views.setSelection(reply, {id1, id2}));

    auto const projPtr = env.views.detailProjection(ExplicitViewTarget{reply}, env.workspace, env.changes);
    auto const snap = projPtr->snapshot();

    CHECK(snap.selectionKind == SelectionKind::Multiple);

    // Titles differ
    CHECK(snap.fields[static_cast<std::size_t>(F::Title)].mixed);

    // Artists are the same
    auto const& artistAgg = snap.fields[static_cast<std::size_t>(F::Artist)];
    CHECK_FALSE(artistAgg.mixed);
    REQUIRE(artistAgg.optValue);
    CHECK(aggregateString(artistAgg) == "Same");

    // Albums differ
    CHECK(snap.fields[static_cast<std::size_t>(F::Album)].mixed);
  }

  TEST_CASE("TrackDetailProjection - explicit selection target snapshots provided track ids",
            "[runtime][unit][projection][detail]")
  {
    auto env = TrackDetailProjectionFixture{};
    auto const id1 = env.libraryFixture.addTrack(library::test::TrackSpec{.title = "Song A"});
    auto const projPtr =
      env.views.detailProjection(ExplicitSelectionTarget{std::vector{id1}}, env.workspace, env.changes);
    auto const snap = projPtr->snapshot();
    CHECK(snap.selectionKind == SelectionKind::Single);
    CHECK(aggregateString(snap.fields[static_cast<std::size_t>(F::Title)]) == "Song A");
  }

  TEST_CASE("TrackDetailProjection - focused view target follows focused view selection",
            "[runtime][unit][track-detail][workspace]")
  {
    auto env = TrackDetailProjectionFixture{};
    auto const id1 = env.libraryFixture.addTrack(library::test::TrackSpec{.title = "Song A"});
    auto const id2 = env.libraryFixture.addTrack(library::test::TrackSpec{.title = "Song B"});

    auto const projPtr = env.views.detailProjection(FocusedViewTarget{}, env.workspace, env.changes);

    // Subscribe to verify notifications
    std::int32_t callCount = 0;
    auto sub = projPtr->subscribe([&](TrackDetailSnapshot const&) { callCount++; });
    CHECK(callCount == 1); // Called immediately

    auto const reply1 = ao::test::requireValue(env.views.createView(TrackListViewConfig{.listId = kAllTracksListId}));
    REQUIRE(env.views.setSelection(reply1, {id1}));
    REQUIRE(env.workspace.navigateTo(GlobalViewKind::AllTracks));

    CHECK(callCount >= 2);
    CHECK(aggregateString(projPtr->snapshot().fields[static_cast<std::size_t>(F::Title)]) == "Song A");

    // Change selection in the focused view
    REQUIRE(env.views.setSelection(reply1, {id2}));
    CHECK(aggregateString(projPtr->snapshot().fields[static_cast<std::size_t>(F::Title)]) == "Song B");

    // Change focus away
    REQUIRE(env.workspace.closeView(reply1));
    CHECK(projPtr->snapshot().selectionKind == SelectionKind::None);

    // Unsubscribe
    sub = {};

    // Now trigger a selection change in the old view, should NOT update because it's no longer focused
    auto const removedSelection = env.views.setSelection(reply1, {id1});
    REQUIRE_FALSE(removedSelection);
    CHECK(removedSelection.error().code == Error::Code::NotFound);
    CHECK(projPtr->snapshot().selectionKind == SelectionKind::None);
  }

  TEST_CASE("TrackDetailProjection - focused view target initializes from active selection",
            "[runtime][unit][track-detail][workspace]")
  {
    auto env = TrackDetailProjectionFixture{};
    auto const id1 = env.libraryFixture.addTrack(library::test::TrackSpec{.title = "Already Selected"});

    auto const viewId = ao::test::requireValue(env.workspace.navigateTo(GlobalViewKind::AllTracks));
    REQUIRE(env.views.setSelection(viewId, {id1}));

    auto const projPtr = env.views.detailProjection(FocusedViewTarget{}, env.workspace, env.changes);
    auto const snap = projPtr->snapshot();

    CHECK(snap.selectionKind == SelectionKind::Single);
    auto const& titleAgg = snap.fields[static_cast<std::size_t>(F::Title)];
    REQUIRE(titleAgg.optValue);
    CHECK(aggregateString(titleAgg) == "Already Selected");
  }

  TEST_CASE("TrackDetailProjection - missing tracks produce empty field values", "[runtime][unit][projection][detail]")
  {
    auto env = TrackDetailProjectionFixture{};
    auto const projPtr =
      env.views.detailProjection(ExplicitSelectionTarget{std::vector{TrackId{9999}}}, env.workspace, env.changes);
    auto const snap = projPtr->snapshot();
    CHECK(snap.selectionKind == SelectionKind::Single);
    CHECK_FALSE(snap.fields[static_cast<std::size_t>(F::Title)].optValue);
  }

  TEST_CASE("TrackDetailProjection - tag aggregation exposes common tag ids", "[runtime][unit][track-detail][tag]")
  {
    auto env = TrackDetailProjectionFixture{};
    auto const id1 = env.libraryFixture.addTrack(library::test::TrackSpec{.title = "Song A"});

    // Add tag
    auto const targetIds = std::vector{id1};
    auto const tagsToAdd = std::vector<std::string>{"MyTag"};
    REQUIRE(env.writerFixture.editTags(targetIds, tagsToAdd, {}));

    auto const projPtr =
      env.views.detailProjection(ExplicitSelectionTarget{std::vector{id1}}, env.workspace, env.changes);
    auto const snap = projPtr->snapshot();
    CHECK(snap.selectionKind == SelectionKind::Single);
    CHECK(snap.commonTagIds.size() == 1);
  }

  TEST_CASE("TrackDetailProjection - custom metadata aggregation marks partial shared and mixed values",
            "[runtime][unit][projection][detail]")
  {
    auto env = TrackDetailProjectionFixture{};

    auto const id1 = env.libraryFixture.addTrack("Song 1");
    auto const id2 = env.libraryFixture.addTrack("Song 2");

    // Add custom metadata to id1
    {
      auto patch = MetadataPatch{};
      patch.customUpdates["Key1"] = "Value1";
      patch.customUpdates["Shared"] = "Same";
      patch.customUpdates["Mixed"] = "One";
      REQUIRE(env.writerFixture.updateMetadata(std::vector{id1}, patch));
    }

    // Add custom metadata to id2
    {
      auto patch = MetadataPatch{};
      patch.customUpdates["Key2"] = "Value2";
      patch.customUpdates["Shared"] = "Same";
      patch.customUpdates["Mixed"] = "Two";
      REQUIRE(env.writerFixture.updateMetadata(std::vector{id2}, patch));
    }

    auto const projPtr =
      env.views.detailProjection(ExplicitSelectionTarget{std::vector{id1, id2}}, env.workspace, env.changes);
    auto const snap = projPtr->snapshot();

    CHECK(snap.customMetadata.size() == 4); // Key1, Key2, Shared, Mixed (sorted by key)

    auto const findCustom = [&](std::string_view key) -> CustomMetadataItem const*
    {
      for (auto const& item : snap.customMetadata)
      {
        if (item.key == key)
        {
          return &item;
        }
      }

      return nullptr;
    };

    SECTION("Key1 is partial")
    {
      auto const* item = findCustom("Key1");
      REQUIRE(item);
      CHECK(item->presentOnAny);
      CHECK_FALSE(item->presentOnAll);
      CHECK_FALSE(item->value.mixed);
      REQUIRE(item->value.optValue);
      CHECK(*item->value.optValue == "Value1");
    }

    SECTION("Shared is present on all and not mixed")
    {
      auto const* item = findCustom("Shared");
      REQUIRE(item);
      CHECK(item->presentOnAny);
      CHECK(item->presentOnAll);
      CHECK_FALSE(item->value.mixed);
      REQUIRE(item->value.optValue);
      CHECK(*item->value.optValue == "Same");
    }

    SECTION("Mixed is present on all and mixed")
    {
      auto const* item = findCustom("Mixed");
      REQUIRE(item);
      CHECK(item->presentOnAny);
      CHECK(item->presentOnAll);
      CHECK(item->value.mixed);
      CHECK_FALSE(item->value.optValue);
    }
  }
} // namespace ao::rt::test
