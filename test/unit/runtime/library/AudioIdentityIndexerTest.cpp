// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureUtils.h"
#include <ao/library/AudioIdentity.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/library/AudioIdentityIndexer.h>
#include <ao/rt/library/LibraryScan.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/utility/Fnv1a.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <stop_token>
#include <string_view>
#include <utility>

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
      auto txn = ml.readTransaction();
      auto manifestResult = ml.manifest().reader(txn).get(uri);
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
      REQUIRE(runResult->processedIds.size() == expectedNewCount);
      REQUIRE(runResult->failureCount == 0);
    }

    void writeManifestIdentity(library::MusicLibrary& ml, std::string_view uri)
    {
      auto txn = ml.writeTransaction();
      auto writer = ml.manifest().writer(txn);
      auto currentResult = writer.get(uri);
      REQUIRE(currentResult);

      auto builder = library::FileManifestBuilder::fromView(*currentResult);
      builder.audioPayloadLength(1).audioSignature(utility::fnv1a128("test-identity"));
      REQUIRE(writer.put(uri, builder.serialize()));
      REQUIRE(txn.commit());
    }

    void appendAndAdvanceMtime(std::filesystem::path const& path)
    {
      auto const oldMtimePoint = std::filesystem::last_write_time(path);
      auto out = std::ofstream{path, std::ios::binary | std::ios::app};
      out << "changed during backfill";
      out.close();
      std::filesystem::last_write_time(path, oldMtimePoint + std::chrono::seconds{10});
    }
  } // namespace

  TEST_CASE("AudioIdentityIndexer - fills pending available manifest rows", "[runtime][unit][library][audio-identity]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);
    copyFixture(musicRoot, "song.flac");

    auto ml = library::MusicLibrary{musicRoot, std::filesystem::path{temp.path()} / "db"};
    importWithPolicy(ml, AudioIdentityPolicy::DeferNew);
    CHECK_FALSE(manifestHasIdentity(ml, "song.flac"));

    auto indexer = AudioIdentityIndexer{ml};
    auto result = indexer.indexPending();

    REQUIRE(result);
    CHECK(result->completedCount == 1);
    CHECK(result->skippedCount == 0);
    CHECK(result->failureCount == 0);
    CHECK_FALSE(result->cancelled);
    CHECK(manifestHasIdentity(ml, "song.flac"));
  }

  TEST_CASE("AudioIdentityIndexer - skips rows whose file stat changed before write",
            "[runtime][unit][library][audio-identity]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);
    auto const trackPath = musicRoot / "song.flac";
    copyFixture(musicRoot, "song.flac");

    auto ml = library::MusicLibrary{musicRoot, std::filesystem::path{temp.path()} / "db"};
    importWithPolicy(ml, AudioIdentityPolicy::DeferNew);

    bool mutated = false;
    auto indexer = AudioIdentityIndexer{ml};
    auto result = indexer.indexPending(
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

    auto ml = library::MusicLibrary{musicRoot, std::filesystem::path{temp.path()} / "db"};
    importWithPolicy(ml, AudioIdentityPolicy::Eager);
    auto const originalIdentity = manifestIdentity(ml, "song.flac");

    auto indexer = AudioIdentityIndexer{ml};
    auto result = indexer.indexPending();

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

    auto ml = library::MusicLibrary{musicRoot, std::filesystem::path{temp.path()} / "db"};
    importWithPolicy(ml, AudioIdentityPolicy::DeferNew);

    auto stopSource = std::stop_source{};
    auto indexer = AudioIdentityIndexer{ml};
    auto result = indexer.indexPending(
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

    auto ml = library::MusicLibrary{musicRoot, std::filesystem::path{temp.path()} / "db"};
    importWithPolicy(ml, AudioIdentityPolicy::DeferNew, 2);

    // While the batch is being hashed, fill b's identity behind the indexer's
    // back: its in-transaction revalidation must skip b without losing a.
    bool mutated = false;
    auto indexer = AudioIdentityIndexer{ml};
    auto result = indexer.indexPending(
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
    CHECK(bIdentity.audioSignature == utility::fnv1a128("test-identity"));
  }

  TEST_CASE("AudioIdentityIndexer - cancellation flushes rows hashed earlier in the batch",
            "[runtime][unit][library][audio-identity]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "music";
    std::filesystem::create_directories(musicRoot);
    copyFixture(musicRoot, "a.flac");
    copyFixture(musicRoot, "b.flac");

    auto ml = library::MusicLibrary{musicRoot, std::filesystem::path{temp.path()} / "db"};
    importWithPolicy(ml, AudioIdentityPolicy::DeferNew, 2);

    auto stopSource = std::stop_source{};
    auto indexer = AudioIdentityIndexer{ml};
    auto result = indexer.indexPending(
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
