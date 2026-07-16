// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/uimodel/library/property/TrackAuthoringTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/uimodel/library/property/TagEditWorkflow.h>
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

  TEST_CASE("TagEditWorkflow - mutations report messages and final tag state", "[uimodel][unit][workflow][tag]")
  {
    auto fixture = TrackAuthoringFixture{2};
    auto const trackId = fixture.trackIds()[0];
    auto const trackId2 = fixture.trackIds()[1];

    SECTION("no selected tracks does not mutate the library")
    {
      auto sessionPtr = beginSession(fixture, std::array{trackId});
      auto workflow = TagEditWorkflow{*sessionPtr};
      auto const result = workflow.apply(TagEditRequest{.tagsToAdd = {"Tag1"}});

      CHECK_FALSE(result.applied);
      CHECK(result.notificationText.empty());
      CHECK(fixture.tags(trackId).empty());
      CHECK(fixture.tags(trackId2).empty());
    }

    SECTION("empty tag changes do not mutate selected tracks")
    {
      auto sessionPtr = beginSession(fixture, std::array{trackId});
      auto workflow = TagEditWorkflow{*sessionPtr};
      auto const result = workflow.apply(TagEditRequest{.selectedIds = {trackId}});

      CHECK_FALSE(result.applied);
      CHECK(result.notificationText.empty());
      CHECK(fixture.tags(trackId).empty());
    }

    SECTION("changed targets reject the edit instead of retargeting it")
    {
      auto sessionPtr = beginSession(fixture, std::array{trackId});
      auto workflow = TagEditWorkflow{*sessionPtr};
      auto const result = workflow.apply(TagEditRequest{.selectedIds = {trackId2}, .tagsToAdd = {"Tag1"}});

      CHECK_FALSE(result.applied);
      CHECK(result.rejected);
      CHECK_FALSE(result.stale);
      CHECK(result.notificationText == "Tag edit targets changed while the editor was open.");
      CHECK(fixture.tags(trackId).empty());
      CHECK(fixture.tags(trackId2).empty());
    }

    SECTION("an intervening commit reports the edit as stale")
    {
      auto sessionPtr = beginSession(fixture, std::array{trackId});
      auto workflow = TagEditWorkflow{*sessionPtr};
      REQUIRE(fixture.library().writer().createList(
        rt::LibraryWriter::ListDraft{.kind = rt::LibraryWriter::ListKind::Manual, .name = "Unrelated"}));

      auto const result = workflow.apply(TagEditRequest{.selectedIds = {trackId}, .tagsToAdd = {"Tag1"}});

      CHECK_FALSE(result.applied);
      CHECK_FALSE(result.rejected);
      CHECK(result.stale);
      CHECK(result.notificationText == "Library changed while the tag editor was open. Reload and try again.");
      CHECK(fixture.tags(trackId).empty());
    }

    SECTION("adding a single tag mutates the selected track and reports the count")
    {
      auto sessionPtr = beginSession(fixture, std::array{trackId});
      auto workflow = TagEditWorkflow{*sessionPtr};
      auto const result = workflow.apply(TagEditRequest{.selectedIds = {trackId}, .tagsToAdd = {"Tag1"}});

      CHECK(result.applied);
      CHECK(result.notificationText == "Tags added 1 for 1 track");
      CHECK(fixture.tags(trackId) == std::vector<std::string>{"Tag1"});
      CHECK(fixture.tags(trackId2).empty());
    }

    SECTION("removing a single tag mutates every selected track and reports the count")
    {
      auto const targetIds = std::array{trackId, trackId2};
      auto sessionPtr = beginSession(fixture, targetIds);
      REQUIRE(sessionPtr->submitTags(std::array{std::string{"Tag1"}}, {}));
      auto workflow = TagEditWorkflow{*sessionPtr};
      auto const result = workflow.apply(TagEditRequest{.selectedIds = {trackId, trackId2}, .tagsToRemove = {"Tag1"}});

      CHECK(result.applied);
      CHECK(result.notificationText == "Tags removed 1 for 2 tracks");
      CHECK(fixture.tags(trackId).empty());
      CHECK(fixture.tags(trackId2).empty());
    }

    SECTION("adding and removing tags reports requested counts and final tag sets")
    {
      auto const targetIds = std::array{trackId, trackId2};
      auto sessionPtr = beginSession(fixture, targetIds);
      auto workflow = TagEditWorkflow{*sessionPtr};
      auto const result = workflow.apply(
        TagEditRequest{.selectedIds = {trackId, trackId2}, .tagsToAdd = {"Tag1", "Tag2"}, .tagsToRemove = {"Tag3"}});

      CHECK(result.applied);
      CHECK(result.notificationText == "Tags added 2 and removed 1 for 2 tracks");
      CHECK(fixture.tags(trackId) == std::vector<std::string>{"Tag1", "Tag2"});
      CHECK(fixture.tags(trackId2) == std::vector<std::string>{"Tag1", "Tag2"});
    }

    SECTION("adding and removing tags mutates the library in one request")
    {
      auto sessionPtr = beginSession(fixture, std::array{trackId});
      REQUIRE(sessionPtr->submitTags(std::array{std::string{"OldTag"}}, {}));
      auto workflow = TagEditWorkflow{*sessionPtr};
      auto const result =
        workflow.apply(TagEditRequest{.selectedIds = {trackId}, .tagsToAdd = {"NewTag"}, .tagsToRemove = {"OldTag"}});

      CHECK(result.applied);
      CHECK(result.notificationText == "Tags added 1 and removed 1 for 1 track");
      CHECK(fixture.tags(trackId) == std::vector<std::string>{"NewTag"});
    }
  }
} // namespace ao::uimodel::test
