// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/library/ResourceStore.h>

#include "test/TestFixtureSupport.h"
#include "test/library/LibraryStoreTestSupport.h"
#include "test/library/WritableLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/ResourceLayout.h>
#include <ao/utility/ByteView.h>
#include <ao/utility/Sha256.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>

namespace ao::library::test
{
  namespace
  {
    std::size_t rowCount(ResourceStore::Reader const& reader)
    {
      std::size_t count = 0;

      for ([[maybe_unused]] auto const& entry : reader)
      {
        ++count;
      }

      return count;
    }
  } // namespace

  TEST_CASE("ResourceStore - a row is a descriptor and holds no content")
  {
    auto fixture = LibraryStoreFixture{};
    auto& library = fixture.library;
    auto const& store = library.resources();
    auto const buffer = utility::bytes::view(std::string_view{"hello"});

    auto writeTxn = writeTransaction(library);
    auto idRes = physicalWriter(store, writeTxn).create(buffer);
    REQUIRE(idRes);
    auto const id = *idRes;
    CHECK(id != kInvalidResourceId);
    REQUIRE(writeTxn.commit());

    auto const readTxn = library.readTransaction();
    auto const reader = store.reader(readTxn);
    STATIC_REQUIRE(std::is_same_v<decltype(reader.maxKey()), ResourceId>);
    CHECK(reader.maxKey() == id);

    auto const optDescriptor = reader.get(id);
    REQUIRE(optDescriptor);
    STATIC_REQUIRE(std::is_same_v<std::remove_cvref_t<decltype(*optDescriptor)>, ResourceDescriptor>);
    CHECK(optDescriptor->digest == utility::computeSha256(buffer));
    CHECK(optDescriptor->byteLength == buffer.size());

    // The handle is derived from the stored identity rather than recorded beside it.
    CHECK(deriveResourceId(optDescriptor->digest) == id);

    auto it = reader.begin();
    STATIC_REQUIRE(std::is_same_v<std::remove_cvref_t<decltype(it->first)>, ResourceId>);
    STATIC_REQUIRE(std::is_same_v<std::remove_cvref_t<decltype(it->second)>, ResourceDescriptor>);
    REQUIRE(it != reader.end());
    CHECK(it->first == id);
  }

  TEST_CASE("ResourceStore - identical content produces one row and one id")
  {
    auto fixture = LibraryStoreFixture{};
    auto& library = fixture.library;
    auto const& store = library.resources();
    auto const buffer = utility::bytes::view(std::string_view{"samedata"});

    auto firstTxn = writeTransaction(library);
    auto firstRes = physicalWriter(store, firstTxn).create(buffer);
    REQUIRE(firstRes);
    REQUIRE(firstTxn.commit());

    auto secondTxn = writeTransaction(library);
    auto secondRes = physicalWriter(store, secondTxn).create(buffer);
    REQUIRE(secondRes);
    CHECK(*secondRes == *firstRes);
    REQUIRE(secondTxn.commit());

    auto const readTxn = library.readTransaction();
    CHECK(rowCount(store.reader(readTxn)) == 1);
  }

  TEST_CASE("ResourceStore - a handle collision occupies two rows resolved by the probe")
  {
    // Both inputs have SHA-256 prefix dc478bbc, but distinct full digests.
    constexpr auto kFirst = std::array{std::byte{0xeb}, std::byte{0x2f}, std::byte{0}, std::byte{0}};
    constexpr auto kSecond = std::array{std::byte{0xcb}, std::byte{0x80}, std::byte{0}, std::byte{0}};
    auto const firstDigest = utility::computeSha256(kFirst);
    auto const secondDigest = utility::computeSha256(kSecond);
    REQUIRE(firstDigest != secondDigest);
    auto const homeKey = deriveResourceId(firstDigest);
    REQUIRE(deriveResourceId(secondDigest) == homeKey);

    auto fixture = LibraryStoreFixture{};
    auto& library = fixture.library;
    auto const& store = library.resources();

    auto writeTxn = writeTransaction(library);
    auto writer = physicalWriter(store, writeTxn);
    auto firstIdRes = writer.create(kFirst);
    REQUIRE(firstIdRes);
    auto secondIdRes = writer.create(kSecond);
    REQUIRE(secondIdRes);

    // The first content keeps the shared home key; the second is displaced one
    // slot upward, which is what makes the row still reachable from that key.
    CHECK(*firstIdRes == homeKey);
    CHECK(secondIdRes->raw() == homeKey.raw() + 1U);

    // A later create for the displaced content finds its existing row rather than
    // minting a second one for content that already has one.
    auto repeatRes = writer.create(kSecond);
    REQUIRE(repeatRes);
    CHECK(*repeatRes == *secondIdRes);
    REQUIRE(writeTxn.commit());

    auto const readTxn = library.readTransaction();
    auto const reader = store.reader(readTxn);
    CHECK(rowCount(reader) == 2);
    REQUIRE(reader.get(*firstIdRes));
    CHECK(reader.get(*firstIdRes)->digest == firstDigest);
    REQUIRE(reader.get(*secondIdRes));
    CHECK(reader.get(*secondIdRes)->digest == secondDigest);
  }

