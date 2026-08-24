// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/library/ResourceStore.h>

#include "test/unit/library/LibraryStoreTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/ResourceLayout.h>
#include <ao/utility/ByteView.h>
#include <ao/utility/Sha256.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ao::library::test
{
  namespace
  {
    /**
     * @brief Two byte strings whose handles collide but whose digests differ.
     *
     * A handle is the digest's first four bytes, so a collision is a 32-bit
     * birthday event: searching short inputs upward from zero finds one in a few
     * tens of thousands of hashes, deterministically, because SHA-256 is fixed.
     * Searching rather than hard-coding keeps the pair honest — a constant would
     * silently stop colliding if the derivation ever changed.
     */
    std::pair<std::vector<std::byte>, std::vector<std::byte>> findHandleCollision()
    {
      constexpr std::uint32_t kSearchLimit = 4'000'000;
      auto seen = std::unordered_map<std::uint32_t, std::uint32_t>{};

      for (std::uint32_t candidate = 0; candidate < kSearchLimit; ++candidate)
      {
        auto const bytes = std::array{static_cast<std::byte>(candidate & 0xFFU),
                                      static_cast<std::byte>((candidate >> 8U) & 0xFFU),
                                      static_cast<std::byte>((candidate >> 16U) & 0xFFU),
                                      static_cast<std::byte>((candidate >> 24U) & 0xFFU)};
        auto const handle = deriveResourceId(utility::computeSha256(bytes)).raw();
        auto const [entry, inserted] = seen.emplace(handle, candidate);

        if (inserted)
        {
          continue;
        }

        auto const makeBytes = [](std::uint32_t const value)
        {
          return std::vector{static_cast<std::byte>(value & 0xFFU),
                             static_cast<std::byte>((value >> 8U) & 0xFFU),
                             static_cast<std::byte>((value >> 16U) & 0xFFU),
                             static_cast<std::byte>((value >> 24U) & 0xFFU)};
        };
        return {makeBytes(entry->second), makeBytes(candidate)};
      }

      return {};
    }

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

  TEST_CASE("ResourceStore - a row is a descriptor and holds no content", "[library][unit][resource]")
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

  TEST_CASE("ResourceStore - identical content produces one row and one id", "[library][unit][resource]")
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

  TEST_CASE("ResourceStore - a handle collision occupies two rows resolved by the probe", "[library][unit][resource]")
  {
    auto const [first, second] = findHandleCollision();
    REQUIRE_FALSE(first.empty());
    REQUIRE_FALSE(second.empty());
    auto const firstDigest = utility::computeSha256(first);
    auto const secondDigest = utility::computeSha256(second);
    REQUIRE(firstDigest != secondDigest);
    auto const homeKey = deriveResourceId(firstDigest);
    REQUIRE(deriveResourceId(secondDigest) == homeKey);

    auto fixture = LibraryStoreFixture{};
    auto& library = fixture.library;
    auto const& store = library.resources();

    auto writeTxn = writeTransaction(library);
    auto writer = physicalWriter(store, writeTxn);
    auto firstIdRes = writer.create(first);
    REQUIRE(firstIdRes);
    auto secondIdRes = writer.create(second);
    REQUIRE(secondIdRes);

    // The first content keeps the shared home key; the second is displaced one
    // slot upward, which is what makes the row still reachable from that key.
    CHECK(*firstIdRes == homeKey);
    CHECK(secondIdRes->raw() == homeKey.raw() + 1U);

    // A later create for the displaced content finds its existing row rather than
    // minting a second one for content that already has one.
    auto repeatRes = writer.create(second);
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

  TEST_CASE("ResourceStore - a counted length corrects a row and a declared one never does",
            "[library][unit][resource]")
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

  TEST_CASE("ResourceStore - a removed row is gone", "[library][unit][resource]")
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
    REQUIRE(physicalWriter(store, removeTxn).remove(*idRes));
    REQUIRE(removeTxn.commit());

    auto const readTxn = library.readTransaction();
    CHECK(rowCount(store.reader(readTxn)) == 0);
  }
} // namespace ao::library::test
