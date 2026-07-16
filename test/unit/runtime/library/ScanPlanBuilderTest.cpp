// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/FilesystemTestSupport.h"
#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/media/file/File.h>
#include <ao/media/flac/MetadataBlockLayout.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/utility/Hash128.h>
#include <ao/utility/Xxh3.h>
#include <runtime/library/ScanApplyOperation.h>
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
#include <type_traits>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("ScanPlan - is an opaque move-only value", "[runtime][unit][library][scan]")
  {
    STATIC_REQUIRE_FALSE(std::is_default_constructible_v<ScanPlan>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<ScanPlan>);
    STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<ScanPlan>);
    STATIC_REQUIRE(std::is_move_constructible_v<ScanPlan>);
    STATIC_REQUIRE(std::is_move_assignable_v<ScanPlan>);
  }

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

    Result<ScanPlan> attemptRelink(ScanPlan& plan, std::string_view oldUri, std::string_view newUri)
    {
      return std::move(plan).makeRelinkPlan(oldUri, newUri);
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
      auto fileResult = media::file::File::open(path);
      REQUIRE(fileResult);

      auto payloadResult = fileResult->audioPayload();
      REQUIRE(payloadResult);

      return AudioIdentity{.payloadLength = static_cast<std::uint64_t>(payloadResult->bytes.size()),
                           .signature = utility::xxh3Hash128(payloadResult->bytes)};
    }

    void putManifestEntry(library::MusicLibrary& ml, std::string_view uri, TrackId trackId, AudioIdentity identity)
    {
      auto transaction = library::test::writeTransaction(ml);
      auto builder = library::FileManifestBuilder::makeEmpty();
      builder.trackId(trackId).audioPayloadLength(identity.payloadLength).audioSignature(identity.signature);
      REQUIRE(ml.manifest().writer(transaction).put(uri, builder.serialize()));
      REQUIRE(transaction.commit());
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

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{root} / "db");

    // Setup manifest for existing files
    {
      auto transaction = library::test::writeTransaction(ml);
      auto manifestWriter = ml.manifest().writer(transaction);

      // Unchanged
      char const* const unchangedUri = "unchanged.mp3";
      auto builder1 = library::FileManifestBuilder::makeEmpty();
      builder1.trackId(TrackId{1})
        .fileSize(std::filesystem::file_size(musicRoot / unchangedUri))
        .mtime(
          static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::filesystem::last_write_time(musicRoot / unchangedUri).time_since_epoch())
                                       .count()));
      REQUIRE(manifestWriter.put(unchangedUri, builder1.serialize()));

      // Changed (different size)
      char const* const changedUri = "changed.m4a";
      auto builder2 = library::FileManifestBuilder::makeEmpty();
      builder2.trackId(TrackId{2}).fileSize(99999).mtime(0);
      REQUIRE(manifestWriter.put(changedUri, builder2.serialize()));

      // Missing (in manifest but not on disk)
      char const* const missingUri = "missing.flac";
      auto builder3 = library::FileManifestBuilder::makeEmpty();
      builder3.trackId(TrackId{3});
      REQUIRE(manifestWriter.put(missingUri, builder3.serialize()));

      REQUIRE(transaction.commit());
    }

    auto scanner = ScanPlanBuilder{ml};
    auto plan = scanner.buildPlan().value();

    CHECK(plan.count(ScanClassification::New) == 1);
    CHECK(plan.count(ScanClassification::Unchanged) == 1);
    CHECK(plan.count(ScanClassification::Changed) == 1);
    CHECK(plan.count(ScanClassification::Missing) == 1);

    // The non-audio file is filtered at the walk: it never enters the plan.
    CHECK(plan.size() == 4);

    // Verify specific items
    bool foundMissing = false;

    for (auto const& item : plan.items())
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

  TEST_CASE("ScanPlanBuilder - reports IO errors while scanning", "[runtime][unit][library-scan][error]")
  {
    auto const temp = ao::test::TempDir{};
    auto const& root = temp.path();
    auto const musicRoot = std::filesystem::path{root} / "music";
    std::filesystem::create_directories(musicRoot / "ok_dir");
    std::filesystem::create_directories(musicRoot / "restricted_dir");

    createFile(musicRoot / "ok_dir" / "song1.flac");
    createFile(musicRoot / "another.mp3");

    auto const denied =
      ao::test::ScopedDirectoryAccessGuard{musicRoot / "restricted_dir", ao::test::DeniedDirectoryAccess::Read};

    if (!denied.effective())
    {
      SKIP("the current process bypasses directory read restrictions");
    }

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{root} / "db");
    auto scanner = ScanPlanBuilder{ml};
    auto const plan = scanner.buildPlan().value();

    // We expect:
    // 1. ok_dir/song1.flac (New)
    // 2. another.mp3 (New)
    // 3. restricted_dir (Error)

    bool foundOk = false;
    bool foundAnother = false;
    bool foundRestricted = false;

    for (auto const& item : plan.items())
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

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");
    auto scanner = ScanPlanBuilder{ml};
    auto const plan = scanner.buildPlan().value();

    CHECK(plan.empty());
  }

  TEST_CASE("ScanPlanBuilder - treats missing roots as fatal", "[runtime][unit][library-scan][error]")
  {
    auto const temp = ao::test::TempDir{};
    // Point the library at a music root that does not exist. The database still
    // lives under a real directory, so the library itself opens cleanly and only
    // the scan fails - distinguishing "cannot scan" from "scanned an empty root".
    auto const musicRoot = std::filesystem::path{temp.path()} / "does_not_exist";

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");
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

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{root} / "db");
    putManifestEntry(ml, "old-name.flac", TrackId{42}, identity);

    auto scanner = ScanPlanBuilder{ml};
    auto const plan = scanner.buildPlan().value();

    REQUIRE(plan.size() == 1);
    auto const& item = plan.items().front();
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

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{root} / "db");
    putManifestEntry(ml, "old-title.flac", TrackId{42}, identity);

    auto scanner = ScanPlanBuilder{ml};
    auto const plan = scanner.buildPlan().value();

    REQUIRE(plan.size() == 1);
    auto const& item = plan.items().front();
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

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{root} / "db");
    putManifestEntry(ml,
                     "old-name.flac",
                     TrackId{42},
                     AudioIdentity{.payloadLength = actualIdentity.payloadLength, .signature = wrongSignature});

    auto scanner = ScanPlanBuilder{ml};
    auto const plan = scanner.buildPlan().value();

    CHECK(plan.count(ScanClassification::Moved) == 0);
    CHECK(plan.count(ScanClassification::New) == 1);
    CHECK(plan.count(ScanClassification::Missing) == 1);
    REQUIRE(plan.size() == 2);

    bool foundNew = false;
    bool foundMissing = false;

    for (auto const& item : plan.items())
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

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{root} / "db");
    putManifestEntry(ml, "old/copy-a.flac", TrackId{100}, identity);
    putManifestEntry(ml, "old/copy-b.flac", TrackId{200}, identity);

    auto scanner = ScanPlanBuilder{ml};
    auto plan = scanner.buildPlan().value();

    CHECK(plan.count(ScanClassification::Moved) == 0);
    CHECK(plan.count(ScanClassification::New) == 2);
    CHECK(plan.count(ScanClassification::Missing) == 2);
    CHECK(plan.size() == 4);

    auto invalidSourceResult = attemptRelink(plan, "old/not-found.flac", "disc-1/copy-a.flac");
    REQUIRE_FALSE(invalidSourceResult);
    CHECK(invalidSourceResult.error().code == Error::Code::InvalidInput);
    CHECK(plan.size() == 4);

    auto invalidDestinationResult = attemptRelink(plan, "old/copy-a.flac", "disc-3/not-found.flac");
    REQUIRE_FALSE(invalidDestinationResult);
    CHECK(invalidDestinationResult.error().code == Error::Code::InvalidInput);
    CHECK(plan.size() == 4);

    auto relinkResult = attemptRelink(plan, "old/copy-a.flac", "disc-1/copy-a.flac");
    REQUIRE(relinkResult);
    REQUIRE(relinkResult->size() == 1);
    auto const& relinkItem = relinkResult->items().front();
    CHECK(relinkItem.classification == ScanClassification::Moved);
    CHECK(relinkItem.oldUri == "old/copy-a.flac");
    CHECK(relinkItem.uri == "disc-1/copy-a.flac");
    CHECK(relinkItem.trackId == TrackId{100});

    auto consumedResult = ScanApplyOperation{ml, std::move(plan), nullptr, nullptr}.run();
    REQUIRE_FALSE(consumedResult);
    CHECK(consumedResult.error().code == Error::Code::InvalidState);
  }

  TEST_CASE("ScanPlanBuilder - explicit relink rejects pending and mismatched identities",
            "[runtime][unit][library][scan]")
  {
    SECTION("pending identity")
    {
      auto const temp = ao::test::TempDir{};
      auto const musicRoot = std::filesystem::path{temp.path()} / "music";
      std::filesystem::create_directories(musicRoot);
      std::filesystem::copy_file(audio::test::requireAudioFixture("basic_metadata.flac"), musicRoot / "new.flac");

      auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");
      putManifestEntry(ml, "old.flac", TrackId{100}, AudioIdentity{});
      auto plan = ScanPlanBuilder{ml}.buildPlan().value();

      auto result = std::move(plan).makeRelinkPlan("old.flac", "new.flac");
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::InvalidInput);
    }

    SECTION("different non-pending identities")
    {
      auto const temp = ao::test::TempDir{};
      auto const musicRoot = std::filesystem::path{temp.path()} / "music";
      std::filesystem::create_directories(musicRoot);
      auto const newFile = musicRoot / "new.flac";
      std::filesystem::copy_file(audio::test::requireAudioFixture("hires.flac"), newFile);

      auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{temp.path()} / "db");
      auto const basicIdentity = requireAudioIdentity(audio::test::requireAudioFixture("basic_metadata.flac"));
      auto competingIdentity = requireAudioIdentity(newFile);
      competingIdentity.signature.bytes.front() ^= std::byte{1};
      putManifestEntry(ml, "old-basic.flac", TrackId{100}, basicIdentity);
      putManifestEntry(ml, "old-competing.flac", TrackId{200}, competingIdentity);
      auto plan = ScanPlanBuilder{ml}.buildPlan().value();

      REQUIRE(plan.count(ScanClassification::New) == 1);
      REQUIRE(plan.count(ScanClassification::Missing) == 2);
      auto result = std::move(plan).makeRelinkPlan("old-basic.flac", "new.flac");
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::InvalidInput);
    }
  }

  TEST_CASE("ScanPlanBuilder - canonicalizes URI edge cases", "[runtime][unit][library-scan][uri]")
  {
    auto const temp = ao::test::TempDir{};
    auto const& root = temp.path();
    auto const musicRoot = std::filesystem::path{root} / "music";
    std::filesystem::create_directories(musicRoot / "nested" / "dir");

    createFile(musicRoot / "nested" / "dir" / "song.flac");

    auto ml = library::test::makeTestMusicLibrary(musicRoot, std::filesystem::path{root} / "db");
    auto scanner = ScanPlanBuilder{ml};
    auto const plan = scanner.buildPlan().value();

    REQUIRE(plan.size() == 1);

    // Verify that the computed URI is standard, generic, and uses forward slashes
    for (auto const& item : plan.items())
    {
      CHECK(item.uri == "nested/dir/song.flac");
      CHECK_FALSE(item.uri.contains('\\'));
      CHECK_FALSE(item.uri.contains("./"));
    }
  }

  TEST_CASE("ScanPlanBuilder - rejects file symlinks escaping the music root", "[runtime][unit][library-scan][uri]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = temp.path() / "music";
    auto const outsideRoot = temp.path() / "outside";
    std::filesystem::create_directories(musicRoot);
    std::filesystem::create_directories(outsideRoot);
    auto const outsideFile = outsideRoot / "outside.flac";
    std::filesystem::copy_file(audio::test::requireAudioFixture("basic_metadata.flac"), outsideFile);
    std::filesystem::create_symlink(outsideFile, musicRoot / "alias.flac");

    auto library = library::test::makeTestMusicLibrary(musicRoot, temp.path() / "db");
    putManifestEntry(library, "alias.flac", TrackId{42}, AudioIdentity{});
    auto const plan = ScanPlanBuilder{library}.buildPlan().value();

    REQUIRE(plan.size() == 1);
    CHECK(plan.count(ScanClassification::New) == 0);
    CHECK(plan.count(ScanClassification::Missing) == 0);
    CHECK(plan.count(ScanClassification::Error) == 1);
    CHECK(plan.items().front().uri == "alias.flac");
    CHECK(plan.items().front().errorMessage.contains("outside the library root"));
  }

  TEST_CASE("ScanPlanBuilder - in-root symlinks use one canonical target URI", "[runtime][unit][library-scan][uri]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = temp.path() / "music";
    std::filesystem::create_directories(musicRoot);
    auto const actualFile = musicRoot / "actual.flac";
    std::filesystem::copy_file(audio::test::requireAudioFixture("basic_metadata.flac"), actualFile);
    std::filesystem::create_symlink(actualFile, musicRoot / "alias.flac");

    auto library = library::test::makeTestMusicLibrary(musicRoot, temp.path() / "db");
    auto const plan = ScanPlanBuilder{library}.buildPlan().value();

    REQUIRE(plan.size() == 1);
    CHECK(plan.count(ScanClassification::New) == 1);
    CHECK(plan.items().front().uri == "actual.flac");
    CHECK(plan.items().front().fullPath == std::filesystem::canonical(actualFile));
  }

  TEST_CASE("ScanPlanBuilder - an escaping directory symlink does not mark descendants missing",
            "[runtime][regression][library-scan][uri]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = temp.path() / "music";
    auto const outsideRoot = temp.path() / "outside";
    std::filesystem::create_directories(musicRoot);
    std::filesystem::create_directories(outsideRoot);
    std::filesystem::copy_file(audio::test::requireAudioFixture("basic_metadata.flac"), outsideRoot / "song.flac");
    std::filesystem::create_directory_symlink(outsideRoot, musicRoot / "alias");

    auto library = library::test::makeTestMusicLibrary(musicRoot, temp.path() / "db");
    putManifestEntry(library, "alias/song.flac", TrackId{42}, AudioIdentity{});
    auto const plan = ScanPlanBuilder{library}.buildPlan().value();

    REQUIRE(plan.size() == 1);
    CHECK(plan.count(ScanClassification::Missing) == 0);
    CHECK(plan.count(ScanClassification::Error) == 1);
    CHECK(plan.items().front().uri == "alias");
  }
} // namespace ao::rt::test
