// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/library/list/ListAuthoring.h>

#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibrarySnapshot.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("ListAuthoring - owns List CRUD across native frontends", "[uimodel][integration][list-authoring]")
  {
    auto storage = rt::test::MusicLibraryFixture{};
    auto changes = rt::test::makeStateOnlyLibraryChanges(storage.library());
    auto commands = rt::test::LibraryCommandsFixture{storage.library(), changes};
    auto const listIdRes = commands.runTask(saveListAsync(&commands.library(), rt::ListDraft{.name = "First name"}));
    REQUIRE(listIdRes);
    auto const optCreated = commands.library().snapshot().listNode(*listIdRes);
    REQUIRE(optCreated);
    CHECK(optCreated->id == *listIdRes);
    CHECK(optCreated->name == "First name");

    auto updated = rt::ListDraft{.listId = *listIdRes, .name = "Renamed"};
    auto const updatedIdRes = commands.runTask(saveListAsync(&commands.library(), std::move(updated)));
    REQUIRE(updatedIdRes);
    CHECK(*updatedIdRes == *listIdRes);
    REQUIRE(commands.library().snapshot().listNode(*listIdRes));
    CHECK(commands.library().snapshot().listNode(*listIdRes)->name == "Renamed");

    auto const previewRes = commands.runTask(previewListDeletionAsync(&commands.library(), *listIdRes, false));
    REQUIRE(previewRes);
    REQUIRE(previewRes->deletedLists.size() == 1);
    CHECK(previewRes->rootListId == *listIdRes);
    CHECK(previewRes->deletedLists.front().listId == *listIdRes);

    auto const deletedRes = commands.runTask(deleteListAsync(&commands.library(), *listIdRes, false));
    REQUIRE(deletedRes);
    CHECK_FALSE(commands.library().snapshot().listNode(*listIdRes));
  }

  TEST_CASE("ListAuthoring - previews and deletes a List subtree", "[uimodel][integration][list-authoring]")
  {
    auto storage = rt::test::MusicLibraryFixture{};
    auto changes = rt::test::makeStateOnlyLibraryChanges(storage.library());
    auto commands = rt::test::LibraryCommandsFixture{storage.library(), changes};

    auto const parentIdRes = commands.runTask(saveListAsync(&commands.library(), rt::ListDraft{.name = "Parent"}));
    REQUIRE(parentIdRes);
    auto const childIdRes =
      commands.runTask(saveListAsync(&commands.library(), rt::ListDraft{.parentId = *parentIdRes, .name = "Child"}));
    REQUIRE(childIdRes);

    auto const previewRes = commands.runTask(previewListDeletionAsync(&commands.library(), *parentIdRes, true));
    REQUIRE(previewRes);
    CHECK(previewRes->rootListId == *parentIdRes);
    REQUIRE(previewRes->deletedLists.size() == 2);
    CHECK(previewRes->deletedLists[0].listId == *parentIdRes);
    CHECK(previewRes->deletedLists[1].listId == *childIdRes);

    auto const deletedRes = commands.runTask(deleteListAsync(&commands.library(), *parentIdRes, true));
    REQUIRE(deletedRes);
    CHECK(*deletedRes == *previewRes);
    CHECK_FALSE(commands.library().snapshot().listNode(*parentIdRes));
    CHECK_FALSE(commands.library().snapshot().listNode(*childIdRes));
  }

  TEST_CASE("ListAuthoring - forwards missing List errors without changing retained state",
            "[uimodel][integration][list-authoring]")
  {
    auto storage = rt::test::MusicLibraryFixture{};
    auto changes = rt::test::makeStateOnlyLibraryChanges(storage.library());
    auto commands = rt::test::LibraryCommandsFixture{storage.library(), changes};
    auto const retainedIdRes = commands.runTask(saveListAsync(
      &commands.library(), rt::ListDraft{.name = "Retained", .description = "Sentinel", .expression = "$year = 2000"}));
    REQUIRE(retainedIdRes);
    auto const revisionBefore = commands.library().snapshot().revision();
    auto const missingId = ListId{999};
    REQUIRE_FALSE(commands.library().snapshot().listNode(missingId));

    SECTION("update")
    {
      auto const res =
        commands.runTask(saveListAsync(&commands.library(), rt::ListDraft{.listId = missingId, .name = "Missing"}));
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::NotFound);
    }

    SECTION("single preview")
    {
      auto const res = commands.runTask(previewListDeletionAsync(&commands.library(), missingId, false));
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::NotFound);
    }

    SECTION("subtree preview")
    {
      auto const res = commands.runTask(previewListDeletionAsync(&commands.library(), missingId, true));
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::NotFound);
    }

    SECTION("single deletion")
    {
      auto const res = commands.runTask(deleteListAsync(&commands.library(), missingId, false));
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::NotFound);
    }

    SECTION("subtree deletion")
    {
      auto const res = commands.runTask(deleteListAsync(&commands.library(), missingId, true));
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::NotFound);
    }

    auto snapshot = commands.library().snapshot();
    CHECK(snapshot.revision() == revisionBefore);
    auto const lists = snapshot.lists();
    REQUIRE(lists.size() == 1);
    CHECK(lists[0].id == *retainedIdRes);
    CHECK(lists[0].parentId == kInvalidListId);
    CHECK(lists[0].name == "Retained");
    CHECK(lists[0].description == "Sentinel");
    CHECK(lists[0].expression == "$year = 2000");
  }

  TEST_CASE("ListAuthoring - ordinary deletion preserves a parent and its dependent child",
            "[uimodel][integration][list-authoring]")
  {
    auto storage = rt::test::MusicLibraryFixture{};
    auto changes = rt::test::makeStateOnlyLibraryChanges(storage.library());
    auto commands = rt::test::LibraryCommandsFixture{storage.library(), changes};
    auto const parentIdRes = commands.runTask(saveListAsync(&commands.library(), rt::ListDraft{.name = "Parent"}));
    REQUIRE(parentIdRes);
    auto const childIdRes =
      commands.runTask(saveListAsync(&commands.library(), rt::ListDraft{.parentId = *parentIdRes, .name = "Child"}));
    REQUIRE(childIdRes);
    auto const revisionBefore = commands.library().snapshot().revision();

    SECTION("preview")
    {
      auto const res = commands.runTask(previewListDeletionAsync(&commands.library(), *parentIdRes, false));
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::Conflict);
    }

    SECTION("delete")
    {
      auto const res = commands.runTask(deleteListAsync(&commands.library(), *parentIdRes, false));
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::Conflict);
    }

    auto snapshot = commands.library().snapshot();
    CHECK(snapshot.revision() == revisionBefore);
    REQUIRE(snapshot.lists().size() == 2);
    auto const optParent = snapshot.listNode(*parentIdRes);
    auto const optChild = snapshot.listNode(*childIdRes);
    REQUIRE(optParent);
    REQUIRE(optChild);
    CHECK(optParent->name == "Parent");
    CHECK(optParent->parentId == kInvalidListId);
    CHECK(optChild->name == "Child");
    CHECK(optChild->parentId == *parentIdRes);
  }

  TEST_CASE("ListAuthoring - forwards writable tag removal for single and subtree deletion",
            "[uimodel][integration][list-authoring]")
  {
    auto const includeDescendants = GENERATE(false, true);
    auto const removeTag = GENERATE(false, true);
    CAPTURE(includeDescendants, removeTag);
    auto storage = rt::test::MusicLibraryFixture{};
    auto changes = rt::test::makeStateOnlyLibraryChanges(storage.library());
    auto commands = rt::test::LibraryCommandsFixture{storage.library(), changes};
    auto const trackId = commands.addTrack({.title = "Tagged", .tags = {"favorite", "keep"}});
    auto const listIdRes = commands.runTask(
      saveListAsync(&commands.library(), rt::ListDraft{.name = "Favorites", .expression = "#favorite"}));
    REQUIRE(listIdRes);
    auto const ids = std::array{trackId};
    auto beforeTags = commands.library().snapshot().selectionTags(ids);
    std::ranges::sort(beforeTags);
    REQUIRE(beforeTags == std::vector<std::string>{"favorite", "keep"});

    auto const res = commands.runTask(deleteListAsync(&commands.library(),
                                                      *listIdRes,
                                                      includeDescendants,
                                                      rt::DeleteListOptions{.removeWritableTagFromTracks = removeTag}));
    REQUIRE(res);
    CHECK(res->rootListId == *listIdRes);
    REQUIRE(res->deletedLists.size() == 1);
    CHECK(res->deletedLists[0].listId == *listIdRes);
    REQUIRE(res->deletedLists[0].optTagImpact);
    CHECK(res->deletedLists[0].optTagImpact->tag == "favorite");
    CHECK(res->deletedLists[0].optTagImpact->removedFromTrackCount == (removeTag ? 1U : 0U));
    auto snapshot = commands.library().snapshot();
    CHECK_FALSE(snapshot.listNode(*listIdRes));
    auto afterTags = snapshot.selectionTags(ids);
    std::ranges::sort(afterTags);
    CHECK(afterTags == (removeTag ? std::vector<std::string>{"keep"} : std::vector<std::string>{"favorite", "keep"}));
  }
} // namespace ao::uimodel::test
