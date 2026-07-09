// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/uimodel/library/property/TagEditWorkflow.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace ao::uimodel::test
{
  using namespace ao::rt::test;

  namespace
  {
    bool trackHasTag(MusicLibraryFixture& libraryFixture, TrackId trackId, std::string const& expectedTag)
    {
      auto transaction = libraryFixture.library().readTransaction();
      auto reader = libraryFixture.library().tracks().reader(transaction);
      auto const optView = reader.get(trackId);

      if (!optView)
      {
        return false;
      }

      auto const& dictionary = libraryFixture.library().dictionary();

      return std::ranges::any_of(
        optView->tags(), [&](auto const tagId) { return dictionary.getOrDefault(tagId) == expectedTag; });
    }

    std::vector<std::string> trackTagNames(MusicLibraryFixture& libraryFixture, TrackId trackId)
    {
      auto transaction = libraryFixture.library().readTransaction();
      auto reader = libraryFixture.library().tracks().reader(transaction);
      auto const optView = reader.get(trackId);
      REQUIRE(optView);

      auto result = std::vector<std::string>{};
      auto const& dictionary = libraryFixture.library().dictionary();

      for (auto const tagId : optView->tags())
      {
        result.emplace_back(dictionary.getOrDefault(tagId));
      }

      std::ranges::sort(result);
      return result;
    }
  } // namespace

  TEST_CASE("TagEditWorkflow - mutations report messages and final tag state", "[uimodel][unit][workflow][tag]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = rt::LibraryChanges{};
    auto writer = rt::LibraryWriter{libraryFixture.library(), changes};
    auto workflow = TagEditWorkflow{writer};

    auto trackId = libraryFixture.addTrack("Target 1");
    auto trackId2 = libraryFixture.addTrack("Target 2");

    SECTION("no selected tracks does not mutate the library")
    {
      auto const req = TagEditRequest{.tagsToAdd = {"Tag1"}};
      auto result = workflow.apply(req);
      CHECK_FALSE(result.applied);
      CHECK(result.notificationText.empty());
      CHECK(trackTagNames(libraryFixture, trackId).empty());
      CHECK(trackTagNames(libraryFixture, trackId2).empty());
    }

    SECTION("empty tag changes do not mutate selected tracks")
    {
      auto const req = TagEditRequest{.selectedIds = {trackId}};
      auto result = workflow.apply(req);
      CHECK_FALSE(result.applied);
      CHECK(result.notificationText.empty());
      CHECK(trackTagNames(libraryFixture, trackId).empty());
    }

    SECTION("adding a single tag mutates the selected track and reports the count")
    {
      auto const req = TagEditRequest{.selectedIds = {trackId}, .tagsToAdd = {"Tag1"}};
      auto result = workflow.apply(req);
      CHECK(result.applied);
      CHECK(result.notificationText == "Tags added 1 for 1 track");
      CHECK(trackTagNames(libraryFixture, trackId) == std::vector<std::string>{"Tag1"});
      CHECK(trackTagNames(libraryFixture, trackId2).empty());
    }

    SECTION("removing a single tag mutates every selected track and reports the count")
    {
      auto const initialTags = std::vector<std::string>{"Tag1"};
      REQUIRE(writer.editTags(std::vector{trackId, trackId2}, initialTags, {}));
      REQUIRE(trackTagNames(libraryFixture, trackId) == std::vector<std::string>{"Tag1"});
      REQUIRE(trackTagNames(libraryFixture, trackId2) == std::vector<std::string>{"Tag1"});

      auto const req = TagEditRequest{.selectedIds = {trackId, trackId2}, .tagsToRemove = {"Tag1"}};
      auto result = workflow.apply(req);
      CHECK(result.applied);
      CHECK(result.notificationText == "Tags removed 1 for 2 tracks");
      CHECK(trackTagNames(libraryFixture, trackId).empty());
      CHECK(trackTagNames(libraryFixture, trackId2).empty());
    }

    SECTION("adding and removing tags reports requested counts and final tag sets")
    {
      auto const req =
        TagEditRequest{.selectedIds = {trackId, trackId2}, .tagsToAdd = {"Tag1", "Tag2"}, .tagsToRemove = {"Tag3"}};
      auto result = workflow.apply(req);
      CHECK(result.applied);
      CHECK(result.notificationText == "Tags added 2 and removed 1 for 2 tracks");
      CHECK(trackTagNames(libraryFixture, trackId) == std::vector<std::string>{"Tag1", "Tag2"});
      CHECK(trackTagNames(libraryFixture, trackId2) == std::vector<std::string>{"Tag1", "Tag2"});
    }

    SECTION("adding and removing tags mutates the library in one request")
    {
      REQUIRE(writer.editTags(std::vector{trackId}, std::vector<std::string>{"OldTag"}, {}));

      auto const req = TagEditRequest{.selectedIds = {trackId}, .tagsToAdd = {"NewTag"}, .tagsToRemove = {"OldTag"}};

      auto result = workflow.apply(req);

      CHECK(result.applied);
      CHECK(result.notificationText == "Tags added 1 and removed 1 for 1 track");
      CHECK(trackHasTag(libraryFixture, trackId, "NewTag"));
      CHECK_FALSE(trackHasTag(libraryFixture, trackId, "OldTag"));
    }
  }
} // namespace ao::uimodel::test
