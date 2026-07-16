// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/library/LibraryMutationService.h"
#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include <ao/Error.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <future>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    void advanceLibraryRevision(library::MusicLibrary& library, std::uint64_t targetRevision)
    {
      for (std::uint64_t revision = 0; revision < targetRevision; ++revision)
      {
        auto transaction = library::test::writeTransaction(library);
        REQUIRE(transaction.commit());
      }
    }
  } // namespace

  TEST_CASE("LibraryChanges - holdback publishes committed changesets in revision order",
            "[runtime][unit][library][concurrency]")
  {
    auto revision12Fixture = MusicLibraryFixture{};
    auto revision11Fixture = MusicLibraryFixture{};
    advanceLibraryRevision(revision12Fixture.library(), 11);
    advanceLibraryRevision(revision11Fixture.library(), 10);
    auto executor = ManualExecutor{};
    auto changes = LibraryChanges{executor, 10};
    auto revision12MutationService = LibraryMutationService{
      executor, ao::test::requireValue(library::WritableMusicLibrary::acquire(revision12Fixture.library())), changes};
    auto revision11MutationService = LibraryMutationService{
      executor, ao::test::requireValue(library::WritableMusicLibrary::acquire(revision11Fixture.library())), changes};
    auto revisions = std::vector<std::uint64_t>{};
    auto subscription = changes.onChanged([&revisions](LibraryChangeSet const& changeSet)
                                          { revisions.push_back(changeSet.libraryRevision); });

    auto revision12Committed = std::promise<void>{};
    auto revision12CommittedFuture = revision12Committed.get_future().share();
    auto revision12Worker =
      std::async(std::launch::async,
                 [&revision12MutationService, committed = std::move(revision12Committed)] mutable -> Result<>
                 {
                   auto mutationResult = revision12MutationService.beginInteractiveMutation();

                   if (!mutationResult)
                   {
                     return std::unexpected{mutationResult.error()};
                   }

                   auto commitResult = mutationResult->commit(LibraryChangeSet{.tracksMutated = {TrackId{12}}});
                   committed.set_value();

                   if (!commitResult)
                   {
                     return std::unexpected{commitResult.error()};
                   }

                   return {};
                 });
    auto revision11Worker = std::async(std::launch::async,
                                       [&revision11MutationService, revision12CommittedFuture] -> Result<>
                                       {
                                         revision12CommittedFuture.wait();
                                         auto mutationResult = revision11MutationService.beginInteractiveMutation();

                                         if (!mutationResult)
                                         {
                                           return std::unexpected{mutationResult.error()};
                                         }

                                         auto commitResult =
                                           mutationResult->commit(LibraryChangeSet{.tracksMutated = {TrackId{11}}});

                                         if (!commitResult)
                                         {
                                           return std::unexpected{commitResult.error()};
                                         }

                                         return {};
                                       });

    REQUIRE(revision12Worker.get());
    REQUIRE(revision11Worker.get());

    CHECK(revisions.empty());
    CHECK(executor.queuedCount() == 1);
    executor.runUntilIdle();
    CHECK(revisions == std::vector<std::uint64_t>{11, 12});
  }

  TEST_CASE("LibraryChanges - writer commits publish once with the in-band revision",
            "[runtime][unit][library][changeset]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Before");
    auto changes = LibraryChanges{};
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto observed = std::vector<LibraryChangeSet>{};
    auto subscription =
      changes.onChanged([&observed](LibraryChangeSet const& changeSet) { observed.push_back(changeSet); });

    REQUIRE(writerFixture.updateMetadata(std::array{trackId}, MetadataPatch{.optTitle = "After"}));
    REQUIRE(writerFixture.updateMetadata(std::array{trackId}, MetadataPatch{.optTitle = "After"}));

    REQUIRE(observed.size() == 1);
    CHECK(observed[0].tracksMutated == std::vector{trackId});
    auto transaction = libraryFixture.library().readTransaction();
    CHECK(libraryFixture.library().libraryRevision(transaction) == observed[0].libraryRevision);
  }

  TEST_CASE("MusicLibrary - aborted write does not advance the snapshot revision", "[library][unit][revision]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    {
      auto transaction = libraryFixture.library().readTransaction();
      CHECK(libraryFixture.library().libraryRevision(transaction) == 0);
    }
    {
      auto transaction = library::test::writeTransaction(libraryFixture.library());
      CHECK(libraryFixture.library().libraryRevision(transaction) == 1);
    }
    {
      auto transaction = libraryFixture.library().readTransaction();
      CHECK(libraryFixture.library().libraryRevision(transaction) == 0);
    }
    {
      auto transaction = library::test::writeTransaction(libraryFixture.library());
      CHECK(libraryFixture.library().libraryRevision(transaction) == 1);
      REQUIRE(transaction.commit());
    }
    auto transaction = libraryFixture.library().readTransaction();
    CHECK(libraryFixture.library().libraryRevision(transaction) == 1);
  }
} // namespace ao::rt::test
