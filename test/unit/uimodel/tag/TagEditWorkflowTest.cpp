// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/runtime/TestUtils.h"
#include <ao/async/Runtime.h>
#include <ao/rt/LibraryMutationService.h>
#include <ao/uimodel/tag/TagEditWorkflow.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace ao::uimodel::tag::test
{
  using namespace ao::rt::test;

  TEST_CASE("TagEditWorkflow - logic and messages", "[unit][uimodel][tag]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = MockExecutor{};
    auto runtime = async::Runtime{executor};
    auto mutation = rt::LibraryMutationService{runtime, testLib.library()};
    auto workflow = TagEditWorkflow{mutation};

    auto trackId = testLib.addTrack("Target 1");
    auto trackId2 = testLib.addTrack("Target 2");

    SECTION("empty request does nothing")
    {
      auto const req = TagEditRequest{};
      auto result = workflow.apply(req);
      CHECK_FALSE(result.applied);
      CHECK(result.notificationText.empty());
    }

    SECTION("empty tags does nothing")
    {
      auto req = TagEditRequest{};
      req.selectedIds = {trackId};
      auto result = workflow.apply(req);
      CHECK_FALSE(result.applied);
      CHECK(result.notificationText.empty());
    }

    SECTION("add single tag generates correct message")
    {
      auto req = TagEditRequest{};
      req.selectedIds = {trackId};
      req.tagsToAdd = {"Tag1"};
      auto result = workflow.apply(req);
      CHECK(result.applied);
      CHECK(result.notificationText == "Tags added 1 for 1 track");
    }

    SECTION("remove single tag generates correct message")
    {
      auto req = TagEditRequest{};
      req.selectedIds = {trackId, trackId2};
      req.tagsToRemove = {"Tag1"};
      auto result = workflow.apply(req);
      CHECK(result.applied);
      CHECK(result.notificationText == "Tags removed 1 for 2 tracks");
    }

    SECTION("add and remove tags generates correct message")
    {
      auto req = TagEditRequest{};
      req.selectedIds = {trackId, trackId2};
      req.tagsToAdd = {"Tag1", "Tag2"};
      req.tagsToRemove = {"Tag3"};
      auto result = workflow.apply(req);
      CHECK(result.applied);
      CHECK(result.notificationText == "Tags added 2 and removed 1 for 2 tracks");
    }

    SECTION("adding tags actually mutates the library")
    {
      auto req = TagEditRequest{};
      req.selectedIds = {trackId};
      req.tagsToAdd = {"ActualTag"};
      auto result = workflow.apply(req);
      CHECK(result.applied);

      auto txn = testLib.library().readTransaction();
      auto reader = testLib.library().tracks().reader(txn);
      auto const optView = reader.get(trackId);
      REQUIRE(optView);

      auto const& dictionary = testLib.library().dictionary();
      bool hasTag = false;

      for (auto const tagId : optView->tags())
      {
        if (dictionary.getOrDefault(tagId) == "ActualTag")
        {
          hasTag = true;
          break;
        }
      }

      CHECK(hasTag);
    }
  }
} // namespace ao::uimodel::tag::test
