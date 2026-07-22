// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/uimodel/library/property/TrackAuthoringTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/uimodel/library/property/TagEdit.h>
#include <ao/uimodel/library/property/TrackAuthoringSession.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ao::uimodel::test
{
  namespace
  {
    std::unique_ptr<TrackAuthoringSession> beginSession(TrackAuthoringFixture& fixture,
                                                        std::span<TrackId const> trackIds)
    {
      auto result = TrackAuthoringSession::begin(fixture.library(), trackIds);
      REQUIRE(result);
      return std::move(*result);
    }
  } // namespace

  TEST_CASE("applyTagEdit reports tag mutations for a bound authoring session", "[uimodel][unit][tag-edit]")
  {
    auto fixture = TrackAuthoringFixture{2};
    auto const trackId = fixture.trackIds()[0];
    auto const trackId2 = fixture.trackIds()[1];

    SECTION("empty tag changes do not submit a mutation")
    {
      auto sessionPtr = beginSession(fixture, std::array{trackId});
      auto const result = applyTagEdit(*sessionPtr, {}, {});

      REQUIRE(result);
      CHECK(result->status == rt::TrackAuthoringStatus::NoOp);
      CHECK(result->notificationText.empty());
      CHECK(fixture.tags(trackId).empty());
    }

    SECTION("an intervening commit reports the edit as stale")
    {
      auto sessionPtr = beginSession(fixture, std::array{trackId});
      REQUIRE(fixture.library().writer().createList(
        rt::LibraryWriter::ListDraft{.kind = rt::LibraryWriter::ListKind::Manual, .name = "Unrelated"}));

      auto const result = applyTagEdit(*sessionPtr, std::array{std::string{"Tag1"}}, {});

      REQUIRE(result);
      CHECK(result->status == rt::TrackAuthoringStatus::Stale);
      CHECK(result->notificationText == "Library changed while the tag editor was open. Reload and try again.");
      CHECK(fixture.tags(trackId).empty());
    }

    SECTION("adding a single tag mutates the bound track and reports the count")
    {
      auto sessionPtr = beginSession(fixture, std::array{trackId});
      auto const result = applyTagEdit(*sessionPtr, std::array{std::string{"Tag1"}}, {});

      REQUIRE(result);
      CHECK(result->status == rt::TrackAuthoringStatus::Applied);
      CHECK(result->notificationText == "Tags added 1 for 1 track");
      CHECK(fixture.tags(trackId) == std::vector<std::string>{"Tag1"});
      CHECK(fixture.tags(trackId2).empty());
    }

    SECTION("removing a tag mutates every bound track and reports the count")
    {
      auto const targetIds = std::array{trackId, trackId2};
      auto sessionPtr = beginSession(fixture, targetIds);
      REQUIRE(sessionPtr->submitTags(std::array{std::string{"Tag1"}}, {}));

      auto const result = applyTagEdit(*sessionPtr, {}, std::array{std::string{"Tag1"}});

      REQUIRE(result);
      CHECK(result->status == rt::TrackAuthoringStatus::Applied);
      CHECK(result->notificationText == "Tags removed 1 for 2 tracks");
      CHECK(fixture.tags(trackId).empty());
      CHECK(fixture.tags(trackId2).empty());
    }

    SECTION("adding and removing tags remains one atomic session submission")
    {
      auto sessionPtr = beginSession(fixture, std::array{trackId});
      REQUIRE(sessionPtr->submitTags(std::array{std::string{"OldTag"}}, {}));

      auto const result =
        applyTagEdit(*sessionPtr, std::array{std::string{"NewTag"}}, std::array{std::string{"OldTag"}});

      REQUIRE(result);
      CHECK(result->status == rt::TrackAuthoringStatus::Applied);
      CHECK(result->notificationText == "Tags added 1 and removed 1 for 1 track");
      CHECK(fixture.tags(trackId) == std::vector<std::string>{"NewTag"});
    }
  }
} // namespace ao::uimodel::test
