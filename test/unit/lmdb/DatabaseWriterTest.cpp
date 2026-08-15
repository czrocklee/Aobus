// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/lmdb/detail/ReservationWriterAccess.h"
#include "lib/lmdb/detail/TransactionFailure.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/lmdb/LmdbTestSupport.h"
#include <ao/Error.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <utility>

namespace ao::lmdb::test
{
  namespace
  {
    struct PotentiallyThrowingEncoder final
    {
      void operator()(std::span<std::byte> /*unused*/) const {}
    };

    struct ValueReturningEncoder final
    {
      std::int32_t operator()(std::span<std::byte> /*unused*/) const noexcept { return 0; }
    };

    struct LvalueReferenceEncoder final
    {
      void operator()(std::span<std::byte>& /*unused*/) const noexcept {}
    };

    struct NoexceptEncoder final
    {
      void operator()(std::span<std::byte> /*unused*/) const noexcept {}
    };

    template<typename Writer>
    concept HasIntegerCreateSize = requires(Writer& writer) { writer.create(std::uint32_t{1}, std::size_t{1}); };

    template<typename Writer>
    concept HasIntegerCreate =
      requires(Writer& writer) { writer.create(std::uint32_t{1}, std::span<std::byte const>{}); };

    template<typename Writer>
    concept HasByteCreate =
      requires(Writer& writer) { writer.create(std::span<std::byte const>{}, std::span<std::byte const>{}); };

    template<typename Writer>
    concept HasBlobCreateSize =
      requires(Writer& writer) { writer.create(std::span<std::byte const>{}, std::size_t{1}); };

    template<typename Writer>
    concept HasAppendSize = requires(Writer& writer) { writer.append(std::size_t{1}); };

    template<typename Writer>
    concept HasAppend = requires(Writer& writer) { writer.append(std::span<std::byte const>{}); };

    template<typename Writer>
    concept HasMaxKey = requires(Writer const& writer) { writer.maxKey(); };

    template<typename Writer>
    concept HasIntegerUpdateSize = requires(Writer& writer) { writer.update(std::uint32_t{1}, std::size_t{1}); };

    template<typename Writer>
    concept HasBlobUpdateSize =
      requires(Writer& writer) { writer.update(std::span<std::byte const>{}, std::size_t{1}); };

    template<typename Writer>
    concept HasIntegerUpdate =
      requires(Writer& writer) { writer.update(std::uint32_t{1}, std::span<std::byte const>{}); };

    template<typename Writer>
    concept HasByteUpdate =
      requires(Writer& writer) { writer.update(std::span<std::byte const>{}, std::span<std::byte const>{}); };

    template<typename Writer>
    concept HasIntegerGet = requires(Writer const& writer) { writer.get(std::uint32_t{1}); };

    template<typename Writer>
    concept HasByteGet = requires(Writer const& writer) { writer.get(std::span<std::byte const>{}); };

    template<typename Writer>
    concept AcceptsCreateEncoder =
      requires(Writer& writer) { writer.create(std::uint32_t{1}, std::size_t{1}, NoexceptEncoder{}); };

    template<typename Writer>
    concept AcceptsByteCreateEncoder =
      requires(Writer& writer) { writer.create(std::span<std::byte const>{}, std::size_t{1}, NoexceptEncoder{}); };

    template<typename Writer>
    concept AcceptsAppendEncoder = requires(Writer& writer) { writer.append(std::size_t{1}, NoexceptEncoder{}); };

    template<typename Writer>
    concept AcceptsUpdateEncoder =
      requires(Writer& writer) { writer.update(std::uint32_t{1}, std::size_t{1}, NoexceptEncoder{}); };

    template<typename Writer>
    concept AcceptsByteUpdateEncoder =
      requires(Writer& writer) { writer.update(std::span<std::byte const>{}, std::size_t{1}, NoexceptEncoder{}); };

    using IntegerWriter = IntegerKeyDatabase::Writer;
    using ByteWriter = ByteKeyDatabase::Writer;
    using ReservationAccess = detail::ReservationWriterAccess;

    template<typename Encoder>
    concept ReservationAccessAcceptsCreateEncoder = requires(IntegerWriter& writer) {
      ReservationAccess::create(writer, std::uint32_t{1}, std::size_t{1}, Encoder{});
    };

    template<typename Encoder>
    concept ReservationAccessAcceptsAppendEncoder =
      requires(IntegerWriter& writer) { ReservationAccess::append(writer, std::size_t{1}, Encoder{}); };

