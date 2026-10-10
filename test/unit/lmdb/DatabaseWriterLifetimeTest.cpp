// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/fatal/ProbeProcess.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/lmdb/LmdbTestSupport.h"
#include <ao/Error.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ao::lmdb::test
{
  namespace
  {
    template<typename Database>
    Database openLifetimeDatabase(WriteTransaction& transaction)
    {
      auto databaseRes = Database::open(transaction, "records");
      REQUIRE(databaseRes);
      return *databaseRes;
    }

    template<typename Database, typename Key>
    void checkWrapperMoves(Key const key, Key const extraKey)
    {
      auto const temp = ao::test::TempDir{};
      auto environment = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 2});
      auto owner = beginWriteTransaction(environment);
      auto database = openLifetimeDatabase<Database>(owner);
      REQUIRE(database.writer(owner).create(key, createStringData("baseline")));
      REQUIRE(owner.commit());
      owner = beginWriteTransaction(environment);
      auto source = database.writer(owner);
      REQUIRE(source.update(key, createStringData("before wrapper move")));
      auto moved = typename Database::Writer{std::move(source)};
      auto destination = database.writer(owner);

      if constexpr (std::is_same_v<Database, IntegerKeyDatabase>)
      {
        CHECK(moved.maxKey() == 1);
        auto const appendRes = moved.append(createStringData("appended before assignment"));
        REQUIRE(appendRes);
        CHECK(*appendRes == 2);
        CHECK(moved.maxKey() == 2);
        CHECK(destination.maxKey() == 1);
      }

      destination = std::move(moved);
      auto* const sameWriter = &destination;
      destination = std::move(*sameWriter);

      if constexpr (std::is_same_v<Database, IntegerKeyDatabase>)
      {
        CHECK(destination.maxKey() == 2);
        auto const appendRes = destination.append(createStringData("appended after moves"));
        REQUIRE(appendRes);
        CHECK(*appendRes == 3);
        CHECK(destination.maxKey() == 3);
      }

      REQUIRE(destination.update(key, createStringData("after wrapper moves")));
      REQUIRE(destination.create(extraKey, createStringData("extra")));
      auto const conflictRes = destination.create(extraKey, createStringData("must not replace"));
      REQUIRE_FALSE(conflictRes);
      CHECK(conflictRes.error().code == Error::Code::Conflict);
      auto const optExtra = destination.get(extraKey);
      REQUIRE(optExtra);
      CHECK(utility::bytes::stringView(*optExtra) == "extra");
      REQUIRE(destination.tryDelete(extraKey));
      CHECK_FALSE(destination.tryDelete(extraKey));
      REQUIRE(owner.commit());

      {
        auto const snapshot = beginReadTransaction(environment);
        auto reader = database.reader(snapshot);
        auto const optValue = reader.get(key);
        REQUIRE(optValue);
        CHECK(utility::bytes::stringView(*optValue) == "after wrapper moves");
        CHECK_FALSE(reader.get(extraKey));

        if constexpr (std::is_same_v<Database, IntegerKeyDatabase>)
        {
          auto const optBeforeAssignment = reader.get(2);
          auto const optAfterMoves = reader.get(3);
          REQUIRE(optBeforeAssignment);
          REQUIRE(optAfterMoves);
          CHECK(utility::bytes::stringView(*optBeforeAssignment) == "appended before assignment");
          CHECK(utility::bytes::stringView(*optAfterMoves) == "appended after moves");
        }
      }

      // Replacing a finished wrapper transfers a fresh identity and leaves the
      // old, transaction-owned cursor alone, including after owner rebind.
      owner = beginWriteTransaction(environment);
      auto fresh = database.writer(owner);
      destination = std::move(fresh);
      destination = std::move(*sameWriter);
      REQUIRE(destination.clear());

      if constexpr (std::is_same_v<Database, IntegerKeyDatabase>)
      {
        CHECK(destination.maxKey() == 0);
        auto const appendRes = destination.append(createStringData("fresh binding"));
        REQUIRE(appendRes);
        CHECK(*appendRes == 1);
      }
      else
      {
        REQUIRE(destination.create(key, createStringData("fresh binding")));
      }

      REQUIRE(owner.commit());
      auto const snapshot = beginReadTransaction(environment);
      auto reader = database.reader(snapshot);
      auto const optValue = reader.get(key);
      REQUIRE(optValue);
      CHECK(utility::bytes::stringView(*optValue) == "fresh binding");
      CHECK(reader.entryCount() == 1);
    }

    template<typename Database, typename Key>
    void checkFreshOwnerBinding(Key const key, std::string_view const transfer)
    {
      auto const firstTemp = ao::test::TempDir{};
      auto const secondTemp = ao::test::TempDir{};
      auto firstEnvironment = openEnvironment(firstTemp.path(), {.flags = kEnvNoTls, .maxDatabases = 2});
      auto secondEnvironment = openEnvironment(secondTemp.path(), {.flags = kEnvNoTls, .maxDatabases = 2});
      auto setup = beginWriteTransaction(firstEnvironment);
      auto firstDatabase = openLifetimeDatabase<Database>(setup);
      REQUIRE(firstDatabase.writer(setup).create(key, createStringData("first baseline")));
      REQUIRE(setup.commit());
      auto secondSetup = beginWriteTransaction(secondEnvironment);
      auto secondDatabase = openLifetimeDatabase<Database>(secondSetup);
      REQUIRE(secondDatabase.writer(secondSetup).create(key, createStringData("second baseline")));
      REQUIRE(secondSetup.commit());
      auto owner = beginWriteTransaction(firstEnvironment);
      auto oldWriter = firstDatabase.writer(owner);
      REQUIRE(oldWriter.update(key, createStringData("staged")));
      auto optMovedOwner = std::optional<WriteTransaction>{};
      auto* currentOwner = &owner;
      auto* currentEnvironment = &firstEnvironment;
      auto const* currentDatabase = &firstDatabase;
      auto expected = std::string_view{"staged"};

      if (transfer == "construction" || transfer == "round-trip" || transfer == "move-out-rebind")
      {
        optMovedOwner.emplace(std::move(owner));
        currentOwner = &*optMovedOwner;

        if (transfer == "round-trip")
        {
          owner = std::move(*optMovedOwner);
          currentOwner = &owner;
        }
      }
      else if (transfer == "committed-rebind")
      {
        REQUIRE(owner.commit());
        owner = beginWriteTransaction(firstEnvironment);
      }
      else
      {
        REQUIRE((transfer == "replacement" || transfer == "self"));
      }

      if (transfer == "replacement" || transfer == "move-out-rebind")
      {
        owner = beginWriteTransaction(secondEnvironment);
        currentOwner = &owner;
        currentEnvironment = &secondEnvironment;
        currentDatabase = &secondDatabase;
        expected = "second baseline";
      }

      auto fresh = currentDatabase->writer(*currentOwner);
      auto* const sameOwner = currentOwner;
      *currentOwner = std::move(*sameOwner);
      auto const optValue = fresh.get(key);
      REQUIRE(optValue);
      CHECK(utility::bytes::stringView(*optValue) == expected);

      if (transfer == "self")
      {
        // Existing bindings, not just freshly created ones, survive self-move.
        auto const optOldValue = oldWriter.get(key);
        REQUIRE(optOldValue);
        CHECK(utility::bytes::stringView(*optOldValue) == "staged");
      }

      REQUIRE(fresh.update(key, createStringData("fresh owner binding")));
      REQUIRE(currentOwner->commit());

      if (optMovedOwner)
      {
        optMovedOwner->abort();
      }

      auto const snapshot = beginReadTransaction(*currentEnvironment);
      auto const optPersisted = currentDatabase->reader(snapshot).get(key);
      REQUIRE(optPersisted);
      CHECK(utility::bytes::stringView(*optPersisted) == "fresh owner binding");

      if (currentEnvironment == &secondEnvironment)
      {
        auto const firstSnapshot = beginReadTransaction(firstEnvironment);
        auto const optFirst = firstDatabase.reader(firstSnapshot).get(key);
        REQUIRE(optFirst);
        CHECK(utility::bytes::stringView(*optFirst) == "first baseline");
      }
    }

    template<typename Database, typename Key>
    void checkDestructionAfterOwner(Key const key, std::string_view const termination)
    {
      auto const temp = ao::test::TempDir{};
      auto environment = openEnvironment(temp.path(), {.flags = kEnvNoTls, .maxDatabases = 2});
      auto setup = beginWriteTransaction(environment);
      auto database = openLifetimeDatabase<Database>(setup);
      REQUIRE(database.writer(setup).create(key, createStringData("baseline")));
      REQUIRE(setup.commit());
      auto optWriter = std::optional<typename Database::Writer>{};

      {
        auto owner = beginWriteTransaction(environment);
        optWriter.emplace(database.writer(owner));
        REQUIRE(optWriter->update(key, createStringData("committed")));

        if (termination == "commit")
        {
          REQUIRE(owner.commit());
        }
        else if (termination == "abort")
        {
          owner.abort();
        }
      }

      // Even the owner object is gone; disposal must not inspect that borrow or
      // close a cursor already reclaimed by native commit/abort/destruction.
      optWriter.reset();
      auto const snapshot = beginReadTransaction(environment);
      auto const optValue = database.reader(snapshot).get(key);
      REQUIRE(optValue);
      CHECK(utility::bytes::stringView(*optValue) == (termination == "commit" ? "committed" : "baseline"));
    }
  } // namespace

  TEST_CASE("Database Writers - both key families reject stale bindings before any native access",
            "[lmdb][unit][database][writer]")
  {
    auto const family = GENERATE(std::string_view{"integer"}, std::string_view{"byte"});
    auto const transfer = GENERATE(std::string_view{"committed-rebind"},
                                   std::string_view{"replacement"},
                                   std::string_view{"construction"},
                                   std::string_view{"round-trip"},
                                   std::string_view{"move-out-rebind"},
                                   std::string_view{"wrapper-construction"},
                                   std::string_view{"wrapper-assignment"});
    auto const operation = family == "integer" ? GENERATE(std::string_view{"clear"},
                                                          std::string_view{"get"},
                                                          std::string_view{"create"},
                                                          std::string_view{"update"},
                                                          std::string_view{"delete"},
                                                          std::string_view{"append"},
                                                          std::string_view{"max"})
                                               : GENERATE(std::string_view{"clear"},
                                                          std::string_view{"get"},
                                                          std::string_view{"create"},
                                                          std::string_view{"update"},
                                                          std::string_view{"delete"});
    auto const executablePath = ao::test::siblingProbeExecutablePath("ao_library_probe");
    REQUIRE_FALSE(executablePath.empty());
    auto const scratch = ao::test::TempDir{};
    auto scenario = std::string{"lmdb-writer-binding-"};
    scenario.append(family).append("-").append(transfer).append("-").append(operation);
    INFO("probe: " << scenario);
    scenario.append(":").append(scratch.path().filename().string());
    auto const result = ao::test::runProbeProcess(executablePath, scenario, std::chrono::seconds{15});

    REQUIRE(result.started);
    CHECK(result.launchError.empty());
    CHECK_FALSE(result.timedOut);
    REQUIRE(result.hasFatalTermination());
    CHECK(result.hasPlatformAbort());
    CHECK(result.standardError.contains("AOBUS_FATAL"));
    CHECK(result.standardError.contains("category=expects"));
    CHECK(result.standardError.contains("condition=_txn != nullptr && _txn->isActive() && "
                                        "detail::DatabaseAccess::bindingGeneration(*_txn) == _bindingGeneration && "
                                        "detail::DatabaseAccess::handle(*_txn) == _nativeTxn"));
    CHECK(result.standardError.contains(family == "integer"
                                          ? "IntegerKeyDatabase::Writer used after its transaction finished"
                                          : "ByteKeyDatabase::Writer used after its transaction finished"));
    CHECK(result.standardError.contains("Database.cpp:"));
    CHECK(result.standardError.contains("ensureActive"));
    CHECK(result.standardOutput.empty());
  }

  TEST_CASE("Database Writers - wrapper moves and self-moves preserve complete write capability",
            "[lmdb][unit][database][writer]")
  {
    SECTION("integer keys")
    {
      checkWrapperMoves<IntegerKeyDatabase>(std::uint32_t{1}, std::uint32_t{4});
    }

    SECTION("byte keys")
    {
      checkWrapperMoves<ByteKeyDatabase>(createStringData("key"), createStringData("extra"));
    }
  }

  TEST_CASE("Database Writers - fresh bindings work after complete owner moves and rebinds",
            "[lmdb][unit][database][writer]")
  {
    auto const transfer = GENERATE(std::string_view{"construction"},
                                   std::string_view{"round-trip"},
                                   std::string_view{"self"},
                                   std::string_view{"committed-rebind"},
                                   std::string_view{"replacement"},
                                   std::string_view{"move-out-rebind"});
    INFO("transfer: " << transfer);

    SECTION("integer keys")
    {
      checkFreshOwnerBinding<IntegerKeyDatabase>(std::uint32_t{1}, transfer);
    }

    SECTION("byte keys")
    {
      checkFreshOwnerBinding<ByteKeyDatabase>(createStringData("key"), transfer);
    }
  }

  TEST_CASE("Database Writers - destruction is safe after transaction and owner teardown",
            "[lmdb][unit][database][writer]")
  {
    auto const termination =
      GENERATE(std::string_view{"commit"}, std::string_view{"abort"}, std::string_view{"destruction"});
    INFO("termination: " << termination);

    SECTION("integer keys")
    {
      checkDestructionAfterOwner<IntegerKeyDatabase>(std::uint32_t{1}, termination);
    }

    SECTION("byte keys")
    {
      checkDestructionAfterOwner<ByteKeyDatabase>(createStringData("key"), termination);
    }
  }
} // namespace ao::lmdb::test
