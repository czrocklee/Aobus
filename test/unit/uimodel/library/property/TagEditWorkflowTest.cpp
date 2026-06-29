// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
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
    bool trackHasTag(TestMusicLibrary& testLib, TrackId trackId, std::string const& expectedTag)
    {
      auto txn = testLib.library().readTransaction();
      auto reader = testLib.library().tracks().reader(txn);
      auto const optView = reader.get(trackId);

      if (!optView)
      {
        return false;
      }

      auto const& dictionary = testLib.library().dictionary();

      return std::ranges::any_of(
        optView->tags(), [&](auto const tagId) { return dictionary.getOrDefault(tagId) == expectedTag; });
    }

    std::vector<std::string> trackTagNames(TestMusicLibrary& testLib, TrackId trackId)
    {
      auto txn = testLib.library().readTransaction();
      auto reader = testLib.library().tracks().reader(txn);
      auto const optView = reader.get(trackId);
      REQUIRE(optView);

      auto result = std::vector<std::string>{};
      auto const& dictionary = testLib.library().dictionary();

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
    auto testLib = TestMusicLibrary{};
    auto changes = rt::LibraryChanges{};
    auto writer = rt::LibraryWriter{testLib.library(), changes};
    auto workflow = TagEditWorkflow{writer};

    auto trackId = testLib.addTrack("Target 1");
    auto trackId2 = testLib.addTrack("Target 2");

    SECTION("no selected tracks does not mutate the library")
    {
      auto const req = TagEditRequest{.tagsToAdd = {"Tag1"}};
      auto result = workflow.apply(req);
      CHECK_FALSE(result.applied);
      CHECK(result.notificationText.empty());
      CHECK(trackTagNames(testLib, trackId).empty());
      CHECK(trackTagNames(testLib, trackId2).empty());
    }

    SECTION("empty tag changes do not mutate selected tracks")
    {
      auto const req = TagEditRequest{.selectedIds = {trackId}};
      auto result = workflow.apply(req);
      CHECK_FALSE(result.applied);
      CHECK(result.notificationText.empty());
      CHECK(trackTagNames(testLib, trackId).empty());
    }

    SECTION("adding a single tag mutates the selected track and reports the count")
    {
      auto const req = TagEditRequest{.selectedIds = {trackId}, .tagsToAdd = {"Tag1"}};
      auto result = workflow.apply(req);
      CHECK(result.applied);
      CHECK(result.notificationText == "Tags added 1 for 1 track");
      CHECK(trackTagNames(testLib, trackId) == std::vector<std::string>{"Tag1"});
      CHECK(trackTagNames(testLib, trackId2).empty());
    }

    SECTION("removing a single tag mutates every selected track and reports the count")
    {
      auto const initialTags = std::vector<std::string>{"Tag1"};
      writer.editTags(std::vector{trackId, trackId2}, initialTags, {});
      REQUIRE(trackTagNames(testLib, trackId) == std::vector<std::string>{"Tag1"});
      REQUIRE(trackTagNames(testLib, trackId2) == std::vector<std::string>{"Tag1"});

      auto const req = TagEditRequest{.selectedIds = {trackId, trackId2}, .tagsToRemove = {"Tag1"}};
      auto result = workflow.apply(req);
      CHECK(result.applied);
      CHECK(result.notificationText == "Tags removed 1 for 2 tracks");
      CHECK(trackTagNames(testLib, trackId).empty());
      CHECK(trackTagNames(testLib, trackId2).empty());
    }

    SECTION("adding and removing tags reports requested counts and final tag sets")
    {
      auto const req =
        TagEditRequest{.selectedIds = {trackId, trackId2}, .tagsToAdd = {"Tag1", "Tag2"}, .tagsToRemove = {"Tag3"}};
      auto result = workflow.apply(req);
      CHECK(result.applied);
      CHECK(result.notificationText == "Tags added 2 and removed 1 for 2 tracks");
      CHECK(trackTagNames(testLib, trackId) == std::vector<std::string>{"Tag1", "Tag2"});
      CHECK(trackTagNames(testLib, trackId2) == std::vector<std::string>{"Tag1", "Tag2"});
    }

    SECTION("adding and removing tags mutates the library in one request")
    {
      writer.editTags(std::vector{trackId}, std::vector<std::string>{"OldTag"}, {});

      auto const req = TagEditRequest{.selectedIds = {trackId}, .tagsToAdd = {"NewTag"}, .tagsToRemove = {"OldTag"}};

      auto result = workflow.apply(req);

      CHECK(result.applied);
      CHECK(result.notificationText == "Tags added 1 and removed 1 for 1 track");
      CHECK(trackHasTag(testLib, trackId, "NewTag"));
      CHECK_FALSE(trackHasTag(testLib, trackId, "OldTag"));
    }
  }
} // namespace ao::uimodel::test
