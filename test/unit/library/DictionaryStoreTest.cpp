// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestUtils.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/ListStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackStore.h>
#include <ao/library/WriteTransaction.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ao::library::test
{
  namespace
  {
    MusicLibrary openTestLibrary(ao::test::TempDir const& temp)
    {
      return MusicLibrary{temp.path(), temp.path() / "db"};
    }

    DictionaryId requireIntern(WriteTransaction& transaction, std::string_view value)
    {
      return ao::test::requireValue(transaction.dictionary().intern(value));
    }
  } // namespace

  TEST_CASE("DictionaryStore - publishes a transaction delta only after commit", "[library][unit][dictionary]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = openTestLibrary(temp);
    auto const& dictionary = library.dictionary();
    auto const initialGeneration = dictionary.generation();
    auto transaction = library.writeTransaction();
    auto trackWriter = library.tracks().writer(transaction);
    auto listWriter = library.lists().writer(transaction);
    auto resourceWriter = library.resources().writer(transaction);
    auto manifestWriter = library.manifest().writer(transaction);
    auto& dictionaryWriter = transaction.dictionary();
    auto const id = requireIntern(transaction, "Bach");

    CHECK(id == DictionaryId{1});
    CHECK_FALSE(dictionary.contains("Bach"));
    CHECK_FALSE(dictionary.findId("Bach"));
    CHECK(dictionary.size() == 0);
    CHECK(dictionary.generation() == initialGeneration);

    REQUIRE(transaction.commit());

    CHECK(dictionary.get(id) == "Bach");
    CHECK(dictionary.lookupId("Bach") == id);
    CHECK(dictionary.findId("Bach") == id);
    CHECK(dictionary.size() == 1);
    CHECK(dictionary.generation() == initialGeneration + 1);

    CHECK_THROWS_AS(trackWriter.clear(), Exception);
    CHECK_THROWS_AS(listWriter.clear(), Exception);
    CHECK_THROWS_AS(resourceWriter.clear(), Exception);
    CHECK_THROWS_AS(manifestWriter.clear(), Exception);
    auto const lateIntern = dictionaryWriter.intern("after-commit");
    REQUIRE_FALSE(lateIntern);
    CHECK(lateIntern.error().code == Error::Code::InvalidState);
    CHECK_THROWS_AS(transaction.dictionary(), Exception);
  }

  TEST_CASE("DictionaryStore - discards an uncommitted overlay and reuses its ID", "[library][unit][dictionary]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = openTestLibrary(temp);
    auto const& dictionary = library.dictionary();

    {
      auto preview = library.writeTransaction();
      CHECK(requireIntern(preview, "preview-only") == DictionaryId{1});
      CHECK(requireIntern(preview, "preview-only") == DictionaryId{1});
    }

    CHECK_FALSE(dictionary.contains("preview-only"));
    CHECK(dictionary.size() == 0);

    auto committed = library.writeTransaction();
    auto const id = requireIntern(committed, "committed");
    CHECK(id == DictionaryId{1});
    REQUIRE(committed.commit());
    CHECK(dictionary.get(id) == "committed");
  }

  TEST_CASE("DictionaryStore - rolls back prepared publication when commit fails", "[library][unit][dictionary]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = openTestLibrary(temp);
    auto const& dictionary = library.dictionary();
    auto const initialGeneration = dictionary.generation();
    auto transaction = library.writeTransaction(WriteTransaction::Options{
      .optInjectedCommitFailure = Error{.code = Error::Code::IoError, .message = "injected commit failure"},
    });
    auto trackWriter = library.tracks().writer(transaction);
    auto listWriter = library.lists().writer(transaction);
    auto resourceWriter = library.resources().writer(transaction);
    auto manifestWriter = library.manifest().writer(transaction);
    auto& dictionaryWriter = transaction.dictionary();

    auto const failedId = requireIntern(transaction, "failed");
    auto result = transaction.commit();
    REQUIRE_FALSE(result);
    CHECK(result.error().message == "injected commit failure");
    CHECK_FALSE(dictionary.contains("failed"));
    CHECK(dictionary.size() == 0);
    CHECK(dictionary.generation() == initialGeneration);

    CHECK_THROWS_AS(trackWriter.clear(), Exception);
    CHECK_THROWS_AS(listWriter.clear(), Exception);
    CHECK_THROWS_AS(resourceWriter.clear(), Exception);
    CHECK_THROWS_AS(manifestWriter.clear(), Exception);
    auto const lateIntern = dictionaryWriter.intern("after-failure");
    REQUIRE_FALSE(lateIntern);
    CHECK(lateIntern.error().code == Error::Code::InvalidState);
    CHECK_THROWS_AS(transaction.dictionary(), Exception);

    auto retry = library.writeTransaction();
    CHECK(requireIntern(retry, "retry") == failedId);
    REQUIRE(retry.commit());
    CHECK(dictionary.get(failedId) == "retry");
  }

  TEST_CASE("DictionaryStore - commits repeated and distinct transaction symbols exactly once",
            "[library][unit][dictionary]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = openTestLibrary(temp);
    auto transaction = library.writeTransaction();
    auto const first = requireIntern(transaction, "first");
    auto const duplicate = requireIntern(transaction, "first");
    auto const second = requireIntern(transaction, "second");

    CHECK(first == duplicate);
    CHECK(first == DictionaryId{1});
    CHECK(second == DictionaryId{2});
    REQUIRE(transaction.commit());
    CHECK(library.dictionary().size() == 2);
  }

  TEST_CASE("DictionaryStore - committed rows survive reopening", "[library][unit][dictionary]")
  {
    auto const temp = ao::test::TempDir{};
    auto const databasePath = temp.path() / "db";
    auto id = kInvalidDictionaryId;

    {
      auto library = MusicLibrary{temp.path(), databasePath};
      auto transaction = library.writeTransaction();
      id = requireIntern(transaction, "persistent");
      REQUIRE(transaction.commit());
    }

    auto reopened = MusicLibrary{temp.path(), databasePath};
    CHECK(reopened.dictionary().get(id) == "persistent");
    CHECK(reopened.dictionary().lookupId("persistent") == id);
  }

  TEST_CASE("DictionaryStore - validates invalid lookups and defaults", "[library][unit][dictionary]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = openTestLibrary(temp);
    auto transaction = library.writeTransaction();
    auto const id = requireIntern(transaction, "value");
    REQUIRE(transaction.commit());

    CHECK(library.dictionary().getOrDefault(id) == "value");
    CHECK(library.dictionary().getOrDefault(kInvalidDictionaryId, "fallback") == "fallback");
    CHECK(library.dictionary().getOrDefault(DictionaryId{999}).empty());
    CHECK_THROWS_AS(library.dictionary().get(kInvalidDictionaryId), Exception);
    CHECK_THROWS_AS(library.dictionary().get(DictionaryId{999}), Exception);
    CHECK_THROWS_AS(library.dictionary().lookupId("missing"), Exception);
  }

  TEST_CASE("DictionaryStore - keeps borrowed values stable across ten thousand committed inserts",
            "[library][unit][dictionary]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = openTestLibrary(temp);

    auto seed = library.writeTransaction();
    auto const stableId = requireIntern(seed, "stable");
    REQUIRE(seed.commit());
    auto const stableView = library.dictionary().get(stableId);

    auto growth = library.writeTransaction();

    for (std::int32_t index = 0; index < 10000; ++index)
    {
      std::ignore = requireIntern(growth, "growth_" + std::to_string(index));
    }

    REQUIRE(growth.commit());
    CHECK(stableView == "stable");
    CHECK(library.dictionary().lookupId("stable") == stableId);
  }

  TEST_CASE("DictionaryReadCache - resolves values after bounded collisions", "[library][unit][dictionary]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = openTestLibrary(temp);
    auto transaction = library.writeTransaction();
    constexpr std::int32_t kValueCount = 5000;
    auto ids = std::vector<DictionaryId>{};
    ids.reserve(kValueCount);

    for (std::int32_t index = 0; index < kValueCount; ++index)
    {
      ids.push_back(requireIntern(transaction, "value_" + std::to_string(index)));
    }

    REQUIRE(transaction.commit());
    auto cache = DictionaryReadCache{library.dictionary()};

    for (std::int32_t index = 0; index < kValueCount; ++index)
    {
      CHECK(cache.get(ids[static_cast<std::size_t>(index)]) == "value_" + std::to_string(index));
    }
  }

  TEST_CASE("DictionaryReadContext - binds symbols with one committed generation", "[library][unit][dictionary]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = openTestLibrary(temp);
    auto transaction = library.writeTransaction();
    auto const firstId = requireIntern(transaction, "first");
    REQUIRE(transaction.commit());

    auto context = DictionaryReadContext{library.dictionary()};
    auto const symbols = std::vector<std::string>{"first", "missing"};
    auto ids = std::vector<DictionaryId>(symbols.size());
    auto const generation = context.bind(symbols, ids);

    CHECK(generation == library.dictionary().generation());
    CHECK(ids == std::vector{firstId, kInvalidDictionaryId});
  }

  TEST_CASE("DictionaryStore - readers observe complete committed publications",
            "[library][unit][dictionary][concurrency]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = openTestLibrary(temp);
    auto seed = library.writeTransaction();
    auto const stableId = requireIntern(seed, "stable");
    REQUIRE(seed.commit());

    auto start = std::barrier{4};
    auto failed = std::atomic{false};
    auto writerDone = std::atomic{false};

    auto writer = std::jthread{[&]
                               {
                                 start.arrive_and_wait();

                                 for (std::int32_t batch = 0; batch < 128; ++batch)
                                 {
                                   auto transaction = library.writeTransaction();

                                   for (std::int32_t index = 0; index < 8; ++index)
                                   {
                                     auto const result = transaction.dictionary().intern(
                                       "batch_" + std::to_string(batch) + "_" + std::to_string(index));

                                     if (!result)
                                     {
                                       failed.store(true, std::memory_order_relaxed);
                                       break;
                                     }
                                   }

                                   if (failed.load(std::memory_order_relaxed) || !transaction.commit())
                                   {
                                     failed.store(true, std::memory_order_relaxed);
                                     break;
                                   }
                                 }

                                 writerDone.store(true, std::memory_order_release);
                               }};

    auto reader = [&]
    {
      start.arrive_and_wait();
      auto context = DictionaryReadContext{library.dictionary()};
      std::int32_t probeBatch = 0;

      while (true)
      {
        if (library.dictionary().get(stableId) != "stable" || library.dictionary().lookupId("stable") != stableId)
        {
          failed.store(true, std::memory_order_relaxed);
        }

        auto const size = library.dictionary().size();

        if (auto const generation = library.dictionary().generation(); size < 1 || generation < 2)
        {
          failed.store(true, std::memory_order_relaxed);
        }

        auto symbols = std::array<std::string, 8>{};
        auto ids = std::array<DictionaryId, 8>{};

        for (std::int32_t index = 0; index < 8; ++index)
        {
          symbols[static_cast<std::size_t>(index)] =
            "batch_" + std::to_string(probeBatch) + "_" + std::to_string(index);
        }

        std::ignore = context.bind(symbols, ids);
        auto const resolved = std::ranges::count_if(ids, [](DictionaryId id) { return id != kInvalidDictionaryId; });

        if (resolved != 0 && std::cmp_not_equal(resolved, ids.size()))
        {
          failed.store(true, std::memory_order_relaxed);
        }

        probeBatch = (probeBatch + 1) % 128;

        if (writerDone.load(std::memory_order_acquire) || failed.load(std::memory_order_relaxed))
        {
          break;
        }
      }
    };

    auto reader1 = std::jthread{reader};
    auto reader2 = std::jthread{reader};
    auto reader3 = std::jthread{reader};

    writer.join();
    reader1.join();
    reader2.join();
    reader3.join();
    CHECK_FALSE(failed.load(std::memory_order_relaxed));
    CHECK(library.dictionary().size() == 1025);
  }
} // namespace ao::library::test
