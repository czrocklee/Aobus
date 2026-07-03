// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include <ao/CoreIds.h>
#include <ao/library/TrackBuilder.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("LibraryWriter editTags adds a new tag and publishes a mutation",
            "[runtime][unit][library][mutation][tags]")
  {
    auto testLib = TestMusicLibrary{};
    auto const trackId = testLib.addTrack("Test Track");

    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{testLib.library(), changes};

    auto mutated = std::vector<TrackId>{};
    auto sub = changes.onTracksMutated([&](auto const& trackIds) { mutated = trackIds; });

    auto const favorite = std::array{std::string{"Favorite"}};

    auto const reply = writer.editTags(std::array{trackId}, favorite, {});
    REQUIRE(reply);
    CHECK_FALSE(reply->mutatedIds.empty());
    REQUIRE(mutated.size() == 1);
    CHECK(mutated[0] == trackId);

    // Verify the tag was added by fetching it
    auto txn = testLib.library().readTransaction();
    auto const optTrackView =
      testLib.library().tracks().reader(txn).get(trackId, library::TrackStore::Reader::LoadMode::Hot);
    REQUIRE(optTrackView.has_value());
    auto builder = library::TrackBuilder::fromView(*optTrackView, testLib.library().dictionary());
    CHECK(std::ranges::contains(builder.tags().names(), std::string_view{"Favorite"}));
  }

  TEST_CASE("LibraryWriter editTags ignores an existing tag", "[runtime][unit][library][mutation][tags]")
  {
    auto testLib = TestMusicLibrary{};
    auto const trackId = testLib.addTrack("Test Track");

    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{testLib.library(), changes};

    auto mutated = std::vector<TrackId>{};
    auto sub = changes.onTracksMutated([&](auto const& trackIds) { mutated = trackIds; });

    auto const favorite = std::array{std::string{"Favorite"}};

    REQUIRE(writer.editTags(std::array{trackId}, favorite, {}));
    mutated.clear();

    auto const reply = writer.editTags(std::array{trackId}, favorite, {});
    REQUIRE(reply);
    CHECK(reply->mutatedIds.empty());
    CHECK(mutated.empty());
  }

  TEST_CASE("LibraryWriter editTags ignores tag additions for missing tracks",
            "[runtime][unit][library][mutation][tags]")
  {
    auto testLib = TestMusicLibrary{};
    [[maybe_unused]] auto const trackId = testLib.addTrack("Test Track");

    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{testLib.library(), changes};

    auto mutated = std::vector<TrackId>{};
    auto sub = changes.onTracksMutated([&](auto const& trackIds) { mutated = trackIds; });

    auto const favorite = std::array{std::string{"Favorite"}};

    auto const reply = writer.editTags(std::array{TrackId{99999}}, favorite, {});
    REQUIRE(reply);
    CHECK(reply->mutatedIds.empty());
  }

  TEST_CASE("LibraryWriter editTags removes an existing tag and publishes a mutation",
            "[runtime][unit][library][mutation][tags]")
  {
    auto testLib = TestMusicLibrary{};
    auto const trackId = testLib.addTrack("Test Track");

    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{testLib.library(), changes};

    auto mutated = std::vector<TrackId>{};
    auto sub = changes.onTracksMutated([&](auto const& trackIds) { mutated = trackIds; });

    auto const favorite = std::array{std::string{"Favorite"}};

    REQUIRE(writer.editTags(std::array{trackId}, favorite, {}));
    mutated.clear();

    auto const reply = writer.editTags(std::array{trackId}, {}, favorite);
    REQUIRE(reply);
    CHECK_FALSE(reply->mutatedIds.empty());
    REQUIRE(mutated.size() == 1);
    CHECK(mutated[0] == trackId);
  }

  TEST_CASE("LibraryWriter editTags ignores missing tags", "[runtime][unit][library][mutation][tags]")
  {
    auto testLib = TestMusicLibrary{};
    auto const trackId = testLib.addTrack("Test Track");

    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{testLib.library(), changes};

    auto mutated = std::vector<TrackId>{};
    auto sub = changes.onTracksMutated([&](auto const& trackIds) { mutated = trackIds; });

    auto const nonExistent = std::array{std::string{"NonExistent"}};
    auto const reply = writer.editTags(std::array{trackId}, {}, nonExistent);
    REQUIRE(reply);
    CHECK(reply->mutatedIds.empty());
    CHECK(mutated.empty());
  }

  TEST_CASE("LibraryWriter editTags ignores tag removals for missing tracks",
            "[runtime][unit][library][mutation][tags]")
  {
    auto testLib = TestMusicLibrary{};
    [[maybe_unused]] auto const trackId = testLib.addTrack("Test Track");

    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{testLib.library(), changes};

    auto mutated = std::vector<TrackId>{};
    auto sub = changes.onTracksMutated([&](auto const& trackIds) { mutated = trackIds; });

    auto const favorite = std::array{std::string{"Favorite"}};

    auto const reply = writer.editTags(std::array{TrackId{99999}}, {}, favorite);
    REQUIRE(reply);
    CHECK(reply->mutatedIds.empty());
  }
} // namespace ao::rt::test
