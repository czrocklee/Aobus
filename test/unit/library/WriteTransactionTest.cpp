// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/library/WriteTransaction.h>

#include "lib/library/detail/LibraryError.h"
#include "lib/lmdb/detail/ReadFaultInjection.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <lmdb.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <vector>

namespace ao::library::test
{
  TEST_CASE("WriteTransaction - logical identity restore changes only the library id",
            "[library][unit][write-transaction]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto const before = library.metadataHeader();
    auto replacementId = std::array<std::byte, 16>{};

    for (std::size_t index = 0; index < replacementId.size(); ++index)
    {
      replacementId[index] = static_cast<std::byte>(index + 1U);
    }

    auto transaction = writeTransaction(library);
    REQUIRE(
      transaction.apply([&replacementId](LibraryWrite& write) { return write.restoreLibraryIdentity(replacementId); }));
    REQUIRE(transaction.commit());

    auto const after = library.metadataHeader();
    CHECK(after.libraryId == replacementId);
    CHECK(after.magic == before.magic);
    CHECK(after.libraryVersion == before.libraryVersion);
    CHECK(after.flags == before.flags);
    CHECK(after.createdTime == before.createdTime);
  }

  TEST_CASE("WriteTransaction - successful root operation remains committable", "[library][unit][write-transaction]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto writable = ao::test::requireValue(WritableMusicLibrary::acquire(library));
    auto transaction = writable.writeTransaction();
    auto const bytes = utility::bytes::view(std::string_view{"committed resource"});

    auto createRes = transaction.apply([&library, bytes](LibraryWrite& write)
                                       { return physicalWriter(library.resources(), write).create(bytes); });

    REQUIRE(createRes);
    auto const resourceId = *createRes;
    REQUIRE(transaction.commit());

    auto readTransaction = library.readTransaction();
    auto const optStored = library.resources().reader(readTransaction).get(resourceId);
    REQUIRE(optStored);
    CHECK(std::ranges::equal(*optStored, bytes));
  }

  TEST_CASE("WriteTransaction - operation error aborts staged writes and releases the writer gate",
            "[library][unit][write-transaction][concurrency]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto writable = ao::test::requireValue(WritableMusicLibrary::acquire(library));
    auto transaction = writable.writeTransaction();
    auto const bytes = utility::bytes::view(std::string_view{"rolled back resource"});
    auto stagedId = kInvalidResourceId;

    auto operationRes = transaction.apply(
      [&library, bytes, &stagedId](LibraryWrite& write) -> Result<ResourceId>
      {
        auto createRes = physicalWriter(library.resources(), write).create(bytes);

        if (!createRes)
        {
          return std::unexpected{createRes.error()};
        }

        stagedId = *createRes;
        return makeError(Error::Code::Conflict, "reject staged write");
      });

    REQUIRE_FALSE(operationRes);
    CHECK(operationRes.error().code == Error::Code::Conflict);
    CHECK(operationRes.error().message == "reject staged write");

    auto retryTransaction = writable.writeTransaction();
    retryTransaction.abort();

    auto readTransaction = library.readTransaction();
    CHECK_FALSE(library.resources().reader(readTransaction).get(stagedId));
  }

  TEST_CASE("WriteTransaction - native mutation failure becomes a terminal Result error",
            "[library][regression][write-transaction][concurrency]")
  {
    constexpr std::size_t kMapSize = std::size_t{256} * 1024;
    auto const temp = ao::test::TempDir{};
    auto library = ao::test::requireValue(
      MusicLibrary::open(temp.path(), temp.path() / "db", MusicLibrary::Options{.mapSize = kMapSize}));
    auto writable = ao::test::requireValue(WritableMusicLibrary::acquire(library));
    auto transaction = writable.writeTransaction();
    auto const oversizedValue = std::vector<std::byte>(kMapSize * 4);

    auto failureRes = transaction.apply([&library, &oversizedValue](LibraryWrite& write)
                                        { return physicalWriter(library.resources(), write).create(oversizedValue); });

    REQUIRE_FALSE(failureRes);
    CHECK(failureRes.error().code == Error::Code::IoError);
    auto retryTransaction = writable.writeTransaction();
    retryTransaction.abort();
  }

  TEST_CASE("WriteTransaction - native writer read fault aborts the root operation",
            "[library][regression][write-transaction]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto setup = writeTransaction(library);
    auto resourceIdRes = setup.apply(
      [&library](LibraryWrite& write)
      { return physicalWriter(library.resources(), write).create(utility::bytes::view(std::string_view{"probe"})); });
    REQUIRE(resourceIdRes);
    REQUIRE(setup.commit());

    auto transaction = writeTransaction(library);

    auto failureRes = transaction.apply(
      [&library, resourceId = *resourceIdRes](LibraryWrite& write) -> Result<>
      {
        auto writer = physicalWriter(library.resources(), write);
        [[maybe_unused]] auto injection = lmdb::detail::ReadFaultInjection{MDB_PANIC};
        std::ignore = writer.get(resourceId);
        return {};
      });

    REQUIRE_FALSE(failureRes);
    CHECK(failureRes.error().code == Error::Code::IoError);
    INFO(failureRes.error().message);
    CHECK(failureRes.error().message.contains("mdb_cursor_get"));

    auto retry = writeTransaction(library);
    CHECK(library.libraryRevision(retry) == 2);
    retry.abort();
    auto read = library.readTransaction();
    CHECK(library.libraryRevision(read) == 1);
  }

  TEST_CASE("WriteTransaction - private library failure becomes a terminal Result error",
            "[library][regression][write-transaction]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto writable = ao::test::requireValue(WritableMusicLibrary::acquire(library));
    auto transaction = writable.writeTransaction();

    auto failureRes =
      transaction.apply([](LibraryWrite&) -> Result<>
                        { detail::throwLibraryError(Error::Code::CorruptData, "private library failure"); });

    REQUIRE_FALSE(failureRes);
    CHECK(failureRes.error().code == Error::Code::CorruptData);
    CHECK(failureRes.error().message == "private library failure");
    auto retryTransaction = writable.writeTransaction();
    retryTransaction.abort();
  }

  TEST_CASE("WriteTransaction - unexpected exception aborts before it is rethrown",
            "[library][unit][write-transaction][concurrency]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto writable = ao::test::requireValue(WritableMusicLibrary::acquire(library));
    auto transaction = writable.writeTransaction();
    auto const bytes = utility::bytes::view(std::string_view{"exception rollback"});
    auto stagedId = kInvalidResourceId;

    CHECK_THROWS_WITH(transaction.apply(
                        [&library, bytes, &stagedId](LibraryWrite& write) -> Result<>
                        {
                          auto createRes = physicalWriter(library.resources(), write).create(bytes);

                          if (!createRes)
                          {
                            return std::unexpected{createRes.error()};
                          }

                          stagedId = *createRes;
                          throw std::runtime_error{"unexpected operation failure"};
                        }),
                      "unexpected operation failure");

    auto retryTransaction = writable.writeTransaction();
    retryTransaction.abort();

    auto readTransaction = library.readTransaction();
    CHECK_FALSE(library.resources().reader(readTransaction).get(stagedId));
  }
} // namespace ao::library::test
