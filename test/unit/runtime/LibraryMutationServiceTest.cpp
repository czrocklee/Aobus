// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/rt/LibraryMutationService.h>

#include "TestUtils.h"
#include "ao/Type.h"
#include <ao/rt/StateTypes.h>
#include <ao/rt/async/Runtime.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <functional>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    struct NullExecutor final : public IControlExecutor
    {
      bool isCurrent() const noexcept override { return true; }
      void dispatch(std::move_only_function<void()> task) override { task(); }
      void defer(std::move_only_function<void()> task) override { task(); }
    };
  }

  TEST_CASE("LibraryMutationService - updateMetadata publishes TracksMutated", "[app][runtime][mutation]")
  {
    auto testLib = TestMusicLibrary{};
    auto const trackId = testLib.addTrack("Original Title");

    auto executor = NullExecutor{};
    auto runtime = async::Runtime{executor};
    auto service = LibraryMutationService{runtime, testLib.library()};

    auto mutated = std::vector<TrackId>{};
    auto sub = service.onTracksMutated([&](auto const& trackIds) { mutated = trackIds; });

    auto const targetIds = std::array{trackId};
    auto const result = service.updateMetadata(targetIds, MetadataPatch{.optTitle = "New Title"});

    REQUIRE(result.has_value());
    REQUIRE(mutated.size() == 1);
    CHECK(mutated[0] == trackId);
  }

  TEST_CASE("LibraryMutationService - editTags publishes TracksMutated", "[app][runtime][mutation]")
  {
    auto testLib = TestMusicLibrary{};
    auto const trackId = testLib.addTrack("Track");

    auto executor = NullExecutor{};
    auto runtime = async::Runtime{executor};
    auto service = LibraryMutationService{runtime, testLib.library()};

    auto mutated = std::vector<TrackId>{};
    auto sub = service.onTracksMutated([&](auto const& trackIds) { mutated = trackIds; });

    auto const trackIdsArr = std::array{trackId};
    auto const toAdd = std::array{std::string{"rock"}};
    auto const result = service.editTags(trackIdsArr, toAdd, {});

    REQUIRE(result.has_value());
    REQUIRE(mutated.size() == 1);
    CHECK(mutated[0] == trackId);
  }

  TEST_CASE("LibraryMutationService - no-op patch does not publish", "[app][runtime][mutation]")
  {
    auto testLib = TestMusicLibrary{};
    auto const trackId = testLib.addTrack("Track");

    auto executor = NullExecutor{};
    auto runtime = async::Runtime{executor};
    auto service = LibraryMutationService{runtime, testLib.library()};

    auto mutated = std::vector<TrackId>{};
    auto sub = service.onTracksMutated([&](auto const& trackIds) { mutated = trackIds; });

    auto const noPatchIds = std::array{trackId};
    auto const result = service.updateMetadata(noPatchIds, {});

    REQUIRE(result.has_value());
    CHECK(mutated.size() == 1);
  }

  TEST_CASE("LibraryMutationService - missing track does not publish", "[app][runtime][mutation]")
  {
    auto testLib = TestMusicLibrary{};

    auto executor = NullExecutor{};
    auto runtime = async::Runtime{executor};
    auto service = LibraryMutationService{runtime, testLib.library()};

    auto mutated = std::vector<TrackId>{};
    auto sub = service.onTracksMutated([&](auto const& trackIds) { mutated = trackIds; });

    auto const missingIds = std::array{TrackId{99999}};
    auto const result = service.updateMetadata(missingIds, MetadataPatch{.optTitle = "X"});

    REQUIRE(result.has_value());
    CHECK(mutated.empty());
  }

  TEST_CASE("LibraryMutationService - createList publishes ListsMutated", "[app][runtime][mutation]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = NullExecutor{};
    auto runtime = async::Runtime{executor};
    auto service = LibraryMutationService{runtime, testLib.library()};

    auto upserted = std::vector<ListId>{};
    auto sub = service.onListsMutated([&](auto const& ev) { upserted = ev.upserted; });

    auto draft = LibraryMutationService::ListDraft{};
    draft.name = "Test List";
    draft.kind = LibraryMutationService::ListKind::Smart;
    draft.expression = "artist:test";

    auto const listId = service.createList(draft);

    REQUIRE(listId != kInvalidListId);
    REQUIRE(upserted.size() == 1);
    CHECK(upserted[0] == listId);
  }
} // namespace ao::rt::test
