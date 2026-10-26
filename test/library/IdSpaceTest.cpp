// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/TestFixtureSupport.h"
#include "test/library/MusicLibraryTestSupport.h"
#include "test/library/TrackStoreTestSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/library/WritableLibraryTestSupport.h"
#include "test/lmdb/LmdbTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListLayout.h>
#include <ao/library/ListWriter.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackWriter.h>
#include <ao/library/WriteTransaction.h>
#include <ao/lmdb/Transaction.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <tuple>
#include <utility>

namespace ao::library::test
{
  namespace
  {
    std::uint64_t committedRevision(MusicLibrary const& library)
    {
      auto transaction = library.readTransaction();
      return library.libraryRevision(transaction);
    }

    Result<ListId> createList(MusicLibrary& library, ListBuilder const& builder)
    {
      auto transaction = writeTransaction(library);
      auto idRes = transaction.apply([&builder](LibraryWrite& write) { return write.lists().create(builder); });

      if (!idRes)
      {
        return idRes;
      }

      REQUIRE(transaction.commit());
      return idRes;
    }

    void removeTrack(MusicLibrary& library, TrackId const id)
    {
      auto transaction = writeTransaction(library);
      auto const removedRes = transaction.apply([id](LibraryWrite& write) { return write.tracks().remove(id); });
      REQUIRE(removedRes);
      REQUIRE(*removedRes);
      REQUIRE(transaction.commit());
    }

    void removeList(MusicLibrary& library, ListId const id)
    {
      auto transaction = writeTransaction(library);
      REQUIRE(transaction.apply([id](LibraryWrite& write) { return write.lists().remove(id); }));
      REQUIRE(transaction.commit());
    }

    void setRawLastListId(std::filesystem::path const& path, std::uint32_t const lastListId)
    {
      auto environment = lmdb::test::openEnvironment(path, {.flags = MDB_NOTLS, .maxDatabases = 8});
      auto transaction = lmdb::test::beginWriteTransaction(environment);
      coverRawIdSpace(transaction, 0, lastListId);
      REQUIRE(transaction.commit());
    }
  } // namespace

  TEST_CASE("IdSpace - a fresh library starts with no allocated ids", "[library][unit][id-space]")
  {
    auto const temp = ao::test::TempDir{};
    std::ignore = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    CHECK(readRawIdSpace(temp.path() / "db") == IdSpace{});
  }

  TEST_CASE("IdSpace - deleting the highest Track or List never frees its id", "[library][unit][id-space]")
  {
    auto const temp = ao::test::TempDir{};
    {
      auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
      std::ignore = addTrack(library, TrackSpec{.uri = "first.flac"});
      auto const second = addTrack(library, TrackSpec{.uri = "second.flac"});
      removeTrack(library, second);
      CHECK(addTrack(library, TrackSpec{.uri = "third.flac"}) == TrackId{3});

      std::ignore = ao::test::requireValue(createList(library, ListBuilder::makeEmpty().name("First")));
      auto const secondList = ao::test::requireValue(createList(library, ListBuilder::makeEmpty().name("Second")));
      removeList(library, secondList);
      CHECK(ao::test::requireValue(createList(library, ListBuilder::makeEmpty().name("Third"))) == ListId{3});
    }

    CHECK(readRawIdSpace(temp.path() / "db") == IdSpace{.lastTrackId = 3, .lastListId = 3});

    // The marks are durable, so a later process continues above them.
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    removeTrack(library, TrackId{3});
    CHECK(addTrack(library, TrackSpec{.uri = "fourth.flac"}) == TrackId{4});
  }

