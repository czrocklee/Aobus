// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureUtils.h"
#include <ao/CoreIds.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/media/flac/MetadataBlockLayout.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/tag/TagFile.h>
#include <ao/utility/Hash128.h>
#include <ao/utility/Xxh3.h>
#include <runtime/library/ScanPlanBuilder.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    struct AudioIdentity final
    {
      std::uint64_t payloadLength = 0;
      utility::Hash128 signature = {};
    };

    void createFile(std::filesystem::path const& path)
    {
      auto f = std::ofstream{path};
      f << "dummy";
    }

    void writeBinaryFile(std::filesystem::path const& path, std::vector<std::uint8_t> const& data)
    {
      auto f = std::ofstream{path, std::ios::binary};
      REQUIRE(f);
      f.write(reinterpret_cast<char const*>(data.data()), static_cast<std::streamsize>(data.size()));
      REQUIRE(f.good());
    }

    void addFlacBlockHeader(std::vector<std::uint8_t>& data,
                            media::flac::MetadataBlockType type,
                            bool isLast,
                            std::uint32_t size)
    {
      auto first = static_cast<std::uint8_t>(type);

      if (isLast)
      {
        first |= 0x80;
      }

      data.push_back(first);
      data.push_back(static_cast<std::uint8_t>((size >> 16U) & 0xFFU));
      data.push_back(static_cast<std::uint8_t>((size >> 8U) & 0xFFU));
      data.push_back(static_cast<std::uint8_t>(size & 0xFFU));
    }

    std::vector<std::uint8_t> createRetaggableFlac(std::string_view title,
                                                   std::vector<std::uint8_t> const& audioPayload)
    {
      auto data = std::vector<std::uint8_t>{'f', 'L', 'a', 'C'};

      addFlacBlockHeader(data, media::flac::MetadataBlockType::StreamInfo, false, 34);
      auto streamInfo = media::flac::StreamInfoLayout{};
      streamInfo.packedFields = (44100ULL << 44U) | (1ULL << 41U) | (15ULL << 36U) | 44100ULL;
      auto const* streamInfoBytes = reinterpret_cast<std::uint8_t const*>(&streamInfo);
      data.insert(data.end(), streamInfoBytes, streamInfoBytes + media::flac::StreamInfoLayout::kSize);

      auto comments = std::vector<std::uint8_t>{};
      auto addCommentString = [&comments](std::string_view text)
      {
        auto const size = static_cast<std::uint32_t>(text.size());
        comments.push_back(static_cast<std::uint8_t>(size & 0xFFU));
        comments.push_back(static_cast<std::uint8_t>((size >> 8U) & 0xFFU));
        comments.push_back(static_cast<std::uint8_t>((size >> 16U) & 0xFFU));
        comments.push_back(static_cast<std::uint8_t>((size >> 24U) & 0xFFU));

        for (char const ch : text)
        {
          comments.push_back(static_cast<std::uint8_t>(static_cast<unsigned char>(ch)));
        }
      };

      addCommentString("Aobus Test");
      comments.push_back(1);
      comments.push_back(0);
      comments.push_back(0);
      comments.push_back(0);

      auto titleComment = std::string{"TITLE="};
      titleComment += title;
      addCommentString(titleComment);

      addFlacBlockHeader(
        data, media::flac::MetadataBlockType::VorbisComment, true, static_cast<std::uint32_t>(comments.size()));
      data.insert(data.end(), comments.begin(), comments.end());
      data.insert(data.end(), audioPayload.begin(), audioPayload.end());

      return data;
    }

    AudioIdentity requireAudioIdentity(std::filesystem::path const& path)
    {
      auto tagFileResult = tag::TagFile::open(path);
      REQUIRE(tagFileResult);

      auto payloadResult = (*tagFileResult)->audioPayload();
      REQUIRE(payloadResult);

      return AudioIdentity{.payloadLength = static_cast<std::uint64_t>(payloadResult->bytes.size()),
                           .signature = utility::xxh3Hash128(payloadResult->bytes)};
    }

    void putManifestEntry(library::MusicLibrary& ml, std::string_view uri, TrackId trackId, AudioIdentity identity)
    {
      auto txn = ml.writeTransaction();
      auto builder = library::FileManifestBuilder::createNew();
      builder.trackId(trackId).audioPayloadLength(identity.payloadLength).audioSignature(identity.signature);
      REQUIRE(ml.manifest().writer(txn).put(uri, builder.serialize()));
      REQUIRE(txn.commit());
    }
  } // namespace

  TEST_CASE("ScanPlanBuilder - classifies supported and hidden entries", "[runtime][unit][library][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const& root = temp.path();
    auto const musicRoot = std::filesystem::path{root} / "music";
    std::filesystem::create_directories(musicRoot);

    createFile(musicRoot / "new.flac");
    createFile(musicRoot / "unchanged.mp3");
    createFile(musicRoot / "changed.m4a");
    createFile(musicRoot / "unsupported.txt");

    auto ml = library::MusicLibrary{musicRoot, std::filesystem::path{root} / "db"};

    // Setup manifest for existing files
    {
      auto txn = ml.writeTransaction();
      auto manifestWriter = ml.manifest().writer(txn);

      // Unchanged
      char const* const unchangedUri = "unchanged.mp3";
      auto builder1 = library::FileManifestBuilder::createNew();
      builder1.trackId(TrackId{1})
        .fileSize(std::filesystem::file_size(musicRoot / unchangedUri))
        .mtime(
          static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::filesystem::last_write_time(musicRoot / unchangedUri).time_since_epoch())
                                       .count()));
      REQUIRE(manifestWriter.put(unchangedUri, builder1.serialize()));

      // Changed (different size)
      char const* const changedUri = "changed.m4a";
      auto builder2 = library::FileManifestBuilder::createNew();
      builder2.trackId(TrackId{2}).fileSize(99999).mtime(0);
      REQUIRE(manifestWriter.put(changedUri, builder2.serialize()));

      // Missing (in manifest but not on disk)
      char const* const missingUri = "missing.flac";
      auto builder3 = library::FileManifestBuilder::createNew();
      builder3.trackId(TrackId{3});
      REQUIRE(manifestWriter.put(missingUri, builder3.serialize()));

      REQUIRE(txn.commit());
    }

    auto scanner = ScanPlanBuilder{ml};
    auto const plan = scanner.buildPlan().value();

    CHECK(plan.count(ScanClassification::New) == 1);
    CHECK(plan.count(ScanClassification::Unchanged) == 1);
    CHECK(plan.count(ScanClassification::Changed) == 1);
    CHECK(plan.count(ScanClassification::Missing) == 1);

    // The non-audio file is filtered at the walk: it never enters the plan.
    CHECK(plan.items.size() == 4);

    // Verify specific items
    bool foundMissing = false;

    for (auto const& item : plan.items)
    {
      CHECK(item.uri != "unsupported.txt");

      if (item.uri == "missing.flac")
      {
        CHECK(item.classification == ScanClassification::Missing);
        CHECK(item.trackId == TrackId{3});
        foundMissing = true;
      }
    }

    CHECK(foundMissing);
  }

  TEST_CASE("ScanPlanBuilder - reports IO errors while scanning", "[runtime][unit][library][scan][error]")
  {
    auto const temp = ao::test::TempDir{};
    auto const& root = temp.path();
    auto const musicRoot = std::filesystem::path{root} / "music";
    std::filesystem::create_directories(musicRoot / "ok_dir");
    std::filesystem::create_directories(musicRoot / "restricted_dir");

    createFile(musicRoot / "ok_dir" / "song1.flac");
    createFile(musicRoot / "another.mp3");

    // Make restricted_dir inaccessible.
    // permissions() is a no-op when running as root, so skip in that case.
    std::filesystem::permissions(musicRoot / "restricted_dir", std::filesystem::perms::none);

    if (::geteuid() == 0)
    {
      SKIP("permissions test is meaningless when running as root");
    }

    auto ml = library::MusicLibrary{musicRoot, std::filesystem::path{root} / "db"};
    auto scanner = ScanPlanBuilder{ml};
    auto const plan = scanner.buildPlan().value();

    // Reset permissions so ao::test::TempDir can clean up
    std::filesystem::permissions(musicRoot / "restricted_dir", std::filesystem::perms::owner_all);

    // We expect:
    // 1. ok_dir/song1.flac (New)
    // 2. another.mp3 (New)
    // 3. restricted_dir (Error)

    bool foundOk = false;
    bool foundAnother = false;
    bool foundRestricted = false;

    for (auto const& item : plan.items)
    {
      if (item.uri == "ok_dir/song1.flac")
      {
        CHECK(item.classification == ScanClassification::New);
        foundOk = true;
      }
      else if (item.uri == "another.mp3")
      {
        CHECK(item.classification == ScanClassification::New);
        foundAnother = true;
      }
      else if (item.uri == "restricted_dir")
      {
        CHECK(item.classification == ScanClassification::Error);
        foundRestricted = true;
      }
    }

    CHECK(foundOk);
    CHECK(foundAnother);
    CHECK(foundRestricted);
  }

  TEST_CASE("ScanPlanBuilder - handles empty roots", "[runtime][unit][library][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = std::filesystem::path{temp.path()} / "empty_music";
    std::filesystem::create_directories(musicRoot);

    auto ml = library::MusicLibrary{musicRoot, std::filesystem::path{temp.path()} / "db"};
    auto scanner = ScanPlanBuilder{ml};
    auto const plan = scanner.buildPlan().value();

    CHECK(plan.items.empty());
  }

  TEST_CASE("ScanPlanBuilder - treats missing roots as fatal", "[runtime][unit][library][scan][error]")
  {
    auto const temp = ao::test::TempDir{};
    // Point the library at a music root that does not exist. The database still
    // lives under a real directory, so the library itself opens cleanly and only
    // the scan fails - distinguishing "cannot scan" from "scanned an empty root".
    auto const musicRoot = std::filesystem::path{temp.path()} / "does_not_exist";

    auto ml = library::MusicLibrary{musicRoot, std::filesystem::path{temp.path()} / "db"};
    auto scanner = ScanPlanBuilder{ml};
    auto const result = scanner.buildPlan();

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::NotFound);
  }

  TEST_CASE("ScanPlanBuilder - classifies unambiguous moved files by audio identity", "[runtime][unit][library][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const& root = temp.path();
    auto const musicRoot = std::filesystem::path{root} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const movedFile = musicRoot / "renamed.flac";
    std::filesystem::copy_file(sourceFile, movedFile);
    auto const identity = requireAudioIdentity(movedFile);

    auto ml = library::MusicLibrary{musicRoot, std::filesystem::path{root} / "db"};
    putManifestEntry(ml, "old-name.flac", TrackId{42}, identity);

    auto scanner = ScanPlanBuilder{ml};
    auto const plan = scanner.buildPlan().value();

    REQUIRE(plan.items.size() == 1);
    auto const& item = plan.items.front();
    CHECK(item.classification == ScanClassification::Moved);
    CHECK(item.uri == "renamed.flac");
    CHECK(item.oldUri == "old-name.flac");
    CHECK(item.trackId == TrackId{42});
    CHECK(item.audioPayloadLength == identity.payloadLength);
    CHECK(item.audioSignature == identity.signature);
    CHECK(plan.count(ScanClassification::Missing) == 0);
    CHECK(plan.count(ScanClassification::New) == 0);
  }

  TEST_CASE("ScanPlanBuilder - relinks moved files after metadata retag", "[runtime][unit][library][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const& root = temp.path();
    auto const musicRoot = std::filesystem::path{root} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const audioPayload = std::vector<std::uint8_t>{0xC0, 0xC1, 0xC2, 0xC3};
    auto const retaggedFile = musicRoot / "retagged.flac";
    writeBinaryFile(retaggedFile, createRetaggableFlac("Retagged Title", audioPayload));

    auto const identity = AudioIdentity{.payloadLength = static_cast<std::uint64_t>(audioPayload.size()),
                                        .signature = utility::xxh3Hash128(std::as_bytes(std::span{audioPayload}))};

    auto ml = library::MusicLibrary{musicRoot, std::filesystem::path{root} / "db"};
    putManifestEntry(ml, "old-title.flac", TrackId{42}, identity);

    auto scanner = ScanPlanBuilder{ml};
    auto const plan = scanner.buildPlan().value();

    REQUIRE(plan.items.size() == 1);
    auto const& item = plan.items.front();
    CHECK(item.classification == ScanClassification::Moved);
    CHECK(item.uri == "retagged.flac");
    CHECK(item.oldUri == "old-title.flac");
    CHECK(item.trackId == TrackId{42});
    CHECK(item.audioPayloadLength == identity.payloadLength);
    CHECK(item.audioSignature == identity.signature);
  }

  TEST_CASE("ScanPlanBuilder - leaves equal-length signature mismatches unresolved", "[runtime][unit][library][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const& root = temp.path();
    auto const musicRoot = std::filesystem::path{root} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const movedFile = musicRoot / "renamed.flac";
    std::filesystem::copy_file(sourceFile, movedFile);

    auto const actualIdentity = requireAudioIdentity(movedFile);
    auto wrongSignature = actualIdentity.signature;
    wrongSignature.bytes[0] = static_cast<std::byte>(std::to_integer<std::uint8_t>(wrongSignature.bytes[0]) ^ 0xFFU);

    auto ml = library::MusicLibrary{musicRoot, std::filesystem::path{root} / "db"};
    putManifestEntry(ml,
                     "old-name.flac",
                     TrackId{42},
                     AudioIdentity{.payloadLength = actualIdentity.payloadLength, .signature = wrongSignature});

    auto scanner = ScanPlanBuilder{ml};
    auto const plan = scanner.buildPlan().value();

    CHECK(plan.count(ScanClassification::Moved) == 0);
    CHECK(plan.count(ScanClassification::New) == 1);
    CHECK(plan.count(ScanClassification::Missing) == 1);
    REQUIRE(plan.items.size() == 2);

    bool foundNew = false;
    bool foundMissing = false;

    for (auto const& item : plan.items)
    {
      if (item.classification == ScanClassification::New)
      {
        CHECK(item.uri == "renamed.flac");
        CHECK(item.audioPayloadLength == actualIdentity.payloadLength);
        CHECK(item.audioSignature == actualIdentity.signature);
        foundNew = true;
      }
      else if (item.classification == ScanClassification::Missing)
      {
        CHECK(item.uri == "old-name.flac");
        CHECK(item.audioPayloadLength == actualIdentity.payloadLength);
        CHECK(item.audioSignature == wrongSignature);
        foundMissing = true;
      }
    }

    CHECK(foundNew);
    CHECK(foundMissing);
  }

  TEST_CASE("ScanPlanBuilder - leaves duplicate-content moves unresolved", "[runtime][unit][library][scan]")
  {
    auto const temp = ao::test::TempDir{};
    auto const& root = temp.path();
    auto const musicRoot = std::filesystem::path{root} / "music";
    std::filesystem::create_directories(musicRoot);

    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto const firstMovedFile = musicRoot / "disc-1" / "copy-a.flac";
    auto const secondMovedFile = musicRoot / "disc-2" / "copy-b.flac";
    std::filesystem::create_directories(firstMovedFile.parent_path());
    std::filesystem::create_directories(secondMovedFile.parent_path());
    std::filesystem::copy_file(sourceFile, firstMovedFile);
    std::filesystem::copy_file(sourceFile, secondMovedFile);
    auto const identity = requireAudioIdentity(firstMovedFile);

    auto ml = library::MusicLibrary{musicRoot, std::filesystem::path{root} / "db"};
    putManifestEntry(ml, "old/copy-a.flac", TrackId{100}, identity);
    putManifestEntry(ml, "old/copy-b.flac", TrackId{200}, identity);

    auto scanner = ScanPlanBuilder{ml};
    auto const plan = scanner.buildPlan().value();

    CHECK(plan.count(ScanClassification::Moved) == 0);
    CHECK(plan.count(ScanClassification::New) == 2);
    CHECK(plan.count(ScanClassification::Missing) == 2);
    CHECK(plan.items.size() == 4);
  }

  TEST_CASE("ScanPlanBuilder - canonicalizes URI edge cases", "[runtime][unit][library][scan][uri]")
  {
    auto const temp = ao::test::TempDir{};
    auto const& root = temp.path();
    auto const musicRoot = std::filesystem::path{root} / "music";
    std::filesystem::create_directories(musicRoot / "nested" / "dir");

    createFile(musicRoot / "nested" / "dir" / "song.flac");

    auto ml = library::MusicLibrary{musicRoot, std::filesystem::path{root} / "db"};
    auto scanner = ScanPlanBuilder{ml};
    auto const plan = scanner.buildPlan().value();

    REQUIRE(plan.items.size() == 1);

    // Verify that the computed URI is standard, generic, and uses forward slashes
    for (auto const& item : plan.items)
    {
      CHECK(item.uri == "nested/dir/song.flac");
      CHECK(item.uri.find('\\') == std::string::npos);
      CHECK(item.uri.find("./") == std::string::npos);
    }
  }
} // namespace ao::rt::test
