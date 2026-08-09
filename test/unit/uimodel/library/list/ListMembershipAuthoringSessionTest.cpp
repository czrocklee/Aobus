// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/library/list/ListMembershipAuthoringSession.h>

#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/library/LibraryAuthoring.h>

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

  TEST_CASE("ListMembershipAuthoringSession - Remove notification names the tag and forgotten position",
            "[uimodel][unit][list][list-membership]")
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
    auto sessionRes = ListMembershipAuthoringSession::begin(writerFixture.library(), std::array{trackId});
    REQUIRE(sessionRes);

    auto const result = (*sessionRes)->removeFromList(listId);

    REQUIRE(result);
    CHECK(result->status == rt::TrackAuthoringStatus::Applied);
    CHECK(result->forgottenPositionCount == 1);
    CHECK(result->notificationText == R"(Removed #"road-trip" from 1 track and forgot 1 saved position in Road Trip.)");
  }
} // namespace ao::uimodel::test
