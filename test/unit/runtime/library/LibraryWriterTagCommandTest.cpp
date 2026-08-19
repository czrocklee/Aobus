// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
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
  TEST_CASE("LibraryWriter - editTags adds a new tag and publishes a mutation", "[runtime][unit][library][tag]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Test Track");

    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture;

    auto mutated = std::vector<TrackId>{};
    auto sub =
      changes.onChanged([&](LibraryChangeSet const& changeSet) noexcept { mutated = changeSet.tracksMutated; });

    auto const favorite = std::array{std::string{"Favorite"}};

    auto const replyRes = writer.editTags(std::array{trackId}, favorite, {});
    REQUIRE(replyRes);
    CHECK_FALSE(replyRes->changes.empty());
    REQUIRE(mutated.size() == 1);
    CHECK(mutated[0] == trackId);

    // Verify the tag was added by fetching it
    auto transaction = libraryFixture.library().readTransaction();
    auto const optTrackView =
      libraryFixture.library().tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Hot);
    REQUIRE(optTrackView);
    auto builder = library::TrackBuilder::fromHotView(*optTrackView, libraryFixture.library().dictionary());
    CHECK(std::ranges::contains(builder.tags().names(), std::string_view{"Favorite"}));
  }

  TEST_CASE("LibraryWriter - editTags ignores an existing tag", "[runtime][unit][library][tag]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Test Track");

    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture;

    auto mutated = std::vector<TrackId>{};
    auto sub =
      changes.onChanged([&](LibraryChangeSet const& changeSet) noexcept { mutated = changeSet.tracksMutated; });

    auto const favorite = std::array{std::string{"Favorite"}};

    REQUIRE(writer.editTags(std::array{trackId}, favorite, {}));
    mutated.clear();

    auto const replyRes = writer.editTags(std::array{trackId}, favorite, {});
    REQUIRE(replyRes);
    CHECK(replyRes->changes.empty());
    CHECK(mutated.empty());
  }

  TEST_CASE("LibraryWriter - editTags uses canonical Unicode identity", "[runtime][unit][library][tag][unicode]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Test Track");
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto const composed = std::array{std::string{"résumé"}};
    auto const decomposed = std::array{std::string{"re\u0301sume\u0301"}};

    REQUIRE(writerFixture.editTags(std::array{trackId}, composed, {}));
    auto const duplicateRes = writerFixture.editTags(std::array{trackId}, decomposed, {});
    REQUIRE(duplicateRes);
    CHECK(duplicateRes->changes.empty());

    auto const removeRes = writerFixture.editTags(std::array{trackId}, {}, decomposed);
    REQUIRE(removeRes);
    REQUIRE(removeRes->changes.size() == 1);
    REQUIRE(removeRes->changes[0].removedTags.size() == 1);
    CHECK(removeRes->changes[0].removedTags[0] == "résumé");
  }

  TEST_CASE("LibraryWriter - editTags rejects missing tag-add targets", "[runtime][unit][library][tag]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    [[maybe_unused]] auto const trackId = libraryFixture.addTrack("Test Track");

    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture;

    auto mutated = std::vector<TrackId>{};
    auto sub =
      changes.onChanged([&](LibraryChangeSet const& changeSet) noexcept { mutated = changeSet.tracksMutated; });

    auto const favorite = std::array{std::string{"Favorite"}};

    auto const replyRes = writer.editTags(std::array{TrackId{99999}}, favorite, {});
    REQUIRE_FALSE(replyRes);
    CHECK(replyRes.error().code == Error::Code::NotFound);
  }

  TEST_CASE("LibraryWriter - editTags removes an existing tag and publishes a mutation",
            "[runtime][unit][library][tag]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Test Track");

    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture;

    auto mutated = std::vector<TrackId>{};
    auto sub =
      changes.onChanged([&](LibraryChangeSet const& changeSet) noexcept { mutated = changeSet.tracksMutated; });

    auto const favorite = std::array{std::string{"Favorite"}};

    REQUIRE(writer.editTags(std::array{trackId}, favorite, {}));
    mutated.clear();

    auto const replyRes = writer.editTags(std::array{trackId}, {}, favorite);
    REQUIRE(replyRes);
    CHECK_FALSE(replyRes->changes.empty());
    REQUIRE(mutated.size() == 1);
    CHECK(mutated[0] == trackId);
  }

  TEST_CASE("LibraryWriter - editTags ignores missing tags", "[runtime][unit][library][tag]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Test Track");

    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture;

    auto mutated = std::vector<TrackId>{};
    auto sub =
      changes.onChanged([&](LibraryChangeSet const& changeSet) noexcept { mutated = changeSet.tracksMutated; });

    auto const nonExistent = std::array{std::string{"NonExistent"}};
    auto const replyRes = writer.editTags(std::array{trackId}, {}, nonExistent);
    REQUIRE(replyRes);
    CHECK(replyRes->changes.empty());
    CHECK(mutated.empty());
  }

  TEST_CASE("LibraryWriter - editTags rejects missing tag-remove targets", "[runtime][unit][library][tag]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    [[maybe_unused]] auto const trackId = libraryFixture.addTrack("Test Track");

    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture;

    auto mutated = std::vector<TrackId>{};
    auto sub =
      changes.onChanged([&](LibraryChangeSet const& changeSet) noexcept { mutated = changeSet.tracksMutated; });

    auto const favorite = std::array{std::string{"Favorite"}};

    auto const replyRes = writer.editTags(std::array{TrackId{99999}}, {}, favorite);
    REQUIRE_FALSE(replyRes);
    CHECK(replyRes.error().code == Error::Code::NotFound);
  }
} // namespace ao::rt::test