  TEST_CASE("IdSpace - clearing keeps later ids above the old marks and the library id unchanged",
            "[library][unit][id-space]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    std::ignore = addTrack(library, TrackSpec{.uri = "first.flac"});
    std::ignore = addTrack(library, TrackSpec{.uri = "second.flac"});
    std::ignore = ao::test::requireValue(createList(library, ListBuilder::makeEmpty().name("Only")));
    auto const libraryId = library.metadataHeader().libraryId;

    SECTION("recreated in the clearing transaction")
    {
      auto transaction = writeTransaction(library);
      auto const idsRes = transaction.apply(
        [&library](LibraryWrite& write) -> Result<std::pair<TrackId, ListId>>
        {
          REQUIRE(write.tracks().clear());
          REQUIRE(write.lists().clear());
          auto const trackId = addTrack(library, write, TrackSpec{.uri = "again.flac"});
          auto listRes = write.lists().create(ListBuilder::makeEmpty().name("Again"));

          if (!listRes)
          {
            return std::unexpected{listRes.error()};
          }

          return std::pair{trackId, *listRes};
        });
      REQUIRE(idsRes);
      REQUIRE(transaction.commit());
      CHECK(idsRes->first == TrackId{3});
      CHECK(idsRes->second == ListId{2});
    }

    SECTION("recreated in a later transaction")
    {
      auto transaction = writeTransaction(library);
      REQUIRE(transaction.apply(
        [](LibraryWrite& write) -> Result<>
        {
          REQUIRE(write.tracks().clear());
          return write.lists().clear();
        }));
      REQUIRE(transaction.commit());

      CHECK(addTrack(library, TrackSpec{.uri = "again.flac"}) == TrackId{3});
      CHECK(ao::test::requireValue(createList(library, ListBuilder::makeEmpty().name("Again"))) == ListId{2});
    }

    CHECK(library.metadataHeader().libraryId == libraryId);
  }

  TEST_CASE("IdSpace - commit marks cover ids allocated and deleted in the same transaction",
            "[library][unit][id-space]")
  {
    auto const temp = ao::test::TempDir{};
    auto const databasePath = temp.path() / "db";
    {
      auto library = makeTestMusicLibrary(temp.path(), databasePath);
      auto transaction = writeTransaction(library);
      REQUIRE(transaction.apply(
        [&library](LibraryWrite& write) -> Result<>
        {
          // Neither highest id survives to commit, so only the staged writers
          // know they were allocated.
          auto const trackId = addTrack(library, write, TrackSpec{.uri = "transient.flac"});
          REQUIRE(ao::test::requireValue(write.tracks().remove(trackId)));
          auto ranked = ListBuilder::makeEmpty();
          ranked.name("Keeps rank").orderTrackIds().add(trackId);
          REQUIRE(ao::test::requireValue(write.lists().create(ranked)) == ListId{1});
          auto const transientList =
            ao::test::requireValue(write.lists().create(ListBuilder::makeEmpty().name("Gone")));
          return write.lists().remove(transientList);
        }));
      REQUIRE(transaction.commit());
    }

    CHECK(readRawIdSpace(databasePath) == IdSpace{.lastTrackId = 1, .lastListId = 2});
    auto library = makeTestMusicLibrary(temp.path(), databasePath);
    CHECK(addTrack(library, TrackSpec{.uri = "next.flac"}) == TrackId{2});
    CHECK(ao::test::requireValue(createList(library, ListBuilder::makeEmpty().name("Next"))) == ListId{3});
  }

  TEST_CASE("IdSpace - commit marks cover rows a physical seam stored", "[library][unit][id-space]")
  {
    auto const temp = ao::test::TempDir{};
    auto const databasePath = temp.path() / "db";
    {
      auto library = makeTestMusicLibrary(temp.path(), databasePath);
      // The builder borrows the spec's strings, so the spec outlives the commit.
      auto const spec = TrackSpec{.uri = "seam.flac"};
      auto trackBuilder = TrackBuilder::makeEmpty();
      applyTrackSpec(trackBuilder, spec);
      CHECK(createCommittedTrack(library, trackBuilder) == TrackId{1});

      auto const preparedList = ao::test::requireValue(ListBuilder::makeEmpty().name("Seam").prepare());
      auto transaction = writeTransaction(library);
      CHECK(ao::test::requireValue(physicalWriter(library.lists(), transaction).create(preparedList)) == ListId{1});
      REQUIRE(transaction.commit());
    }

    // The seams bypass the transaction's own writers, so only the stored keys
    // can raise the marks to the admission rule's bound on every row.
    CHECK(readRawIdSpace(databasePath) == IdSpace{.lastTrackId = 1, .lastListId = 1});
  }

