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

    auto createRes = transaction.apply([&library, bytes](WriteTransaction& activeTransaction)
                                       { return library.resources().writer(activeTransaction).create(bytes); });

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
      [&library, bytes, &stagedId](WriteTransaction& activeTransaction) -> Result<ResourceId>
      {
        auto createRes = library.resources().writer(activeTransaction).create(bytes);

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

    auto failureRes =
      transaction.apply([&library, &oversizedValue](WriteTransaction& activeTransaction)
                        { return library.resources().writer(activeTransaction).create(oversizedValue); });

    REQUIRE_FALSE(failureRes);
    CHECK(failureRes.error().code == Error::Code::IoError);
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

    auto failureRes =
      transaction.apply([](WriteTransaction&) -> Result<>
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
                        [&library, bytes, &stagedId](WriteTransaction& activeTransaction) -> Result<>
                        {
                          auto createRes = library.resources().writer(activeTransaction).create(bytes);

                          if (!createRes)
                          {
                            return std::unexpected{createRes.error()};
                          }

                          stagedId = *createRes;
                          throw Exception{"unexpected operation failure"};
                        }),
                      "unexpected operation failure");

    auto retryTransaction = writable.writeTransaction();
    retryTransaction.abort();

    auto readTransaction = library.readTransaction();
    CHECK_FALSE(library.resources().reader(readTransaction).get(stagedId));
  }
} // namespace ao::library::test
