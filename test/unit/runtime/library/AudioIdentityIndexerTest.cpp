// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/library/AudioIdentity.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/library/AudioIdentityIndexer.h>
#include <ao/rt/library/LibraryScan.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/utility/Hash128.h>
#include <ao/utility/Xxh3.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    struct ManifestIdentityState final
    {
      std::uint64_t audioPayloadLength = 0;
      utility::Hash128 audioSignature{};
    };

    void copyFixture(std::filesystem::path const& musicRoot, std::string_view uri)
    {
      auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
      std::filesystem::copy_file(sourceFile, musicRoot / std::filesystem::path{uri});
    }

    ManifestIdentityState manifestIdentity(library::MusicLibrary& ml, std::string_view uri)
    {
      auto transaction = ml.readTransaction();
      auto manifestResult = ml.manifest().reader(transaction).get(uri);
      REQUIRE(manifestResult);
      return ManifestIdentityState{
        .audioPayloadLength = manifestResult->audioPayloadLength(), .audioSignature = manifestResult->audioSignature()};
    }

    bool manifestHasIdentity(library::MusicLibrary& ml, std::string_view uri)
    {
      auto const identity = manifestIdentity(ml, uri);
      return library::hasAudioIdentity(identity.audioPayloadLength, identity.audioSignature);
    }

    void importWithPolicy(library::MusicLibrary& ml, AudioIdentityPolicy policy, std::size_t expectedNewCount = 1)
    {
      auto scanService = LibraryScan{ml};
      auto plan = scanService.buildPlan().value();
      REQUIRE(plan.count(ScanClassification::New) == expectedNewCount);

      auto runResult = scanService.applyPlan(std::move(plan), ScanApplyOptions{.audioIdentityPolicy = policy});
      REQUIRE(runResult);
      REQUIRE(runResult->insertedIds.size() == expectedNewCount);
      REQUIRE(runResult->failureCount == 0);
    }

    void writeManifestIdentity(library::MusicLibrary& ml, std::string_view uri)
    {
      auto transaction = ml.writeTransaction();
      auto writer = ml.manifest().writer(transaction);
      auto currentResult = writer.get(uri);
      REQUIRE(currentResult);

      auto builder = library::FileManifestBuilder::fromView(*currentResult);
      builder.audioPayloadLength(1).audioSignature(utility::xxh3Hash128("test-identity"));
      REQUIRE(writer.put(uri, builder.serialize()));
      REQUIRE(transaction.commit());
    }

    void appendAndAdvanceMtime(std::filesystem::path const& path)
    {
      auto const oldMtimePoint = std::filesystem::last_write_time(path);
      auto out = std::ofstream{path, std::ios::binary | std::ios::app};
      out << "changed during backfill";
      out.close();
      std::filesystem::last_write_time(path, oldMtimePoint + std::chrono::seconds{10});
    }

    // Drives the indexer coroutine to completion on a private runtime. The
    // future blocks the test thread, never a pool thread.
    Result<AudioIdentityIndexResult> runIndexPending(library::MusicLibrary& ml,
                                                     AudioIdentityIndexer::Options options = {},
                                                     AudioIdentityIndexer::ProgressCallback progressCallback = {},
                                                     AudioIdentityIndexer::ItemFailureCallback failureCallback = {},
                                                     std::stop_token stopToken = {},
                                                     std::mutex* optMutationMutex = nullptr)
    {
      auto executor = InlineExecutor{};
      auto runtime = async::Runtime{executor, 4};
      auto fallbackMutationMutex = std::mutex{};
      auto& mutationMutex = optMutationMutex == nullptr ? fallbackMutationMutex : *optMutationMutex;
      auto indexer = AudioIdentityIndexer{runtime, ml, mutationMutex};
      auto future = runtime.spawn(indexer.indexPending(
        std::move(options), std::move(progressCallback), std::move(failureCallback), std::move(stopToken)));
      return future.get();
    }

    library::AudioIdentity fakeIdentity()
    {
      return library::AudioIdentity{.signature = utility::xxh3Hash128("fake-identity"), .payloadLength = 7};
    }
  } // namespace

  TEST_CASE("AudioIdentityIndexer - fills pending available manifest rows", "[runtime][unit][library][audio-identity]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);
    copyFixture(musicRoot, "song.flac");

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");
    importWithPolicy(ml, AudioIdentityPolicy::DeferNew);
    CHECK_FALSE(manifestHasIdentity(ml, "song.flac"));

    auto result = runIndexPending(ml);

    REQUIRE(result);
    CHECK(result->completedCount == 1);
    CHECK(result->skippedCount == 0);
    CHECK(result->failureCount == 0);
    CHECK_FALSE(result->cancelled);
    CHECK(manifestHasIdentity(ml, "song.flac"));
  }

  TEST_CASE("AudioIdentityIndexer - concurrent backfill fills many pending rows",
            "[runtime][unit][audio-identity][concurrency]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);
    constexpr std::size_t kTrackCount = 6;

    for (std::size_t index = 0; index < kTrackCount; ++index)
    {
      copyFixture(musicRoot, std::format("song{}.flac", index));
    }

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");
    importWithPolicy(ml, AudioIdentityPolicy::DeferNew, kTrackCount);

    auto result = runIndexPending(ml, AudioIdentityIndexer::Options{.maxConcurrency = 3});

    REQUIRE(result);
    CHECK(std::cmp_equal(result->completedCount, kTrackCount));
    CHECK(result->skippedCount == 0);
    CHECK(result->failureCount == 0);
    CHECK_FALSE(result->cancelled);

    for (std::size_t index = 0; index < kTrackCount; ++index)
    {
      CHECK(manifestHasIdentity(ml, std::format("song{}.flac", index)));
    }
  }

  TEST_CASE("AudioIdentityIndexer - fingerprints run concurrently", "[runtime][unit][audio-identity][concurrency]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);
    copyFixture(musicRoot, "a.flac");
    copyFixture(musicRoot, "b.flac");

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");
    importWithPolicy(ml, AudioIdentityPolicy::DeferNew, 2);

    auto started = AsyncTestState<std::int32_t>::create(0);
    auto release = AsyncBarrier{};
    auto options = AudioIdentityIndexer::Options{
      .maxConcurrency = 2,
      .fingerprint = [&started, &release](std::filesystem::path const&,
                                          library::AudioIdentityProgressCallback,
                                          std::stop_token) -> Result<std::optional<library::AudioIdentity>>
      {
        started.increment();
        release.wait();
        return std::optional{fakeIdentity()};
      }};

    auto executor = InlineExecutor{};
    auto runtime = async::Runtime{executor, 4};
    auto mutationMutex = std::mutex{};
    auto indexer = AudioIdentityIndexer{runtime, ml, mutationMutex};
    auto future = runtime.spawn(indexer.indexPending(std::move(options)));
    auto const bothStarted = started.waitUntil(2);
    release.release();
    auto result = future.get();

    REQUIRE(bothStarted);
    REQUIRE(result);
    CHECK(result->completedCount == 2);
    CHECK(manifestHasIdentity(ml, "a.flac"));
    CHECK(manifestHasIdentity(ml, "b.flac"));
  }

  TEST_CASE("AudioIdentityIndexer - mutation lock is free while fingerprinting",
            "[runtime][unit][audio-identity][concurrency]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);
    copyFixture(musicRoot, "song.flac");

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");
    importWithPolicy(ml, AudioIdentityPolicy::DeferNew);

    auto mutationMutex = std::mutex{};
    auto lockWasFreeDuringFingerprint = std::atomic{false};
    auto options =
      AudioIdentityIndexer::Options{.fingerprint = [&mutationMutex, &lockWasFreeDuringFingerprint](
                                                     std::filesystem::path const&,
                                                     library::AudioIdentityProgressCallback,
                                                     std::stop_token) -> Result<std::optional<library::AudioIdentity>>
                                    {
                                      // If the indexer held the mutation lock around fingerprinting, this
                                      // try_lock from inside the fingerprint would fail.
                                      auto probeLock = std::unique_lock{mutationMutex, std::try_to_lock};
                                      lockWasFreeDuringFingerprint.store(probeLock.owns_lock());
                                      return std::optional{fakeIdentity()};
                                    }};

    auto result = runIndexPending(ml, std::move(options), {}, {}, {}, &mutationMutex);

    REQUIRE(result);
    CHECK(result->completedCount == 1);
    CHECK(lockWasFreeDuringFingerprint.load());
    auto const identity = manifestIdentity(ml, "song.flac");
    CHECK(identity.audioPayloadLength == fakeIdentity().payloadLength);
    CHECK(identity.audioSignature == fakeIdentity().signature);
  }

  TEST_CASE("AudioIdentityIndexer - single fingerprint failure does not abort the index",
            "[runtime][unit][library][audio-identity]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);
    copyFixture(musicRoot, "bad.flac");
    copyFixture(musicRoot, "good.flac");

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");
    importWithPolicy(ml, AudioIdentityPolicy::DeferNew, 2);

    auto failureMutex = std::mutex{};
    auto failures = std::vector<AudioIdentityIndexFailure>{};
    auto options =
      AudioIdentityIndexer::Options{.fingerprint = [](std::filesystem::path const& path,
                                                      library::AudioIdentityProgressCallback,
                                                      std::stop_token) -> Result<std::optional<library::AudioIdentity>>
                                    {
                                      if (path.filename() == "bad.flac")
                                      {
                                        return makeError(Error::Code::IoError, "injected fingerprint failure");
                                      }

                                      return std::optional{fakeIdentity()};
                                    }};

    auto result = runIndexPending(ml,
                                  std::move(options),
                                  {},
                                  [&failureMutex, &failures](AudioIdentityIndexFailure const& failure)
                                  {
                                    auto const lock = std::scoped_lock{failureMutex};
                                    failures.push_back(failure);
                                  });

    REQUIRE(result);
    CHECK(result->completedCount == 1);
    CHECK(result->failureCount == 1);
    CHECK_FALSE(result->cancelled);
    CHECK(manifestHasIdentity(ml, "good.flac"));
    CHECK_FALSE(manifestHasIdentity(ml, "bad.flac"));
    REQUIRE(failures.size() == 1);
    CHECK(failures.front().uri == "bad.flac");
    CHECK(failures.front().stage == "fingerprint");
    CHECK(failures.front().message == "injected fingerprint failure");
  }

  TEST_CASE("AudioIdentityIndexer - skips rows whose file stat changed before write",
            "[runtime][unit][library][audio-identity]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);
    auto const trackPath = musicRoot / "song.flac";
    copyFixture(musicRoot, "song.flac");

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");
    importWithPolicy(ml, AudioIdentityPolicy::DeferNew);

    bool mutated = false;
    auto result = runIndexPending(ml,
                                  {},
                                  [&mutated, &trackPath](AudioIdentityIndexProgress const& progress)
                                  {
                                    if (!mutated && progress.itemFraction == 0.0)
                                    {
                                      mutated = true;
                                      appendAndAdvanceMtime(trackPath);
                                    }
                                  });

    REQUIRE(result);
    CHECK(result->completedCount == 0);
    CHECK(result->skippedCount == 1);
    CHECK(result->failureCount == 0);
    CHECK_FALSE(result->cancelled);
    CHECK_FALSE(manifestHasIdentity(ml, "song.flac"));
  }

  TEST_CASE("AudioIdentityIndexer - leaves existing identity untouched", "[runtime][unit][library][audio-identity]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);
    copyFixture(musicRoot, "song.flac");

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");
    importWithPolicy(ml, AudioIdentityPolicy::Eager);
    auto const originalIdentity = manifestIdentity(ml, "song.flac");

    auto result = runIndexPending(ml);

    REQUIRE(result);
    CHECK(result->completedCount == 0);
    CHECK(result->skippedCount == 0);
    CHECK(result->failureCount == 0);
    CHECK_FALSE(result->cancelled);
    auto const afterIdentity = manifestIdentity(ml, "song.flac");
    CHECK(afterIdentity.audioPayloadLength == originalIdentity.audioPayloadLength);
    CHECK(afterIdentity.audioSignature == originalIdentity.audioSignature);
  }

  TEST_CASE("AudioIdentityIndexer - cancellation preserves pending rows", "[runtime][unit][library][audio-identity]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);
    copyFixture(musicRoot, "song.flac");

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");
    importWithPolicy(ml, AudioIdentityPolicy::DeferNew);

    auto stopSource = std::stop_source{};
    auto result = runIndexPending(
      ml,
      {},
      [&stopSource](AudioIdentityIndexProgress const& progress)
      {
        if (progress.itemFraction == 0.0)
        {
          stopSource.request_stop();
        }
      },
      {},
      stopSource.get_token());

    REQUIRE(result);
    CHECK(result->completedCount == 0);
    CHECK(result->failureCount == 0);
    CHECK(result->cancelled);
    CHECK_FALSE(manifestHasIdentity(ml, "song.flac"));
  }

  TEST_CASE("AudioIdentityIndexer - batch write skips only rows changed mid-batch",
            "[runtime][unit][library][audio-identity]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);
    copyFixture(musicRoot, "a.flac");
    copyFixture(musicRoot, "b.flac");

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");
    importWithPolicy(ml, AudioIdentityPolicy::DeferNew, 2);

    // While the batch is being hashed, fill b's identity behind the indexer's
    // back: its in-transaction revalidation must skip b without losing a.
    // Serial workers keep the trigger point deterministic.
    bool mutated = false;
    auto result =
      runIndexPending(ml,
                      AudioIdentityIndexer::Options{.maxConcurrency = 1},
                      [&mutated, &ml](AudioIdentityIndexProgress const& progress)
                      {
                        if (!mutated && progress.path.filename() == "a.flac" && progress.itemFraction == 0.0)
                        {
                          mutated = true;
                          writeManifestIdentity(ml, "b.flac");
                        }
                      });

    REQUIRE(result);
    CHECK(mutated);
    CHECK(result->completedCount == 1);
    CHECK(result->skippedCount == 1);
    CHECK(result->failureCount == 0);
    CHECK_FALSE(result->cancelled);
    CHECK(manifestHasIdentity(ml, "a.flac"));
    auto const bIdentity = manifestIdentity(ml, "b.flac");
    CHECK(bIdentity.audioPayloadLength == 1);
    CHECK(bIdentity.audioSignature == utility::xxh3Hash128("test-identity"));
  }

  TEST_CASE("AudioIdentityIndexer - cancellation flushes rows hashed earlier in the batch",
            "[runtime][unit][library][audio-identity]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);
    copyFixture(musicRoot, "a.flac");
    copyFixture(musicRoot, "b.flac");

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");
    importWithPolicy(ml, AudioIdentityPolicy::DeferNew, 2);

    // One worker hashes a then b in URI order, so requesting stop when b
    // starts leaves exactly a's hash to flush.
    auto stopSource = std::stop_source{};
    auto result = runIndexPending(
      ml,
      AudioIdentityIndexer::Options{.maxConcurrency = 1},
      [&stopSource](AudioIdentityIndexProgress const& progress)
      {
        if (progress.path.filename() == "b.flac" && progress.itemFraction == 0.0)
        {
          stopSource.request_stop();
        }
      },
      {},
      stopSource.get_token());

    REQUIRE(result);
    CHECK(result->cancelled);
    CHECK(result->completedCount == 1);
    CHECK(result->failureCount == 0);
    CHECK(manifestHasIdentity(ml, "a.flac"));
    CHECK_FALSE(manifestHasIdentity(ml, "b.flac"));
  }
} // namespace ao::rt::test