    template<typename Encoder>
    concept ReservationAccessAcceptsUpdateEncoder = requires(IntegerWriter& writer) {
      ReservationAccess::update(writer, std::uint32_t{1}, std::size_t{1}, Encoder{});
    };

    static_assert(HasIntegerCreate<IntegerWriter>);
    static_assert(!HasByteCreate<IntegerWriter>);
    static_assert(!HasIntegerCreate<ByteWriter>);
    static_assert(HasByteCreate<ByteWriter>);
    static_assert(HasAppend<IntegerWriter>);
    static_assert(!HasAppend<ByteWriter>);
    static_assert(HasMaxKey<IntegerWriter>);
    static_assert(!HasMaxKey<ByteWriter>);
    static_assert(HasIntegerUpdate<IntegerWriter>);
    static_assert(!HasByteUpdate<IntegerWriter>);
    static_assert(!HasIntegerUpdate<ByteWriter>);
    static_assert(HasByteUpdate<ByteWriter>);
    static_assert(HasIntegerGet<IntegerWriter>);
    static_assert(!HasByteGet<IntegerWriter>);
    static_assert(!HasIntegerGet<ByteWriter>);
    static_assert(HasByteGet<ByteWriter>);

    static_assert(!HasIntegerCreateSize<IntegerWriter>);
    static_assert(!HasBlobCreateSize<IntegerWriter>);
    static_assert(!HasBlobCreateSize<ByteWriter>);
    static_assert(!HasAppendSize<IntegerWriter>);
    static_assert(!HasAppendSize<ByteWriter>);
    static_assert(!HasIntegerUpdateSize<IntegerWriter>);
    static_assert(!HasBlobUpdateSize<IntegerWriter>);
    static_assert(!HasBlobUpdateSize<ByteWriter>);
    static_assert(!AcceptsCreateEncoder<IntegerWriter>);
    static_assert(!AcceptsByteCreateEncoder<ByteWriter>);
    static_assert(!AcceptsAppendEncoder<IntegerWriter>);
    static_assert(!AcceptsUpdateEncoder<IntegerWriter>);
    static_assert(!AcceptsByteUpdateEncoder<ByteWriter>);

    static_assert(ReservationAccessAcceptsCreateEncoder<NoexceptEncoder>);
    static_assert(!ReservationAccessAcceptsCreateEncoder<PotentiallyThrowingEncoder>);
    static_assert(ReservationAccessAcceptsAppendEncoder<NoexceptEncoder>);
    static_assert(!ReservationAccessAcceptsAppendEncoder<PotentiallyThrowingEncoder>);
    static_assert(!ReservationAccessAcceptsAppendEncoder<ValueReturningEncoder>);
    static_assert(!ReservationAccessAcceptsAppendEncoder<LvalueReferenceEncoder>);
    static_assert(ReservationAccessAcceptsUpdateEncoder<NoexceptEncoder>);
    static_assert(!ReservationAccessAcceptsUpdateEncoder<PotentiallyThrowingEncoder>);
  } // namespace

