// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/fatal/ProbeProcess.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/lmdb/LmdbTestSupport.h"
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
  }

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

  TEST_CASE("ReadTransaction-bound readers - replaced-owner use aborts through the fatal facility",
            "[lmdb][unit][transaction]")
  {
    struct ReplacementScenario final
    {
      std::string_view scenario;
      std::string_view context;
    };

    static constexpr auto kScenarios = std::array<ReplacementScenario, 4>{
      ReplacementScenario{"lmdb-reader-after-transaction-replacement",
                          "IntegerKeyDatabase::Reader used after its transaction finished or was replaced"},
      ReplacementScenario{"lmdb-iterator-after-transaction-replacement",
                          "IntegerKeyDatabase::Reader::Iterator used after its transaction finished or was replaced"},
      ReplacementScenario{"lmdb-reader-after-transaction-move-round-trip",
                          "IntegerKeyDatabase::Reader used after its transaction finished or was replaced"},
      ReplacementScenario{"lmdb-iterator-after-transaction-move-round-trip",
                          "IntegerKeyDatabase::Reader::Iterator used after its transaction finished or was replaced"},
    };
    auto const executablePath = ao::test::siblingProbeExecutablePath("ao_library_probe");
    REQUIRE_FALSE(executablePath.empty());

    for (auto const& scenario : kScenarios)
    {
      INFO("probe: " << scenario.scenario);
      auto const scratch = ao::test::TempDir{};
      auto scenarioArgument = std::string{scenario.scenario};
      scenarioArgument.append(":").append(scratch.path().filename().string());
      auto const result = ao::test::runProbeProcess(executablePath, scenarioArgument, kProbeTimeout);

      REQUIRE(result.started);
      CHECK(result.launchError.empty());
      CHECK_FALSE(result.timedOut);
      REQUIRE(result.hasFatalTermination());
      CHECK(result.hasPlatformAbort());
      CHECK(result.standardError.contains("AOBUS_FATAL"));
      CHECK(result.standardError.contains("category=expects"));
      CHECK(result.standardError.contains("condition=_owner != nullptr && _owner->isActive() && "
                                          "detail::DatabaseAccess::bindingGeneration(*_owner) == _bindingGeneration && "
                                          "detail::DatabaseAccess::handle(*_owner) == _txn"));
      CHECK(result.standardError.contains(std::string{scenario.context}));
      CHECK(result.standardError.contains("Database.cpp:"));
      CHECK(result.standardError.contains("ensureActive"));
      CHECK(result.standardOutput.empty());
    }
  }

  TEST_CASE("Transaction-bound readers - both key families reject stale bindings after complete owner moves",
            "[lmdb][unit][transaction]")
  {
    auto const family = GENERATE(std::string_view{"integer"}, std::string_view{"byte"});
    auto const owner = GENERATE(std::string_view{"read"}, std::string_view{"write"});
    auto const transfer = GENERATE(std::string_view{"construction"},
                                   std::string_view{"replacement"},
                                   std::string_view{"move-out-rebind"},
                                   std::string_view{"round-trip"});
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
    auto const executablePath = ao::test::siblingProbeExecutablePath("ao_library_probe");
    REQUIRE_FALSE(executablePath.empty());
    auto const scratch = ao::test::TempDir{};
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
    CHECK(result.standardError.contains("condition=_owner != nullptr && _owner->isActive() && "
                                        "detail::DatabaseAccess::bindingGeneration(*_owner) == _bindingGeneration && "
                                        "detail::DatabaseAccess::handle(*_owner) == _txn"));
    auto context = std::string{family == "byte" ? "ByteKeyDatabase::Reader" : "IntegerKeyDatabase::Reader"};

    if (operation.ends_with("dereference") || operation.ends_with("advance"))
    {
      context.append("::Iterator");
    }

    context.append(" used after its transaction finished or was replaced");
    CHECK(result.standardError.contains(context));
    CHECK(result.standardError.contains("Database.cpp:"));
    CHECK(result.standardError.contains("ensureActive"));
    CHECK(result.standardOutput.empty());
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
      auto const scratch = ao::test::TempDir{};
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
