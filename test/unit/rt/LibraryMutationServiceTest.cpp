// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <catch2/catch_test_macros.hpp>

#include <runtime/LibraryMutationService.h>

#include "TestUtils.h"

namespace ao::rt::test
{
  namespace
  {
    struct NullExecutor final : public IControlExecutor
    {
      bool isCurrent() const noexcept override { return true; }
      void dispatch(std::move_only_function<void()> task) override { task(); } void defer(std::move_only_function<void()> task) override { task(); }
    };
  }

  TEST_CASE("LibraryMutationService - updateMetadata publishes TracksMutated", "[app][runtime][mutation]")
  {
    auto testLib = TestMusicLibrary{};
    auto trackId = testLib.addTrack("Original Title");

    NullExecutor executor;
    auto service = LibraryMutationService{executor, testLib.library()};

    auto mutated = std::vector<ao::TrackId>{};
    auto sub = service.onTracksMutated([&](auto const& trackIds) { mutated = trackIds; });

    auto result = service.updateMetadata({trackId}, MetadataPatch{.optTitle = "New Title"});

    REQUIRE(result.has_value());
    REQUIRE(mutated.size() == 1);
    CHECK(mutated[0] == trackId);
  }

  TEST_CASE("LibraryMutationService - editTags publishes TracksMutated", "[app][runtime][mutation]")
  {
    auto testLib = TestMusicLibrary{};
    auto trackId = testLib.addTrack("Track");

    NullExecutor executor;
    auto service = LibraryMutationService{executor, testLib.library()};

    auto mutated = std::vector<ao::TrackId>{};
    auto sub = service.onTracksMutated([&](auto const& trackIds) { mutated = trackIds; });

    auto result = service.editTags({trackId}, {"rock"}, {});

    REQUIRE(result.has_value());
    REQUIRE(mutated.size() == 1);
    CHECK(mutated[0] == trackId);
  }

  TEST_CASE("LibraryMutationService - no-op patch does not publish", "[app][runtime][mutation]")
  {
    auto testLib = TestMusicLibrary{};
    auto trackId = testLib.addTrack("Track");

    NullExecutor executor;
    auto service = LibraryMutationService{executor, testLib.library()};

    auto mutated = std::vector<ao::TrackId>{};
    auto sub = service.onTracksMutated([&](auto const& trackIds) { mutated = trackIds; });

    auto result = service.updateMetadata({trackId}, {});

    REQUIRE(result.has_value());
    CHECK(mutated.size() == 1);
  }

  TEST_CASE("LibraryMutationService - missing track does not publish", "[app][runtime][mutation]")
  {
    auto testLib = TestMusicLibrary{};

    NullExecutor executor;
    auto service = LibraryMutationService{executor, testLib.library()};

    auto mutated = std::vector<ao::TrackId>{};
    auto sub = service.onTracksMutated([&](auto const& trackIds) { mutated = trackIds; });

    auto result = service.updateMetadata({ao::TrackId{99999}}, MetadataPatch{.optTitle = "X"});

    REQUIRE(result.has_value());
    CHECK(mutated.empty());
  }
}