  TEST_CASE("IdSpace - a failed commit leaves the marks and revision unchanged", "[library][unit][id-space]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    std::ignore = addTrack(library, TrackSpec{.uri = "first.flac"});
    auto const revisionBefore = committedRevision(library);

    {
      auto transaction = writeTransaction(
        library,
        WriteTransaction::Options{.optInjectedCommitFailure = Error{.code = Error::Code::IoError, .message = "fault"}});
      REQUIRE(transaction.apply(
        [&library](LibraryWrite& write) -> Result<>
        {
          std::ignore = addTrack(library, write, TrackSpec{.uri = "lost.flac"});
          REQUIRE(write.tracks().clear());
          return {};
        }));
      CHECK_FALSE(transaction.commit());
    }

    CHECK(committedRevision(library) == revisionBefore);
    // The marks did not move, so ids from a transaction that never published
    // may be allocated again.
    CHECK(addTrack(library, TrackSpec{.uri = "second.flac"}) == TrackId{2});
  }

  TEST_CASE("ListWriter - saved order refuses a Track id that was never allocated", "[library][unit][id-space]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto unallocated = ListBuilder::makeEmpty();
    unallocated.name("Ordered").orderTrackIds().add(TrackId{1});

    SECTION("on create")
    {
      auto const createRes = createList(library, unallocated);
      REQUIRE_FALSE(createRes);
      CHECK(createRes.error().code == Error::Code::InvalidInput);
    }

    SECTION("on update")
    {
      auto const listId = ao::test::requireValue(createList(library, ListBuilder::makeEmpty().name("Ordered")));
      auto transaction = writeTransaction(library);
      auto const updateRes = transaction.apply([listId, &unallocated](LibraryWrite& write)
                                               { return write.lists().update(listId, unallocated); });
      REQUIRE_FALSE(updateRes);
      CHECK(updateRes.error().code == Error::Code::InvalidInput);
    }

    SECTION("a Track staged in the same transaction may be ordered")
    {
      auto transaction = writeTransaction(library);
      auto const listRes = transaction.apply(
        [&library](LibraryWrite& write)
        {
          auto const trackId = addTrack(library, write, TrackSpec{.uri = "staged.flac"});
          auto ordered = ListBuilder::makeEmpty();
          ordered.name("Ordered").orderTrackIds().add(trackId);
          return write.lists().create(ordered);
        });
      REQUIRE(listRes);
      REQUIRE(transaction.commit());
    }

    SECTION("a deleted Track keeps its rank")
    {
      auto const trackId = addTrack(library, TrackSpec{.uri = "deleted.flac"});
      removeTrack(library, trackId);
      auto ordered = ListBuilder::makeEmpty();
      ordered.name("Ordered").orderTrackIds().add(trackId);
      CHECK(createList(library, ordered));
    }
  }

  TEST_CASE("ListWriter - List allocation never reaches the reserved maximum id", "[library][unit][id-space]")
  {
    auto const temp = ao::test::TempDir{};
    auto const databasePath = temp.path() / "db";
    {
      std::ignore = makeTestMusicLibrary(temp.path(), databasePath);
    }

    setRawLastListId(databasePath, kMaxListId - 1U);
    {
      auto library = makeTestMusicLibrary(temp.path(), databasePath);
      CHECK(ao::test::requireValue(createList(library, ListBuilder::makeEmpty().name("Last"))) == ListId{kMaxListId});

      auto const exhaustedRes = createList(library, ListBuilder::makeEmpty().name("Beyond"));
      REQUIRE_FALSE(exhaustedRes);
      CHECK(exhaustedRes.error().code == Error::Code::ResourceExhausted);
    }

    // The exhausted mark is the highest one a writer can commit, and it opens.
    CHECK(readRawIdSpace(databasePath).lastListId == kMaxListId);
    auto library = makeTestMusicLibrary(temp.path(), databasePath);
    auto const reopenedRes = createList(library, ListBuilder::makeEmpty().name("Beyond"));
    REQUIRE_FALSE(reopenedRes);
    CHECK(reopenedRes.error().code == Error::Code::ResourceExhausted);
  }
} // namespace ao::library::test
