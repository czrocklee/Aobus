// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/library/CueBinding.h"
#include "runtime/library/ScanApplyOperation.h"
#include "test/FilesystemTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/audio/AudioFixtureSupport.h"
#include "test/library/MusicLibraryTestSupport.h"
#include "test/runtime/ExecutorTestSupport.h"
#include "test/runtime/RuntimeLibraryTestSupport.h"
#include "test/runtime/library/ScanApplyTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibraryScan.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/utility/Hash128.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <utility>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace ao::rt::test
{
  namespace
  {
    constexpr auto kSheet = "FILE ignored.wav WAVE\nTRACK 01 AUDIO\nTITLE First\nINDEX 01 00:00:00\n"
                            "TRACK 02 AUDIO\nTITLE Second\nINDEX 01 00:00:01\n";

    void writeCueSource(std::filesystem::path const& path, std::string_view text)
    {
      std::filesystem::create_directories(path.parent_path());
      auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
      output << text;
      REQUIRE(output.good());
    }

    struct CueAccessFixture final
    {
      ao::test::TempDir temp;
      std::filesystem::path root = temp.path() / "music";
      library::MusicLibrary library = library::test::makeTestMusicLibrary(root, temp.path() / "db");

      CueAccessFixture()
      {
        std::filesystem::create_directories(root);
        writeScanFlacMetadataFixture(root / "disc.flac", {"TITLE=Carrier", "CUESHEET=" + std::string{kSheet}}, 88200);
      }

      ScanPlan plan() const
      {
        auto res = LibraryScan{library}.buildPlan();
        REQUIRE(res);
        return std::move(*res);
      }

      void requireEmpty() const
      {
        auto transaction = library.readTransaction();
        CHECK(library.tracks().reader(transaction).entryCount() == 0);
        auto manifests = library.manifest().reader(transaction);
        CHECK(manifests.begin() == manifests.end());
        CHECK(library.libraryRevision(transaction) == 0);
      }
    };
  } // namespace

  TEST_CASE("readCueDirectory - rejected symlinks remain blocking and valid in-root symlinks bind")
  {
    auto const temp = ao::test::TempDir{};
    auto const root = temp.path() / "music";
    std::filesystem::create_directories(root);
    auto target = temp.path() / "outside.txt";
    bool isValid = false;
    bool isDangling = false;

    SECTION("external target")
    {
    }

    SECTION("unresolved target")
    {
      isDangling = true;
    }

    SECTION("valid target below root")
    {
      target = root / "sheets" / "inside.txt";
      isValid = true;
    }

    if (!isDangling)
    {
      writeCueSource(target, kSheet);
    }

    auto const alias = ao::test::SymlinkFixture{target, root / "disc.cue", ao::test::SymlinkType::File};
    auto sheetsRes = readCueDirectory(root, root);
    REQUIRE(sheetsRes);
    REQUIRE(sheetsRes->size() == 1);
    auto const& sheet = sheetsRes->front();
    CHECK(sheet.evidence.name == "disc.cue");
    CHECK(sheet.optSheet.has_value() == isValid);
    CHECK(sheet.evidence.cueDiagnostic ==
          (isValid ? ScanCueDiagnostic::None : ScanCueDiagnostic::SheetUnreadableOrMalformed));
    auto const embeddedSheets = std::array{std::string_view{kSheet}};
    auto const binding = bindCueSheet(
      "disc.flac",
      CueMediaFacts{
        .codec = AudioCodec::Flac, .sampleRate = 44100, .optTotalFrames = 88200, .embeddedSheets = embeddedSheets},
      *sheetsRes);
    CHECK(binding.state == (isValid ? CueBindingState::Bound : CueBindingState::Undetermined));
    CHECK(binding.layout.size() == (isValid ? 2 : 0));
    REQUIRE(binding.evidence.general.size() == 1);
    CHECK(binding.evidence.general.front().name == "disc.cue");

    if (!isValid)
    {
      CHECK_FALSE(sheet.evidence.errorMessage.empty());
      CHECK_FALSE(sheet.evidence.errorMessage.contains("UTF-8"));
      CHECK(sheet.evidence.digest == utility::Hash128{});
    }
  }

  TEST_CASE("readCueDirectory - an entry named like a sheet blocks binding whatever its type")
  {
    auto const temp = ao::test::TempDir{};
    auto const root = temp.path() / "music";
    std::filesystem::create_directories(root);

    SECTION("directory")
    {
      std::filesystem::create_directories(root / "disc.cue");
    }

#ifndef _WIN32
    SECTION("FIFO")
    {
      // A FIFO is never opened: a read would block until a writer appears.
      REQUIRE(::mkfifo((root / "disc.cue").c_str(), 0600) == 0);
    }
#endif

    auto sheetsRes = readCueDirectory(root, root);
    REQUIRE(sheetsRes);
    REQUIRE(sheetsRes->size() == 1);
    auto const& sheet = sheetsRes->front();
    CHECK(sheet.evidence.name == "disc.cue");
    CHECK_FALSE(sheet.optSheet);
    CHECK(sheet.evidence.cueDiagnostic == ScanCueDiagnostic::SheetUnreadableOrMalformed);
    CHECK(sheet.evidence.errorMessage.contains("not an accessible regular file"));
    // Dropping the candidate would expose the embedded sheet; it blocks it instead.
    auto const embeddedSheets = std::array{std::string_view{kSheet}};
    auto const binding = bindCueSheet(
      "disc.flac",
      CueMediaFacts{
        .codec = AudioCodec::Flac, .sampleRate = 44100, .optTotalFrames = 88200, .embeddedSheets = embeddedSheets},
      *sheetsRes);
    CHECK(binding.state == CueBindingState::Undetermined);
    CHECK(binding.layout.empty());
  }

  TEST_CASE("CUE access - a file symlink into an unlistable folder fails only that item")
  {
    auto fixture = CueAccessFixture{};
    auto const hidden = fixture.root / "hidden";
    std::filesystem::create_directories(hidden);
    std::filesystem::create_directories(fixture.root / "album");
    writeScanFlacMetadataFixture(hidden / "inner.flac", {"TITLE=Inner"}, 88200);
    auto const alias = ao::test::SymlinkFixture{
      hidden / "inner.flac", fixture.root / "album" / "link.flac", ao::test::SymlinkType::File};
    auto const access = ao::test::ScopedDirectoryAccessGuard{hidden, ao::test::DeniedDirectoryAccess::List};

    if (!access.isEffective())
    {
      SKIP("The current process can bypass directory access restrictions");
    }

    // The link's item takes its target's canonical URI and path. The planner
    // cannot prove that the unlistable target folder holds no sheet, so it
    // fails that one file instead of importing it whole or failing the plan.
    auto plan = fixture.plan();
    auto const link = std::ranges::find(plan.items(), std::string_view{"hidden/inner.flac"}, &ScanItem::uri);
    REQUIRE(link != plan.items().end());
    CHECK(link->classification == ScanClassification::Error);
    CHECK(link->errorMessage.contains("Cannot list CUE directory"));
    auto const disc = std::ranges::find(plan.items(), std::string_view{"disc.flac"}, &ScanItem::uri);
    REQUIRE(disc != plan.items().end());
    CHECK(disc->classification == ScanClassification::New);
    auto res = ScanApplyOperation{fixture.library, std::move(plan), {}, {}}.run();
    REQUIRE(res);
    CHECK(res->insertedIds.size() == 2);
    auto transaction = fixture.library.readTransaction();
    CHECK_FALSE(fixture.library.manifest().reader(transaction).get("hidden/inner.flac"));
    CHECK(fixture.library.manifest().reader(transaction).get("disc.flac"));
  }

  TEST_CASE("CUE access - an unchanged file whose folder becomes unlistable is only reported")
  {
    auto fixture = CueAccessFixture{};
    auto const hidden = fixture.root / "hidden";
    std::filesystem::create_directories(hidden);
    std::filesystem::create_directories(fixture.root / "album");
    writeScanFlacMetadataFixture(hidden / "inner.flac", {"TITLE=Inner"}, 88200);
    auto const alias = ao::test::SymlinkFixture{
      hidden / "inner.flac", fixture.root / "album" / "link.flac", ao::test::SymlinkType::File};
    auto importedRes = ScanApplyOperation{fixture.library, fixture.plan(), {}, {}}.run();
    REQUIRE(importedRes);
    REQUIRE(importedRes->insertedIds.size() == 3);
    auto const access = ao::test::ScopedDirectoryAccessGuard{hidden, ao::test::DeniedDirectoryAccess::List};

    if (!access.isEffective())
    {
      SKIP("The current process can bypass directory access restrictions");
    }

    // The walk fails the folder itself. The file it reaches through the link
    // applies nothing, so unknown sheet state is reported on it, not failed.
    auto plan = fixture.plan();
    auto const folder = std::ranges::find(plan.items(), std::string_view{"hidden"}, &ScanItem::uri);
    REQUIRE(folder != plan.items().end());
    CHECK(folder->classification == ScanClassification::Error);
    auto const link = std::ranges::find(plan.items(), std::string_view{"hidden/inner.flac"}, &ScanItem::uri);
    REQUIRE(link != plan.items().end());
    CHECK(link->classification == ScanClassification::Unchanged);
    CHECK(isScanNotice(*link));
    CHECK(link->cueDiagnostic == ScanCueDiagnostic::SheetUnreadableOrMalformed);
    CHECK(link->errorMessage.contains("Cannot list CUE directory"));
    auto const revision = fixture.library.libraryRevision(fixture.library.readTransaction());
    auto res = ScanApplyOperation{fixture.library, std::move(plan), {}, {}}.run();
    REQUIRE(res);
    CHECK(res->failureCount == 1);
    CHECK(res->mutatedIds.empty());
    auto transaction = fixture.library.readTransaction();
    CHECK(fixture.library.libraryRevision(transaction) == revision);
    CHECK(fixture.library.manifest().reader(transaction).get("hidden/inner.flac"));
  }

  TEST_CASE("CUE access - a move into an unlistable folder leaves its source missing")
  {
    auto fixture = CueAccessFixture{};
    auto const hidden = fixture.root / "hidden";
    std::filesystem::create_directories(hidden);
    std::filesystem::create_directories(fixture.root / "album");
    std::filesystem::copy_file(audio::test::requireAudioFixture("basic_metadata.flac"), fixture.root / "song.flac");
    auto importedRes = ScanApplyOperation{fixture.library, fixture.plan(), {}, {}}.run();
    REQUIRE(importedRes);
    REQUIRE(importedRes->insertedIds.size() == 3);
    auto songId = kInvalidTrackId;

    {
      auto transaction = fixture.library.readTransaction();
      auto const optImported = fixture.library.manifest().reader(transaction).get("song.flac");
      REQUIRE(optImported);
      songId = optImported->trackId();
    }

    std::filesystem::rename(fixture.root / "song.flac", hidden / "song.flac");
    auto const alias =
      ao::test::SymlinkFixture{hidden / "song.flac", fixture.root / "album" / "link.flac", ao::test::SymlinkType::File};
    auto const access = ao::test::ScopedDirectoryAccessGuard{hidden, ao::test::DeniedDirectoryAccess::List};

    if (!access.isEffective())
    {
      SKIP("The current process can bypass directory access restrictions");
    }

    // Identity pairs the files, but unknown sheet state refuses the move. The
    // source must then read as missing instead of leaving the plan.
    auto plan = fixture.plan();
    CHECK(plan.count(ScanClassification::Moved) == 0);
    auto const destination = std::ranges::find(plan.items(), std::string_view{"hidden/song.flac"}, &ScanItem::uri);
    REQUIRE(destination != plan.items().end());
    CHECK(destination->classification == ScanClassification::Error);
    CHECK(destination->oldUri.empty());
    CHECK_FALSE(destination->optManifestEvidence);
    auto const source = std::ranges::find(plan.items(), std::string_view{"song.flac"}, &ScanItem::uri);
    REQUIRE(source != plan.items().end());
    CHECK(source->classification == ScanClassification::Missing);
    auto res = ScanApplyOperation{fixture.library, std::move(plan), {}, {}}.run();
    REQUIRE(res);
    CHECK(res->failureCount == 2);
    CHECK(res->missingCount == 1);
    CHECK(res->relinkedIds.empty());
    auto transaction = fixture.library.readTransaction();
    auto const optSource = fixture.library.manifest().reader(transaction).get("song.flac");
    REQUIRE(optSource);
    CHECK(optSource->status() == library::FileStatus::Missing);
    CHECK(optSource->trackId() == songId);
    CHECK_FALSE(fixture.library.manifest().reader(transaction).get("hidden/song.flac"));
  }

  TEST_CASE("CUE access - scan and add-file never import an out-of-root sheet or embedded fallback")
  {
    auto fixture = CueAccessFixture{};
    auto const outside = fixture.temp.path() / "outside.txt";
    writeCueSource(outside, kSheet);
    auto const alias = ao::test::SymlinkFixture{outside, fixture.root / "disc.cue", ao::test::SymlinkType::File};

    SECTION("scan reports the rejected candidate without importing")
    {
      auto plan = fixture.plan();
      auto const found = std::ranges::find(plan.items(), std::string_view{"disc.flac"}, &ScanItem::uri);
      REQUIRE(found != plan.items().end());
      CHECK(found->classification == ScanClassification::Error);
      CHECK(found->cueDiagnostic == ScanCueDiagnostic::SheetUnreadableOrMalformed);
      CHECK(found->errorMessage.contains("outside the library root"));
      REQUIRE(found->cueEvidence.general.size() == 1);
      CHECK(found->cueLayout.empty());
      auto res = ScanApplyOperation{fixture.library, std::move(plan), {}, {}}.run();
      REQUIRE(res);
      CHECK(res->insertedIds.empty());
      CHECK(res->failureCount >= 1);
    }

    SECTION("add-file rejects before writing or publishing")
    {
      auto executor = QueuedExecutor{};
      auto changes = makeLibraryChanges(executor, fixture.library);
      auto commands = LibraryCommandsFixture{fixture.library, changes, executor};
      std::size_t publicationCount = 0;
      auto subscription =
        changes.onChanged([&publicationCount](LibraryChangeSet const&) noexcept { ++publicationCount; });
      auto res = commands.runTask(commands.commands().createTrackFromFileAsync("disc.flac"));
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::InvalidInput);
      CHECK(res.error().message.contains("outside the library root"));
      CHECK(publicationCount == 0);
    }

    fixture.requireEmpty();
  }

  TEST_CASE("CUE access - sheet retargeting outside root invalidates preparation and final proof", "[concurrency]")
  {
    auto fixture = CueAccessFixture{};
    auto const inside = fixture.root / "sheets" / "inside.txt";
    auto const outside = fixture.temp.path() / "outside.txt";
    writeCueSource(inside, kSheet);
    writeCueSource(outside, kSheet);
    auto const path = fixture.root / "disc.cue";
    auto const alias = ao::test::SymlinkFixture{inside, path, ao::test::SymlinkType::File};
    auto plan = fixture.plan();
    REQUIRE(plan.count(ScanClassification::New) == 1);
    auto operation = ScanApplyOperation{fixture.library, std::move(plan), {}, {}};
    bool retargetBeforePreparation = false;

    SECTION("before preparation")
    {
      retargetBeforePreparation = true;
    }

    SECTION("after preparation before final proof")
    {
    }

    if (!retargetBeforePreparation)
    {
      requirePrepared(operation);
    }

    REQUIRE(std::filesystem::remove(path));
    auto const retargeted = ao::test::SymlinkFixture{outside, path, ao::test::SymlinkType::File};

    if (retargetBeforePreparation)
    {
      auto const preparedRes = operation.prepare();
      REQUIRE(preparedRes);
      CHECK(preparedRes->staleCount == 1);
    }

    requireRevalidation(operation, 1, 0);
    auto res = operation.run();
    REQUIRE(res);
    CHECK(res->insertedIds.empty());
    CHECK(res->staleCount == 1);
    CHECK(res->failureCount == 0);
    fixture.requireEmpty();
  }

  TEST_CASE("CUE access - escaped moved-carrier sheet aborts the complete plan without sibling writes", "[concurrency]")
  {
    auto fixture = CueAccessFixture{};
    auto const inside = fixture.root / "sheets" / "inside.txt";
    auto const outside = fixture.temp.path() / "outside.txt";
    writeCueSource(inside, kSheet);
    writeCueSource(outside, kSheet);
    auto const oldAlias = ao::test::SymlinkFixture{inside, fixture.root / "disc.cue", ao::test::SymlinkType::File};
    auto importedRes = ScanApplyOperation{fixture.library, fixture.plan(), {}, {}}.run();
    REQUIRE(importedRes);
    REQUIRE(importedRes->insertedIds.size() == 2);
    REQUIRE(importedRes->failureCount == 0);
    auto const revision = fixture.library.libraryRevision(fixture.library.readTransaction());
    REQUIRE(std::filesystem::remove(fixture.root / "disc.cue"));
    std::filesystem::rename(fixture.root / "disc.flac", fixture.root / "renamed.flac");
    audio::test::installAudioFixture(fixture.root, "basic_metadata.wav", "sibling.wav");
    auto const path = fixture.root / "renamed.cue";
    auto const plannedAlias = ao::test::SymlinkFixture{inside, path, ao::test::SymlinkType::File};
    auto plan = fixture.plan();
    REQUIRE(plan.count(ScanClassification::Moved) == 1);
    REQUIRE(plan.count(ScanClassification::New) == 1);
    auto operation = ScanApplyOperation{fixture.library, std::move(plan), {}, {}};
    bool retargetBeforePreparation = false;

    SECTION("before preparation")
    {
      retargetBeforePreparation = true;
    }

    SECTION("after preparation before final proof")
    {
    }

    if (!retargetBeforePreparation)
    {
      requirePrepared(operation);
    }

    REQUIRE(std::filesystem::remove(path));
    auto const retargeted = ao::test::SymlinkFixture{outside, path, ao::test::SymlinkType::File};
    auto res = operation.run();
    REQUIRE(res);
    CHECK(res->failureCount == 1);
    CHECK(res->insertedIds.empty());
    CHECK(res->mutatedIds.empty());
    CHECK(res->relinkedIds.empty());
    CHECK(res->relinkedFileCount == 0);
    CHECK_FALSE(operation.shouldCommitTransaction());
    auto transaction = fixture.library.readTransaction();
    CHECK(fixture.library.libraryRevision(transaction) == revision);
    auto manifests = fixture.library.manifest().reader(transaction);
    CHECK_FALSE(manifests.get("renamed.flac"));
    CHECK_FALSE(manifests.get("sibling.wav"));
    auto const optOld = manifests.get("disc.flac");
    REQUIRE(optOld);
    CHECK(optOld->segmentCount() == 2);
    auto tracks = fixture.library.tracks().reader(transaction);
    CHECK(tracks.entryCount() == 2);

    for (auto const id : importedRes->insertedIds)
    {
      auto const optTrack = tracks.get(id);
      REQUIRE(optTrack);
      CHECK(optTrack->property().uri() == "disc.flac");
      REQUIRE(optTrack->property().segment());
    }
  }

  TEST_CASE("readCueDirectory - external source byte limit accepts its boundary and blocks larger sheets")
  {
    auto const temp = ao::test::TempDir{};
    auto const root = temp.path() / "music";
    std::filesystem::create_directories(root);
    constexpr std::size_t kMaximumBytes = 4'194'304;
    bool acceptsSheet = false;

    SECTION("exact limit remains accepted")
    {
      auto text = std::string{kSheet};
      text.resize(kMaximumBytes, ' ');
      writeCueSource(root / "disc.cue", text);
      acceptsSheet = true;
    }

    SECTION("oversized source remains a blocking candidate")
    {
      writeCueSource(root / "disc.cue", std::string(kMaximumBytes + 1, 'X'));
    }

    auto sheetsRes = readCueDirectory(root, root);
    REQUIRE(sheetsRes);
    REQUIRE(sheetsRes->size() == 1);
    CHECK(sheetsRes->front().optSheet.has_value() == acceptsSheet);

    if (!acceptsSheet)
    {
      CHECK(sheetsRes->front().evidence.errorMessage == "CUE sheet exceeds the maximum of 4194304 bytes");
      CHECK(sheetsRes->front().evidence.errorMessage.size() < 100);
      CHECK(sheetsRes->front().evidence.cueDiagnostic == ScanCueDiagnostic::SheetUnreadableOrMalformed);
    }
  }

  TEST_CASE("readCueDirectory - directory symlink is nonregular and gets no encoding advice")
  {
    auto const temp = ao::test::TempDir{};
    auto const root = temp.path() / "music";
    auto const target = root / "directory";
    std::filesystem::create_directories(target);
    auto const alias = ao::test::SymlinkFixture{target, root / "disc.cue", ao::test::SymlinkType::Directory};
    auto sheetsRes = readCueDirectory(root, root);
    REQUIRE(sheetsRes);
    REQUIRE(sheetsRes->size() == 1);
    CHECK_FALSE(sheetsRes->front().optSheet);
    CHECK(sheetsRes->front().evidence.errorMessage == "CUE sheet is not an accessible regular file");
    CHECK(sheetsRes->front().evidence.cueDiagnostic == ScanCueDiagnostic::SheetUnreadableOrMalformed);
  }
} // namespace ao::rt::test
