// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/TestFixtureSupport.h"
#include "test/library/MusicLibraryTestSupport.h"
#include "test/library/WritableLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/library/WriteTransaction.h>
#include <ao/utility/ScopedRegistration.h>
#include <ao/utility/ThreadName.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>
#include <pthread.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <future>
#include <semaphore>
#include <string>
#include <utility>

namespace ao::library::test
{
  namespace
  {
    constexpr auto kObservationTimeout = std::chrono::seconds{10};
    constexpr std::size_t kPublishedCount = 3;
    constexpr auto kPublishedIds = std::array{DictionaryId{1}, DictionaryId{2}, DictionaryId{3}};
    using DictionaryTexts = std::array<std::string, kPublishedCount>;

    struct CommitGate final
    {
      std::size_t transactionId = 0;
      std::binary_semaphore committed{0};
      std::binary_semaphore resumeWriter{0};
    };

    struct ReaderProbe final
    {
      CommitGate const& commit;
      std::binary_semaphore admissionObserved{0};
      bool armed = false;
      bool contended = false;
      std::atomic<bool> returned{false};
    };

    // Non-owning markers for the two workers' individual public calls.
    struct ThreadProbes final
    {
      CommitGate* writer = nullptr;
      ReaderProbe* reader = nullptr;
    };

    ThreadProbes& threadProbes()
    {
      static constinit thread_local ThreadProbes probes;
      return probes;
    }

    struct DictionaryObservation final
    {
      std::size_t count = 0;
      std::uint64_t generation = 0;
      std::array<DictionaryId, kPublishedCount> ids{};
      DictionaryTexts texts{};
    };

    struct ReadObservation final
    {
      std::uint64_t revision;
      DictionaryObservation dictionary;
    };

    DictionaryObservation observeDictionary(DictionaryStore const& dictionary, DictionaryTexts const& symbols)
    {
      auto observation = DictionaryObservation{};
      observation.generation = DictionaryReadContext{dictionary}.bind(symbols, observation.ids);
      observation.count = dictionary.size();

      for (std::size_t index = 0; index < kPublishedIds.size(); ++index)
      {
        observation.texts[index] = dictionary.getOrDefault(kPublishedIds[index]);
      }

      return observation;
    }
  } // namespace

  // Keep linker-reserved spellings in asm labels, not C++ identifiers.
  extern "C"
  {
    int nativeCommit(::MDB_txn* transaction) asm("__real_mdb_txn_commit");
    int gatedCommit(::MDB_txn* transaction) asm("__wrap_mdb_txn_commit");
    int nativeStat(::MDB_txn* transaction, ::MDB_dbi dbi, ::MDB_stat* stat) asm("__real_mdb_stat");
    int observedStat(::MDB_txn* transaction, ::MDB_dbi dbi, ::MDB_stat* stat) asm("__wrap_mdb_stat");
    int nativeMutexLock(::pthread_mutex_t* mutex) asm("__real_pthread_mutex_lock");
    int observedMutexLock(::pthread_mutex_t* mutex) asm("__wrap_pthread_mutex_lock");

    int gatedCommit(::MDB_txn* transaction)
    {
      auto* const gate = std::exchange(threadProbes().writer, nullptr);

      if (gate == nullptr)
      {
        return nativeCommit(transaction);
      }

      // Native commit consumes the handle, so obtain the id before calling it.
      auto const transactionId = ::mdb_txn_id(transaction);
      auto const result = nativeCommit(transaction);

      if (result == MDB_SUCCESS)
      {
        gate->transactionId = transactionId;
        gate->committed.release();
        gate->resumeWriter.acquire();
      }

      return result;
    }

    int observedStat(::MDB_txn* transaction, ::MDB_dbi dbi, ::MDB_stat* stat)
    {
      auto const result = nativeStat(transaction, dbi, stat);

      // Only the public readTransaction call is marked. Its entry count is
      // the dictionary admission check; require this writer's grown snapshot.
      if (auto* const probe = threadProbes().reader; probe != nullptr && result == MDB_SUCCESS &&
                                                     stat->ms_entries == kPublishedCount &&
                                                     ::mdb_txn_id(transaction) == probe->commit.transactionId)
      {
        probe->armed = true;
      }

      return result;
    }

    int observedMutexLock(::pthread_mutex_t* mutex)
    {
      auto* const probe = threadProbes().reader;

      if (probe == nullptr || !probe->armed)
      {
        return nativeMutexLock(mutex);
      }

      auto const result = ::pthread_mutex_trylock(mutex);

      if (result == 0)
      {
        return 0;
      }

      if (result == EBUSY && !probe->contended)
      {
        probe->contended = true;
        // Do not observe any calls made by the signal implementation itself.
        threadProbes().reader = nullptr;
        probe->admissionObserved.release();
        threadProbes().reader = probe;
      }

      return nativeMutexLock(mutex);
    }
  }

