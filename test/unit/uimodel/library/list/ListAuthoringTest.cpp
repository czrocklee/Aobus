// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/library/list/ListAuthoring.h>

#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/rt/ListMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibrarySnapshot.h>

#include <catch2/catch_test_macros.hpp>

#include <utility>

namespace ao::uimodel::test
{
  TEST_CASE("ListAuthoring - owns List CRUD across native frontends", "[uimodel][unit][list-authoring]")
  {
    auto storage = rt::test::MusicLibraryFixture{};
    auto changes = rt::test::makeStateOnlyLibraryChanges(storage.library());
    auto commands = rt::test::LibraryCommandsFixture{storage.library(), changes};
    auto const listIdRes = commands.runTask(saveList(&commands.library(), rt::ListDraft{.name = "First name"}));
    REQUIRE(listIdRes);

    auto updated = rt::ListDraft{.listId = *listIdRes, .name = "Renamed"};
    auto const updatedIdRes = commands.runTask(saveList(&commands.library(), std::move(updated)));
    REQUIRE(updatedIdRes);
    CHECK(*updatedIdRes == *listIdRes);
    REQUIRE(commands.library().snapshot().listNode(*listIdRes));
    CHECK(commands.library().snapshot().listNode(*listIdRes)->name == "Renamed");

    auto const previewRes = commands.runTask(previewListDeletion(&commands.library(), *listIdRes, false));
    REQUIRE(previewRes);
    REQUIRE(previewRes->deletedLists.size() == 1);
    CHECK(previewRes->rootListId == *listIdRes);
    CHECK(previewRes->deletedLists.front().listId == *listIdRes);

    auto const deletedRes = commands.runTask(deleteList(&commands.library(), *listIdRes, false));
    REQUIRE(deletedRes);
    CHECK_FALSE(commands.library().snapshot().listNode(*listIdRes));
  }

  TEST_CASE("ListAuthoring - previews and deletes a List subtree", "[uimodel][unit][list-authoring]")
  {
    auto storage = rt::test::MusicLibraryFixture{};
    auto changes = rt::test::makeStateOnlyLibraryChanges(storage.library());
    auto commands = rt::test::LibraryCommandsFixture{storage.library(), changes};

    auto const parentIdRes = commands.runTask(saveList(&commands.library(), rt::ListDraft{.name = "Parent"}));
    REQUIRE(parentIdRes);
    auto const childIdRes =
      commands.runTask(saveList(&commands.library(), rt::ListDraft{.parentId = *parentIdRes, .name = "Child"}));
    REQUIRE(childIdRes);

    auto const previewRes = commands.runTask(previewListDeletion(&commands.library(), *parentIdRes, true));
    REQUIRE(previewRes);
    CHECK(previewRes->rootListId == *parentIdRes);
    REQUIRE(previewRes->deletedLists.size() == 2);
    CHECK(previewRes->deletedLists[0].listId == *parentIdRes);
    CHECK(previewRes->deletedLists[1].listId == *childIdRes);

    auto const deletedRes = commands.runTask(deleteList(&commands.library(), *parentIdRes, true));
    REQUIRE(deletedRes);
    CHECK(*deletedRes == *previewRes);
    CHECK_FALSE(commands.library().snapshot().listNode(*parentIdRes));
    CHECK_FALSE(commands.library().snapshot().listNode(*childIdRes));
  }
} // namespace ao::uimodel::test
