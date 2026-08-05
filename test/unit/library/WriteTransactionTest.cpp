// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/library/WriteTransaction.h>

#include "lib/library/detail/LibraryError.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <string_view>
#include <vector>

namespace ao::library::test
{
  TEST_CASE("WriteTransaction - successful root operation remains committable", "[library][unit][write-transaction]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto writable = ao::test::requireValue(WritableMusicLibrary::acquire(library));
    auto transaction = writable.writeTransaction();
    auto const bytes = utility::bytes::view(std::string_view{"committed resource"});

    auto createResult = transaction.apply([&library, bytes](WriteTransaction& activeTransaction)
                                          { return library.resources().writer(activeTransaction).create(bytes); });

    REQUIRE(createResult);
    auto const resourceId = *createResult;
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

    auto operationResult = transaction.apply(
      [&library, bytes, &stagedId](WriteTransaction& activeTransaction) -> Result<ResourceId>
      {
        auto createResult = library.resources().writer(activeTransaction).create(bytes);

        if (!createResult)
        {
          return std::unexpected{createResult.error()};
        }

        stagedId = *createResult;
        return makeError(Error::Code::Conflict, "reject staged write");
      });

    REQUIRE_FALSE(operationResult);
    CHECK(operationResult.error().code == Error::Code::Conflict);
    CHECK(operationResult.error().message == "reject staged write");

    auto commitResult = transaction.commit();
    REQUIRE_FALSE(commitResult);
    CHECK(commitResult.error().code == Error::Code::InvalidState);

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

    auto failureResult =
      transaction.apply([&library, &oversizedValue](WriteTransaction& activeTransaction)
                        { return library.resources().writer(activeTransaction).create(oversizedValue); });

    REQUIRE_FALSE(failureResult);
    CHECK(failureResult.error().code == Error::Code::IoError);
    auto retryTransaction = writable.writeTransaction();
    retryTransaction.abort();
  }

  TEST_CASE("WriteTransaction - private library failure becomes a terminal Result error",
            "[library][regression][write-transaction]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto writable = ao::test::requireValue(WritableMusicLibrary::acquire(library));
    auto transaction = writable.writeTransaction();

    auto failureResult =
      transaction.apply([](WriteTransaction&) -> Result<>
                        { detail::throwLibraryError(Error::Code::CorruptData, "private library failure"); });

    REQUIRE_FALSE(failureResult);
    CHECK(failureResult.error().code == Error::Code::CorruptData);
    CHECK(failureResult.error().message == "private library failure");
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
                        [&library, bytes, &stagedId](WriteTransaction& activeTransaction) -> Result<>
                        {
                          auto createResult = library.resources().writer(activeTransaction).create(bytes);

                          if (!createResult)
                          {
                            return std::unexpected{createResult.error()};
                          }

                          stagedId = *createResult;
                          throw Exception{"unexpected operation failure"};
                        }),
                      "unexpected operation failure");

    auto retryTransaction = writable.writeTransaction();
    retryTransaction.abort();

    auto readTransaction = library.readTransaction();
    CHECK_FALSE(library.resources().reader(readTransaction).get(stagedId));
  }

  TEST_CASE("WriteTransaction - root operation cannot nest or commit", "[library][unit][write-transaction]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto writable = ao::test::requireValue(WritableMusicLibrary::acquire(library));

    SECTION("nested operation")
    {
      auto transaction = writable.writeTransaction();
      auto operationResult = transaction.apply(
        [](WriteTransaction& activeTransaction) -> Result<>
        {
          return activeTransaction.apply([](WriteTransaction&) -> Result<> { return {}; });
        });

      REQUIRE_FALSE(operationResult);
      CHECK(operationResult.error().code == Error::Code::InvalidState);
      CHECK_FALSE(transaction.commit());
    }

    SECTION("commit from operation")
    {
      auto transaction = writable.writeTransaction();
      auto operationResult =
        transaction.apply([](WriteTransaction& activeTransaction) { return activeTransaction.commit(); });

      REQUIRE_FALSE(operationResult);
      CHECK(operationResult.error().code == Error::Code::InvalidState);
      CHECK_FALSE(transaction.commit());
    }
  }
} // namespace ao::library::test
