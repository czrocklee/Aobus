// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/lmdb/detail/ReadFaultInjection.h"
#include "lib/lmdb/detail/TransactionFailure.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/lmdb/LmdbTestSupport.h"
#include <ao/Error.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <lmdb.h>

#include <cstdint>
#include <optional>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ao::lmdb::test
{
  namespace
  {
    template<typename Database>
    struct BindingRecordsFixture final
    {
      ao::test::TempDir temp;
      Environment environment;
      Database database;

      explicit BindingRecordsFixture(std::string_view const firstValue = "original")
        : environment{openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 2})}
        , database{seed(environment, firstValue)}
      {
      }

      static auto key(std::uint32_t const id)
      {
        if constexpr (std::is_same_v<Database, IntegerKeyDatabase>)
        {
          return id;
        }
        else
        {
          return utility::bytes::view(id == 1 ? std::string_view{"alpha"} : std::string_view{"omega"});
        }
      }

      static Database seed(Environment& environment, std::string_view const firstValue)
      {
        auto transaction = beginWriteTransaction(environment);
        auto databaseRes = Database::open(transaction, "records");
        REQUIRE(databaseRes);
        auto writer = databaseRes->writer(transaction);
        REQUIRE(writer.create(key(1), createStringData(firstValue)));
        REQUIRE(writer.create(key(2), createStringData("second")));
        REQUIRE(transaction.commit());
        return *databaseRes;
      }
    };

    struct RecordsFixture final
    {
      ao::test::TempDir temp;
      Environment environment;
      IntegerKeyDatabase database;

      RecordsFixture()
        : environment{openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 3})}
        , database{seed(environment)}
      {
      }

      static IntegerKeyDatabase seed(Environment& environment)
      {
        auto transaction = beginWriteTransaction(environment);
        auto database = openIntegerKeyDatabase(transaction, "records");
        auto writer = database.writer(transaction);
        REQUIRE(writer.create(256, createStringData("two hundred fifty-six")));
        REQUIRE(writer.create(2, createStringData("two")));
        REQUIRE(writer.create(9, createStringData("")));
        REQUIRE(transaction.commit());
        return database;
      }
    };
  } // namespace

  TEST_CASE("IntegerKeyDatabase - retained DBI readonly reads preserve integer order and empty versus missing values",
            "[lmdb][unit][database]")
  {
    auto fixture = RecordsFixture{};
    auto transaction = beginReadTransaction(fixture.environment);
    auto reader = fixture.database.reader(transaction);
    auto const optValue = reader.get(256);
    REQUIRE(optValue);
    CHECK(utility::bytes::stringView(*optValue) == "two hundred fifty-six");
    auto const optEmpty = reader.get(9);
    REQUIRE(optEmpty);
    CHECK(optEmpty->empty());
    CHECK_FALSE(reader.get(10));
    CHECK(reader.entryCount() == 3);
    CHECK(reader.maxKey() == 256);
    auto iterator = reader.begin();
    REQUIRE(iterator != reader.end());
    CHECK(static_cast<std::uint32_t>(iterator->first) == 2);
    CHECK(utility::bytes::stringView(iterator->second) == "two");
    ++iterator;
    REQUIRE(iterator != reader.end());
    CHECK(static_cast<std::uint32_t>(iterator->first) == 9);
    CHECK(iterator->second.empty());
    ++iterator;
    REQUIRE(iterator != reader.end());
    CHECK(static_cast<std::uint32_t>(iterator->first) == 256);
    CHECK(utility::bytes::stringView(iterator->second) == "two hundred fifty-six");
    ++iterator;
    CHECK(iterator == reader.end());
  }

  TEST_CASE("IntegerKeyDatabase - write-backed retained readers see staged rows and survive commit for destruction",
            "[lmdb][unit][database]")
  {
    auto fixture = RecordsFixture{};

    {
      auto transaction = beginWriteTransaction(fixture.environment);
      auto writer = fixture.database.writer(transaction);
      REQUIRE(writer.update(2, createStringData("staged")));
      REQUIRE(writer.create(10, createStringData("new row")));
      auto const& readBase = static_cast<ReadTransaction const&>(transaction);
      auto reader = fixture.database.reader(readBase);
      auto const optValue = reader.get(2);
      REQUIRE(optValue);
      CHECK(utility::bytes::stringView(*optValue) == "staged");
      CHECK(reader.entryCount() == 4);
      CHECK(reader.maxKey() == 256);
      auto iterator = reader.begin();
      REQUIRE(iterator != reader.end());
      CHECK(static_cast<std::uint32_t>(iterator->first) == 2);
      CHECK(utility::bytes::stringView(iterator->second) == "staged");
      ++iterator;
      REQUIRE(iterator != reader.end());
      CHECK(static_cast<std::uint32_t>(iterator->first) == 9);
      CHECK(iterator->second.empty());
      ++iterator;
      REQUIRE(iterator != reader.end());
      CHECK(static_cast<std::uint32_t>(iterator->first) == 10);
      CHECK(utility::bytes::stringView(iterator->second) == "new row");
      ++iterator;
      REQUIRE(iterator != reader.end());
      CHECK(static_cast<std::uint32_t>(iterator->first) == 256);
      ++iterator;
      CHECK(iterator == reader.end());
      // Keep a positioned write-owned cursor alive across native commit.
      iterator = reader.begin();
      REQUIRE(transaction.commit());
      CHECK(transaction.isFinished());
    }

    auto transaction = beginReadTransaction(fixture.environment);
    auto reader = fixture.database.reader(transaction);
    auto const optUpdated = reader.get(2);
    auto const optCreated = reader.get(10);
    REQUIRE(optUpdated);
    REQUIRE(optCreated);
    CHECK(utility::bytes::stringView(*optUpdated) == "staged");
    CHECK(utility::bytes::stringView(*optCreated) == "new row");
    CHECK(reader.entryCount() == 4);
  }

  TEST_CASE("IntegerKeyDatabase - existing write open DBI capacity faults unwind the owner and roll back staged reads",
            "[lmdb][unit][database]")
  {
    auto const temp = ao::test::TempDir{};

    {
      auto environment = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 3});
      std::ignore = RecordsFixture::seed(environment);
      auto transaction = beginWriteTransaction(environment);
      REQUIRE(IntegerKeyDatabase::open(transaction, "other"));
      REQUIRE(transaction.commit());
    }

    auto optFailure = std::optional<Error>{};

    {
      // Both named databases exist, but only one DBI can be opened here.
      auto environment = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 1});

      try
      {
        auto transaction = beginWriteTransaction(environment);
        auto databaseRes = IntegerKeyDatabase::openExisting(transaction, "records");
        REQUIRE(databaseRes);
        auto writer = databaseRes->writer(transaction);
        REQUIRE(writer.update(2, createStringData("must roll back")));
        REQUIRE(writer.create(10, createStringData("must disappear")));
        auto const& readBase = static_cast<ReadTransaction const&>(transaction);
        auto reader = databaseRes->reader(readBase);
        auto const optStaged = reader.get(2);
        REQUIRE(optStaged);
        CHECK(utility::bytes::stringView(*optStaged) == "must roll back");
        auto iterator = reader.begin();
        REQUIRE(iterator != reader.end());
        std::ignore = IntegerKeyDatabase::openExisting(transaction, "other");
        FAIL("capacity failure must throw TransactionFailure, not return a miss");
      }
      catch (detail::TransactionFailure const& failure)
      {
        optFailure = failure.error();
      }
    }

    REQUIRE(optFailure);
    CHECK(optFailure->code == Error::Code::IoError);
    CHECK(optFailure->message.starts_with("mdb_dbi_open: MDB_DBS_FULL"));
    CHECK_FALSE(optFailure->message.contains("does not exist"));
    auto const optValue = readExistingIntegerKeyRecord(temp.path(), "records", 2);
    REQUIRE(optValue);
    CHECK(utility::bytes::stringView(*optValue) == "two");
    CHECK_FALSE(readExistingIntegerKeyRecord(temp.path(), "records", 10));
    auto environment = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 3});
    auto setup = beginWriteTransaction(environment);
    auto recordsRes = IntegerKeyDatabase::openExisting(setup, "records");
    auto otherRes = IntegerKeyDatabase::openExisting(setup, "other");
    REQUIRE(recordsRes);
    REQUIRE(otherRes);
    REQUIRE(setup.commit());
    auto transaction = beginReadTransaction(environment);
    CHECK(recordsRes->reader(transaction).entryCount() == 3);
    CHECK(recordsRes->reader(transaction).maxKey() == 256);
    CHECK(otherRes->reader(transaction).entryCount() == 0);
    CHECK(otherRes->reader(transaction).maxKey() == 0);
    CHECK(otherRes->reader(transaction).begin() == otherRes->reader(transaction).end());
  }

  TEST_CASE("IntegerKeyDatabase - retained writer-base read faults preserve root rollback and cursor ownership",
            "[lmdb][unit][database]")
  {
    auto fixture = RecordsFixture{};
    auto optFailure = std::optional<Error>{};
    auto optReader = std::optional<IntegerKeyDatabase::Reader>{};
    auto iterator = IntegerKeyDatabase::Reader::Iterator{};
    enum class Operation : std::uint8_t
    {
      PointRead,
      EntryCount,
      MaximumKey,
      CursorConstruction,
      CursorAdvance,
    };
    auto const operation = GENERATE(Operation::PointRead,
                                    Operation::EntryCount,
                                    Operation::MaximumKey,
                                    Operation::CursorConstruction,
                                    Operation::CursorAdvance);

    try
    {
      auto transaction = beginWriteTransaction(fixture.environment);
      REQUIRE(fixture.database.writer(transaction).update(2, createStringData("must roll back")));
      auto const& base = static_cast<ReadTransaction const&>(transaction);
      optReader.emplace(fixture.database.reader(base));
      iterator = optReader->begin();
      auto injection = detail::ReadFaultInjection{MDB_PANIC};

      switch (operation)
      {
        case Operation::PointRead: std::ignore = optReader->get(2); break;
        case Operation::EntryCount: std::ignore = optReader->entryCount(); break;
        case Operation::MaximumKey: std::ignore = optReader->maxKey(); break;
        case Operation::CursorConstruction: std::ignore = optReader->begin(); break;
        case Operation::CursorAdvance: ++iterator; break;
      }

      FAIL("a write-owned read fault must unwind the root owner");
    }
    catch (detail::TransactionFailure const& failure)
    {
      optFailure = failure.error();
    }

    REQUIRE(optFailure);
    CHECK(optFailure->code == Error::Code::IoError);
    CHECK(optFailure->message.contains("MDB_PANIC"));
    // Wrappers outlive the aborted native writer and its C++ owner; destruction
    // must not inspect that owner or double-close its native cursor.
    iterator = {};
    optReader.reset();
    auto transaction = beginReadTransaction(fixture.environment);
    auto const optValue = fixture.database.reader(transaction).get(2);
    REQUIRE(optValue);
    CHECK(utility::bytes::stringView(*optValue) == "two");
    CHECK(fixture.database.reader(transaction).entryCount() == 3);
  }

  TEST_CASE("IntegerKeyDatabase - moved retained readers and iterators stay usable within their live transaction",
            "[lmdb][unit][database]")
  {
    auto fixture = RecordsFixture{};
    auto transaction = beginReadTransaction(fixture.environment);
    auto reader = fixture.database.reader(transaction);
    auto movedReader = IntegerKeyDatabase::Reader{std::move(reader)};
    auto const optValue = movedReader.get(2);
    REQUIRE(optValue);
    CHECK(utility::bytes::stringView(*optValue) == "two");
    auto assignedReader = fixture.database.reader(transaction);
    assignedReader = std::move(movedReader);
    CHECK(assignedReader.entryCount() == 3);
    auto originalIterator = assignedReader.begin();
    auto movedIterator = IntegerKeyDatabase::Reader::Iterator{std::move(originalIterator)};
    REQUIRE(movedIterator != assignedReader.end());
    CHECK(static_cast<std::uint32_t>(movedIterator->first) == 2);
    CHECK(utility::bytes::stringView(movedIterator->second) == "two");
    auto assignedIterator = IntegerKeyDatabase::Reader::Iterator{};
    assignedIterator = std::move(movedIterator);
    auto* const sameIterator = &assignedIterator;
    assignedIterator = std::move(*sameIterator);
    ++assignedIterator;
    REQUIRE(assignedIterator != assignedReader.end());
    CHECK(static_cast<std::uint32_t>(assignedIterator->first) == 9);
    CHECK(assignedIterator->second.empty());
  }

  TEST_CASE("ReadTransaction - retained bindings recreate after owner moves and survive self-move",
            "[lmdb][unit][transaction]")
  {
    auto fixture = RecordsFixture{};
    auto transaction = beginReadTransaction(fixture.environment);
    auto reader = fixture.database.reader(transaction);
    auto iterator = reader.begin();
    REQUIRE(iterator != reader.end());

    SECTION("move construction and round trip require fresh bindings")
    {
      auto moved = ReadTransaction{std::move(transaction)};
      // NOLINTNEXTLINE(bugprone-use-after-move)
      CHECK_FALSE(transaction.isActive());
      CHECK(fixture.database.reader(moved).entryCount() == 3);
      transaction = std::move(moved);
      iterator = {};
    }

    SECTION("move assignment ends the replaced snapshot")
    {
      auto source = beginReadTransaction(fixture.environment);
      transaction = std::move(source);
      // NOLINTNEXTLINE(bugprone-use-after-move)
      CHECK_FALSE(source.isActive());
      iterator = {};
    }

    SECTION("self-move preserves current bindings")
    {
      auto* const sameOwner = &transaction;
      transaction = std::move(*sameOwner);
      CHECK(reader.entryCount() == 3);
      CHECK(static_cast<std::uint32_t>(iterator->first) == 2);
      ++iterator;
      REQUIRE(iterator != reader.end());
      CHECK(static_cast<std::uint32_t>(iterator->first) == 9);
    }

    auto freshReader = fixture.database.reader(transaction);
    auto const optValue = freshReader.get(2);
    REQUIRE(optValue);
    CHECK(utility::bytes::stringView(*optValue) == "two");
    CHECK(freshReader.entryCount() == 3);
    auto freshIterator = freshReader.begin();
    CHECK(static_cast<std::uint32_t>(freshIterator->first) == 2);
  }

  TEMPLATE_TEST_CASE("Transaction bindings - complete moves require fresh readers and self-move preserves borrowers",
                     "[lmdb][unit][transaction]",
                     (std::pair<IntegerKeyDatabase, ReadTransaction>),
                     (std::pair<ByteKeyDatabase, ReadTransaction>),
                     (std::pair<IntegerKeyDatabase, WriteTransaction>),
                     (std::pair<ByteKeyDatabase, WriteTransaction>))
  {
    using Database = TestType::first_type;
    using Transaction = TestType::second_type;
    using Fixture = BindingRecordsFixture<Database>;
    auto fixture = Fixture{};
    auto replacementFixture = Fixture{"replacement"};
    auto const transfer = GENERATE(std::string_view{"construction"},
                                   std::string_view{"replacement"},
                                   std::string_view{"move-out-rebind"},
                                   std::string_view{"round-trip"},
                                   std::string_view{"self"});
    auto optReader = std::optional<typename Database::Reader>{};
    auto iterator = typename Database::Reader::Iterator{};
    auto seekIterator = typename Database::Reader::Iterator{};

    {
      auto transactionRes = Transaction::begin(fixture.environment);
      REQUIRE(transactionRes);
      auto& transaction = *transactionRes;
      auto optMoved = std::optional<Transaction>{};
      auto expected = std::string_view{"original"};

      if constexpr (std::is_same_v<Transaction, WriteTransaction>)
      {
        REQUIRE(fixture.database.writer(transaction).update(Fixture::key(1), createStringData("staged")));
        expected = "staged";
      }

      optReader.emplace(fixture.database.reader(static_cast<ReadTransaction const&>(transaction)));
      iterator = optReader->begin();
      REQUIRE(iterator != optReader->end());
      CHECK(utility::bytes::stringView(iterator->second) == expected);

      if constexpr (std::is_same_v<Database, ByteKeyDatabase>)
      {
        seekIterator = optReader->lowerBound(Fixture::key(1));
        REQUIRE(seekIterator != optReader->end());
        CHECK(utility::bytes::stringView(seekIterator->first) == "alpha");
      }

      auto* activeOwner = &transaction;
      auto* activeDatabase = &fixture.database;

      if (transfer == "construction" || transfer == "round-trip" || transfer == "move-out-rebind")
      {
        optMoved.emplace(std::move(transaction));
        // NOLINTNEXTLINE(bugprone-use-after-move)
        CHECK_FALSE(transaction.isActive());
        auto const movedReader = fixture.database.reader(*optMoved);
        auto const optValue = movedReader.get(Fixture::key(1));
        REQUIRE(optValue);
        CHECK(utility::bytes::stringView(*optValue) == expected);
        activeOwner = &*optMoved;
      }

      if (transfer == "round-trip")
      {
        transaction = std::move(*optMoved);
        activeOwner = &transaction;
      }
      else if (transfer == "replacement" || transfer == "move-out-rebind")
      {
        // Retained DBIs acquire no admission. The replacement native writer is
        // in a different environment, so neither native writer waits on itself.
        auto replacementRes = Transaction::begin(replacementFixture.environment);
        REQUIRE(replacementRes);
        transaction = std::move(*replacementRes);
        activeOwner = &transaction;
        activeDatabase = &replacementFixture.database;
        expected = "replacement";
      }
      else if (transfer == "self")
      {
        auto* const sameOwner = &transaction;
        transaction = std::move(*sameOwner);
        CHECK(optReader->entryCount() == 2);
        CHECK(utility::bytes::stringView((*iterator).second) == expected);
        ++iterator;
        REQUIRE(iterator != optReader->end());
        CHECK(utility::bytes::stringView(iterator->second) == "second");

        if constexpr (std::is_same_v<Database, ByteKeyDatabase>)
        {
          CHECK(utility::bytes::stringView(seekIterator->second) == expected);
          ++seekIterator;
          REQUIRE(seekIterator != optReader->end());
          CHECK(utility::bytes::stringView(seekIterator->first) == "omega");
        }
      }

      // Fresh bindings also preserve captured identity through wrapper moves.
      auto freshReader = activeDatabase->reader(*activeOwner);
      auto movedReader = typename Database::Reader{std::move(freshReader)};
      freshReader = std::move(movedReader);
      auto* const sameReader = &freshReader;
      freshReader = std::move(*sameReader);
      auto const optValue = freshReader.get(Fixture::key(1));
      REQUIRE(optValue);
      CHECK(utility::bytes::stringView(*optValue) == expected);
      CHECK(freshReader.entryCount() == 2);
      auto freshIterator = freshReader.begin();
      REQUIRE(freshIterator != freshReader.end());

      if constexpr (std::is_same_v<Database, ByteKeyDatabase>)
      {
        CHECK(utility::bytes::stringView(freshIterator->first) == "alpha");
        freshIterator = freshReader.lowerBound(Fixture::key(1));
      }
      else
      {
        CHECK(freshReader.maxKey() == 2);
        CHECK(static_cast<std::uint32_t>(freshIterator->first) == 1);
      }

      auto movedIterator = typename Database::Reader::Iterator{std::move(freshIterator)};
      freshIterator = std::move(movedIterator);
      auto* const sameIterator = &freshIterator;
      freshIterator = std::move(*sameIterator);
      REQUIRE(freshIterator != freshReader.end());
      CHECK(utility::bytes::stringView(freshIterator->second) == expected);
      ++freshIterator;
      REQUIRE(freshIterator != freshReader.end());
      CHECK(utility::bytes::stringView(freshIterator->second) == "second");
      ++freshIterator;
      CHECK(freshIterator == freshReader.end());

      if constexpr (std::is_same_v<Transaction, WriteTransaction>)
      {
        // Keep a fresh write-owned cursor positioned across native commit too.
        freshIterator = freshReader.begin();
        REQUIRE(activeOwner->commit());
      }
    }

    // Old borrowers outlive both the original C++ owner and any moved native
    // owner. Cleanup must use the captured cursor policy, not a dead owner.
    iterator = {};
    seekIterator = {};
    optReader.reset();
    auto verification = beginReadTransaction(fixture.environment);
    auto const optValue = fixture.database.reader(verification).get(Fixture::key(1));
    REQUIRE(optValue);
    bool const committedOriginal =
      std::is_same_v<Transaction, WriteTransaction> && transfer != "replacement" && transfer != "move-out-rebind";
    CHECK(utility::bytes::stringView(*optValue) == (committedOriginal ? "staged" : "original"));
    auto replacementVerification = beginReadTransaction(replacementFixture.environment);
    auto const optReplacement = replacementFixture.database.reader(replacementVerification).get(Fixture::key(1));
    REQUIRE(optReplacement);
    CHECK(utility::bytes::stringView(*optReplacement) == "replacement");
  }

  TEST_CASE("ByteKeyDatabase - retained writer-base faults preserve rollback after reader and seek iterator moves",
            "[lmdb][unit][database]")
  {
    using Fixture = BindingRecordsFixture<ByteKeyDatabase>;
    auto fixture = Fixture{};
    auto optFailure = std::optional<Error>{};
    auto optReader = std::optional<ByteKeyDatabase::Reader>{};
    auto iterator = ByteKeyDatabase::Reader::Iterator{};
    auto const operation = GENERATE(std::string_view{"get"},
                                    std::string_view{"count"},
                                    std::string_view{"begin"},
                                    std::string_view{"lower-bound"},
                                    std::string_view{"advance"});

    try
    {
      auto transaction = beginWriteTransaction(fixture.environment);
      REQUIRE(fixture.database.writer(transaction).update(Fixture::key(1), createStringData("must roll back")));
      auto moved = WriteTransaction{std::move(transaction)};
      optReader.emplace(fixture.database.reader(static_cast<ReadTransaction const&>(moved)));
      iterator = optReader->lowerBound(Fixture::key(1));
      auto movedIterator = ByteKeyDatabase::Reader::Iterator{std::move(iterator)};
      iterator = std::move(movedIterator);
      auto injection = detail::ReadFaultInjection{MDB_PANIC};

      if (operation == "get")
      {
        std::ignore = optReader->get(Fixture::key(1));
      }
      else if (operation == "count")
      {
        std::ignore = optReader->entryCount();
      }
      else if (operation == "begin")
      {
        std::ignore = optReader->begin();
      }
      else if (operation == "lower-bound")
      {
        std::ignore = optReader->lowerBound(Fixture::key(1));
      }
      else
      {
        ++iterator;
      }

      FAIL("a write-owned read fault must unwind the complete moved writer");
    }
    catch (detail::TransactionFailure const& failure)
    {
      optFailure = failure.error();
    }

    REQUIRE(optFailure);
    CHECK(optFailure->code == Error::Code::IoError);
    CHECK(optFailure->message.contains("MDB_PANIC"));
    iterator = {};
    optReader.reset();
    auto transaction = beginReadTransaction(fixture.environment);
    auto const optValue = fixture.database.reader(transaction).get(Fixture::key(1));
    REQUIRE(optValue);
    CHECK(utility::bytes::stringView(*optValue) == "original");
    CHECK(fixture.database.reader(transaction).entryCount() == 2);
  }

  TEST_CASE("IntegerKeyDatabase - retained reader and cursor destruction is safe after readonly owner ends",
            "[lmdb][unit][database]")
  {
    auto fixture = RecordsFixture{};
    auto optReader = std::optional<IntegerKeyDatabase::Reader>{};
    auto iterator = IntegerKeyDatabase::Reader::Iterator{};

    {
      auto transaction = beginReadTransaction(fixture.environment);
      optReader.emplace(fixture.database.reader(transaction));
      iterator = optReader->begin();
      REQUIRE(iterator != optReader->end());
      CHECK(utility::bytes::stringView(iterator->second) == "two");
    }

    iterator = {};
    optReader.reset();
    auto transaction = beginReadTransaction(fixture.environment);
    CHECK(fixture.database.reader(transaction).entryCount() == 3);
  }
} // namespace ao::lmdb::test
