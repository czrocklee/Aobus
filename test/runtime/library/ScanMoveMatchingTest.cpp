// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/library/ScanMoveMatching.h"

#include "test/TestFixtureSupport.h"
#include "test/audio/AudioFixtureSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/AudioIdentity.h>
#include <ao/media/file/File.h>
#include <ao/rt/library/ScanPlan.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    constexpr auto kOldUri = "Album.flac";
    constexpr auto kNewUri = "album.flac";
    constexpr auto kTrackId = TrackId{17};
    constexpr auto kOtherTrackId = TrackId{28};
    constexpr auto kAudioFixture = "basic_metadata.flac";
    constexpr auto kOtherAudioFixture = "hires.flac";

    library::AudioIdentity readFixtureIdentity(std::string_view fileName)
    {
      auto file = ao::test::requireValue(media::file::File::open(audio::test::requireAudioFixture(fileName)));
      auto const payload = ao::test::requireValue(file.audioPayload());
      auto const optIdentity = library::readAudioIdentity(payload.bytes);
      REQUIRE(optIdentity);
      return *optIdentity;
    }

    ScanItem makeNewItem(std::filesystem::path const& root,
                         std::string_view uri,
                         std::string_view fixtureName = kAudioFixture)
    {
      auto const path = root / audio::test::installAudioFixture(root, fixtureName, uri);
      return {.uri = std::string{uri},
              .fullPath = path,
              .classification = ScanClassification::New,
              .fileSize = std::filesystem::file_size(path)};
    }

    ScanItem makeMissingItem(std::string_view uri, library::AudioIdentity const& identity, TrackId trackId)
    {
      return {.uri = std::string{uri},
              .classification = ScanClassification::Missing,
              .audioPayloadLength = identity.payloadLength,
              .audioSignature = identity.signature,
              .trackId = trackId,
              .optManifestEvidence = ScanManifestEvidence{
                .audioPayloadLength = identity.payloadLength, .audioSignature = identity.signature}};
    }

    ScanItem const& requireEntry(std::vector<ScanItem> const& items, std::string_view uri)
    {
      CAPTURE(uri);
      auto const found = std::ranges::find(items, uri, &ScanItem::uri);
      REQUIRE(found != items.end());
      return *found;
    }

    void checkRefusedPair(std::vector<ScanItem> const& items,
                          std::string_view oldUri,
                          std::string_view newUri,
                          TrackId trackId)
    {
      CAPTURE(oldUri, newUri);
      auto const& source = requireEntry(items, oldUri);
      auto const& destination = requireEntry(items, newUri);
      CHECK(source.classification == ScanClassification::Error);
      CHECK(source.trackId == trackId);
      CHECK_FALSE(source.errorMessage.empty());
      CHECK(destination.classification == ScanClassification::Error);
      CHECK(destination.oldUri.empty());
      CHECK(destination.trackId == kInvalidTrackId);
      CHECK_FALSE(destination.errorMessage.empty());
    }

    void checkNewEntry(std::vector<ScanItem> const& items, std::string_view uri)
    {
      CAPTURE(uri);
      auto const& item = requireEntry(items, uri);
      CHECK(item.classification == ScanClassification::New);
      CHECK(item.oldUri.empty());
      CHECK(item.trackId == kInvalidTrackId);
    }
  } // namespace

  TEST_CASE("Scan move matching - a unique designated spelling match preserves the original Track")
  {
    auto const temp = ao::test::TempDir{};
    auto const identity = readFixtureIdentity(kAudioFixture);
    constexpr auto kOrdinaryOldUri = "old.flac";
    constexpr auto kOrdinaryNewUri = "renamed.flac";
    auto items = std::vector{makeNewItem(temp.path(), kNewUri),
                             makeMissingItem(kOrdinaryOldUri, readFixtureIdentity(kOtherAudioFixture), kOtherTrackId),
                             makeNewItem(temp.path(), kOrdinaryNewUri, kOtherAudioFixture),
                             makeMissingItem(kOldUri, identity, kTrackId)};

    // A supplied fallback pair represents already verified regular-file topology, independent of the host filesystem.
    auto const sources = classifyMovedEntries(items, {{kOldUri, kNewUri}}, {});

    REQUIRE(sources.size() == 2);
    CHECK(requireEntry(sources, kOldUri).trackId == kTrackId);
    CHECK(requireEntry(sources, kOrdinaryOldUri).trackId == kOtherTrackId);
    REQUIRE(items.size() == 2);
    auto const& destination = requireEntry(items, kNewUri);
    CHECK(destination.classification == ScanClassification::Moved);
    CHECK(destination.trackId == kTrackId);
    CHECK(destination.oldUri == kOldUri);
    CHECK(destination.audioPayloadLength == identity.payloadLength);
    CHECK(destination.audioSignature == identity.signature);
    auto const& ordinaryMove = requireEntry(items, kOrdinaryNewUri);
    CHECK(ordinaryMove.classification == ScanClassification::Moved);
    CHECK(ordinaryMove.trackId == kOtherTrackId);
    CHECK(ordinaryMove.oldUri == kOrdinaryOldUri);
  }

  TEST_CASE("Scan move matching - an unproven spelling pair does not become a Missing source or New import")
  {
    auto const temp = ao::test::TempDir{};
    auto identity = readFixtureIdentity(kAudioFixture);
    auto destination = makeNewItem(temp.path(), kNewUri);

    SECTION("the destination FLAC has no audio payload")
    {
      // Keep the regular file and its FLAC metadata, removing only the encoded audio suffix.
      REQUIRE(destination.fileSize > identity.payloadLength);
      destination.fileSize -= identity.payloadLength;
      std::filesystem::resize_file(destination.fullPath, destination.fileSize);
      auto file = ao::test::requireValue(media::file::File::open(destination.fullPath));
      REQUIRE_FALSE(file.audioPayload());
    }

    SECTION("the stored identity is pending")
    {
      identity = {};
    }

    SECTION("equal payload lengths have different signatures")
    {
      identity.signature.bytes.front() ^= std::byte{1};
      REQUIRE(library::hasAudioIdentity(identity.payloadLength, identity.signature));
    }

    constexpr auto kIndependentUri = "independent.flac";
    auto items = std::vector{destination,
                             makeMissingItem(kOldUri, identity, kTrackId),
                             makeNewItem(temp.path(), kIndependentUri, kOtherAudioFixture)};

    CHECK(classifyMovedEntries(items, {{kOldUri, kNewUri}}, {}).empty());

    CHECK(items.size() == 3);
    checkRefusedPair(items, kOldUri, kNewUri, kTrackId);
    checkNewEntry(items, kIndependentUri);
  }

  TEST_CASE("Scan move matching - duplicate identities prevent even a designated spelling match")
  {
    auto const temp = ao::test::TempDir{};
    auto const identity = readFixtureIdentity(kAudioFixture);
    auto items = std::vector{makeNewItem(temp.path(), kNewUri), makeMissingItem(kOldUri, identity, kTrackId)};
    constexpr auto kDuplicateUri = "duplicate.flac";
    auto duplicateClassification = ScanClassification::New;

    SECTION("a second New file has the same payload")
    {
      items.push_back(makeNewItem(temp.path(), kDuplicateUri));
    }

    SECTION("a second Missing row has the same payload")
    {
      items.push_back(makeMissingItem(kDuplicateUri, identity, kOtherTrackId));
      duplicateClassification = ScanClassification::Missing;
    }

    CHECK(classifyMovedEntries(items, {{kOldUri, kNewUri}}, {}).empty());

    CHECK(items.size() == 3);
    checkRefusedPair(items, kOldUri, kNewUri, kTrackId);
    CHECK(requireEntry(items, kDuplicateUri).classification == duplicateClassification);
  }

  TEST_CASE("Scan move matching - unrelated unique fingerprints cannot claim either endpoint of a spelling pair")
  {
    auto const temp = ao::test::TempDir{};
    constexpr auto kCopyUri = "unrelated-copy.flac";
    constexpr auto kOtherOldUri = "unrelated-old.flac";
    // Each payload has exactly one Missing source and one New destination, but neither match is the designated pair.
    auto items = std::vector{makeMissingItem(kOldUri, readFixtureIdentity(kAudioFixture), kTrackId),
                             makeNewItem(temp.path(), kNewUri, kOtherAudioFixture),
                             makeNewItem(temp.path(), kCopyUri),
                             makeMissingItem(kOtherOldUri, readFixtureIdentity(kOtherAudioFixture), kOtherTrackId)};

    CHECK(classifyMovedEntries(items, {{kOldUri, kNewUri}}, {}).empty());

    CHECK(items.size() == 4);
    checkRefusedPair(items, kOldUri, kNewUri, kTrackId);
    checkNewEntry(items, kCopyUri);
    auto const& unrelatedSource = requireEntry(items, kOtherOldUri);
    CHECK(unrelatedSource.classification == ScanClassification::Missing);
    CHECK(unrelatedSource.trackId == kOtherTrackId);
  }

  TEST_CASE("Scan move matching - two protected spelling pairs cannot swap destinations")
  {
    auto const temp = ao::test::TempDir{};
    constexpr auto kOtherOldUri = "Disc.flac";
    constexpr auto kOtherNewUri = "disc.flac";
    auto items = std::vector{makeMissingItem(kOldUri, readFixtureIdentity(kAudioFixture), kTrackId),
                             makeNewItem(temp.path(), kNewUri, kOtherAudioFixture),
                             makeMissingItem(kOtherOldUri, readFixtureIdentity(kOtherAudioFixture), kOtherTrackId),
                             makeNewItem(temp.path(), kOtherNewUri)};

    CHECK(classifyMovedEntries(items, {{kOldUri, kNewUri}, {kOtherOldUri, kOtherNewUri}}, {}).empty());

    CHECK(items.size() == 4);
    checkRefusedPair(items, kOldUri, kNewUri, kTrackId);
    checkRefusedPair(items, kOtherOldUri, kOtherNewUri, kOtherTrackId);
  }

  TEST_CASE("Scan move matching - a protected carrier with wrong extent keeps its CUE failure reason")
  {
    auto const temp = ao::test::TempDir{};
    auto source = makeMissingItem(kOldUri, readFixtureIdentity(kAudioFixture), kInvalidTrackId);
    REQUIRE(source.optManifestEvidence);
    // The fixture contains only 44,100 frames but the stored segments span 88,200.
    source.optManifestEvidence->sampleRate = 44'100;
    source.optManifestEvidence->totalFrames = 88'200;
    source.optManifestEvidence->members = {
      {.trackId = kTrackId, .segment = {.frameCount = 44'100, .ordinal = 1}},
      {.trackId = kOtherTrackId, .segment = {.startFrame = 44'100, .frameCount = 44'100, .ordinal = 2}}};
    auto items = std::vector{makeNewItem(temp.path(), kNewUri), source};

    CHECK(classifyMovedEntries(items, {{kOldUri, kNewUri}}, {}).empty());

    CHECK(items.size() == 2);
    checkRefusedPair(items, kOldUri, kNewUri, kInvalidTrackId);
    auto const& destination = requireEntry(items, kNewUri);
    CHECK(destination.cueDiagnostic == ScanCueDiagnostic::CarrierFactsMismatch);
    CHECK(destination.errorMessage.contains("extent"));
  }
} // namespace ao::rt::test
