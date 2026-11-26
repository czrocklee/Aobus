// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/presentation/ListPresentations.h>

#include "test/TestFixtureSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/runtime/ExecutorTestSupport.h"
#include "test/runtime/RuntimeLibraryTestSupport.h"
#include "test/uimodel/library/presentation/TrackPresentationTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <tuple>
#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("ListPresentations - stores list presentation ids and emits changed lists")
  {
    auto fixture = TrackPresentationFixture{};
    auto& store = fixture.listPresentations;
    auto events = std::vector<ListId>{};
    auto sub = store.signalChanged().connect([&events](ListId listId) noexcept { events.push_back(listId); });

    store.setPresentationIdForList(kInvalidListId, rt::BuiltinPreset::Albums);
    CHECK_FALSE(store.presentationIdForList(kInvalidListId));
    store.clearPresentationForList(rt::kAllTracksListId);
    CHECK(store.snapshot().empty());
    CHECK(events.empty());

    store.setPresentationIdForList(rt::kAllTracksListId, rt::BuiltinPreset::Albums);
    store.setPresentationIdForList(rt::kAllTracksListId, rt::BuiltinPreset::Albums);

    CHECK(store.presentationIdForList(rt::kAllTracksListId) == rt::TrackPresetId{rt::BuiltinPreset::Albums});
    REQUIRE(events.size() == 1);
    CHECK(events[0] == rt::kAllTracksListId);

    store.clearPresentationForList(rt::kAllTracksListId);

    CHECK_FALSE(store.presentationIdForList(rt::kAllTracksListId));
    REQUIRE(events.size() == 2);
    CHECK(events[1] == rt::kAllTracksListId);
  }

  TEST_CASE("ListPresentations - All Tracks rejects a persisted List Order pin but accepts a flat unsorted custom pin")
  {
    auto fixture = TrackPresentationFixture{};
    auto commandsFixture = rt::test::LibraryCommandsFixture{fixture.libraryFixture.library(), fixture.changes};
    auto const& library = commandsFixture.library();
    auto& store = fixture.listPresentations;

    store.restore({{rt::kAllTracksListId, rt::BuiltinPreset::ListOrder}}, {});
    CHECK(store.presentationForList(rt::kAllTracksListId, library).isPreset(rt::BuiltinPreset::Albums));
    CHECK(store.presentationIdForList(rt::kAllTracksListId) == rt::TrackPresetId{rt::BuiltinPreset::ListOrder});

    auto const presetId = rt::CustomPresetId{0x1a};
    auto const flatSpec = rt::TrackPresentationSpec{
      .presetId = presetId,
      .groupBy = rt::TrackGroupKey::None,
      .visibleFields = {rt::TrackField::Title, rt::TrackField::Tags},
    };
    fixture.catalog.addCustomPresentation(rt::CustomTrackPresentationPreset{
      .label = "Tag Audit",
      .optBasePresetId = rt::TrackPresetId{rt::BuiltinPreset::Library},
      .spec = flatSpec,
    });
    store.setPresentationIdForList(rt::kAllTracksListId, presetId);

    CHECK(store.presentationForList(rt::kAllTracksListId, library) == flatSpec);
  }

  TEST_CASE("ListPresentations - saved list recommends from its local expression when no pin resolves")
  {
    auto fixture = TrackPresentationFixture{};
    auto executor = rt::test::QueuedExecutor{};
    auto& storage = fixture.libraryFixture.library();
    auto changes = rt::test::makeLibraryChanges(executor, storage);
    auto commandsFixture = rt::test::LibraryCommandsFixture{storage, changes, executor};
    auto& commands = commandsFixture.commands();
    auto const& library = commandsFixture.library();
    auto store = ListPresentations{fixture.catalog, changes};
    auto const parentId = ao::test::requireValue(commandsFixture.runTask(
      commands.createListAsync(rt::ListDraft{.name = "Works", .expression = "$work = \"Concerto\""})));
    auto const childId = ao::test::requireValue(commandsFixture.runTask(commands.createListAsync(
      rt::ListDraft{.parentId = parentId, .name = "Bach", .expression = "$composer = \"Bach\""})));

    // The inherited work would outrank the child expression if included.
    CHECK(store.presentationForList(childId, library).isPreset(rt::BuiltinPreset::ClassicalComposers));

    store.setPresentationIdForList(childId, rt::BuiltinPreset::ListOrder);
    CHECK(store.presentationForList(childId, library).isPreset(rt::BuiltinPreset::ListOrder));

    auto const missingPresetId = rt::CustomPresetId{0x1a};
    store.setPresentationIdForList(childId, missingPresetId);
    CHECK(store.presentationForList(childId, library).isPreset(rt::BuiltinPreset::ClassicalComposers));
    CHECK(store.presentationIdForList(childId) == rt::TrackPresetId{missingPresetId});
  }

  TEST_CASE("ListPresentations - missing list uses the All Tracks recommendation but keeps its own pin identity")
  {
    auto fixture = TrackPresentationFixture{};
    auto commandsFixture = rt::test::LibraryCommandsFixture{fixture.libraryFixture.library(), fixture.changes};
    auto const& library = commandsFixture.library();
    auto& store = fixture.listPresentations;
    auto const missingListId = ListId{42};
    store.setPresentationIdForList(rt::kAllTracksListId, rt::BuiltinPreset::Artists);

    CHECK(store.presentationForList(missingListId, library).isPreset(rt::BuiltinPreset::Albums));

    store.setPresentationIdForList(missingListId, rt::BuiltinPreset::ListOrder);
    CHECK(store.presentationForList(missingListId, library).isPreset(rt::BuiltinPreset::ListOrder));
  }

  TEST_CASE("ListPresentations - bulk state emits only when changed")
  {
    auto fixture = TrackPresentationFixture{};
    auto& store = fixture.listPresentations;
    auto events = std::vector<ListId>{};
    auto sub = store.signalChanged().connect([&events](ListId listId) noexcept { events.push_back(listId); });
    auto const presentations = ListPresentations::Snapshot{{rt::kAllTracksListId, rt::BuiltinPreset::Albums}};

    store.restore(presentations, {});
    store.restore(presentations, {});

    REQUIRE(events.size() == 1);
    CHECK(events[0] == kInvalidListId);
    CHECK(store.snapshot().at(rt::kAllTracksListId) == rt::TrackPresetId{rt::BuiltinPreset::Albums});
  }

  TEST_CASE("ListPresentations - restore drops preferences whose list the library no longer has")
  {
    auto presentationFixture = TrackPresentationFixture{};
    auto& listPresentations = presentationFixture.listPresentations;
    auto const liveId = ListId{42};
    auto const staleId = ListId{99};

    // A list deleted while the frontend was down produces no LibraryChanges
    // event, so only the restore path can retire its entry.
    listPresentations.restore(
      {
        {liveId, rt::BuiltinPreset::Songs},
        {staleId, rt::BuiltinPreset::Albums},
        {rt::kAllTracksListId, rt::BuiltinPreset::Songs},
      },
      std::vector{liveId});

    REQUIRE(listPresentations.snapshot().size() == 2);
    CHECK(listPresentations.snapshot().contains(liveId));
    CHECK_FALSE(listPresentations.snapshot().contains(staleId));
    // All Tracks references no user-created list, so no live id vouches for it.
    CHECK(listPresentations.snapshot().contains(rt::kAllTracksListId));
  }

  TEST_CASE("ListPresentations - cascade deletion clears every preference")
  {
    auto presentationFixture = TrackPresentationFixture{};
    auto libraryFixture = rt::test::MusicLibraryFixture{};
    auto executor = rt::test::QueuedExecutor{};
    auto changes = rt::test::makeLibraryChanges(executor, libraryFixture.library());
    auto commandsFixture = rt::test::LibraryCommandsFixture{libraryFixture.library(), changes, executor};
    auto& commands = commandsFixture.commands();
    auto const parentId =
      ao::test::requireValue(commandsFixture.runTask(commands.createListAsync(rt::ListDraft{.name = "Parent"})));
    auto const childId = ao::test::requireValue(
      commandsFixture.runTask(commands.createListAsync(rt::ListDraft{.parentId = parentId, .name = "Child"})));
    auto const grandchildId = ao::test::requireValue(
      commandsFixture.runTask(commands.createListAsync(rt::ListDraft{.parentId = childId, .name = "Grandchild"})));
    auto const unrelatedId =
      ao::test::requireValue(commandsFixture.runTask(commands.createListAsync(rt::ListDraft{.name = "Unrelated"})));
    auto listPresentations = ListPresentations{presentationFixture.catalog, changes};
    listPresentations.restore(
      {
        {parentId, rt::BuiltinPreset::Songs},
        {childId, rt::BuiltinPreset::Albums},
        {grandchildId, rt::BuiltinPreset::ListOrder},
        {unrelatedId, rt::BuiltinPreset::Songs},
      },
      std::vector{parentId, childId, grandchildId, unrelatedId});
    auto removed = std::vector<ListId>{};
    // A persisting observer snapshots from inside this callback, so the entry
    // must already be gone by the time the change is announced.
    auto sub = listPresentations.signalChanged().connect(
      [&removed, &listPresentations](ListId const listId) noexcept
      {
        CHECK_FALSE(listPresentations.snapshot().contains(listId));
        removed.push_back(listId);
      });

    REQUIRE(commandsFixture.runTask(commands.deleteListAndDescendantsAsync(parentId)));

    CHECK(removed == std::vector{parentId, childId, grandchildId});
    REQUIRE(listPresentations.snapshot().size() == 1);
    CHECK(listPresentations.snapshot().contains(unrelatedId));
  }

  TEST_CASE("ListPresentations - library reset clears every preference")
  {
    auto presentationFixture = TrackPresentationFixture{};
    auto libraryFixture = rt::test::MusicLibraryFixture{};
    auto executor = rt::test::QueuedExecutor{};
    auto changes = rt::test::makeLibraryChanges(executor, libraryFixture.library());
    auto listPresentations = ListPresentations{presentationFixture.catalog, changes};
    listPresentations.restore({{ListId{42}, rt::BuiltinPreset::Songs}, {ListId{43}, rt::BuiltinPreset::Albums}},
                              std::vector{ListId{42}, ListId{43}});
    auto removed = std::vector<ListId>{};
    // A persisting observer snapshots from inside this callback, so the entry
    // must already be gone by the time the change is announced.
    auto sub = listPresentations.signalChanged().connect(
      [&removed, &listPresentations](ListId const listId) noexcept
      {
        CHECK_FALSE(listPresentations.snapshot().contains(listId));
        removed.push_back(listId);
      });

    std::ignore = rt::test::addTrackAndPublishReset(
      libraryFixture.library(), changes, library::test::TrackSpec{.title = "Reset"}, executor);

    CHECK(listPresentations.snapshot().empty());
    CHECK(removed == std::vector{ListId{42}, ListId{43}});
  }

  TEST_CASE("ListPresentations - deletion callback may destroy its owner")
  {
    auto presentationFixture = TrackPresentationFixture{};
    auto libraryFixture = rt::test::MusicLibraryFixture{};
    auto executor = rt::test::QueuedExecutor{};
    auto changes = rt::test::makeLibraryChanges(executor, libraryFixture.library());
    auto commandsFixture = rt::test::LibraryCommandsFixture{libraryFixture.library(), changes, executor};
    auto& commands = commandsFixture.commands();
    auto const parentId =
      ao::test::requireValue(commandsFixture.runTask(commands.createListAsync(rt::ListDraft{.name = "Parent"})));
    auto const childId = ao::test::requireValue(
      commandsFixture.runTask(commands.createListAsync(rt::ListDraft{.parentId = parentId, .name = "Child"})));
    auto presentationsPtr = std::make_unique<ListPresentations>(presentationFixture.catalog, changes);
    presentationsPtr->restore(
      {{parentId, rt::BuiltinPreset::Songs}, {childId, rt::BuiltinPreset::Albums}}, std::vector{parentId, childId});
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

    REQUIRE(commandsFixture.runTask(commands.deleteListAndDescendantsAsync(parentId)));

    CHECK(removed == std::vector{parentId, childId});
    CHECK(presentationsPtr == nullptr);
  }
} // namespace ao::uimodel::test