  TEST_CASE("IntegerKeyDatabase::Writer - create with id and data", "[lmdb][unit][database][writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openIntegerKeyDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);
    REQUIRE(writer.create(1, createStringData("hello")));
    REQUIRE(wtxn.commit());

    // Verify via reader
    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);
    auto const optData = reader.get(1);
    REQUIRE(optData);
    REQUIRE(utility::bytes::stringView(*optData) == "hello");
  }

  TEST_CASE("ReservationWriterAccess - create encoder receives exact storage once and persists bytes",
            "[lmdb][unit][database][writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openIntegerKeyDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);
    std::int32_t encoderCalls = 0;
    std::size_t encodedSize = 0;
    auto const result = ReservationAccess::create(writer,
                                                  1,
                                                  10,
                                                  [&](std::span<std::byte> output) noexcept
                                                  {
                                                    ++encoderCalls;
                                                    encodedSize = output.size();
                                                    std::memset(output.data(), 'x', output.size());
                                                  });
    REQUIRE(result);
    CHECK(encoderCalls == 1);
    CHECK(encodedSize == 10);

    REQUIRE(wtxn.commit());

    // Verify via reader
    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);
    auto const optData = reader.get(1);
    REQUIRE(optData);
    REQUIRE(optData->size() == 10);
    REQUIRE(utility::bytes::stringView(*optData) == std::string(10, 'x'));
  }

  TEST_CASE("IntegerKeyDatabase::Writer - append with data", "[lmdb][unit][database][writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openIntegerKeyDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);

    auto const id1Res = writer.append(createStringData("first"));
    REQUIRE(id1Res);
    REQUIRE(*id1Res == 1);

    auto const id2Res = writer.append(createStringData("second"));
    REQUIRE(id2Res);
    REQUIRE(*id2Res == 2);

    REQUIRE(wtxn.commit());

    // Verify via reader
    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);
    auto const optData1 = reader.get(1);
    auto const optData2 = reader.get(2);
    REQUIRE(optData1);
    REQUIRE(optData2);
    CHECK(utility::bytes::stringView(*optData1) == "first");
    CHECK(utility::bytes::stringView(*optData2) == "second");
  }

  TEST_CASE("ReservationWriterAccess - append encoders receive exact storage once and persist bytes",
            "[lmdb][unit][database][writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openIntegerKeyDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);

    std::int32_t firstEncoderCalls = 0;
    std::size_t firstEncodedSize = 0;
    auto append1Res = ReservationAccess::append(writer,
                                                8,
                                                [&](std::span<std::byte> output) noexcept
                                                {
                                                  ++firstEncoderCalls;
                                                  firstEncodedSize = output.size();
                                                  std::memset(output.data(), 'a', output.size());
                                                });
    REQUIRE(append1Res);
    CHECK(*append1Res == 1);
    CHECK(firstEncoderCalls == 1);
    CHECK(firstEncodedSize == 8);

    std::int32_t secondEncoderCalls = 0;
    std::size_t secondEncodedSize = 0;
    auto append2Res = ReservationAccess::append(writer,
                                                12,
                                                [&](std::span<std::byte> output) noexcept
                                                {
                                                  ++secondEncoderCalls;
                                                  secondEncodedSize = output.size();
                                                  std::memset(output.data(), 'b', output.size());
                                                });
    REQUIRE(append2Res);
    CHECK(*append2Res == 2);
    CHECK(secondEncoderCalls == 1);
    CHECK(secondEncodedSize == 12);

    REQUIRE(wtxn.commit());

    // Verify via reader
    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);
    auto const optData1 = reader.get(1);
    auto const optData2 = reader.get(2);
    REQUIRE(optData1);
    REQUIRE(optData2);
    CHECK(utility::bytes::stringView(*optData1) == std::string(8, 'a'));
    CHECK(utility::bytes::stringView(*optData2) == std::string(12, 'b'));
  }

  TEST_CASE("ReservationWriterAccess - append conflict restores cached maximum and skips encoder",
            "[lmdb][unit][database][writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});
    auto transaction = beginWriteTransaction(env);
    auto db = openIntegerKeyDatabase(transaction, "test");
    auto writer = db.writer(transaction);

    REQUIRE(writer.create(1, createStringData("explicit")));
    CHECK(writer.maxKey() == 0);

    std::int32_t encoderCalls = 0;
    auto appendRes = ReservationAccess::append(writer, 8, [&](std::span<std::byte>) noexcept { ++encoderCalls; });

    REQUIRE_FALSE(appendRes);
    CHECK(appendRes.error().code == Error::Code::Conflict);
    CHECK(encoderCalls == 0);
    CHECK(writer.maxKey() == 0);

    REQUIRE(writer.update(1, createStringData("still active")));
    REQUIRE(transaction.commit());
  }

  TEST_CASE("IntegerKeyDatabase::Writer - clear resets integer append allocation on the same writer",
            "[lmdb][regression][database][writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});
    auto transaction = beginWriteTransaction(env);
    auto db = openIntegerKeyDatabase(transaction, "test");
    auto writer = db.writer(transaction);
    auto const firstIdRes = writer.append(createStringData("first"));
    auto const secondIdRes = writer.append(createStringData("second"));
    REQUIRE(firstIdRes);
    REQUIRE(secondIdRes);
    CHECK(*firstIdRes == 1);
    CHECK(*secondIdRes == 2);

    REQUIRE(writer.clear());
    auto const replacementIdRes = writer.append(createStringData("replacement"));
    REQUIRE(replacementIdRes);
    CHECK(*replacementIdRes == 1);
    REQUIRE(transaction.commit());

    auto const readTransaction = beginReadTransaction(env);
    auto const reader = db.reader(readTransaction);
    auto const optReplacement = reader.get(1);
    REQUIRE(optReplacement);
    CHECK(utility::bytes::stringView(*optReplacement) == "replacement");
    CHECK_FALSE(reader.get(2));
  }

  TEST_CASE("IntegerKeyDatabase::Writer - append reports exhausted integer key space", "[lmdb][unit][database][writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openIntegerKeyDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);

    REQUIRE(writer.create(std::numeric_limits<std::uint32_t>::max(), createStringData("last")));
    REQUIRE(wtxn.commit());

    auto wtxn2 = beginWriteTransaction(env);
    auto writer2 = db.writer(wtxn2);

    auto const dataRes = writer2.append(createStringData("overflow"));
    REQUIRE(!dataRes);
    CHECK(dataRes.error().code == Error::Code::ResourceExhausted);

    std::int32_t encoderCalls = 0;
    auto const reserveRes =
      ReservationAccess::append(writer2, 4, [&](std::span<std::byte>) noexcept { ++encoderCalls; });
    REQUIRE(!reserveRes);
    CHECK(reserveRes.error().code == Error::Code::ResourceExhausted);
    CHECK(encoderCalls == 0);

    REQUIRE(writer2.update(std::numeric_limits<std::uint32_t>::max(), createStringData("still active")));
    REQUIRE(wtxn2.commit());

    auto const rtxn = beginReadTransaction(env);
    auto const optData = db.reader(rtxn).get(std::numeric_limits<std::uint32_t>::max());
    REQUIRE(optData);
    CHECK(utility::bytes::stringView(*optData) == "still active");
  }

  TEST_CASE("IntegerKeyDatabase::Writer - update existing record", "[lmdb][unit][database][writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openIntegerKeyDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);

    // Create initial record
    REQUIRE(writer.create(1, createStringData("original")));
    REQUIRE(wtxn.commit());

    // Update the record
    auto wtxn2 = beginWriteTransaction(env);
    auto writer2 = db.writer(wtxn2);
    REQUIRE(writer2.update(1, createStringData("updated")));
    REQUIRE(wtxn2.commit());

    // Verify via reader
    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);
    auto const optData = reader.get(1);
    REQUIRE(optData);
    CHECK(optData->size() == 7);
    CHECK(utility::bytes::stringView(*optData) == "updated");
  }

  TEST_CASE("ReservationWriterAccess - update encoder receives exact storage once and persists bytes",
            "[lmdb][unit][database][writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

    auto setupTransaction = beginWriteTransaction(env);
    auto db = openIntegerKeyDatabase(setupTransaction, "test");
    REQUIRE(db.writer(setupTransaction).create(1, createStringData("original")));
    REQUIRE(setupTransaction.commit());

    auto updateTransaction = beginWriteTransaction(env);
    auto writer = db.writer(updateTransaction);
    std::int32_t encoderCalls = 0;
    std::size_t encodedSize = 0;
    auto const result = ReservationAccess::update(writer,
                                                  1,
                                                  7,
                                                  [&](std::span<std::byte> output) noexcept
                                                  {
                                                    ++encoderCalls;
                                                    encodedSize = output.size();
                                                    std::memcpy(output.data(), "changed", output.size());
                                                  });
    REQUIRE(result);
    CHECK(encoderCalls == 1);
    CHECK(encodedSize == 7);
    REQUIRE(updateTransaction.commit());

    auto const readTransaction = beginReadTransaction(env);
    auto const optData = db.reader(readTransaction).get(1);
    REQUIRE(optData);
    CHECK(utility::bytes::stringView(*optData) == "changed");
  }

  TEST_CASE("ByteKeyDatabase::Writer - copied create and update persist bytes", "[lmdb][unit][database][writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

    auto writeTransaction = beginWriteTransaction(env);
    auto db = openByteKeyDatabase(writeTransaction, "test");
    auto writer = db.writer(writeTransaction);
    auto const key = createStringData("blob-key");
    auto const result = writer.create(key, createStringData("blob-value"));
    REQUIRE(result);
    REQUIRE(writeTransaction.commit());

    {
      auto const readTransaction = beginReadTransaction(env);
      auto const optData = db.reader(readTransaction).get(key);
      REQUIRE(optData);
      CHECK(utility::bytes::stringView(*optData) == "blob-value");
    }

    auto updateTransaction = beginWriteTransaction(env);
    auto updateWriter = db.writer(updateTransaction);
    auto const updateRes = updateWriter.update(key, createStringData("updated-blob"));
    REQUIRE(updateRes);
    REQUIRE(updateTransaction.commit());

    auto const readTransaction = beginReadTransaction(env);
    auto const optData = db.reader(readTransaction).get(key);
    REQUIRE(optData);
    CHECK(utility::bytes::stringView(*optData) == "updated-blob");
  }

  TEST_CASE("IntegerKeyDatabase::Writer - delete record", "[lmdb][unit][database][writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openIntegerKeyDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);
    REQUIRE(writer.create(1, createStringData("test")));
    REQUIRE(wtxn.commit());

    // Verify exists
    {
      auto const rtxn = beginReadTransaction(env);
      auto const reader = db.reader(rtxn);
      auto const optData1 = reader.get(1);
      REQUIRE(optData1);
    }

    // Delete
    {
      auto deleteTransaction = beginWriteTransaction(env);
      auto deleteWriter = db.writer(deleteTransaction);
      REQUIRE(deleteWriter.del(1));
      REQUIRE(deleteTransaction.commit());
    }

    // Verify deleted
    {
      auto const rtxn = beginReadTransaction(env);
      auto const reader = db.reader(rtxn);
      auto const optData = reader.get(1);
      REQUIRE_FALSE(optData);
    }
  }

  TEST_CASE("IntegerKeyDatabase::Writer - delete missing record returns false", "[lmdb][unit][database][writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openIntegerKeyDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);

    REQUIRE_FALSE(writer.del(123));
    REQUIRE(writer.create(1, createStringData("after miss")));
    REQUIRE(wtxn.commit());

    auto const rtxn = beginReadTransaction(env);
    auto const optData = db.reader(rtxn).get(1);
    REQUIRE(optData);
    CHECK(utility::bytes::stringView(*optData) == "after miss");
  }

  TEST_CASE("IntegerKeyDatabase::Writer - get within write transaction", "[lmdb][unit][database][writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openIntegerKeyDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);

    REQUIRE(writer.create(42, createStringData("answer")));
    auto const optDataRes = writer.get(42);
    REQUIRE(optDataRes);
    CHECK(optDataRes->size() == 6);
    CHECK(utility::bytes::stringView(*optDataRes) == "answer");
  }

  TEST_CASE("IntegerKeyDatabase::Writer - move constructor", "[lmdb][unit][database][writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openIntegerKeyDatabase(wtxn, "test");

    auto writer1 = db.writer(wtxn);
    REQUIRE(writer1.create(1, createStringData("test")));

    auto writer2 = IntegerKeyDatabase::Writer{std::move(writer1)};
    // writer1 is now in moved-from state
    // writer2 should still be usable

    REQUIRE(wtxn.commit());
  }

  TEST_CASE("IntegerKeyDatabase::Writer - create returns Conflict on duplicate id with data",
            "[lmdb][unit][database][writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openIntegerKeyDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);

    REQUIRE(writer.create(1, createStringData("first")));

    auto const result = writer.create(1, createStringData("duplicate"));
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::Conflict);
    REQUIRE(writer.create(2, createStringData("after conflict")));
    REQUIRE(wtxn.commit());

    auto const rtxn = beginReadTransaction(env);
    auto const reader = db.reader(rtxn);
    auto const optFirst = reader.get(1);
    auto const optSecond = reader.get(2);
    REQUIRE(optFirst);
    REQUIRE(optSecond);
    CHECK(utility::bytes::stringView(*optFirst) == "first");
    CHECK(utility::bytes::stringView(*optSecond) == "after conflict");
  }

  TEST_CASE("ReservationWriterAccess - duplicate create does not invoke encoder", "[lmdb][unit][database][writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

    auto wtxn = beginWriteTransaction(env);
    auto db = openIntegerKeyDatabase(wtxn, "test");
    auto writer = db.writer(wtxn);

    REQUIRE(writer.create(1, createStringData("initial")));

    std::int32_t duplicateEncoderCalls = 0;
    auto const result =
      ReservationAccess::create(writer, 1, 5, [&](std::span<std::byte>) noexcept { ++duplicateEncoderCalls; });
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::Conflict);
    CHECK(duplicateEncoderCalls == 0);

    std::int32_t successEncoderCalls = 0;
    auto const afterConflictRes = ReservationAccess::create(writer,
                                                            2,
                                                            6,
                                                            [&](std::span<std::byte> output) noexcept
                                                            {
                                                              ++successEncoderCalls;
                                                              std::memset(output.data(), 'a', output.size());
                                                            });
    REQUIRE(afterConflictRes);
    CHECK(successEncoderCalls == 1);
    REQUIRE(wtxn.commit());

    auto const rtxn = beginReadTransaction(env);
    auto const optData = db.reader(rtxn).get(2);
    REQUIRE(optData);
    CHECK(utility::bytes::stringView(*optData) == "aaaaaa");
  }

  TEST_CASE("IntegerKeyDatabase::Writer - non-conflict mutation failure unwinds and rolls back its transaction",
            "[lmdb][regression][database][writer]")
  {
    constexpr std::size_t kMapSize = std::size_t{64} * 1024;
    constexpr std::size_t kOversizedValue = kMapSize * 4;
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20, .pinnedMapBytes = kMapSize});

    auto setupTransaction = beginWriteTransaction(env);
    auto db = openIntegerKeyDatabase(setupTransaction, "test");
    auto setupWriter = db.writer(setupTransaction);
    REQUIRE(setupWriter.create(1, createStringData("persisted")));
    REQUIRE(setupTransaction.commit());

    {
      auto transaction = beginWriteTransaction(env);
      auto writer = db.writer(transaction);
      auto const oversized = createTestData(kOversizedValue);

      SECTION("create with copied data")
      {
        try
        {
          std::ignore = writer.create(3, oversized);
          FAIL("oversized create should throw");
        }
        catch (detail::TransactionFailure const& failure)
        {
          // A value larger than the whole map exhausts it, which is capacity
          // rather than plain IO.
          CHECK(failure.error().code == Error::Code::StorageFull);
        }
      }

      SECTION("create with reserved data")
      {
        std::int32_t encoderCalls = 0;

        try
        {
          std::ignore = ReservationAccess::create(
            writer, 3, oversized.size(), [&](std::span<std::byte>) noexcept { ++encoderCalls; });
          FAIL("oversized reserved create should throw");
        }
        catch (detail::TransactionFailure const& failure)
        {
          // A value larger than the whole map exhausts it, which is capacity
          // rather than plain IO.
          CHECK(failure.error().code == Error::Code::StorageFull);
        }

        CHECK(encoderCalls == 0);
      }

      SECTION("update with copied data")
      {
        try
        {
          std::ignore = writer.update(1, oversized);
          FAIL("oversized update should throw");
        }
        catch (detail::TransactionFailure const& failure)
        {
          // A value larger than the whole map exhausts it, which is capacity
          // rather than plain IO.
          CHECK(failure.error().code == Error::Code::StorageFull);
        }
      }

      SECTION("update with reserved data")
      {
        std::int32_t encoderCalls = 0;

        try
        {
          std::ignore = ReservationAccess::update(
            writer, 1, oversized.size(), [&](std::span<std::byte>) noexcept { ++encoderCalls; });
          FAIL("oversized reserved update should throw");
        }
        catch (detail::TransactionFailure const& failure)
        {
          // A value larger than the whole map exhausts it, which is capacity
          // rather than plain IO.
          CHECK(failure.error().code == Error::Code::StorageFull);
        }

        CHECK(encoderCalls == 0);
      }
    }

    auto const readTransaction = beginReadTransaction(env);
    auto const reader = db.reader(readTransaction);
    auto const optPersisted = reader.get(1);
    REQUIRE(optPersisted);
    CHECK(utility::bytes::stringView(*optPersisted) == "persisted");
    CHECK_FALSE(reader.get(2));
    CHECK_FALSE(reader.get(3));
  }

  TEST_CASE("IntegerKeyDatabase::Writer - move assignment releases a finished cursor without closing it",
            "[lmdb][unit][database][writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto env = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 20});

    auto firstTransaction = beginWriteTransaction(env);
    auto db = openIntegerKeyDatabase(firstTransaction, "test");
    auto writer = db.writer(firstTransaction);
    REQUIRE(writer.create(1, createStringData("first")));
    REQUIRE(firstTransaction.commit());

    auto secondTransaction = beginWriteTransaction(env);
    auto replacement = db.writer(secondTransaction);
    writer = std::move(replacement);
    REQUIRE(writer.create(2, createStringData("second")));
    REQUIRE(secondTransaction.commit());
  }
} // namespace ao::lmdb::test
