// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/library/list/ListMembershipAuthoringSession.h>

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/i18n/IcuTextOrdering.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryReader.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("ListMembershipAuthoringSession - discovers only direct tag targets",
            "[uimodel][unit][list][list-membership]")
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
    auto writerFixture = rt::test::LibraryWriterFixture{storage.library(), changes};
    auto const tag = std::array{std::string{"road-trip"}};
    REQUIRE(writerFixture.editTags(std::array{trackId}, tag, {}));
    auto sessionRes = ListMembershipAuthoringSession::begin(
      writerFixture.library(), std::array{trackId}, ao::test::messageCatalog("de-DE"));
    REQUIRE(sessionRes);

    auto const result = writerFixture.runTask((*sessionRes)->removeFromList(listId));

    REQUIRE(result);
    CHECK(result->status == rt::AuthoringStatus::Applied);
    CHECK(result->forgottenPositionCount == 1);
    CHECK(result->notificationText ==
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
    auto writerFixture = rt::test::LibraryWriterFixture{storage.library(), changes};
    auto sessionRes = ListMembershipAuthoringSession::begin(
      writerFixture.library(), std::array{trackId}, ao::test::messageCatalog("de-DE"));
    REQUIRE(sessionRes);

    auto const result = writerFixture.runTask((*sessionRes)->addToList(listId));

    REQUIRE(result);
    CHECK(result->status == rt::AuthoringStatus::Applied);
    CHECK(result->targetTrackCount == 1);
    CHECK(result->changedTrackCount == 1);
    CHECK(result->notificationText == R"(#"road-trip" wurde für 1 Titel in Road Trip hinzugefügt.)");

    auto scope = writerFixture.library().reader();
    CHECK(scope.selectionTags(std::array{trackId}) == std::vector<std::string>{"road-trip"});
  }
} // namespace ao::uimodel::test
