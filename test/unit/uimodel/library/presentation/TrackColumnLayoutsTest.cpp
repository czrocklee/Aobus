// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/LibraryCommands.h>

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <memory>
#include <tuple>
#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("TrackColumnLayouts - stores layouts and emits only for changes", "[uimodel][unit][library][presentation]")
  {
    auto store = TrackColumnLayouts{};
    auto events = std::vector<ListId>{};
    auto sub = store.signalChanged().connect([&events](ListId listId) noexcept { events.push_back(listId); });
    auto const layout = std::vector{TrackColumnState{.field = rt::TrackField::Album, .width = 230, .weight = -1.0},
                                    TrackColumnState{.field = rt::TrackField::Title, .width = -1, .weight = 1.25}};

    store.updateLayout(kInvalidListId, layout);
    CHECK(store.layoutForList(kInvalidListId).empty());
    CHECK(events.empty());

    store.updateLayout(rt::kAllTracksListId, layout);
    store.updateLayout(rt::kAllTracksListId, layout);
    REQUIRE(events.size() == 1);
    CHECK(events[0] == rt::kAllTracksListId);
    REQUIRE(store.layoutForList(rt::kAllTracksListId).size() == 2);
    CHECK(store.layoutForList(rt::kAllTracksListId)[0].field == rt::TrackField::Album);
    CHECK(store.layoutForList(rt::kAllTracksListId)[0].width == 230);
    CHECK(store.layoutForList(rt::kAllTracksListId)[1].field == rt::TrackField::Title);
    CHECK(store.layoutForList(rt::kAllTracksListId)[1].weight == 1.25);
  }

  TEST_CASE("TrackColumnLayouts - cascade deletion clears every layout",
            "[uimodel][unit][presentation][delete-subtree]")
  {
    auto libraryFixture = rt::test::MusicLibraryFixture{};
    auto executor = rt::test::QueuedExecutor{};
    auto changes = rt::test::makeLibraryChanges(executor, libraryFixture.library());
    auto commandsFixture = rt::test::LibraryCommandsFixture{libraryFixture.library(), changes, executor};
    auto& commands = commandsFixture.commands();
    auto const parentId =
      ao::test::requireValue(commandsFixture.runTask(commands.createList(rt::ListDraft{.name = "Parent"})));
    auto const childId = ao::test::requireValue(
      commandsFixture.runTask(commands.createList(rt::ListDraft{.parentId = parentId, .name = "Child"})));
    auto const unrelatedId =
      ao::test::requireValue(commandsFixture.runTask(commands.createList(rt::ListDraft{.name = "Unrelated"})));
    auto layouts = TrackColumnLayouts{changes};
    auto const state = std::vector{TrackColumnState{.field = rt::TrackField::Duration, .width = 17}};
    layouts.restore({{parentId, state}, {childId, state}, {unrelatedId, state}});
    auto removed = std::vector<ListId>{};
    auto sub = layouts.signalChanged().connect([&removed](ListId const listId) noexcept { removed.push_back(listId); });

    REQUIRE(commandsFixture.runTask(commands.deleteListAndDescendants(parentId)));

    CHECK(removed == std::vector{parentId, childId});
    REQUIRE(layouts.snapshot().size() == 1);
    CHECK(layouts.snapshot().contains(unrelatedId));
  }

  TEST_CASE("TrackColumnLayouts - library reset clears every layout",
            "[uimodel][regression][presentation][library-reset]")
  {
    auto libraryFixture = rt::test::MusicLibraryFixture{};
    auto executor = rt::test::QueuedExecutor{};
    auto changes = rt::test::makeLibraryChanges(executor, libraryFixture.library());
    auto layouts = TrackColumnLayouts{changes};
    auto const state = std::vector{TrackColumnState{.field = rt::TrackField::Duration, .width = 17}};
    layouts.restore({{ListId{42}, state}, {ListId{43}, state}});
    auto removed = std::vector<ListId>{};
    auto sub = layouts.signalChanged().connect([&removed](ListId const listId) noexcept { removed.push_back(listId); });

    std::ignore = rt::test::addTrackAndPublishReset(
      libraryFixture.library(), changes, library::test::TrackSpec{.title = "Reset"}, executor);

    CHECK(layouts.snapshot().empty());
    CHECK(removed == std::vector{ListId{42}, ListId{43}});
  }

  TEST_CASE("TrackColumnLayouts - deletion callback may destroy its owner",
            "[uimodel][regression][presentation][lifecycle]")
  {
    auto libraryFixture = rt::test::MusicLibraryFixture{};
    auto executor = rt::test::QueuedExecutor{};
    auto changes = rt::test::makeLibraryChanges(executor, libraryFixture.library());
    auto commandsFixture = rt::test::LibraryCommandsFixture{libraryFixture.library(), changes, executor};
    auto& commands = commandsFixture.commands();
    auto const parentId =
      ao::test::requireValue(commandsFixture.runTask(commands.createList(rt::ListDraft{.name = "Parent"})));
    auto const childId = ao::test::requireValue(
      commandsFixture.runTask(commands.createList(rt::ListDraft{.parentId = parentId, .name = "Child"})));
    auto layoutsPtr = std::make_unique<TrackColumnLayouts>(changes);
    auto const state = std::vector{TrackColumnState{.field = rt::TrackField::Duration, .width = 17}};
    layoutsPtr->restore({{parentId, state}, {childId, state}});
    auto removed = std::vector<ListId>{};
    auto sub = layoutsPtr->signalChanged().connect(
      [&removed, &layoutsPtr](ListId const listId)
      {
        removed.push_back(listId);

        if (removed.size() == 1)
        {
          layoutsPtr.reset();
        }
      });

    REQUIRE(commandsFixture.runTask(commands.deleteListAndDescendants(parentId)));

    CHECK(removed == std::vector{parentId, childId});
    CHECK(layoutsPtr == nullptr);
  }

  TEST_CASE("TrackColumnLayouts - bulk state emits only when changed", "[uimodel][unit][library][presentation]")
  {
    auto store = TrackColumnLayouts{};
    auto events = std::vector<ListId>{};
    auto sub = store.signalChanged().connect([&events](ListId listId) noexcept { events.push_back(listId); });
    auto const layouts = std::map<ListId, std::vector<TrackColumnState>>{
      {rt::kAllTracksListId, {TrackColumnState{.field = rt::TrackField::Duration, .width = 95, .weight = -1.0}}},
    };

    store.restore(layouts);
    store.restore(layouts);

    REQUIRE(events.size() == 1);
    CHECK(events[0] == kInvalidListId);
    REQUIRE(store.snapshot().at(rt::kAllTracksListId).size() == 1);
    CHECK(store.snapshot().at(rt::kAllTracksListId).front().field == rt::TrackField::Duration);
    CHECK(store.snapshot().at(rt::kAllTracksListId).front().width == 95);
  }
} // namespace ao::uimodel::test
