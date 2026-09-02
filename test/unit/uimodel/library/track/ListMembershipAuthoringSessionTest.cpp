// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/AsyncTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/i18n/IcuTextOrdering.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("ListMembershipAuthoringSession - discovers only direct tag targets",
            "[uimodel][unit][track-authoring][list-membership]")
  {
    auto const lists = std::array{
      rt::ListNode{.id = ListId{1}, .name = "Zulu", .expression = "#zulu"},
      rt::ListNode{.id = ListId{2}, .name = "Computed", .expression = "#road and $year >= 2020"},
      rt::ListNode{.id = ListId{3}, .name = "Road Trip", .expression = R"(#"road-trip")"},
      rt::ListNode{.id = ListId{4}, .name = "Empty"},
    };

    auto const result = writableTagListTargets(lists);

    REQUIRE(result.size() == 2);
    CHECK(result[0] == WritableTagListTarget{.listId = ListId{3}, .name = "Road Trip", .tag = "road-trip"});
    CHECK(result[1] == WritableTagListTarget{.listId = ListId{1}, .name = "Zulu", .tag = "zulu"});
  }

  TEST_CASE("ListMembershipAuthoringSession - writable targets use locale order and List identity fallback",
            "[uimodel][unit][list-membership][collation]")
  {
    auto policyRes = i18n::createIcuTextOrderingPolicy("de-DE");
    REQUIRE(policyRes);

    SECTION("Locale order")
    {
      auto const lists = std::array{
        rt::ListNode{.id = ListId{2}, .name = "Zulu", .expression = "#zulu"},
        rt::ListNode{.id = ListId{1}, .name = "Äther", .expression = "#aether"},
      };

      auto const result = writableTagListTargets(lists, policyRes->get());
      REQUIRE(result.size() == 2);
      CHECK(result[0].listId == ListId{1});
      CHECK(result[1].listId == ListId{2});
    }

    SECTION("Equal locale keys")
    {
      auto const lists = std::array{
        rt::ListNode{.id = ListId{9}, .name = "ABC", .expression = "#ascii"},
        rt::ListNode{.id = ListId{3}, .name = "ＡＢＣ", .expression = "#wide"},
      };

      auto const result = writableTagListTargets(lists, policyRes->get());
      REQUIRE(result.size() == 2);
      CHECK(result[0].listId == ListId{3});
      CHECK(result[1].listId == ListId{9});
    }
  }

  TEST_CASE("ListMembershipAuthoringSession - formats edit notifications",
            "[uimodel][unit][list-membership][localization]")
  {
    auto const& catalog = ao::test::messageCatalog("en-US");

    CHECK(formatListMembershipEditNotification(catalog,
                                               ListMembershipEditResult{.status = rt::AuthoringStatus::Applied,
                                                                        .operation = ListMembershipOperation::Add,
                                                                        .listName = "Road",
                                                                        .tag = "road",
                                                                        .changedTrackCount = 2}) ==
          "Added #road to 2 tracks in Road.");
    CHECK(formatListMembershipEditNotification(catalog,
                                               ListMembershipEditResult{.status = rt::AuthoringStatus::Busy,
                                                                        .operation = ListMembershipOperation::Add,
                                                                        .listName = "Road",
                                                                        .tag = "road"}) ==
          "Library is busy. Try again.");
    CHECK(formatListMembershipEditNotification(catalog,
                                               ListMembershipEditResult{.status = rt::AuthoringStatus::NoOp,
                                                                        .operation = ListMembershipOperation::Remove,
                                                                        .listName = "Road",
                                                                        .tag = "road"}) ==
          "No #road membership or saved position remained in Road.");
    CHECK(formatListMembershipEditNotification(catalog,
                                               ListMembershipEditResult{.status = rt::AuthoringStatus::Applied,
                                                                        .operation = ListMembershipOperation::Remove,
                                                                        .listName = "Road",
                                                                        .tag = "road",
                                                                        .changedTrackCount = 2,
                                                                        .forgottenPositionCount = 1}) ==
          "Removed #road from 2 tracks and forgot 1 saved position in Road.");
  }

  TEST_CASE("ListMembershipAuthoringSession - Remove notification uses the injected locale",
            "[uimodel][unit][list-membership][localization]")
  {
    auto storage = rt::test::MusicLibraryFixture{};
    auto const trackId = storage.addTrack("Road Song");
    auto transaction = library::test::writeTransaction(storage.library());
    auto builder = library::ListBuilder::makeEmpty().name("Road Trip").filter(R"(#"road-trip")");
    builder.orderTrackIds().add(trackId);
    auto createRes =
      transaction.apply([&builder](library::LibraryWrite& write) { return write.lists().create(builder); });
    REQUIRE(createRes);
    auto const listId = *createRes;
    REQUIRE(transaction.commit());

    auto changes = rt::test::makeStateOnlyLibraryChanges(storage.library());
    auto commandsFixture = rt::test::LibraryCommandsFixture{storage.library(), changes};
    auto const tag = std::array{std::string{"road-trip"}};
    REQUIRE(commandsFixture.editTags(std::array{trackId}, tag, {}));
    auto sessionRes = ListMembershipAuthoringSession::begin(commandsFixture.library(), std::array{trackId});
    REQUIRE(sessionRes);

    auto const result = commandsFixture.runTask(sessionRes->removeFromList(listId));

    REQUIRE(result);
    CHECK(result->status == rt::AuthoringStatus::Applied);
    CHECK(result->forgottenPositionCount == 1);
    CHECK(formatListMembershipEditNotification(ao::test::messageCatalog("de-DE"), *result) ==
          R"(#"road-trip" wurde von 1 Titel entfernt und 1 gespeicherte Position wurde in Road Trip verworfen.)");
  }

  TEST_CASE("ListMembershipAuthoringSession - Add updates membership and uses the injected locale",
            "[uimodel][unit][list-membership][localization]")
  {
    auto storage = rt::test::MusicLibraryFixture{};
    auto const trackId = storage.addTrack("Road Song");
    auto transaction = library::test::writeTransaction(storage.library());
    auto builder = library::ListBuilder::makeEmpty().name("Road Trip").filter(R"(#"road-trip")");
    auto createRes =
      transaction.apply([&builder](library::LibraryWrite& write) { return write.lists().create(builder); });
    REQUIRE(createRes);
    auto const listId = *createRes;
    REQUIRE(transaction.commit());

    auto changes = rt::test::makeStateOnlyLibraryChanges(storage.library());
    auto commandsFixture = rt::test::LibraryCommandsFixture{storage.library(), changes};
    auto sessionRes = ListMembershipAuthoringSession::begin(commandsFixture.library(), std::array{trackId});
    REQUIRE(sessionRes);

    auto const result = commandsFixture.runTask(sessionRes->addToList(listId));

    REQUIRE(result);
    CHECK(result->status == rt::AuthoringStatus::Applied);
    CHECK(result->targetTrackCount == 1);
    CHECK(result->changedTrackCount == 1);
    CHECK(formatListMembershipEditNotification(ao::test::messageCatalog("de-DE"), *result) ==
          R"(#"road-trip" wurde für 1 Titel in Road Trip hinzugefügt.)");

    auto scope = commandsFixture.library().snapshot();
    CHECK(scope.selectionTags(std::array{trackId}) == std::vector<std::string>{"road-trip"});
  }

  TEST_CASE("ListMembershipAuthoringSession - pending edit outlives moved and destroyed facades",
            "[uimodel][regression][list-membership][concurrency]")
  {
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<ListMembershipAuthoringSession>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<ListMembershipAuthoringSession>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<ListMembershipAuthoringSession>);

    auto storage = rt::test::MusicLibraryFixture{};
    auto const trackId = storage.addTrack("Road Song");
    auto transaction = library::test::writeTransaction(storage.library());
    auto builder = library::ListBuilder::makeEmpty().name("Road Trip").filter(R"(#"road-trip")");
    auto createRes =
      transaction.apply([&builder](library::LibraryWrite& write) { return write.lists().create(builder); });
    REQUIRE(createRes);
    auto const listId = *createRes;
    REQUIRE(transaction.commit());

    auto executor = rt::test::ManualExecutor{};
    auto changes = rt::test::makeLibraryChanges(executor, storage.library());
    auto commandsFixture = rt::test::LibraryCommandsFixture{storage.library(), changes, executor};
    auto completedPtr = std::make_shared<std::atomic_bool>(false);
    auto future = [&]
    {
      auto source =
        ao::test::requireValue(ListMembershipAuthoringSession::begin(commandsFixture.library(), std::array{trackId}));
      auto moved = std::move(source);
      auto task = moved.addToList(listId);
      auto pending = commandsFixture.runtime().spawn(rt::test::flagCompletion(completedPtr, std::move(task)));
      REQUIRE(executor.waitUntilQueued());
      return pending;
    }();

    REQUIRE(executor.drainUntil([&completedPtr] { return completedPtr->load(); }));
    auto const result = future.get();
    REQUIRE(result);
    CHECK(result->status == rt::AuthoringStatus::Applied);
    CHECK(result->changedTrackCount == 1);
    CHECK(commandsFixture.library().snapshot().selectionTags(std::array{trackId}) ==
          std::vector<std::string>{"road-trip"});

    auto cleanupSession =
      ao::test::requireValue(ListMembershipAuthoringSession::begin(commandsFixture.library(), std::array{trackId}));
    auto const cleanupRes = commandsFixture.runTask(cleanupSession.removeFromList(listId));
    REQUIRE(cleanupRes);
    CHECK(cleanupRes->status == rt::AuthoringStatus::Applied);
    CHECK(commandsFixture.library().snapshot().selectionTags(std::array{trackId}).empty());
  }
} // namespace ao::uimodel::test
