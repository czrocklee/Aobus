// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/presentation/ListPresentations.h>

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include "test/unit/uimodel/library/presentation/TrackPresentationTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("ListPresentations - stores list presentation ids and emits changed lists",
            "[uimodel][unit][library][presentation]")
  {
    auto fixture = TrackPresentationFixture{};
    auto& store = fixture.listPresentations;
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

  TEST_CASE("ListPresentations - empty presentation id clears without inserting empty state",
            "[uimodel][unit][library][presentation]")
  {
    auto fixture = TrackPresentationFixture{};
    auto& store = fixture.listPresentations;
    auto events = std::vector<ListId>{};
    auto sub = store.signalChanged().connect([&events](ListId listId) noexcept { events.push_back(listId); });

    store.setPresentationIdForList(rt::kAllTracksListId, "");
    CHECK(store.snapshot().empty());
    CHECK(events.empty());

    store.setPresentationIdForList(rt::kAllTracksListId, "albums");
    store.setPresentationIdForList(rt::kAllTracksListId, "");

    CHECK(store.snapshot().empty());
    REQUIRE(events.size() == 2);
    CHECK(events[0] == rt::kAllTracksListId);
    CHECK(events[1] == rt::kAllTracksListId);
  }

  TEST_CASE(
    "ListPresentations - resolves custom list-presentation preferences and preserves unknown ids while falling back",
    "[uimodel][unit][library][presentation]")
  {
    auto fixture = TrackPresentationFixture{};
    auto& store = fixture.listPresentations;
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

  TEST_CASE("ListPresentations - resolves saved-list defaults after preference lookup",
            "[uimodel][unit][library][presentation]")
  {
    auto fixture = TrackPresentationFixture{};
    auto& store = fixture.listPresentations;
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

  TEST_CASE("ListPresentations - bulk state emits only when changed", "[uimodel][unit][library][presentation]")
  {
    auto fixture = TrackPresentationFixture{};
    auto& store = fixture.listPresentations;
    auto events = std::vector<ListId>{};
    auto sub = store.signalChanged().connect([&events](ListId listId) noexcept { events.push_back(listId); });
    auto const presentations = std::map<ListId, std::string>{{rt::kAllTracksListId, "albums"}};

    store.restore(presentations);
    store.restore(presentations);

    REQUIRE(events.size() == 1);
    CHECK(events[0] == kInvalidListId);
    CHECK(store.snapshot().at(rt::kAllTracksListId) == "albums");
  }

  TEST_CASE("ListPresentations - cascade deletion clears every preference",
            "[uimodel][unit][presentation][delete-subtree]")
  {
    auto presentationFixture = TrackPresentationFixture{};
    auto libraryFixture = rt::test::MusicLibraryFixture{};
    auto executor = rt::test::QueuedExecutor{};
    auto changes = rt::test::makeLibraryChanges(executor, libraryFixture.library());
    auto commandsFixture = rt::test::LibraryCommandsFixture{libraryFixture.library(), changes, executor};
    auto& commands = commandsFixture.commands();
    auto const parentId =
      ao::test::requireValue(commandsFixture.runTask(commands.createList(rt::ListDraft{.name = "Parent"})));
    auto const childId = ao::test::requireValue(
      commandsFixture.runTask(commands.createList(rt::ListDraft{.parentId = parentId, .name = "Child"})));
    auto const grandchildId = ao::test::requireValue(
      commandsFixture.runTask(commands.createList(rt::ListDraft{.parentId = childId, .name = "Grandchild"})));
    auto const unrelatedId =
      ao::test::requireValue(commandsFixture.runTask(commands.createList(rt::ListDraft{.name = "Unrelated"})));
    auto listPresentations = ListPresentations{presentationFixture.catalog, changes};
    listPresentations.restore({
      {parentId, "songs"},
      {childId, "albums"},
      {grandchildId, std::string{rt::kListOrderTrackPresentationId}},
      {unrelatedId, "songs"},
    });
    auto removed = std::vector<ListId>{};
    auto sub = listPresentations.signalChanged().connect([&removed](ListId const listId) noexcept
                                                         { removed.push_back(listId); });

    REQUIRE(commandsFixture.runTask(commands.deleteListAndDescendants(parentId)));

    CHECK(removed == std::vector{parentId, childId, grandchildId});
    REQUIRE(listPresentations.snapshot().size() == 1);
    CHECK(listPresentations.snapshot().contains(unrelatedId));
  }

  TEST_CASE("ListPresentations - deletion callback may destroy its owner",
            "[uimodel][regression][presentation][lifecycle]")
  {
    auto presentationFixture = TrackPresentationFixture{};
    auto libraryFixture = rt::test::MusicLibraryFixture{};
    auto executor = rt::test::QueuedExecutor{};
    auto changes = rt::test::makeLibraryChanges(executor, libraryFixture.library());
    auto commandsFixture = rt::test::LibraryCommandsFixture{libraryFixture.library(), changes, executor};
    auto& commands = commandsFixture.commands();
    auto const parentId =
      ao::test::requireValue(commandsFixture.runTask(commands.createList(rt::ListDraft{.name = "Parent"})));
    auto const childId = ao::test::requireValue(
      commandsFixture.runTask(commands.createList(rt::ListDraft{.parentId = parentId, .name = "Child"})));
    auto presentationsPtr = std::make_unique<ListPresentations>(presentationFixture.catalog, changes);
    presentationsPtr->restore({{parentId, "songs"}, {childId, "albums"}});
    auto removed = std::vector<ListId>{};
    auto sub = presentationsPtr->signalChanged().connect(
      [&removed, &presentationsPtr](ListId const listId)
      {
        removed.push_back(listId);

        if (removed.size() == 1)
        {
          presentationsPtr.reset();
        }
      });

    REQUIRE(commandsFixture.runTask(commands.deleteListAndDescendants(parentId)));

    CHECK(removed == std::vector{parentId, childId});
    CHECK(presentationsPtr == nullptr);
  }
} // namespace ao::uimodel::test
