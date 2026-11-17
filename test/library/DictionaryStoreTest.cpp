// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/library/DictionaryStore.h>

#include "test/TestFixtureSupport.h"
#include "test/library/MusicLibraryTestSupport.h"
#include "test/library/WritableLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/WriteTransaction.h>
#include <ao/utility/ScopedRegistration.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::library::test
{
  namespace
  {
    MusicLibrary openTestLibrary(ao::test::TempDir const& temp)
    {
      return makeTestMusicLibrary(temp.path(), temp.path() / "db");
    }

    DictionaryId requireIntern(WriteTransaction& transaction, std::string_view value)
    {
      return ao::test::requireValue(physicalDictionary(transaction).intern(value));
    }
  } // namespace

  TEST_CASE("DictionaryStore - publishes a transaction delta only after commit")
  {
    auto const temp = ao::test::TempDir{};
    auto library = openTestLibrary(temp);
    auto const& dictionary = library.dictionary();
    auto seed = writeTransaction(library);
    auto const originalId = requireIntern(seed, "Bach");
    REQUIRE(seed.commit());
    auto const initialGeneration = dictionary.generation();

    auto transaction = writeTransaction(library);
    auto const firstId = requireIntern(transaction, "Haydn");
    auto const secondId = requireIntern(transaction, "Mozart");
    auto const context = DictionaryReadContext{dictionary};
    auto const symbols = std::array<std::string, 4>{"Bach", "Haydn", "Mozart", "missing"};
    auto ids = std::array{originalId, originalId, originalId, originalId};

    CHECK(firstId == DictionaryId{2});
    CHECK(secondId == DictionaryId{3});
    CHECK(dictionary.size() == 1);
    CHECK(dictionary.generation() == initialGeneration);
    CHECK(dictionary.get(originalId) == "Bach");
    CHECK_FALSE(dictionary.findId("Haydn"));
    CHECK_FALSE(dictionary.findId("Mozart"));
    CHECK(dictionary.getOrDefault(firstId, "fallback") == "fallback");
    CHECK(dictionary.getOrDefault(secondId, "fallback") == "fallback");
    CHECK(context.bind(symbols, ids) == initialGeneration);
    CHECK(ids == std::array{originalId, kInvalidDictionaryId, kInvalidDictionaryId, kInvalidDictionaryId});

    REQUIRE(transaction.commit());

    CHECK(dictionary.size() == 3);
    CHECK(dictionary.generation() == initialGeneration + 1);
    CHECK(dictionary.findId("Haydn") == firstId);
    CHECK(dictionary.findId("Mozart") == secondId);
    CHECK(dictionary.getOrDefault(firstId, "fallback") == "Haydn");
    CHECK(dictionary.getOrDefault(secondId, "fallback") == "Mozart");
    CHECK(context.bind(symbols, ids) == initialGeneration + 1);
    CHECK(ids == std::array{originalId, firstId, secondId, kInvalidDictionaryId});
  }

  TEST_CASE("DictionaryStore - canonicalizes Unicode identity before interning")
  {
    auto const temp = ao::test::TempDir{};
    auto library = openTestLibrary(temp);
    auto transaction = writeTransaction(library);
    auto const decomposed = std::string{"Dvor\u030Ca\u0301k"};

    auto const decomposedId = requireIntern(transaction, decomposed);
    auto const composedId = requireIntern(transaction, "Dvořák");

    CHECK(decomposedId == composedId);
    REQUIRE(transaction.commit());
    CHECK(library.dictionary().size() == 1);
    CHECK(library.dictionary().get(composedId) == "Dvořák");
    CHECK(library.dictionary().findId("Dvořák") == composedId);
    CHECK_FALSE(library.dictionary().findId(decomposed));
  }

  TEST_CASE("DictionaryStore - rejects malformed UTF-8 without staging a symbol")
  {
    auto const temp = ao::test::TempDir{};
    auto library = openTestLibrary(temp);
    auto transaction = writeTransaction(library);
    auto const malformed = std::string{"\xC0\xAF", 2};
    auto const res = physicalDictionary(transaction).intern(malformed);

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::InvalidInput);
    CHECK(res.error().message.contains("Dictionary text"));
    REQUIRE(transaction.commit());
    CHECK(library.dictionary().size() == 0);

    auto validTransaction = writeTransaction(library);
    auto const validId = requireIntern(validTransaction, "valid");
    REQUIRE(validId == DictionaryId{1});
    REQUIRE(validTransaction.commit());
    CHECK(library.dictionary().get(validId) == "valid");
    CHECK(library.dictionary().findId("valid") == validId);
  }

  TEST_CASE("DictionaryStore - discards an uncommitted overlay and reuses its ID")
  {
    auto const temp = ao::test::TempDir{};
    auto library = openTestLibrary(temp);
    auto const& dictionary = library.dictionary();
    auto const initialGeneration = dictionary.generation();
    auto preview = writeTransaction(library);
    auto const provisionalId = requireIntern(preview, "preview-only");

    SECTION("aborting explicitly")
    {
      preview.abort();
    }

    SECTION("dropping the active transaction")
    {
      auto discarded = std::move(preview);
    }

    CHECK_FALSE(dictionary.findId("preview-only"));
    CHECK(dictionary.getOrDefault(provisionalId, "fallback") == "fallback");
    CHECK(dictionary.size() == 0);
    CHECK(dictionary.generation() == initialGeneration);

    auto committed = writeTransaction(library);
    auto const id = requireIntern(committed, "committed");
    CHECK(id == provisionalId);
    REQUIRE(committed.commit());
    CHECK(dictionary.getOrDefault(id) == "committed");
    CHECK_FALSE(dictionary.findId("preview-only"));
  }

  TEST_CASE("DictionaryStore - rolls back prepared publication when commit fails")
  {
    auto const temp = ao::test::TempDir{};
    auto library = openTestLibrary(temp);
    auto const& dictionary = library.dictionary();
    auto seed = writeTransaction(library);
    auto const originalId = requireIntern(seed, "stable prefix");
    REQUIRE(seed.commit());
    auto const original = dictionary.get(originalId);
    auto const initialGeneration = dictionary.generation();
    auto transaction = writeTransaction(
      library,
      WriteTransaction::Options{
        .optInjectedCommitFailure = Error{.code = Error::Code::IoError, .message = "injected commit failure"},
      });
    auto const firstId = requireIntern(transaction, "failed first");
    auto const secondId = requireIntern(transaction, "failed second");

    auto const res = transaction.commit();

    REQUIRE_FALSE(res);
    CHECK(res.error().message == "injected commit failure");
    CHECK(dictionary.size() == 1);
    CHECK(dictionary.generation() == initialGeneration);
    CHECK_FALSE(dictionary.findId("failed first"));
    CHECK_FALSE(dictionary.findId("failed second"));
    CHECK(dictionary.getOrDefault(firstId, "fallback") == "fallback");
    CHECK(dictionary.getOrDefault(secondId, "fallback") == "fallback");
    auto const context = DictionaryReadContext{dictionary};
    auto const symbols = std::array<std::string, 3>{"stable prefix", "failed first", "failed second"};
    auto ids = std::array{originalId, firstId, secondId};
    CHECK(context.bind(symbols, ids) == initialGeneration);
    CHECK(ids == std::array{originalId, kInvalidDictionaryId, kInvalidDictionaryId});

    auto retry = writeTransaction(library);
    CHECK(requireIntern(retry, "retry first") == firstId);
    CHECK(requireIntern(retry, "retry second") == secondId);
    REQUIRE(retry.commit());
    CHECK(dictionary.generation() == initialGeneration + 1);
    CHECK(dictionary.getOrDefault(firstId) == "retry first");
    CHECK(dictionary.getOrDefault(secondId) == "retry second");
    CHECK(dictionary.findId("retry first") == firstId);
    CHECK(dictionary.findId("retry second") == secondId);
    CHECK_FALSE(dictionary.findId("failed first"));
    CHECK_FALSE(dictionary.findId("failed second"));
    CHECK(original == "stable prefix");
    CHECK(dictionary.getOrDefault(originalId).data() == original.data());
  }

  TEST_CASE("DictionaryStore - commits repeated and distinct transaction symbols exactly once")
  {
    auto const temp = ao::test::TempDir{};
    auto library = openTestLibrary(temp);
    auto transaction = writeTransaction(library);
    auto const first = requireIntern(transaction, "first");
    auto const duplicate = requireIntern(transaction, "first");
    auto const second = requireIntern(transaction, "second");

    CHECK(first == duplicate);
    CHECK(first == DictionaryId{1});
    CHECK(second == DictionaryId{2});
    REQUIRE(transaction.commit());
    CHECK(library.dictionary().size() == 2);
  }

  TEST_CASE("DictionaryStore - committed rows survive reopening")
  {
    auto const temp = ao::test::TempDir{};
    auto const databasePath = temp.path() / "db";
    auto id = kInvalidDictionaryId;

    {
      auto library = makeTestMusicLibrary(temp.path(), databasePath);
      auto transaction = writeTransaction(library);
      id = requireIntern(transaction, "persistent");
      REQUIRE(transaction.commit());
    }

    auto reopened = makeTestMusicLibrary(temp.path(), databasePath);
    CHECK(reopened.dictionary().get(id) == "persistent");
    CHECK(reopened.dictionary().findId("persistent") == id);
  }

  TEST_CASE("DictionaryStore - validates invalid lookups and defaults")
  {
    auto const temp = ao::test::TempDir{};
    auto library = openTestLibrary(temp);
    auto transaction = writeTransaction(library);
    auto const id = requireIntern(transaction, "value");
    REQUIRE(transaction.commit());

    CHECK(library.dictionary().getOrDefault(id) == "value");
    CHECK(library.dictionary().getOrDefault(kInvalidDictionaryId, "fallback") == "fallback");
    CHECK(library.dictionary().getOrDefault(DictionaryId{999}).empty());
  }

  TEST_CASE("DictionaryStore - keeps borrowed values stable across ten thousand committed inserts")
  {
    auto const temp = ao::test::TempDir{};
    auto library = openTestLibrary(temp);

    auto seed = writeTransaction(library);
    auto const stableId = requireIntern(seed, "stable");
    REQUIRE(seed.commit());
    auto const stableView = library.dictionary().get(stableId);

    auto growth = writeTransaction(library);

    for (std::int32_t index = 0; index < 10000; ++index)
    {
      std::ignore = requireIntern(growth, "growth_" + std::to_string(index));
    }

    REQUIRE(growth.commit());
    CHECK(stableView == "stable");
    CHECK(library.dictionary().get(stableId).data() == stableView.data());
    CHECK(library.dictionary().findId("stable") == stableId);
  }

  TEST_CASE("DictionaryReadCache - resolves values after bounded collisions")
  {
    auto const temp = ao::test::TempDir{};
    auto library = openTestLibrary(temp);
    auto transaction = writeTransaction(library);
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

  TEST_CASE("DictionaryReadContext - binds symbols with one committed generation")
  {
    auto const temp = ao::test::TempDir{};
    auto library = openTestLibrary(temp);
    auto transaction = writeTransaction(library);
    auto const firstId = requireIntern(transaction, "first");
    REQUIRE(transaction.commit());

    auto context = DictionaryReadContext{library.dictionary()};
    auto const symbols = std::vector<std::string>{"first", "missing"};
    auto ids = std::vector<DictionaryId>(symbols.size());
    auto const generation = context.bind(symbols, ids);

    CHECK(generation == library.dictionary().generation());
    CHECK(ids == std::vector{firstId, kInvalidDictionaryId});
  }

  TEST_CASE("DictionaryStore - readers observe complete committed publications", "[concurrency]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = openTestLibrary(temp);
    auto seed = writeTransaction(library);
    auto const stableId = requireIntern(seed, "stable");
    REQUIRE(seed.commit());

    auto const seedGeneration = library.dictionary().generation();
    constexpr std::uint32_t kBatchCount = 128;
    constexpr std::uint32_t kBatchSize = 8;
    constexpr std::size_t kFinalSize = 1 + (kBatchCount * kBatchSize);
    constexpr std::ptrdiff_t kWorkerCount = 4;
    auto ready = std::latch{kWorkerCount};
    auto start = std::latch{1};
    auto failed = std::atomic<bool>{false};
    auto writerDone = std::atomic<bool>{false};

    auto reader = [&]
    {
      ready.count_down();
      start.wait();
      auto context = DictionaryReadContext{library.dictionary()};
      std::uint32_t probeBatch = 0;

      while (true)
      {
        if (library.dictionary().get(stableId) != "stable" || library.dictionary().findId("stable") != stableId)
        {
          failed.store(true, std::memory_order_relaxed);
        }

        if (auto const size = library.dictionary().size();
            size < 1 || size > kFinalSize || (size - 1) % kBatchSize != 0)
        {
          failed.store(true, std::memory_order_relaxed);
        }

        auto symbols = std::array<std::string, kBatchSize>{};
        auto ids = std::array<DictionaryId, kBatchSize>{};

        for (std::uint32_t index = 0; index < kBatchSize; ++index)
        {
          symbols[index] = "batch_" + std::to_string(probeBatch) + "_" + std::to_string(index);
        }

        // Unlike separate calls to findId() and generation(), bind() must
        // resolve the whole batch at the generation it returns.
        auto const generation = context.bind(symbols, ids);
        auto const published = generation > seedGeneration + probeBatch;

        for (std::uint32_t index = 0; index < kBatchSize; ++index)
        {
          auto const expectedId =
            published ? DictionaryId{2 + (probeBatch * kBatchSize) + index} : kInvalidDictionaryId;

          if (ids[index] != expectedId)
          {
            failed.store(true, std::memory_order_relaxed);
          }
        }

        probeBatch = (probeBatch + 1) % kBatchCount;

        if (writerDone.load(std::memory_order_acquire) || failed.load(std::memory_order_relaxed))
        {
          break;
        }
      }
    };

    auto writer = std::jthread{};
    auto reader1 = std::jthread{};
    auto reader2 = std::jthread{};
    auto reader3 = std::jthread{};
    // Release already-created workers before their destructors join if a later
    // thread construction throws.
    auto releaseWorkers = utility::ScopedRegistration{[&start] { start.count_down(); }};

    writer = std::jthread{
      [&]
      {
        ready.count_down();
        start.wait();

        for (std::uint32_t batch = 0; batch < kBatchCount; ++batch)
        {
          auto transaction = writeTransaction(library);

          for (std::uint32_t index = 0; index < kBatchSize; ++index)
          {
            auto const res =
              physicalDictionary(transaction).intern("batch_" + std::to_string(batch) + "_" + std::to_string(index));

            if (!res)
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
    reader1 = std::jthread{reader};
    reader2 = std::jthread{reader};
    reader3 = std::jthread{reader};

    ready.wait();
    releaseWorkers.reset();
    writer.join();
    reader1.join();
    reader2.join();
    reader3.join();
    CHECK_FALSE(failed.load(std::memory_order_relaxed));
    CHECK(library.dictionary().size() == kFinalSize);
    CHECK(library.dictionary().generation() == seedGeneration + kBatchCount);
  }
} // namespace ao::library::test
