// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/library/LibraryScan.h>

#include "runtime/library/ScanApplyOperation.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/Exception.h>
#include <ao/async/OperationCancelled.h>
#include <ao/library/AudioIdentity.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/TrackStore.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <stop_token>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    void copyBasicAudioFixture(MusicLibraryFixture& libraryFixture, std::string_view uri = "song.flac")
    {
      auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
      std::filesystem::copy_file(sourceFile, libraryFixture.root() / std::filesystem::path{uri});
    }
  } // namespace

  TEST_CASE("LibraryScan - buildPlan reports new audio files", "[runtime][unit][library][scan]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    copyBasicAudioFixture(libraryFixture);

    auto service = LibraryScan{libraryFixture.library()};
    auto progressPaths = std::vector<std::filesystem::path>{};
    auto result =
      service.buildPlan([&progressPaths](std::filesystem::path const& path) { progressPaths.push_back(path); });

    REQUIRE(result);
    REQUIRE(result->size() == 1);
    CHECK(result->items()[0].uri == "song.flac");
    CHECK(result->items()[0].classification == ScanClassification::New);
    CHECK(std::ranges::any_of(
      progressPaths, [](std::filesystem::path const& path) { return path.filename() == "song.flac"; }));
  }

  TEST_CASE("LibraryScan - post-open corrupt manifest iteration fails fast", "[runtime][regression][scan][integrity]")
  {
    auto libraryFixture = MusicLibraryFixture{};

    {
      auto transaction = library::test::writeTransaction(libraryFixture.library());
      auto const payload = library::FileManifestBuilder::makeEmpty().trackId(TrackId{1}).serialize();
      REQUIRE(libraryFixture.library().manifest().writer(transaction).put("a.flac", payload));
      REQUIRE(transaction.commit());
    }

    {
      auto environmentRes = lmdb::Environment::open(
        libraryFixture.root().string(),
        {.flags = lmdb::kEnvNoTls, .maxDatabases = 8, .mapSize = library::test::kTestMusicLibraryMapSize});
      REQUIRE(environmentRes);
      auto environment = std::move(*environmentRes);
      auto transactionRes = lmdb::WriteTransaction::begin(environment);
      REQUIRE(transactionRes);
      auto transaction = std::move(*transactionRes);
      auto manifestRes = lmdb::Database::open(transaction, "file_manifest", lmdb::Database::KeyKind::Blob);
      REQUIRE(manifestRes);
      auto const malformedKey = utility::bytes::view(std::string_view{"zz"});
      auto const payload = library::FileManifestBuilder::makeEmpty().trackId(TrackId{2}).serialize();
      REQUIRE(manifestRes->writer(transaction).create(malformedKey, payload));
      REQUIRE(transaction.commit());
    }

    CHECK_THROWS_AS(std::ignore = LibraryScan{libraryFixture.library()}.buildPlan(), Exception);
  }

  TEST_CASE("LibraryScan - applyPlan imports new tracks", "[runtime][unit][library][scan]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    copyBasicAudioFixture(libraryFixture);

    auto service = LibraryScan{libraryFixture.library()};
    auto plan = service.buildPlan().value();
    REQUIRE(plan.count(ScanClassification::New) == 1);

    auto operation = ScanApplyOperation{libraryFixture.library(), std::move(plan), {}, {}};
    auto result = operation.run();

    REQUIRE(result);
    REQUIRE(result->insertedIds.size() == 1);
    CHECK(result->failureCount == 0);
    CHECK_FALSE(operation.cancelled());

    auto transaction = libraryFixture.library().readTransaction();
    auto trackReader = libraryFixture.library().tracks().reader(transaction);
    auto optTrack = trackReader.get(result->insertedIds[0]);
    REQUIRE(optTrack);
    CHECK(optTrack->metadata().title() == "Test Title");

    auto optManifest = libraryFixture.library().manifest().reader(transaction).get("song.flac");
    REQUIRE(optManifest);
    CHECK(optManifest->trackId() == result->insertedIds[0]);
    CHECK(library::hasAudioIdentity(optManifest->audioPayloadLength(), optManifest->audioSignature()));
  }

  TEST_CASE("LibraryScan - applyPlan can defer new audio identity", "[runtime][unit][library][scan]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    copyBasicAudioFixture(libraryFixture);

    auto service = LibraryScan{libraryFixture.library()};
    auto plan = service.buildPlan().value();
    REQUIRE(plan.count(ScanClassification::New) == 1);

    auto result = ScanApplyOperation{libraryFixture.library(),
                                     std::move(plan),
                                     {},
                                     {},
                                     ScanApplyOptions{.audioIdentityPolicy = AudioIdentityPolicy::DeferNew}}
                    .run();

    REQUIRE(result);
    REQUIRE(result->insertedIds.size() == 1);
    CHECK(result->failureCount == 0);

    auto transaction = libraryFixture.library().readTransaction();
    auto optManifest = libraryFixture.library().manifest().reader(transaction).get("song.flac");
    REQUIRE(optManifest);
    CHECK_FALSE(library::hasAudioIdentity(optManifest->audioPayloadLength(), optManifest->audioSignature()));
  }

  TEST_CASE("LibraryScan - applyPlan honors cancellation", "[runtime][unit][library][scan]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    copyBasicAudioFixture(libraryFixture);

    auto service = LibraryScan{libraryFixture.library()};
    auto plan = service.buildPlan().value();
    REQUIRE(plan.count(ScanClassification::New) == 1);

    auto stopSource = std::stop_source{};
    stopSource.request_stop();
    auto operation = ScanApplyOperation{libraryFixture.library(), std::move(plan), {}, {}, {}};
    REQUIRE_THROWS_AS(operation.run(stopSource.get_token()), async::OperationCancelled);
    CHECK(operation.cancelled());

    auto transaction = libraryFixture.library().readTransaction();
    auto trackReader = libraryFixture.library().tracks().reader(transaction);
    auto manifestReader = libraryFixture.library().manifest().reader(transaction);
    CHECK(trackReader.begin() == trackReader.end());
    CHECK(manifestReader.begin() == manifestReader.end());
  }
} // namespace ao::rt::test
