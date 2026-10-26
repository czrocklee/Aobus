// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/fatal/ProbeProcess.h"
#include "test/TestFixtureSupport.h"
#include "test/lmdb/LmdbProbeScratch.h"
#include "test/lmdb/LmdbTestSupport.h"
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace ao::lmdb::test
{
  namespace
  {
    constexpr auto kProbeTimeout = std::chrono::seconds{15};

    void checkStaleReaderProbe(std::string_view const family,
                               std::string_view const owner,
                               std::string_view const transfer,
                               std::string_view const operation)
    {
      auto const executablePath = ao::test::siblingProbeExecutablePath("ao_library_probe");
      REQUIRE_FALSE(executablePath.empty());
      auto const scratch = ao::test::LmdbProbeScratch{};
      auto scenario = std::string{"lmdb-binding-"};
      scenario.append(family).append("-").append(owner).append("-").append(transfer).append("-").append(operation);
      INFO("probe: " << scenario);
      scenario.append(":").append(scratch.path().filename().string());
      auto const result = ao::test::runProbeProcess(executablePath, scenario, kProbeTimeout);

      REQUIRE(result.started);
      CHECK(result.launchError.empty());
      CHECK_FALSE(result.timedOut);
      REQUIRE(result.hasFatalTermination());
      CHECK(result.hasPlatformAbort());
      CHECK(result.standardError.contains("AOBUS_FATAL"));
      CHECK(result.standardError.contains("category=expects"));
      bool const iteratorOperation = operation.ends_with("dereference") || operation.ends_with("advance");

      if (owner == "read" && transfer == "replacement" && iteratorOperation)
      {
        // The replacement ends the read-only transaction while the iterator's cursor is open.
        CHECK(result.standardError.contains("Read-only LMDB transaction ended while one of its cursors remains open"));
        CHECK(result.standardError.contains("Transaction.cpp:"));
        CHECK(result.standardError.contains("requireNoOpenCursors"));
      }
      else
      {
        auto context = std::string{family == "byte" ? "ByteKeyDatabase::Reader" : "IntegerKeyDatabase::Reader"};

        if (iteratorOperation)
        {
          context.append("::Iterator");
        }

        context.append(" used after its transaction finished or was replaced");
        CHECK(result.standardError.contains(context));
        CHECK(result.standardError.contains("Database.cpp:"));
        CHECK(result.standardError.contains("ensureActive"));
      }

      CHECK(result.standardOutput.empty());
    }
  } // namespace

  TEST_CASE("ReadTransaction - NOTLS handoff recreates retained DBI bindings on the receiving thread",
            "[lmdb][unit][transaction][concurrency]")
  {
    auto const temp = ao::test::TempDir{};
    auto environment = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 2});
    auto setup = beginWriteTransaction(environment);
    auto database = openIntegerKeyDatabase(setup, "records");
    REQUIRE(database.writer(setup).create(1, createStringData("value")));
    auto byteDatabase = openByteKeyDatabase(setup, "bytes");
    auto const byteKey = utility::bytes::view(std::string_view{"alpha"});
    REQUIRE(byteDatabase.writer(setup).create(byteKey, createStringData("byte value")));
    REQUIRE(setup.commit());
    auto transaction = beginReadTransaction(environment);
    auto oldReader = database.reader(transaction);
    auto oldIterator = oldReader.begin();
    REQUIRE(oldIterator != oldReader.end());
    auto oldByteReader = byteDatabase.reader(transaction);
    auto oldByteIterator = oldByteReader.lowerBound(byteKey);
    REQUIRE(oldByteIterator != oldByteReader.end());
    // No binding crosses the handoff; only the readonly transaction moves.
    oldIterator = {};
    oldByteIterator = {};
    bool received = false;
    auto worker =
      std::jthread{[&]
                   {
                     auto local = ReadTransaction{std::move(transaction)};
                     auto reader = database.reader(local);
                     auto const optValue = reader.get(1);
                     auto iterator = reader.begin();

                     if (!optValue || utility::bytes::stringView(*optValue) != "value" || reader.entryCount() != 1 ||
                         iterator == reader.end() || static_cast<std::uint32_t>(iterator->first) != 1 ||
                         utility::bytes::stringView(iterator->second) != "value")
                     {
                       return;
                     }

                     auto byteReader = byteDatabase.reader(local);
                     auto const optByteValue = byteReader.get(byteKey);
                     auto byteIterator = byteReader.lowerBound(byteKey);

                     if (!optByteValue || utility::bytes::stringView(*optByteValue) != "byte value" ||
                         byteReader.entryCount() != 1 || byteIterator == byteReader.end() ||
                         utility::bytes::stringView(byteIterator->first) != "alpha" ||
                         utility::bytes::stringView(byteIterator->second) != "byte value")
                     {
                       return;
                     }

                     ++iterator;
                     ++byteIterator;
                     received = iterator == reader.end() && byteIterator == byteReader.end();
                   }};
    worker.join();
    CHECK(received);
    CHECK_FALSE(transaction.isActive());
    auto fresh = beginReadTransaction(environment);
    CHECK(database.reader(fresh).entryCount() == 1);
    CHECK(byteDatabase.reader(fresh).entryCount() == 1);
  }

  TEST_CASE("Transaction-bound readers - every operation of both key families rejects a stale binding",
            "[lmdb][unit][transaction]")
  {
    auto const family = GENERATE(std::string_view{"integer"}, std::string_view{"byte"});
    auto const owner = GENERATE(std::string_view{"read"}, std::string_view{"write"});
    auto const operation = family == "byte" ? GENERATE(std::string_view{"get"},
                                                       std::string_view{"count"},
                                                       std::string_view{"begin"},
                                                       std::string_view{"lower-bound"},
                                                       std::string_view{"dereference"},
                                                       std::string_view{"advance"},
                                                       std::string_view{"seek-dereference"},
                                                       std::string_view{"seek-advance"})
                                            : GENERATE(std::string_view{"get"},
                                                       std::string_view{"count"},
                                                       std::string_view{"begin"},
                                                       std::string_view{"dereference"},
                                                       std::string_view{"advance"});
    // A round trip restores the owner address and native handle, so only the
    // binding generation can reject the operation.
    checkStaleReaderProbe(family, owner, "round-trip", operation);
  }

  TEST_CASE("Transaction-bound readers - every complete owner transfer invalidates readers and iterators",
            "[lmdb][unit][transaction]")
  {
    auto const owner = GENERATE(std::string_view{"read"}, std::string_view{"write"});
    // Write-backed readers point to a base subobject, which cannot be transparently replaced.
    auto const transfer = owner == "read" ? GENERATE(std::string_view{"construction"},
                                                     std::string_view{"replacement"},
                                                     std::string_view{"move-out-rebind"},
                                                     std::string_view{"round-trip"},
                                                     std::string_view{"rebuild-in-place"})
                                          : GENERATE(std::string_view{"construction"},
                                                     std::string_view{"replacement"},
                                                     std::string_view{"move-out-rebind"},
                                                     std::string_view{"round-trip"});
    auto const operation = GENERATE(std::string_view{"get"}, std::string_view{"advance"});
    // Invalidation belongs to the owner, so one key family covers every transfer.
    checkStaleReaderProbe("integer", owner, transfer, operation);
  }

  TEST_CASE("ReadTransaction - public base moves reject writer sources and destinations even after finish",
            "[lmdb][unit][transaction]")
  {
    static constexpr auto kScenarios = std::array<std::string_view, 16>{
      "construct-active",
      "construct-finished",
      "construct-committed",
      "construct-moved-from",
      "source-active",
      "source-finished",
      "source-committed",
      "source-moved-from",
      "destination-active",
      "destination-finished",
      "destination-committed",
      "destination-moved-from",
      "self-active",
      "self-finished",
      "self-committed",
      "self-moved-from",
    };
    auto const executablePath = ao::test::siblingProbeExecutablePath("ao_library_probe");
    REQUIRE_FALSE(executablePath.empty());

    for (auto const suffix : kScenarios)
    {
      INFO("base transfer: " << suffix);
      auto const scratch = ao::test::LmdbProbeScratch{};
      auto scenario = std::string{"lmdb-writer-base-transfer-"};
      scenario.append(suffix).append(":").append(scratch.path().filename().string());
      auto const result = ao::test::runProbeProcess(executablePath, scenario, kProbeTimeout);
      REQUIRE(result.started);
      CHECK(result.launchError.empty());
      CHECK_FALSE(result.timedOut);
      REQUIRE(result.hasFatalTermination());
      CHECK(result.hasPlatformAbort());
      CHECK(result.standardError.contains("category=expects"));
      CHECK(result.standardError.contains("Cannot transfer writer ownership through ReadTransaction"));
      CHECK(result.standardError.contains("Transaction.cpp:"));
      CHECK(result.standardOutput.empty());
    }
  }
} // namespace ao::lmdb::test