  TEST_CASE("ResourceStore - colliding handles wrap past the reserved zero id")
  {
    auto digest = utility::Sha256Digest{};
    digest.bytes.fill(std::byte{0xff});
    auto const first = ResourceDescriptor{.digest = digest, .byteLength = 1};
    digest.bytes.back() = std::byte{0};
    auto const second = ResourceDescriptor{.digest = digest, .byteLength = 2};

    auto fixture = LibraryStoreFixture{};
    auto const& store = fixture.library.resources();
    auto transaction = writeTransaction(fixture.library);
    auto writer = physicalWriter(store, transaction);
    auto const firstId = ao::test::requireValue(writer.getOrCreate(first));
    auto const secondId = ao::test::requireValue(writer.getOrCreate(second));
    CHECK(firstId == ResourceId{std::numeric_limits<std::uint32_t>::max()});
    CHECK(secondId == ResourceId{1});
    CHECK(ao::test::requireValue(writer.getOrCreate(second)) == secondId);
    REQUIRE(transaction.commit());

    auto const readTransaction = fixture.library.readTransaction();
    auto const reader = store.reader(readTransaction);
    CHECK(rowCount(reader) == 2);
    auto const optFirst = reader.get(firstId);
    auto const optSecond = reader.get(secondId);
    REQUIRE(optFirst);
    REQUIRE(optSecond);
    CHECK(optFirst->digest == first.digest);
    CHECK(optSecond->digest == second.digest);
  }

  TEST_CASE("ResourceStore - a counted length corrects a row and a declared one never does")
  {
    auto fixture = LibraryStoreFixture{};
    auto& library = fixture.library;
    auto const& store = library.resources();
    auto const buffer = utility::bytes::view(std::string_view{"cover-bytes"});
    auto const digest = utility::computeSha256(buffer);

    SECTION("a declared descriptor creates a missing row with its hinted length")
    {
      auto writeTxn = writeTransaction(library);
      auto idRes =
        physicalWriter(store, writeTxn).getOrCreate(ResourceDescriptor{.digest = digest, .byteLength = 4096});
      REQUIRE(idRes);
      REQUIRE(writeTxn.commit());

      auto const readTxn = library.readTransaction();
      auto const optDescriptor = store.reader(readTxn).get(*idRes);
      REQUIRE(optDescriptor);
      CHECK(optDescriptor->byteLength == 4096);
    }

    SECTION("a declared descriptor leaves an existing length alone")
    {
      auto writeTxn = writeTransaction(library);
      auto writer = physicalWriter(store, writeTxn);
      auto countedRes = writer.create(buffer);
      REQUIRE(countedRes);
      auto declaredRes = writer.getOrCreate(ResourceDescriptor{.digest = digest, .byteLength = 4096});
      REQUIRE(declaredRes);
      CHECK(*declaredRes == *countedRes);
      REQUIRE(writeTxn.commit());

      auto const readTxn = library.readTransaction();
      auto const optDescriptor = store.reader(readTxn).get(*countedRes);
      REQUIRE(optDescriptor);
      CHECK(optDescriptor->byteLength == buffer.size());
    }

    SECTION("hashed bytes correct a length a document guessed wrong")
    {
      auto declareTxn = writeTransaction(library);
      auto declaredRes =
        physicalWriter(store, declareTxn).getOrCreate(ResourceDescriptor{.digest = digest, .byteLength = 4096});
      REQUIRE(declaredRes);
      REQUIRE(declareTxn.commit());

      auto countTxn = writeTransaction(library);
      auto countedRes = physicalWriter(store, countTxn).create(buffer);
      REQUIRE(countedRes);
      CHECK(*countedRes == *declaredRes);
      REQUIRE(countTxn.commit());

      auto const readTxn = library.readTransaction();
      auto const optDescriptor = store.reader(readTxn).get(*countedRes);
      REQUIRE(optDescriptor);
      CHECK(optDescriptor->byteLength == buffer.size());
      CHECK(rowCount(store.reader(readTxn)) == 1);
    }

    SECTION("an observed descriptor corrects a length after hashing finished outside the writer")
    {
      auto declareTxn = writeTransaction(library);
      auto declaredRes =
        physicalWriter(store, declareTxn).getOrCreate(ResourceDescriptor{.digest = digest, .byteLength = 4096});
      REQUIRE(declaredRes);
      REQUIRE(declareTxn.commit());

      auto observeTxn = writeTransaction(library);
      auto const observed = ObservedResourceDescriptor{.descriptor = ResourceDescriptor{
                                                         .digest = digest,
                                                         .byteLength = static_cast<std::uint32_t>(buffer.size()),
                                                       }};
      auto observedRes = physicalWriter(store, observeTxn).getOrCreate(observed);
      REQUIRE(observedRes);
      CHECK(*observedRes == *declaredRes);
      REQUIRE(observeTxn.commit());

      auto const readTxn = library.readTransaction();
      auto const optDescriptor = store.reader(readTxn).get(*observedRes);
      REQUIRE(optDescriptor);
      CHECK(optDescriptor->byteLength == buffer.size());
      CHECK(rowCount(store.reader(readTxn)) == 1);
    }
  }

  TEST_CASE("ResourceStore - a removed row is gone")
  {
    auto fixture = LibraryStoreFixture{};
    auto& library = fixture.library;
    auto const& store = library.resources();
    auto const buffer = utility::bytes::view(std::string_view{"test"});

    auto writeTxn = writeTransaction(library);
    auto idRes = physicalWriter(store, writeTxn).create(buffer);
    REQUIRE(idRes);
    REQUIRE(writeTxn.commit());

    auto removeTxn = writeTransaction(library);
    REQUIRE(physicalWriter(store, removeTxn).tryRemove(*idRes));
    REQUIRE(removeTxn.commit());

    auto const readTxn = library.readTransaction();
    CHECK(rowCount(store.reader(readTxn)) == 0);
  }
} // namespace ao::library::test
