// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/Error.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("LibraryWriter - track Properties commits metadata and tags in one revision",
            "[runtime][regression][library-authoring]")
  {
    auto storage = MusicLibraryFixture{};
    auto const trackId = storage.addTrack("Before");
    auto changes = makeStateOnlyLibraryChanges(storage.library());
    auto writerFixture = LibraryWriterFixture{storage.library(), changes};
    auto publications = std::vector<LibraryChangeSet>{};
    [[maybe_unused]] auto subscription = changes.onChanged([&publications](LibraryChangeSet const& changeSet) noexcept
                                                           { publications.push_back(changeSet); });
    auto targets = writerFixture.bind(std::array{trackId});

    auto const result = writerFixture.runTask(writerFixture.writer().updateProperties(
      std::move(targets),
      TrackPropertiesPatch{.metadata = MetadataPatch{.optTitle = "After"}, .tagsToAdd = {"Favorite"}}));

    REQUIRE(result);
    CHECK(result->status == AuthoringStatus::Applied);
    REQUIRE(result->reply.metadata.changes.size() == 1);
    CHECK(result->reply.metadata.changes[0].trackId == trackId);
    REQUIRE(result->reply.tags.changes.size() == 1);
    CHECK(result->reply.tags.changes[0].trackId == trackId);
    REQUIRE(publications.size() == 1);
    CHECK(publications[0].tracksMutated == std::vector<TrackId>{trackId});

    auto transaction = storage.library().readTransaction();
    auto const optTrack =
      storage.library().tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Hot);
    REQUIRE(optTrack);
    auto const track = library::TrackBuilder::fromHotView(*optTrack, storage.library().dictionary());
    CHECK(track.metadata().title() == "After");
    CHECK(std::ranges::contains(track.tags().names(), std::string_view{"Favorite"}));
  }

  TEST_CASE("LibraryWriter - invalid tags roll back an earlier staged Properties metadata edit",
            "[runtime][regression][library-authoring]")
  {
    auto storage = MusicLibraryFixture{};
    auto const trackId = storage.addTrack("Before");
    auto changes = makeStateOnlyLibraryChanges(storage.library());
    auto writerFixture = LibraryWriterFixture{storage.library(), changes};
    std::size_t publicationCount = 0;
    [[maybe_unused]] auto subscription =
      changes.onChanged([&publicationCount](LibraryChangeSet const&) noexcept { ++publicationCount; });
    auto targets = writerFixture.bind(std::array{trackId});
    auto invalidTag = std::string(1, static_cast<char>(0xff));

    auto const result = writerFixture.runTask(
      writerFixture.writer().updateProperties(std::move(targets),
                                              TrackPropertiesPatch{
                                                .metadata = MetadataPatch{.optTitle = "Must roll back"},
                                                .tagsToAdd = {std::move(invalidTag)},
                                              }));

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::InvalidInput);
    CHECK(publicationCount == 0);

    auto transaction = storage.library().readTransaction();
    auto const optTrack =
      storage.library().tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Hot);
    REQUIRE(optTrack);
    CHECK(optTrack->metadata().title() == "Before");
    CHECK(optTrack->tags().empty());
  }
} // namespace ao::rt::test
