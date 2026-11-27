// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/library/list/ListAuthoring.h>

#include "test/MessageCatalogTestSupport.h"
#include "test/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/uimodel/library/LibraryEditing.h>

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
  TEST_CASE("ListAuthoring - describes a single List without offering unavailable cleanup")
  {
    auto const preview = rt::DeleteListSubtreeReply{
      .rootListId = ListId{41},
      .deletedLists = {{.listId = ListId{41}, .name = "Sommer"}},
    };
    auto const german = ao::test::messageCatalog("de-DE");
    auto const description = describeListDeletion(german, preview, false);

    CHECK(description.labels.title == i18n::MessageId::ListDeleteTitle);
    CHECK(description.labels.action == i18n::MessageId::ListDeleteAction);
    CHECK(description.question.starts_with("„Sommer“ löschen?"));
    CHECK_FALSE(description.question.contains("(41)"));
    CHECK_FALSE(description.optCleanup);
  }

  TEST_CASE("ListAuthoring - describes every subtree List and all surviving references to the root tag")
  {
    auto const preview = rt::DeleteListSubtreeReply{
      .rootListId = ListId{41},
      .deletedLists = {{.listId = ListId{41},
                        .name = "Trail",
                        .optTagImpact =
                          rt::DeleteListReply::TagImpact{
                            .tag = R"(雪"Mix\1)",
                            .taggedTrackCount = 2,
                            .otherListReferences = {{.listId = ListId{7}, .name = "One"},
                                                    {.listId = ListId{8}, .name = "Two"},
                                                    {.listId = ListId{9}, .name = "Three"}},
                          }},
                       {.listId = ListId{42}, .name = "Child"}},
    };
    auto const& english = ao::test::englishMessageCatalog();
    auto const description = describeListDeletion(english, preview, true);

    CHECK(description.labels.title == i18n::MessageId::ListDeleteSubtreeTitle);
    CHECK(description.labels.action == i18n::MessageId::ListDeleteAllAction);
    CHECK(description.question.starts_with("Delete 2 Lists in this derived subtree?"));
    CHECK(description.question.contains("• Trail (41)\n• Child (42)\n"));
    CHECK(description.question.contains("Tags used by nested Playlists are kept."));
    CHECK(description.question.contains("Music files will be kept."));
    REQUIRE(description.optCleanup);
    CHECK(description.optCleanup->question == R"(Also remove #"雪\"Mix\\1" from 2 tracks)");
    CHECK(description.optCleanup->referencesWarning ==
          R"(#"雪\"Mix\\1" is also referenced by: One, Two, Three. Removing it may change those Lists.)");

    auto const pseudo = ao::test::messageCatalog("qps-ploc");
    CHECK(describeListDeletion(pseudo, preview, true).question.contains("• Trail (41)\n• Child (42)\n"));
  }

  TEST_CASE("ListAuthoring - owns List CRUD across native frontends")
  {
    auto storage = rt::test::MusicLibraryFixture{};
    auto changes = rt::test::makeStateOnlyLibraryChanges(storage.library());
    auto commands = rt::test::LibraryCommandsFixture{storage.library(), changes};
    auto const editing = LibraryEditing{commands.commands()};
    // The temporary handle is gone before the lazy task starts; its command owner still lives.
    auto save = saveListAsync(LibraryEditing{commands.commands()}, rt::ListDraft{.name = "First name"});
    auto const listIdRes = commands.runTask(std::move(save));
    REQUIRE(listIdRes);
    auto const optCreated = editing.library().snapshot().listNode(*listIdRes);
    REQUIRE(optCreated);
    CHECK(optCreated->id == *listIdRes);
    CHECK(optCreated->name == "First name");

    auto updated = rt::ListDraft{.listId = *listIdRes, .name = "Renamed"};
    auto const updatedIdRes = commands.runTask(saveListAsync(editing, std::move(updated)));
    REQUIRE(updatedIdRes);
    CHECK(*updatedIdRes == *listIdRes);
    REQUIRE(commands.library().snapshot().listNode(*listIdRes));
    CHECK(commands.library().snapshot().listNode(*listIdRes)->name == "Renamed");

    auto const previewRes = commands.runTask(previewListDeletionAsync(editing, *listIdRes, false));
    REQUIRE(previewRes);
    REQUIRE(previewRes->deletedLists.size() == 1);
    CHECK(previewRes->rootListId == *listIdRes);
    CHECK(previewRes->deletedLists.front().listId == *listIdRes);

    auto const deletedRes = commands.runTask(deleteListAsync(editing, *listIdRes, false));
    REQUIRE(deletedRes);
    CHECK_FALSE(commands.library().snapshot().listNode(*listIdRes));
  }

  TEST_CASE("ListAuthoring - previews and deletes a List subtree")
  {
    auto storage = rt::test::MusicLibraryFixture{};
    auto changes = rt::test::makeStateOnlyLibraryChanges(storage.library());
    auto commands = rt::test::LibraryCommandsFixture{storage.library(), changes};
    auto const editing = LibraryEditing{commands.commands()};

    auto const parentIdRes = commands.runTask(saveListAsync(editing, rt::ListDraft{.name = "Parent"}));
    REQUIRE(parentIdRes);
    auto const childIdRes =
      commands.runTask(saveListAsync(editing, rt::ListDraft{.parentId = *parentIdRes, .name = "Child"}));
    REQUIRE(childIdRes);

    auto const previewRes = commands.runTask(previewListDeletionAsync(editing, *parentIdRes, true));
    REQUIRE(previewRes);
    CHECK(previewRes->rootListId == *parentIdRes);
    REQUIRE(previewRes->deletedLists.size() == 2);
    CHECK(previewRes->deletedLists[0].listId == *parentIdRes);
    CHECK(previewRes->deletedLists[1].listId == *childIdRes);

    auto const deletedRes = commands.runTask(deleteListAsync(editing, *parentIdRes, true));
    REQUIRE(deletedRes);
    CHECK(*deletedRes == *previewRes);
    CHECK_FALSE(commands.library().snapshot().listNode(*parentIdRes));
    CHECK_FALSE(commands.library().snapshot().listNode(*childIdRes));
  }

  TEST_CASE("ListAuthoring - forwards missing List errors without changing retained state")
  {
    auto storage = rt::test::MusicLibraryFixture{};
    auto changes = rt::test::makeStateOnlyLibraryChanges(storage.library());
    auto commands = rt::test::LibraryCommandsFixture{storage.library(), changes};
    auto const editing = LibraryEditing{commands.commands()};
    auto const retainedIdRes = commands.runTask(saveListAsync(
      editing, rt::ListDraft{.name = "Retained", .description = "Sentinel", .expression = "$year = 2000"}));
    REQUIRE(retainedIdRes);
    auto const revisionBefore = commands.library().snapshot().revision();
    auto const missingId = ListId{999};
    REQUIRE_FALSE(commands.library().snapshot().listNode(missingId));

    SECTION("update")
    {
      auto const res = commands.runTask(saveListAsync(editing, rt::ListDraft{.listId = missingId, .name = "Missing"}));
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::NotFound);
    }

    SECTION("single preview")
    {
      auto const res = commands.runTask(previewListDeletionAsync(editing, missingId, false));
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::NotFound);
    }

    SECTION("subtree preview")
    {
      auto const res = commands.runTask(previewListDeletionAsync(editing, missingId, true));
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::NotFound);
    }

    SECTION("single deletion")
    {
      auto const res = commands.runTask(deleteListAsync(editing, missingId, false));
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::NotFound);
    }

    SECTION("subtree deletion")
    {
      auto const res = commands.runTask(deleteListAsync(editing, missingId, true));
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

  TEST_CASE("ListAuthoring - ordinary deletion preserves a parent and its dependent child")
  {
    auto storage = rt::test::MusicLibraryFixture{};
    auto changes = rt::test::makeStateOnlyLibraryChanges(storage.library());
    auto commands = rt::test::LibraryCommandsFixture{storage.library(), changes};
    auto const editing = LibraryEditing{commands.commands()};
    auto const parentIdRes = commands.runTask(saveListAsync(editing, rt::ListDraft{.name = "Parent"}));
    REQUIRE(parentIdRes);
    auto const childIdRes =
      commands.runTask(saveListAsync(editing, rt::ListDraft{.parentId = *parentIdRes, .name = "Child"}));
    REQUIRE(childIdRes);
    auto const revisionBefore = commands.library().snapshot().revision();

    SECTION("preview")
    {
      auto const res = commands.runTask(previewListDeletionAsync(editing, *parentIdRes, false));
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::Conflict);
    }

    SECTION("delete")
    {
      auto const res = commands.runTask(deleteListAsync(editing, *parentIdRes, false));
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

  TEST_CASE("ListAuthoring - forwards writable tag removal for single and subtree deletion")
  {
    auto const includeDescendants = GENERATE(false, true);
    auto const removeTag = GENERATE(false, true);
    CAPTURE(includeDescendants, removeTag);
    auto storage = rt::test::MusicLibraryFixture{};
    auto changes = rt::test::makeStateOnlyLibraryChanges(storage.library());
    auto commands = rt::test::LibraryCommandsFixture{storage.library(), changes};
    auto const editing = LibraryEditing{commands.commands()};
    auto const trackId = commands.addTrack({.title = "Tagged", .tags = {"favorite", "keep"}});
    auto const listIdRes =
      commands.runTask(saveListAsync(editing, rt::ListDraft{.name = "Favorites", .expression = "#favorite"}));
    REQUIRE(listIdRes);
    auto childId = kInvalidListId;

    if (includeDescendants)
    {
      auto const childRes = commands.runTask(
        saveListAsync(editing, rt::ListDraft{.parentId = *listIdRes, .name = "Child", .expression = "#keep"}));
      REQUIRE(childRes);
      childId = *childRes;
    }

    auto const ids = std::array{trackId};
    auto beforeTags = commands.library().snapshot().selectionTags(ids);
    std::ranges::sort(beforeTags);
    REQUIRE(beforeTags == std::vector<std::string>{"favorite", "keep"});

    auto const res = commands.runTask(deleteListAsync(
      editing, *listIdRes, includeDescendants, rt::DeleteListOptions{.removeWritableTagFromTracks = removeTag}));
    REQUIRE(res);
    CHECK(res->rootListId == *listIdRes);
    REQUIRE(res->deletedLists.size() == (includeDescendants ? 2U : 1U));
    CHECK(res->deletedLists[0].listId == *listIdRes);
    REQUIRE(res->deletedLists[0].optTagImpact);
    CHECK(res->deletedLists[0].optTagImpact->tag == "favorite");
    CHECK(res->deletedLists[0].optTagImpact->removedFromTrackCount == (removeTag ? 1U : 0U));
    auto snapshot = commands.library().snapshot();
    CHECK_FALSE(snapshot.listNode(*listIdRes));

    if (includeDescendants)
    {
      CHECK(res->deletedLists[1].listId == childId);
      CHECK_FALSE(res->deletedLists[1].optTagImpact);
      CHECK_FALSE(snapshot.listNode(childId));
    }

    CHECK(snapshot.trackRow(trackId));
    auto afterTags = snapshot.selectionTags(ids);
    std::ranges::sort(afterTags);
    CHECK(afterTags == (removeTag ? std::vector<std::string>{"keep"} : std::vector<std::string>{"favorite", "keep"}));
  }
} // namespace ao::uimodel::test
