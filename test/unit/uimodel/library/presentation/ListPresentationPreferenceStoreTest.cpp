// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/presentation/ListPresentationPreferenceStore.h>

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include "test/unit/uimodel/library/presentation/TrackPresentationTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceLifecycle.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>
#include <ao/uimodel/library/presentation/TrackPresentationRecommender.h>

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>
#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("ListPresentationPreferenceStore - stores list presentation ids and emits changed lists",
            "[uimodel][unit][library][presentation]")
  {
    auto fixture = TrackPresentationFixture{};
    auto& store = fixture.preferences;
    auto events = std::vector<ListId>{};
    auto sub = store.signalChanged().connect([&events](ListId listId) noexcept { events.push_back(listId); });

    store.setPresentationIdForList(kInvalidListId, "albums");
    CHECK_FALSE(store.presentationIdForList(kInvalidListId));
    CHECK(events.empty());

    store.setPresentationIdForList(rt::kAllTracksListId, "albums");
    store.setPresentationIdForList(rt::kAllTracksListId, "albums");

    auto const optId = store.presentationIdForList(rt::kAllTracksListId);
    REQUIRE(optId);
    CHECK(*optId == "albums");
    REQUIRE(events.size() == 1);
    CHECK(events[0] == rt::kAllTracksListId);

    store.clearPresentationForList(rt::kAllTracksListId);

    CHECK_FALSE(store.presentationIdForList(rt::kAllTracksListId));
    REQUIRE(events.size() == 2);
    CHECK(events[1] == rt::kAllTracksListId);
  }

  TEST_CASE("ListPresentationPreferenceStore - empty presentation id clears without inserting empty state",
            "[uimodel][unit][library][presentation]")
  {
    auto fixture = TrackPresentationFixture{};
    auto& store = fixture.preferences;
    auto events = std::vector<ListId>{};
    auto sub = store.signalChanged().connect([&events](ListId listId) noexcept { events.push_back(listId); });

    store.setPresentationIdForList(rt::kAllTracksListId, "");
    CHECK(store.listPresentations().empty());
    CHECK(events.empty());

    store.setPresentationIdForList(rt::kAllTracksListId, "albums");
    store.setPresentationIdForList(rt::kAllTracksListId, "");

    CHECK(store.listPresentations().empty());
    REQUIRE(events.size() == 2);
    CHECK(events[0] == rt::kAllTracksListId);
    CHECK(events[1] == rt::kAllTracksListId);
  }

  TEST_CASE(
    "ListPresentationPreferenceStore - resolves custom preferences and preserves unknown ids while falling back",
    "[uimodel][unit][library][presentation]")
  {
    auto fixture = TrackPresentationFixture{};
    auto& store = fixture.preferences;
    fixture.catalog.addCustomPresentation(rt::CustomTrackPresentationPreset{
      .label = "Tag Audit",
      .basePresetId = std::string{rt::kDefaultTrackPresentationId},
      .spec =
        rt::TrackPresentationSpec{.id = "tag-audit", .visibleFields = {rt::TrackField::Title, rt::TrackField::Tags}},
    });

    auto const allTracksContext = ListPresentationContext{
      .listId = rt::kAllTracksListId,
      .sourceKind = ListPresentationSourceKind::AllTracks,
    };

    store.setPresentationIdForList(rt::kAllTracksListId, "tag-audit");
    CHECK(store.presentationForList(allTracksContext).id == "tag-audit");

    store.setPresentationIdForList(rt::kAllTracksListId, "missing-preset");
    CHECK(store.presentationForList(allTracksContext).id == "albums");
    CHECK(store.presentationIdForList(rt::kAllTracksListId) == "missing-preset");
  }

  TEST_CASE("ListPresentationPreferenceStore - resolves saved-list defaults after preference lookup",
            "[uimodel][unit][library][presentation]")
  {
    auto fixture = TrackPresentationFixture{};
    auto& store = fixture.preferences;
    auto const savedListId = ListId{42};
    auto const emptySavedContext = ListPresentationContext{
      .listId = savedListId,
      .sourceKind = ListPresentationSourceKind::SavedList,
    };
    auto const expressionContext = ListPresentationContext{
      .listId = ListId{43},
      .sourceKind = ListPresentationSourceKind::SavedList,
      .listExpression = "$composer = \"Bach\"",
    };
    auto const allTracksContext = ListPresentationContext{
      .listId = rt::kAllTracksListId,
      .sourceKind = ListPresentationSourceKind::AllTracks,
    };

    CHECK(store.presentationForList(emptySavedContext).id == "albums");
    CHECK(store.presentationForList(expressionContext).id == "classical-composers");
    CHECK(store.presentationForList(allTracksContext).id == "albums");

    store.setPresentationIdForList(savedListId, rt::kListOrderTrackPresentationId);
    CHECK(store.presentationForList(emptySavedContext).id == rt::kListOrderTrackPresentationId);

    store.setPresentationIdForList(savedListId, "missing-preset");
    CHECK(store.presentationForList(emptySavedContext).id == "albums");
  }

  TEST_CASE("ListPresentationPreferenceStore - bulk state emits only when changed",
            "[uimodel][unit][library][presentation]")
  {
    auto fixture = TrackPresentationFixture{};
    auto& store = fixture.preferences;
    auto events = std::vector<ListId>{};
    auto sub = store.signalChanged().connect([&events](ListId listId) noexcept { events.push_back(listId); });
    auto const presentations = std::map<ListId, std::string>{{rt::kAllTracksListId, "albums"}};

    store.setListPresentations(presentations);
    store.setListPresentations(presentations);

    REQUIRE(events.size() == 1);
    CHECK(events[0] == kInvalidListId);
    CHECK(store.listPresentations().at(rt::kAllTracksListId) == "albums");
  }

  TEST_CASE("ListPresentationPreferenceLifecycle - cascade deletion clears every preference",
            "[uimodel][unit][presentation][delete-subtree]")
  {
    auto storage = rt::test::MusicLibraryFixture{};
    auto changes = rt::test::makeInlineLibraryChanges(storage.library());
    auto writerFixture = rt::test::LibraryWriterFixture{storage.library(), changes};
    auto& writer = writerFixture.writer();
    auto const parentId = ao::test::requireValue(writer.createList(rt::LibraryWriter::ListDraft{.name = "Parent"}));
    auto const childId =
      ao::test::requireValue(writer.createList(rt::LibraryWriter::ListDraft{.parentId = parentId, .name = "Child"}));
    auto const grandchildId = ao::test::requireValue(
      writer.createList(rt::LibraryWriter::ListDraft{.parentId = childId, .name = "Grandchild"}));
    auto const unrelatedId =
      ao::test::requireValue(writer.createList(rt::LibraryWriter::ListDraft{.name = "Unrelated"}));
    auto preferences = std::map<ListId, std::string>{
      {parentId, "songs"},
      {childId, "albums"},
      {grandchildId, std::string{rt::kListOrderTrackPresentationId}},
      {unrelatedId, "songs"},
    };
    auto removed = std::vector<ListId>{};
    auto lifecycle = ListPresentationPreferenceLifecycle{
      preferences,
      changes,
      [&removed](ListId const listId) noexcept { removed.push_back(listId); },
    };

    REQUIRE(writer.deleteListAndDescendants(parentId));

    CHECK(removed == std::vector{parentId, childId, grandchildId});
    REQUIRE(preferences.size() == 1);
    CHECK(preferences.contains(unrelatedId));
  }
} // namespace ao::uimodel::test