  TEST_CASE("MusicLibrary - read admission waits for dictionary publication after native commit", "[concurrency]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto writable = requireWritableLibrary(library);
    auto const& dictionary = library.dictionary();
    auto const symbols = DictionaryTexts{"Bach", "Haydn", "Mozart"};

    {
      auto seed = writable.writeTransaction();
      CHECK(ao::test::requireValue(physicalDictionary(seed).intern(symbols[0])) == kPublishedIds[0]);
      REQUIRE(seed.commit());
    }

    auto const initialGeneration = dictionary.generation();
    auto gate = CommitGate{};
    auto probe = ReaderProbe{.commit = gate};
    auto prefix = DictionaryObservation{};
    auto writer = std::future<Result<>>{};
    auto reader = std::future<ReadObservation>{};
    // Open the gate before the async futures can join, including on a failing
    // REQUIRE or thread creation failure. Bazel bounds the process itself.
    auto releaseWriter = utility::ScopedRegistration{[&gate] { gate.resumeWriter.release(); }};

    writer = std::async(std::launch::async,
                        [&] -> Result<>
                        {
                          setCurrentThreadName("ao-dict-writer");
                          auto write = writable.writeTransaction();

                          for (std::size_t index = 1; index < symbols.size(); ++index)
                          {
                            if (auto res = physicalDictionary(write).intern(symbols[index]); !res)
                            {
                              return std::unexpected{std::move(res.error())};
                            }
                          }

                          threadProbes().writer = &gate;
                          return write.commit();
                        });
    INFO("The writer must reach the gate after a successful native commit");
    REQUIRE(gate.committed.try_acquire_for(kObservationTimeout));

    reader = std::async(std::launch::async,
                        [&]
                        {
                          setCurrentThreadName("ao-dict-reader");
                          prefix = observeDictionary(dictionary, symbols);
                          auto const read = [&]
                          {
                            [[maybe_unused]] auto const clearProbe =
                              utility::ScopedRegistration{[] { threadProbes().reader = nullptr; }};
                            threadProbes().reader = &probe;
                            return library.readTransaction();
                          }();
                          probe.returned.store(true);

                          if (!probe.contended)
                          {
                            probe.admissionObserved.release();
                          }

                          return ReadObservation{.revision = library.libraryRevision(read),
                                                 .dictionary = observeDictionary(dictionary, symbols)};
                        });

    // The gated writer and this reader are the only participants in admission.
    // A busy native lock after the matching stat is evidence; timeouts are not.
    INFO("The reader must contend during admission of the committed dictionary tail");
    REQUIRE(probe.admissionObserved.try_acquire_for(kObservationTimeout));
    REQUIRE(probe.contended);
    CHECK_FALSE(probe.returned.load());
    CHECK(prefix.count == 1);
    CHECK(prefix.generation == initialGeneration);
    CHECK(prefix.ids == std::array{kPublishedIds[0], kInvalidDictionaryId, kInvalidDictionaryId});
    CHECK(prefix.texts == DictionaryTexts{symbols[0], "", ""});

    releaseWriter.reset();
    REQUIRE(writer.wait_for(kObservationTimeout) == std::future_status::ready);
    REQUIRE(writer.get());
    REQUIRE(reader.wait_for(kObservationTimeout) == std::future_status::ready);
    auto const admitted = reader.get();
    CHECK(admitted.revision == 2);
    CHECK(admitted.dictionary.count == kPublishedCount);
    CHECK(admitted.dictionary.ids == kPublishedIds);
    CHECK(admitted.dictionary.texts == symbols);
    CHECK(admitted.dictionary.generation == initialGeneration + 1);
    CHECK(dictionary.generation() == initialGeneration + 1);
  }
} // namespace ao::library::test
