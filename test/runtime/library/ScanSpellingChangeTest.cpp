// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/library/AudioIdentityBatchWriter.h"
#include "runtime/library/AudioIdentityIndexer.h"
#include "runtime/library/ScanApplyOperation.h"
#include "test/FilesystemTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/audio/AudioFixtureSupport.h"
#include "test/library/MusicLibraryTestSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/library/WritableLibraryTestSupport.h"
#include "test/runtime/library/ScanApplyTestSupport.h"
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/FileTimestamp.h>
#include <ao/async/OperationCancelled.h>
#include <ao/library/AudioIdentity.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackWriter.h>
#include <ao/media/file/File.h>
#include <ao/rt/library/LibraryScan.h>
#include <ao/rt/library/ScanPlan.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <ios>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    constexpr auto kOriginalUri = "A.flac";
    constexpr auto kRenamedUri = "a.flac";
    constexpr auto kInitialMtime = FileTimestamp{.seconds = 1700001000, .nanoseconds = 0};
    constexpr auto kChangedMtime = FileTimestamp{.seconds = 1700001010, .nanoseconds = 0};

    std::filesystem::path makeMusicRoot(std::filesystem::path const& root)
    {
      auto path = root / "music";
      std::filesystem::create_directory(path);
      return path;
    }

    void pinMtime(std::filesystem::path const& path, FileTimestamp const timestamp)
    {
      std::filesystem::last_write_time(
        path,
        ao::test::fileTimeFromSystemTime(std::chrono::sys_time<std::chrono::nanoseconds>{
          std::chrono::seconds{timestamp.seconds} + std::chrono::nanoseconds{timestamp.nanoseconds}}));
    }

    bool hasExactEntry(std::filesystem::path const& path)
    {
      return std::ranges::any_of(std::filesystem::directory_iterator{path.parent_path()},
                                 [&path](std::filesystem::directory_entry const& entry)
                                 { return entry.path().filename() == path.filename(); });
    }

    void renameCase(std::filesystem::path const& original, std::filesystem::path const& renamed)
    {
      // Use an intermediate name so a case-insensitive rename cannot be a no-op.
      auto const intermediate = original.parent_path() / "case-change.tmp";
      std::filesystem::rename(original, intermediate);
      std::filesystem::rename(intermediate, renamed);
      REQUIRE(hasExactEntry(renamed));
      REQUIRE_FALSE(hasExactEntry(original));
    }

    struct SpellingFixture final
    {
      ao::test::TempDir temp;
      std::filesystem::path musicRoot = makeMusicRoot(temp.path());
      std::filesystem::path original = musicRoot / kOriginalUri;
      std::filesystem::path renamed = musicRoot / kRenamedUri;
      library::MusicLibrary library = library::test::makeTestMusicLibrary(musicRoot, temp.path() / "db");

      SpellingFixture()
      {
        writeScanFlacMetadataFixture(original, {"TITLE=Embedded title"}, 88200);
        pinMtime(original, kInitialMtime);
      }

      bool isCaseInsensitive() const
      {
        // Probe the directory under test, not the OS or another volume.
        REQUIRE(hasExactEntry(original));
        REQUIRE_FALSE(hasExactEntry(renamed));

        if (!std::filesystem::exists(renamed))
        {
          return false;
        }

        REQUIRE(std::filesystem::equivalent(original, renamed));
        return true;
      }

      ScanPlan plan() const { return ao::test::requireValue(LibraryScan{library}.buildPlan()); }

      ScanApplyResult apply(ScanPlan plan, ScanApplyOptions options = {})
      {
        return ao::test::requireValue(ScanApplyOperation{library, std::move(plan), {}, {}, options}.run());
      }

      void reopenLibrary()
      {
        // Close the existing LMDB environment before opening the same directory.
        {
          [[maybe_unused]] auto closing = std::move(library);
        }
        library = library::test::makeTestMusicLibrary(musicRoot, temp.path() / "db");
      }

      TrackId importTrack(AudioIdentityPolicy policy = AudioIdentityPolicy::DeferNew)
      {
        auto const result = apply(plan(), {.audioIdentityPolicy = policy});
        REQUIRE(result.failureCount == 0);
        REQUIRE(result.insertedIds.size() == 1);
        return result.insertedIds.front();
      }
    };

    ScanItem const& requireItem(ScanPlan const& plan, std::string_view const uri)
    {
      auto const items = plan.items();
      auto const found = std::ranges::find(items, uri, &ScanItem::uri);
      REQUIRE(found != items.end());
      return *found;
    }

    void requireSpellingChange(ScanItem const& item, TrackId const trackId)
    {
      REQUIRE(item.classification == ScanClassification::SpellingChanged);
      CHECK(item.uri == kRenamedUri);
      CHECK(item.oldUri == kOriginalUri);
      CHECK(item.trackId == trackId);
    }

    void requireSpellingChange(ScanPlan const& plan, TrackId const trackId)
    {
      REQUIRE(plan.size() == 1);
      requireSpellingChange(plan.items().front(), trackId);
    }

    TrackId createHistoricalAlias(SpellingFixture& fixture,
                                  std::string_view const uri,
                                  std::string_view const originalUri = kOriginalUri)
    {
      auto transaction = library::test::writeTransaction(fixture.library);
      auto const id = ao::test::requireValue(transaction.apply(
        [uri, originalUri](library::LibraryWrite& write)
        {
          auto writer = write.tracks();
          auto const optOriginal = writer.manifest(originalUri);
          REQUIRE(optOriginal);
          auto track = library::TrackBuilder::makeEmpty();
          track.property().uri(uri);
          auto manifest = library::FileManifestBuilder::fromView(*optOriginal);
          manifest.status(library::FileStatus::Available);
          return writer.create(track, std::move(manifest));
        }));
      REQUIRE(transaction.commit());
      return id;
    }

    library::AudioIdentity readManifestIdentity(SpellingFixture const& fixture, std::string_view const uri)
    {
      auto transaction = fixture.library.readTransaction();
      auto const optRow = fixture.library.manifest().reader(transaction).get(uri);
      REQUIRE(optRow);
      return {.signature = optRow->audioSignature(), .payloadLength = optRow->audioPayloadLength()};
    }

    void checkUnchangedSpelling(ScanPlan const& plan)
    {
      REQUIRE(plan.size() == 1);
      auto const& item = plan.items().front();
      CHECK(item.classification == ScanClassification::Unchanged);
      CHECK(item.uri == kRenamedUri);
    }

    void checkWholeFileRekey(SpellingFixture const& fixture, TrackId const trackId, FileTimestamp const mtime)
    {
      auto transaction = fixture.library.readTransaction();
      auto const manifest = fixture.library.manifest().reader(transaction);
      CHECK_FALSE(manifest.get(kOriginalUri));
      auto const optRow = manifest.get(kRenamedUri);
      REQUIRE(optRow);
      CHECK(optRow->trackId() == trackId);
      CHECK(optRow->status() == library::FileStatus::Available);
      CHECK(optRow->fileSize() == std::filesystem::file_size(fixture.renamed));
      CHECK(optRow->mtime() == mtime);
      auto const tracks = fixture.library.tracks().reader(transaction);
      CHECK(tracks.entryCount() == 1);
      auto const optTrack = tracks.get(trackId);
      REQUIRE(optTrack);
      CHECK(optTrack->property().uri() == kRenamedUri);
    }
  } // namespace

  TEST_CASE("Scan directory spelling - preserves pending identity across a parent rename")
  {
    auto fixture = SpellingFixture{};

    if (!fixture.isCaseInsensitive())
    {
      SKIP("The temporary music directory is case-sensitive");
    }

    std::filesystem::remove(fixture.original);
    auto const oldDir = fixture.musicRoot / "Album";
    auto const newDir = fixture.musicRoot / "album";
    std::filesystem::create_directory(oldDir);
    writeScanFlacMetadataFixture(oldDir / "x.flac", {"TITLE=Embedded title"}, 88200);
    auto const id = fixture.importTrack();
    auto const identity = readManifestIdentity(fixture, "Album/x.flac");
    REQUIRE_FALSE(library::hasAudioIdentity(identity.payloadLength, identity.signature));
    renameCase(oldDir, newDir);
    auto plan = fixture.plan();
    REQUIRE(plan.size() == 1);
    auto const& item = plan.items().front();
    REQUIRE(item.classification == ScanClassification::SpellingChanged);
    CHECK(item.oldUri == "Album/x.flac");
    CHECK(item.uri == "album/x.flac");
    CHECK(item.trackId == id);
    auto const result = fixture.apply(std::move(plan));
    CHECK(result.relinkedIds == std::vector{id});
    CHECK(result.failureCount == 0);
    fixture.reopenLibrary();
    auto const newIdentity = readManifestIdentity(fixture, "album/x.flac");
    CHECK(newIdentity.signature == identity.signature);
    CHECK(newIdentity.payloadLength == identity.payloadLength);
    {
      auto transaction = fixture.library.readTransaction();
      auto const manifest = fixture.library.manifest().reader(transaction);
      CHECK_FALSE(manifest.get("Album/x.flac"));
      auto const optRow = manifest.get("album/x.flac");
      REQUIRE(optRow);
      CHECK(optRow->trackId() == id);
      CHECK(optRow->status() == library::FileStatus::Available);
      auto const optTrack = fixture.library.tracks().reader(transaction).get(id);
      REQUIRE(optTrack);
      CHECK(optTrack->property().uri() == "album/x.flac");
    }
    auto const rescan = fixture.plan();
    REQUIRE(rescan.size() == 1);
    CHECK(rescan.items().front().classification == ScanClassification::Unchanged);
  }

  TEST_CASE("Scan directory spelling - rekeys multiple tracks under a nested suffix")
  {
    auto fixture = SpellingFixture{};

    if (!fixture.isCaseInsensitive())
    {
      SKIP("The temporary music directory is case-sensitive");
    }

    std::filesystem::remove(fixture.original);
    auto const oldDir = fixture.musicRoot / "Album";
    auto const newDir = fixture.musicRoot / "album";
    std::filesystem::create_directories(oldDir / "disc");
    writeScanFlacMetadataFixture(oldDir / "disc" / "one.flac", {"TITLE=One"}, 88200);
    writeScanFlacMetadataFixture(oldDir / "disc" / "two.flac", {"TITLE=Two"}, 88200);
    auto policy = AudioIdentityPolicy::DeferNew;

    SECTION("pending identities")
    {
    }

    SECTION("known identities")
    {
      policy = AudioIdentityPolicy::Eager;
    }

    auto const imported = fixture.apply(fixture.plan(), {.audioIdentityPolicy = policy});
    REQUIRE(imported.insertedIds.size() == 2);
    REQUIRE(imported.failureCount == 0);
    auto oneId = TrackId{};
    auto twoId = TrackId{};
    {
      auto transaction = fixture.library.readTransaction();
      auto const manifest = fixture.library.manifest().reader(transaction);
      auto const optOne = manifest.get("Album/disc/one.flac");
      auto const optTwo = manifest.get("Album/disc/two.flac");
      REQUIRE(optOne);
      REQUIRE(optTwo);
      oneId = optOne->trackId();
      twoId = optTwo->trackId();
    }
    renameCase(oldDir, newDir);
    auto plan = fixture.plan();
    REQUIRE(plan.size() == 2);

    for (auto const& [uri, oldUri, id] : {std::tuple{"album/disc/one.flac", "Album/disc/one.flac", oneId},
                                          std::tuple{"album/disc/two.flac", "Album/disc/two.flac", twoId}})
    {
      auto const& item = requireItem(plan, uri);
      CHECK(item.classification == ScanClassification::SpellingChanged);
      CHECK(item.oldUri == oldUri);
      CHECK(item.trackId == id);
    }

    auto const result = fixture.apply(std::move(plan), {.audioIdentityPolicy = policy});
    CHECK(result.failureCount == 0);
    CHECK(result.insertedIds.empty());
    CHECK(result.relinkedIds.size() == 2);
    fixture.reopenLibrary();
    auto transaction = fixture.library.readTransaction();
    auto const manifest = fixture.library.manifest().reader(transaction);
    CHECK_FALSE(manifest.get("Album/disc/one.flac"));
    CHECK_FALSE(manifest.get("Album/disc/two.flac"));
    auto const optOne = manifest.get("album/disc/one.flac");
    auto const optTwo = manifest.get("album/disc/two.flac");
    REQUIRE(optOne);
    REQUIRE(optTwo);
    CHECK(optOne->trackId() == oneId);
    CHECK(optTwo->trackId() == twoId);
  }

  TEST_CASE("Scan directory spelling - rejects replacement after preparation", "[concurrency]")
  {
    auto fixture = SpellingFixture{};

    if (!fixture.isCaseInsensitive())
    {
      SKIP("The temporary music directory is case-sensitive");
    }

    std::filesystem::remove(fixture.original);
    auto const oldDir = fixture.musicRoot / "Album";
    auto const newDir = fixture.musicRoot / "album";
    std::filesystem::create_directory(oldDir);
    writeScanFlacMetadataFixture(oldDir / "x.flac", {"TITLE=First"}, 88200);
    auto const id = fixture.importTrack();
    renameCase(oldDir, newDir);
    writeScanFlacMetadataFixture(fixture.musicRoot / "unrelated.flac", {"TITLE=Other"}, 88200);
    auto plan = fixture.plan();
    REQUIRE(requireItem(plan, "album/x.flac").classification == ScanClassification::SpellingChanged);
    REQUIRE(requireItem(plan, "unrelated.flac").classification == ScanClassification::New);
    auto operation = ScanApplyOperation{fixture.library, std::move(plan), {}, {}};
    requirePrepared(operation);
    auto const revision = fixture.library.libraryRevision(fixture.library.readTransaction());
    auto const backup = fixture.temp.path() / "backup";

    SECTION("only the directory identity changes")
    {
      std::filesystem::rename(newDir, backup);
      std::filesystem::create_directory(newDir);
      std::filesystem::rename(backup / "x.flac", newDir / "x.flac");
      REQUIRE_FALSE(std::filesystem::equivalent(backup, newDir));
    }

    SECTION("only the file identity changes")
    {
      std::filesystem::rename(newDir / "x.flac", backup);
      std::filesystem::copy_file(backup, newDir / "x.flac");
      std::filesystem::last_write_time(newDir / "x.flac", std::filesystem::last_write_time(backup));
      REQUIRE_FALSE(std::filesystem::equivalent(backup, newDir / "x.flac"));
    }

    auto const result = ao::test::requireValue(operation.run());
    CHECK(result.relinkedIds.empty());
    CHECK(result.insertedIds.empty());
    CHECK(result.failureCount > 0);
    auto transaction = fixture.library.readTransaction();
    CHECK(fixture.library.libraryRevision(transaction) == revision);
    auto const manifest = fixture.library.manifest().reader(transaction);
    auto const optRow = manifest.get("Album/x.flac");
    REQUIRE(optRow);
    CHECK(optRow->trackId() == id);
    CHECK_FALSE(manifest.get("album/x.flac"));
    CHECK_FALSE(manifest.get("unrelated.flac"));
  }

  TEST_CASE("Scan directory spelling - refuses ambiguous stored parent spellings")
  {
    auto fixture = SpellingFixture{};

    if (!fixture.isCaseInsensitive())
    {
      SKIP("The temporary music directory is case-sensitive");
    }

    std::filesystem::remove(fixture.original);
    auto const oldDir = fixture.musicRoot / "Album";
    auto const newDir = fixture.musicRoot / "album";
    std::filesystem::create_directory(oldDir);
    writeScanFlacMetadataFixture(oldDir / "x.flac", {"TITLE=First"}, 88200);
    auto const id = fixture.importTrack();
    auto const otherId = createHistoricalAlias(fixture, "ALBUM/x.flac", "Album/x.flac");
    renameCase(oldDir, newDir);
    auto plan = fixture.plan();
    CHECK(requireItem(plan, "album/x.flac").classification == ScanClassification::Error);
    auto const result = fixture.apply(std::move(plan));
    CHECK(result.relinkedIds.empty());
    CHECK(result.insertedIds.empty());
    auto read = fixture.library.readTransaction();
    auto const manifest = fixture.library.manifest().reader(read);
    auto const optOld = manifest.get("Album/x.flac");
    auto const optOther = manifest.get("ALBUM/x.flac");
    REQUIRE(optOld);
    REQUIRE(optOther);
    CHECK(optOld->trackId() == id);
    CHECK(optOther->trackId() == otherId);
    CHECK_FALSE(manifest.get("album/x.flac"));
  }

  TEST_CASE("Scan directory spelling - exact owner remains unchanged beside a historical alias")
  {
    auto fixture = SpellingFixture{};

    if (!fixture.isCaseInsensitive())
    {
      SKIP("The temporary music directory is case-sensitive");
    }

    std::filesystem::remove(fixture.original);
    auto const oldDir = fixture.musicRoot / "Album";
    auto const newDir = fixture.musicRoot / "album";
    std::filesystem::create_directory(oldDir);
    writeScanFlacMetadataFixture(oldDir / "x.flac", {"TITLE=First"}, 88200);
    auto const oldId = fixture.importTrack();
    renameCase(oldDir, newDir);
    auto const exactId = createHistoricalAlias(fixture, "album/x.flac", "Album/x.flac");
    auto const revision = fixture.library.libraryRevision(fixture.library.readTransaction());
    auto plan = fixture.plan();
    CHECK(requireItem(plan, "Album/x.flac").classification == ScanClassification::Notice);
    CHECK(requireItem(plan, "album/x.flac").classification == ScanClassification::Unchanged);
    auto const result = fixture.apply(std::move(plan));
    CHECK(result.failureCount == 0);
    CHECK(result.relinkedIds.empty());
    CHECK(result.insertedIds.empty());
    fixture.reopenLibrary();
    auto transaction = fixture.library.readTransaction();
    CHECK(fixture.library.libraryRevision(transaction) == revision);
    auto const manifest = fixture.library.manifest().reader(transaction);
    auto const optOld = manifest.get("Album/x.flac");
    auto const optExact = manifest.get("album/x.flac");
    REQUIRE(optOld);
    REQUIRE(optExact);
    CHECK(optOld->trackId() == oldId);
    CHECK(optExact->trackId() == exactId);
    CHECK(optOld->status() == library::FileStatus::Available);
    CHECK(optExact->status() == library::FileStatus::Available);
    auto const tracks = fixture.library.tracks().reader(transaction);
    auto const optOldTrack = tracks.get(oldId);
    auto const optExactTrack = tracks.get(exactId);
    REQUIRE(optOldTrack);
    REQUIRE(optExactTrack);
    CHECK(optOldTrack->property().uri() == "Album/x.flac");
    CHECK(optExactTrack->property().uri() == "album/x.flac");
  }

  TEST_CASE("Scan directory spelling - rejects a late competing owner", "[concurrency]")
  {
    auto fixture = SpellingFixture{};

    if (!fixture.isCaseInsensitive())
    {
      SKIP("The temporary music directory is case-sensitive");
    }

    std::filesystem::remove(fixture.original);
    auto const oldDir = fixture.musicRoot / "Album";
    auto const newDir = fixture.musicRoot / "album";
    std::filesystem::create_directory(oldDir);
    writeScanFlacMetadataFixture(oldDir / "x.flac", {"TITLE=First"}, 88200);
    auto const id = fixture.importTrack();
    renameCase(oldDir, newDir);
    auto plan = fixture.plan();
    REQUIRE(plan.size() == 1);
    REQUIRE(plan.items().front().classification == ScanClassification::SpellingChanged);
    auto alternate = std::string_view{"ALBUM/x.flac"};

    SECTION("parent variant")
    {
    }

    SECTION("leaf variant")
    {
      alternate = "Album/X.flac";
    }

    SECTION("destination parent and leaf variant")
    {
      alternate = "album/X.flac";
    }

    REQUIRE(std::filesystem::equivalent(fixture.musicRoot / alternate, newDir / "x.flac"));
    auto const otherId = createHistoricalAlias(fixture, alternate, "Album/x.flac");
    auto const revision = fixture.library.libraryRevision(fixture.library.readTransaction());
    auto const result = fixture.apply(std::move(plan));
    CHECK(result.relinkedIds.empty());
    CHECK(result.insertedIds.empty());
    CHECK(result.mutatedIds.empty());
    CHECK(result.failureCount > 0);
    auto read = fixture.library.readTransaction();
    CHECK(fixture.library.libraryRevision(read) == revision);
    auto const manifest = fixture.library.manifest().reader(read);
    auto const optOld = manifest.get("Album/x.flac");
    auto const optOther = manifest.get(alternate);
    REQUIRE(optOld);
    REQUIRE(optOther);
    CHECK(optOld->trackId() == id);
    CHECK(optOther->trackId() == otherId);
    CHECK_FALSE(manifest.get("album/x.flac"));
  }

  TEST_CASE("Scan directory spelling - refuses a redirected old parent")
  {
    auto fixture = SpellingFixture{};

    if (fixture.isCaseInsensitive())
    {
      SKIP("The temporary music directory cannot contain both spellings");
    }

    std::filesystem::remove(fixture.original);
    auto const oldDir = fixture.musicRoot / "Album";
    auto const newDir = fixture.musicRoot / "album";
    std::filesystem::create_directory(oldDir);
    writeScanFlacMetadataFixture(oldDir / "x.flac", {"TITLE=First"}, 88200);
    auto policy = AudioIdentityPolicy::DeferNew;

    SECTION("pending identity")
    {
    }

    SECTION("known identity")
    {
      policy = AudioIdentityPolicy::Eager;
    }

    auto const id = fixture.importTrack(policy);
    renameCase(oldDir, newDir);
    auto const link = ao::test::SymlinkFixture{newDir, oldDir, ao::test::SymlinkType::Directory};
    REQUIRE(std::filesystem::equivalent(oldDir / "x.flac", newDir / "x.flac"));
    auto plan = fixture.plan();
    CHECK(plan.count(ScanClassification::SpellingChanged) == 0);
    auto const result = fixture.apply(std::move(plan));
    CHECK(result.relinkedIds.empty());
    CHECK(result.insertedIds.empty());
    auto transaction = fixture.library.readTransaction();
    auto const manifest = fixture.library.manifest().reader(transaction);
    auto const optOld = manifest.get("Album/x.flac");
    REQUIRE(optOld);
    CHECK(optOld->trackId() == id);
    CHECK_FALSE(manifest.get("album/x.flac"));
    auto const optTrack = fixture.library.tracks().reader(transaction).get(id);
    REQUIRE(optTrack);
    CHECK(optTrack->property().uri() == "Album/x.flac");
  }

  TEST_CASE("Scan directory spelling - an exact regular file cannot alias a coexisting directory")
  {
    auto fixture = SpellingFixture{};

    if (fixture.isCaseInsensitive())
    {
      SKIP("The temporary music directory cannot contain both spellings");
    }

    std::filesystem::remove(fixture.original);
    auto const oldDir = fixture.musicRoot / "Album";
    auto const newDir = fixture.musicRoot / "album";
    constexpr auto kOldUri = "Album/Disc/x.flac";
    constexpr auto kNewUri = "album/Disc/x.flac";
    std::filesystem::create_directories(oldDir / "Disc");
    writeScanFlacMetadataFixture(fixture.musicRoot / kOldUri, {"TITLE=Track in the album"});
    auto const policy = GENERATE(AudioIdentityPolicy::Eager, AudioIdentityPolicy::DeferNew);
    auto const id = fixture.importTrack(policy);
    auto const identity = readManifestIdentity(fixture, kOldUri);
    auto const hasFingerprint = policy == AudioIdentityPolicy::Eager;
    REQUIRE(library::hasAudioIdentity(identity.payloadLength, identity.signature) == hasFingerprint);
    auto blocker = oldDir;

    SECTION("the differently cased component is a regular file")
    {
      renameCase(oldDir, newDir);
    }

    SECTION("an exact suffix component is a regular file")
    {
      REQUIRE(std::filesystem::create_directory(newDir));
      blocker = oldDir / "Disc";
      std::filesystem::rename(blocker, newDir / "Disc");
      REQUIRE(std::filesystem::is_directory(std::filesystem::symlink_status(oldDir)));
    }

    {
      auto file = std::ofstream{blocker, std::ios::binary};
      file << "This is a regular file, not a directory.";
      REQUIRE(file.good());
    }
    REQUIRE(hasExactEntry(oldDir));
    REQUIRE(hasExactEntry(newDir));
    REQUIRE(hasExactEntry(blocker));
    REQUIRE(std::filesystem::is_regular_file(std::filesystem::symlink_status(blocker)));
    REQUIRE(std::filesystem::is_regular_file(std::filesystem::symlink_status(fixture.musicRoot / kNewUri)));

    auto plan = fixture.plan();

    if (auto const& destination = requireItem(plan, kNewUri); hasFingerprint)
    {
      REQUIRE(plan.size() == 1);
      CHECK(destination.classification == ScanClassification::Moved);
      CHECK(destination.oldUri == kOldUri);
      CHECK(destination.trackId == id);
    }
    else
    {
      REQUIRE(plan.size() == 2);
      CHECK(destination.classification == ScanClassification::New);
      auto const& source = requireItem(plan, kOldUri);
      CHECK(source.classification == ScanClassification::Missing);
      CHECK(source.trackId == id);
    }

    auto const result = fixture.apply(std::move(plan), {.audioIdentityPolicy = policy});
    REQUIRE(result.failureCount == 0);
    CHECK(result.staleCount == 0);
    CHECK(result.missingCount == (hasFingerprint ? 0 : 1));
    fixture.reopenLibrary();
    auto const transaction = fixture.library.readTransaction();
    auto const manifest = fixture.library.manifest().reader(transaction);
    auto const optNew = manifest.get(kNewUri);
    REQUIRE(optNew);
    CHECK(optNew->status() == library::FileStatus::Available);
    auto const tracks = fixture.library.tracks().reader(transaction);
    CHECK(tracks.entryCount() == (hasFingerprint ? 1 : 2));
    auto const optTrack = tracks.get(id);
    REQUIRE(optTrack);

    if (hasFingerprint)
    {
      CHECK(result.relinkedIds == std::vector{id});
      CHECK(result.insertedIds.empty());
      CHECK_FALSE(manifest.get(kOldUri));
      CHECK(optNew->trackId() == id);
      CHECK(optTrack->property().uri() == kNewUri);
    }
    else
    {
      CHECK(result.relinkedIds.empty());
      REQUIRE(result.insertedIds.size() == 1);
      CHECK(optNew->trackId() == result.insertedIds.front());
      CHECK(optNew->trackId() != id);
      auto const optOld = manifest.get(kOldUri);
      REQUIRE(optOld);
      CHECK(optOld->trackId() == id);
      CHECK(optOld->status() == library::FileStatus::Missing);
      CHECK(optTrack->property().uri() == kOldUri);
      auto const optNewTrack = tracks.get(result.insertedIds.front());
      REQUIRE(optNewTrack);
      CHECK(optNewTrack->property().uri() == kNewUri);
    }
  }

  TEST_CASE("Scan directory spelling - revalidates a missing descendant when its parent is replaced")
  {
    auto fixture = SpellingFixture{};
    std::filesystem::remove(fixture.original);
    auto const oldDir = fixture.musicRoot / "Album";
    std::filesystem::create_directory(oldDir);
    writeScanFlacMetadataFixture(oldDir / "x.flac", {"TITLE=Missing track"});
    auto const id = fixture.importTrack();
    REQUIRE(std::filesystem::remove(oldDir / "x.flac"));
    REQUIRE(std::filesystem::remove(oldDir));
    auto plan = fixture.plan();
    REQUIRE(requireItem(plan, "Album/x.flac").classification == ScanClassification::Missing);
    auto operation = ScanApplyOperation{fixture.library, std::move(plan), {}, {}};
    requirePrepared(operation);

    auto const checkApply = [&](library::FileStatus expectedStatus)
    {
      auto const result = ao::test::requireValue(operation.run());
      auto const provedMissing = expectedStatus == library::FileStatus::Missing;
      CHECK(result.failureCount == (provedMissing ? 0 : 1));
      CHECK(result.missingCount == (provedMissing ? 1 : 0));
      auto const transaction = fixture.library.readTransaction();
      auto const optRow = fixture.library.manifest().reader(transaction).get("Album/x.flac");
      REQUIRE(optRow);
      CHECK(optRow->trackId() == id);
      CHECK(optRow->status() == expectedStatus);
    };

    SECTION("the exact name is now a regular file")
    {
      {
        auto file = std::ofstream{oldDir, std::ios::binary};
        file << "This is a regular file, not a directory.";
        REQUIRE(file.good());
      }
      checkApply(library::FileStatus::Missing);
    }

    SECTION("the exact name is now an unresolved redirect")
    {
      auto const link = ao::test::SymlinkFixture{fixture.temp.path() / "absent", oldDir, ao::test::SymlinkType::File};
      checkApply(library::FileStatus::Available);
    }
  }

  TEST_CASE("Scan directory spelling - unrelated redirected variant does not block an ordinary move")
  {
    auto fixture = SpellingFixture{};

    if (fixture.isCaseInsensitive())
    {
      SKIP("The temporary music directory cannot contain both spellings");
    }

    std::filesystem::remove(fixture.original);
    auto const oldDir = fixture.musicRoot / "Album";
    auto const variantDir = fixture.musicRoot / "album";
    auto const otherDir = fixture.musicRoot / "Other";
    std::filesystem::create_directory(oldDir);
    std::filesystem::create_directory(variantDir);
    std::filesystem::create_directory(otherDir);
    REQUIRE_FALSE(std::filesystem::equivalent(oldDir, variantDir));
    writeScanFlacMetadataFixture(oldDir / "x.flac", {"TITLE=First"}, 88200);
    auto const id = fixture.importTrack(AudioIdentityPolicy::Eager);
    auto const identity = readManifestIdentity(fixture, "Album/x.flac");
    REQUIRE(library::hasAudioIdentity(identity.payloadLength, identity.signature));
    std::filesystem::rename(oldDir / "x.flac", otherDir / "x.flac");

    SECTION("old parent still exists")
    {
      REQUIRE(std::filesystem::is_empty(oldDir));
    }

    SECTION("old parent is absent")
    {
      REQUIRE(std::filesystem::remove(oldDir));
    }

    auto const link = ao::test::SymlinkFixture{otherDir / "x.flac", variantDir / "x.flac", ao::test::SymlinkType::File};
    REQUIRE(std::filesystem::equivalent(variantDir / "x.flac", otherDir / "x.flac"));
    auto plan = fixture.plan();
    auto const& moved = requireItem(plan, "Other/x.flac");
    CHECK(moved.classification == ScanClassification::Moved);
    CHECK(moved.oldUri == "Album/x.flac");
    CHECK(moved.trackId == id);
    auto const result = fixture.apply(std::move(plan));
    CHECK(result.failureCount == 0);
    CHECK(result.insertedIds.empty());
    CHECK(result.relinkedIds == std::vector{id});
    fixture.reopenLibrary();
    auto transaction = fixture.library.readTransaction();
    auto const manifest = fixture.library.manifest().reader(transaction);
    CHECK_FALSE(manifest.get("Album/x.flac"));
    CHECK_FALSE(manifest.get("album/x.flac"));
    auto const optRow = manifest.get("Other/x.flac");
    REQUIRE(optRow);
    CHECK(optRow->trackId() == id);
    auto const optTrack = fixture.library.tracks().reader(transaction).get(id);
    REQUIRE(optTrack);
    CHECK(optTrack->property().uri() == "Other/x.flac");
  }

  TEST_CASE("Scan directory spelling - keeps coexisting parent directories independent")
  {
    auto fixture = SpellingFixture{};

    if (fixture.isCaseInsensitive())
    {
      SKIP("The temporary music directory cannot contain both spellings");
    }

    std::filesystem::remove(fixture.original);
    auto const oldDir = fixture.musicRoot / "Album";
    auto const newDir = fixture.musicRoot / "album";
    std::filesystem::create_directory(oldDir);
    writeScanFlacMetadataFixture(oldDir / "x.flac", {"TITLE=First"}, 88200);
    auto const id = fixture.importTrack();
    std::filesystem::create_directory(newDir);
    std::filesystem::copy_file(oldDir / "x.flac", newDir / "x.flac");
    REQUIRE_FALSE(std::filesystem::equivalent(oldDir, newDir));
    auto plan = fixture.plan();
    REQUIRE(plan.size() == 2);
    CHECK(requireItem(plan, "Album/x.flac").classification == ScanClassification::Unchanged);
    CHECK(requireItem(plan, "album/x.flac").classification == ScanClassification::New);
    auto const result = fixture.apply(std::move(plan));
    REQUIRE(result.insertedIds.size() == 1);
    CHECK(result.insertedIds.front() != id);
    CHECK(result.relinkedIds.empty());
    CHECK(result.failureCount == 0);
  }

  TEST_CASE("Scan spelling - rekeys the original Track and keeps curation in a case-insensitive directory")
  {
    auto fixture = SpellingFixture{};

    if (!fixture.isCaseInsensitive())
    {
      SKIP("The temporary music directory is case-sensitive");
    }

    auto policy = AudioIdentityPolicy::Eager;

    SECTION("pending audio identity")
    {
      policy = AudioIdentityPolicy::DeferNew;
    }

    SECTION("known audio identity")
    {
      policy = AudioIdentityPolicy::Eager;
    }

    auto const trackId = fixture.importTrack(policy);
    library::test::updateTrackSpec(fixture.library,
                                   trackId,
                                   [](library::test::TrackSpec& spec)
                                   {
                                     spec.title = "Curated Title";
                                     spec.tags = {"favorite"};
                                     spec.customMetadata = {{"catalog", "AOB-42"}};
                                   });
    auto const identity = readManifestIdentity(fixture, kOriginalUri);
    REQUIRE(library::hasAudioIdentity(identity.payloadLength, identity.signature) ==
            (policy == AudioIdentityPolicy::Eager));

    renameCase(fixture.original, fixture.renamed);
    auto plan = fixture.plan();
    requireSpellingChange(plan, trackId);
    auto const result = fixture.apply(std::move(plan), {.audioIdentityPolicy = policy});

    CHECK(result.relinkedIds == std::vector{trackId});
    CHECK(result.relinkedFileCount == 1);
    CHECK(result.insertedIds.empty());
    CHECK(result.missingCount == 0);
    CHECK(result.staleCount == 0);
    CHECK(result.failureCount == 0);
    checkWholeFileRekey(fixture, trackId, kInitialMtime);
    auto const spec = storedTrackSpec(fixture.library, trackId);
    CHECK(spec.title == "Curated Title");
    CHECK(spec.tags == std::vector<std::string>{"favorite"});
    REQUIRE(spec.customMetadata.size() == 1);
    CHECK(spec.customMetadata.front() == std::pair<std::string, std::string>{"catalog", "AOB-42"});
    auto const renamedIdentity = readManifestIdentity(fixture, kRenamedUri);
    CHECK(renamedIdentity.payloadLength == identity.payloadLength);
    CHECK(renamedIdentity.signature == identity.signature);
    checkUnchangedSpelling(fixture.plan());
  }

  TEST_CASE("Scan spelling - preserves a backfill committed after planning in a case-insensitive directory",
            "[concurrency]")
  {
    auto fixture = SpellingFixture{};

    if (!fixture.isCaseInsensitive())
    {
      SKIP("The temporary music directory is case-sensitive");
    }

    auto const trackId = fixture.importTrack();
    auto const candidate = [&]
    {
      auto file = ao::test::requireValue(media::file::File::open(fixture.original));
      auto const payload = ao::test::requireValue(file.audioPayload());
      auto const optIdentity = library::readAudioIdentity(payload.bytes);
      REQUIRE(optIdentity);
      return AudioIdentityWriteCandidate{.uri = kOriginalUri,
                                         .fileSize = std::filesystem::file_size(fixture.original),
                                         .optMtime = kInitialMtime,
                                         .identity = *optIdentity};
    }();
    renameCase(fixture.original, fixture.renamed);
    auto plan = fixture.plan();
    requireSpellingChange(plan, trackId);
    {
      auto transaction = library::test::writeTransaction(fixture.library);
      auto const backfill = ao::test::requireValue(
        transaction.apply([&candidate](library::LibraryWrite& write)
                          { return applyAudioIdentityBatch(write, std::span{&candidate, 1U}); }));
      REQUIRE(backfill.completedCount == 1);
      REQUIRE(transaction.commit());
    }

    auto const result = fixture.apply(std::move(plan));

    CHECK(result.relinkedIds == std::vector{trackId});
    CHECK(result.staleCount == 0);
    CHECK(result.failureCount == 0);
    checkWholeFileRekey(fixture, trackId, kInitialMtime);
    auto const identity = readManifestIdentity(fixture, kRenamedUri);
    CHECK(identity.payloadLength == candidate.identity.payloadLength);
    CHECK(identity.signature == candidate.identity.signature);
  }

  TEST_CASE("Scan spelling - refreshes changed file facts in a case-insensitive directory")
  {
    auto fixture = SpellingFixture{};

    if (!fixture.isCaseInsensitive())
    {
      SKIP("The temporary music directory is case-sensitive");
    }

    auto const trackId = fixture.importTrack();
    library::test::updateTrackSpec(
      fixture.library, trackId, [](library::test::TrackSpec& spec) { spec.title = "Curated"; });
    auto const initialSize = std::filesystem::file_size(fixture.original);
    renameCase(fixture.original, fixture.renamed);
    std::filesystem::copy_file(audio::test::requireAudioFixture("hires.flac"),
                               fixture.renamed,
                               std::filesystem::copy_options::overwrite_existing);
    pinMtime(fixture.renamed, kChangedMtime);
    REQUIRE(std::filesystem::file_size(fixture.renamed) != initialSize);
    auto plan = fixture.plan();
    requireSpellingChange(plan, trackId);

    auto const result = fixture.apply(std::move(plan));

    CHECK(result.relinkedIds == std::vector{trackId});
    CHECK(result.insertedIds.empty());
    CHECK(result.failureCount == 0);
    checkWholeFileRekey(fixture, trackId, kChangedMtime);
    auto const spec = storedTrackSpec(fixture.library, trackId);
    CHECK(spec.title == "Curated");
    CHECK(spec.sampleRate == SampleRate{96000});
    checkUnchangedSpelling(fixture.plan());
  }

  TEST_CASE("Scan spelling - reports corrupt renamed media without blocking its peer in a case-insensitive directory")
  {
    auto fixture = SpellingFixture{};

    if (!fixture.isCaseInsensitive())
    {
      SKIP("The temporary music directory is case-sensitive");
    }

    auto const trackId = fixture.importTrack();
    renameCase(fixture.original, fixture.renamed);
    {
      auto file = std::ofstream{fixture.renamed, std::ios::binary};
      file << "Not a FLAC file";
      REQUIRE(file.good());
    }
    pinMtime(fixture.renamed, kChangedMtime);
    constexpr auto kPeerUri = "peer.flac";
    std::filesystem::copy_file(audio::test::requireAudioFixture("hires.flac"), fixture.musicRoot / kPeerUri);
    auto plan = fixture.plan();

    CHECK(requireItem(plan, kRenamedUri).classification == ScanClassification::Error);
    CHECK(plan.count(ScanClassification::SpellingChanged) == 0);
    CHECK(requireItem(plan, kPeerUri).classification == ScanClassification::New);
    auto const result = fixture.apply(std::move(plan));
    CHECK(result.failureCount == 1);
    CHECK(result.relinkedIds.empty());
    CHECK(result.mutatedIds.empty());
    REQUIRE(result.insertedIds.size() == 1);

    fixture.reopenLibrary();
    {
      auto transaction = fixture.library.readTransaction();
      auto const manifest = fixture.library.manifest().reader(transaction);
      auto const optOld = manifest.get(kOriginalUri);
      auto const optPeer = manifest.get(kPeerUri);
      REQUIRE(optOld);
      REQUIRE(optPeer);
      CHECK(optOld->trackId() == trackId);
      CHECK(optOld->status() == library::FileStatus::Available);
      CHECK(optOld->mtime() == kInitialMtime);
      CHECK_FALSE(manifest.get(kRenamedUri));
      CHECK(optPeer->trackId() == result.insertedIds.front());
      CHECK(fixture.library.tracks().reader(transaction).entryCount() == 2);
    }
    auto const rescanned = fixture.plan();
    CHECK(requireItem(rescanned, kRenamedUri).classification == ScanClassification::Error);
    CHECK(requireItem(rescanned, kPeerUri).classification == ScanClassification::Unchanged);
    CHECK(rescanned.count(ScanClassification::SpellingChanged) == 0);
  }

  TEST_CASE("Scan spelling - restores a missing Track under the current case in a case-insensitive directory")
  {
    auto fixture = SpellingFixture{};

    if (!fixture.isCaseInsensitive())
    {
      SKIP("The temporary music directory is case-sensitive");
    }

    auto const trackId = fixture.importTrack();
    auto const backup = fixture.temp.path() / "backup.flac";
    std::filesystem::rename(fixture.original, backup);
    auto const missing = fixture.apply(fixture.plan());
    REQUIRE(missing.missingCount == 1);
    {
      auto transaction = fixture.library.readTransaction();
      auto const optRow = fixture.library.manifest().reader(transaction).get(kOriginalUri);
      REQUIRE(optRow);
      REQUIRE(optRow->status() == library::FileStatus::Missing);
    }

    std::filesystem::rename(backup, fixture.renamed);
    auto plan = fixture.plan();
    requireSpellingChange(plan, trackId);
    auto const result = fixture.apply(std::move(plan));

    CHECK(result.relinkedIds == std::vector{trackId});
    CHECK(result.insertedIds.empty());
    CHECK(result.missingCount == 0);
    CHECK(result.failureCount == 0);
    checkWholeFileRekey(fixture, trackId, kInitialMtime);
    checkUnchangedSpelling(fixture.plan());
  }

  TEST_CASE("Scan spelling - refuses a replaced pending file in a case-insensitive directory", "[concurrency]")
  {
    auto fixture = SpellingFixture{};

    if (!fixture.isCaseInsensitive())
    {
      SKIP("The temporary music directory is case-sensitive");
    }

    auto const trackId = fixture.importTrack();
    auto const identity = readManifestIdentity(fixture, kOriginalUri);
    REQUIRE_FALSE(library::hasAudioIdentity(identity.payloadLength, identity.signature));
    renameCase(fixture.original, fixture.renamed);
    constexpr auto kPeerUri = "peer.flac";
    std::filesystem::copy_file(audio::test::requireAudioFixture("hires.flac"), fixture.musicRoot / kPeerUri);
    auto plan = fixture.plan();
    requireSpellingChange(requireItem(plan, kRenamedUri), trackId);
    REQUIRE(requireItem(plan, kPeerUri).classification == ScanClassification::New);
    auto operation = ScanApplyOperation{fixture.library, std::move(plan), {}, {}};
    auto const revision = fixture.library.libraryRevision(fixture.library.readTransaction());

    SECTION("replacement before preparation")
    {
      // run() will prepare the operation after the replacement.
    }

    SECTION("replacement after preparation")
    {
      requirePrepared(operation);
    }

    // The original stays alive: the byte-identical replacement has a different
    // native identity even though the path and all ordinary facts still match.
    auto const backup = fixture.temp.path() / "backup.flac";
    auto const plannedWriteTime = std::filesystem::last_write_time(fixture.renamed);
    std::filesystem::rename(fixture.renamed, backup);
    std::filesystem::copy_file(backup, fixture.renamed);
    std::filesystem::last_write_time(fixture.renamed, plannedWriteTime);
    REQUIRE(std::filesystem::file_size(fixture.renamed) == std::filesystem::file_size(backup));
    REQUIRE(std::filesystem::last_write_time(fixture.renamed) == plannedWriteTime);
    REQUIRE_FALSE(std::filesystem::equivalent(fixture.renamed, backup));

    auto const result = ao::test::requireValue(operation.run());

    CHECK(result.relinkedIds.empty());
    CHECK(result.insertedIds.empty());
    CHECK(result.mutatedIds.empty());
    CHECK(result.failureCount == 1);
    CHECK(result.staleCount == 0);
    auto transaction = fixture.library.readTransaction();
    CHECK(fixture.library.libraryRevision(transaction) == revision);
    auto const manifest = fixture.library.manifest().reader(transaction);
    auto const optRow = manifest.get(kOriginalUri);
    REQUIRE(optRow);
    CHECK(optRow->status() == library::FileStatus::Available);
    CHECK(optRow->trackId() == trackId);
    CHECK_FALSE(manifest.get(kRenamedUri));
    CHECK_FALSE(manifest.get(kPeerUri));
    auto const optTrack = fixture.library.tracks().reader(transaction).get(trackId);
    REQUIRE(optTrack);
    CHECK(optTrack->property().uri() == kOriginalUri);
  }

  TEST_CASE("Scan spelling - cancellation during preparation remains terminal without writes")
  {
    auto fixture = SpellingFixture{};

    if (!fixture.isCaseInsensitive())
    {
      SKIP("The temporary music directory is case-sensitive");
    }

    auto const trackId = fixture.importTrack();
    renameCase(fixture.original, fixture.renamed);
    constexpr auto kPeerUri = "peer.flac";
    std::filesystem::copy_file(audio::test::requireAudioFixture("hires.flac"), fixture.musicRoot / kPeerUri);
    auto plan = fixture.plan();
    requireSpellingChange(requireItem(plan, kRenamedUri), trackId);
    REQUIRE(requireItem(plan, kPeerUri).classification == ScanClassification::New);
    auto const revision = fixture.library.libraryRevision(fixture.library.readTransaction());
    auto stopSource = std::stop_source{};
    bool reportedFailure = false;
    auto operation = ScanApplyOperation{
      fixture.library,
      std::move(plan),
      [&stopSource](ScanApplyProgress const& progress)
      {
        // Cancellation first becomes visible inside the native spelling inspection.
        if (progress.stage == ScanApplyProgressStage::Updating && progress.path.filename() == kRenamedUri)
        {
          CHECK(stopSource.request_stop());
        }
      },
      [&reportedFailure](ScanFailure const&) { reportedFailure = true; }};

    auto const result = ao::test::requireValue(operation.prepare(stopSource.get_token()));

    CHECK(stopSource.stop_requested());
    CHECK(operation.isCancelled());
    CHECK_FALSE(operation.isReadyForMutation());
    CHECK_FALSE(operation.shouldCommitTransaction());
    CHECK(result.libraryRevision == 0);
    CHECK(result.insertedIds.empty());
    CHECK(result.mutatedIds.empty());
    CHECK(result.relinkedIds.empty());
    CHECK(result.relinkedFileCount == 0);
    CHECK(result.missingCount == 0);
    CHECK(result.failureCount == 0);
    CHECK(result.staleCount == 0);
    CHECK_FALSE(reportedFailure);

    REQUIRE_THROWS_AS(operation.run(), async::OperationCancelled);
    auto transaction = fixture.library.readTransaction();
    CHECK(fixture.library.libraryRevision(transaction) == revision);
    auto const manifest = fixture.library.manifest().reader(transaction);
    auto const optSource = manifest.get(kOriginalUri);
    REQUIRE(optSource);
    CHECK(optSource->trackId() == trackId);
    CHECK(optSource->status() == library::FileStatus::Available);
    CHECK_FALSE(manifest.get(kRenamedUri));
    CHECK_FALSE(manifest.get(kPeerUri));
    auto const tracks = fixture.library.tracks().reader(transaction);
    CHECK(tracks.entryCount() == 1);
    auto const optTrack = tracks.get(trackId);
    REQUIRE(optTrack);
    CHECK(optTrack->property().uri() == kOriginalUri);
  }

  TEST_CASE("Scan spelling - refuses ambiguous library aliases in a case-insensitive directory", "[concurrency]")
  {
    auto fixture = SpellingFixture{};

    if (!fixture.isCaseInsensitive())
    {
      SKIP("The temporary music directory is case-sensitive");
    }

    auto const trackId = fixture.importTrack();
    constexpr auto kOtherUri = "A.FLAC";
    renameCase(fixture.original, fixture.renamed);
    REQUIRE(std::filesystem::equivalent(fixture.renamed, fixture.musicRoot / kOtherUri));
    auto otherId = kInvalidTrackId;

    SECTION("both rows before preview")
    {
      otherId = createHistoricalAlias(fixture, kOtherUri);
    }

    SECTION("third spelling added after preview")
    {
      // Leave a unique source until the plan below has captured the pair.
    }

    auto plan = fixture.plan();

    if (otherId == kInvalidTrackId)
    {
      requireSpellingChange(plan, trackId);
      otherId = createHistoricalAlias(fixture, kOtherUri);
    }
    else
    {
      auto const& conflict = requireItem(plan, kRenamedUri);
      CHECK(conflict.classification == ScanClassification::Error);
      CHECK_FALSE(conflict.errorMessage.empty());
      CHECK(plan.count(ScanClassification::SpellingChanged) == 0);
      CHECK(plan.count(ScanClassification::Moved) == 0);
    }

    auto const revision = fixture.library.libraryRevision(fixture.library.readTransaction());
    auto const result = fixture.apply(std::move(plan));
    CHECK(result.relinkedIds.empty());
    CHECK(result.insertedIds.empty());
    CHECK(result.missingCount == 0);
    CHECK(result.mutatedIds.empty());
    CHECK(result.failureCount > 0);
    auto transaction = fixture.library.readTransaction();
    CHECK(fixture.library.libraryRevision(transaction) == revision);
    auto const manifest = fixture.library.manifest().reader(transaction);
    CHECK_FALSE(manifest.get(kRenamedUri));
    auto const optFirst = manifest.get(kOriginalUri);
    auto const optSecond = manifest.get(kOtherUri);
    REQUIRE(optFirst);
    REQUIRE(optSecond);
    CHECK(optFirst->trackId() == trackId);
    CHECK(optSecond->trackId() == otherId);
    CHECK(optFirst->status() == library::FileStatus::Available);
    CHECK(optSecond->status() == library::FileStatus::Available);
    auto const tracks = fixture.library.tracks().reader(transaction);
    CHECK(tracks.entryCount() == 2);
    auto const optOriginalTrack = tracks.get(trackId);
    auto const optOtherTrack = tracks.get(otherId);
    REQUIRE(optOriginalTrack);
    REQUIRE(optOtherTrack);
    CHECK(optOriginalTrack->property().uri() == kOriginalUri);
    CHECK(optOtherTrack->property().uri() == kOtherUri);
  }

  TEST_CASE(
    "Scan spelling - reports historical aliases without blocking the exact owner in a case-insensitive directory")
  {
    auto fixture = SpellingFixture{};

    if (!fixture.isCaseInsensitive())
    {
      SKIP("The temporary music directory is case-sensitive");
    }

    auto const oldId = fixture.importTrack(AudioIdentityPolicy::Eager);
    auto const identity = readManifestIdentity(fixture, kOriginalUri);
    REQUIRE(library::hasAudioIdentity(identity.payloadLength, identity.signature));
    auto oldStatus = library::FileStatus::Available;

    SECTION("previously available alias")
    {
      // The original row still records availability.
    }

    SECTION("previously missing alias")
    {
      auto const backup = fixture.temp.path() / "backup.flac";
      std::filesystem::rename(fixture.original, backup);
      REQUIRE(fixture.apply(fixture.plan()).missingCount == 1);
      std::filesystem::rename(backup, fixture.original);
      oldStatus = library::FileStatus::Missing;
    }

    renameCase(fixture.original, fixture.renamed);
    auto const exactId = createHistoricalAlias(fixture, kRenamedUri);
    constexpr auto kPeerUri = "peer.flac";
    // The peer carries the old payload and would be a plausible fingerprint move.
    std::filesystem::copy_file(fixture.renamed, fixture.musicRoot / kPeerUri);
    std::filesystem::copy_file(audio::test::requireAudioFixture("hires.flac"),
                               fixture.renamed,
                               std::filesystem::copy_options::overwrite_existing);
    pinMtime(fixture.renamed, kChangedMtime);
    auto plan = fixture.plan();

    auto const& notice = requireItem(plan, kOriginalUri);
    CHECK(notice.classification == ScanClassification::Notice);
    CHECK(isScanNotice(notice));
    CHECK(requireItem(plan, kRenamedUri).classification == ScanClassification::Changed);
    CHECK(requireItem(plan, kPeerUri).classification == ScanClassification::New);
    CHECK(plan.count(ScanClassification::Moved) == 0);
    CHECK(plan.count(ScanClassification::SpellingChanged) == 0);
    REQUIRE_FALSE(fixture.plan().makeRelinkPlan(kOriginalUri, kPeerUri));
    auto const result = fixture.apply(std::move(plan));

    CHECK(result.failureCount == 0);
    CHECK(result.missingCount == 0);
    CHECK(result.relinkedIds.empty());
    CHECK(result.mutatedIds == std::vector{exactId});
    REQUIRE(result.insertedIds.size() == 1);
    auto transaction = fixture.library.readTransaction();
    auto const manifest = fixture.library.manifest().reader(transaction);
    auto const optOld = manifest.get(kOriginalUri);
    auto const optExact = manifest.get(kRenamedUri);
    auto const optPeer = manifest.get(kPeerUri);
    REQUIRE(optOld);
    REQUIRE(optExact);
    REQUIRE(optPeer);
    CHECK(optOld->trackId() == oldId);
    CHECK(optOld->status() == oldStatus);
    CHECK(optExact->trackId() == exactId);
    CHECK(optExact->mtime() == kChangedMtime);
    CHECK(optPeer->trackId() == result.insertedIds.front());
    CHECK(fixture.library.tracks().reader(transaction).entryCount() == 3);
  }

  TEST_CASE("Scan spelling - a symlink conflict prevents fingerprint relink in a case-insensitive directory")
  {
    auto fixture = SpellingFixture{};

    if (!fixture.isCaseInsensitive())
    {
      SKIP("The temporary music directory is case-sensitive");
    }

    auto const trackId = fixture.importTrack(AudioIdentityPolicy::Eager);
    auto const identity = readManifestIdentity(fixture, kOriginalUri);
    REQUIRE(library::hasAudioIdentity(identity.payloadLength, identity.signature));
    constexpr auto kCopyUri = "unrelated.flac";
    std::filesystem::copy_file(fixture.original, fixture.musicRoot / kCopyUri);
    REQUIRE(std::filesystem::remove(fixture.original));
    auto const target = fixture.musicRoot / "symlink-target.flac";

    SECTION("dangling file symlink")
    {
      REQUIRE_FALSE(std::filesystem::exists(target));
    }

    SECTION("in-root target with different audio")
    {
      std::filesystem::copy_file(audio::test::requireAudioFixture("hires.flac"), target);
    }

    auto const link = ao::test::SymlinkFixture{target, fixture.renamed, ao::test::SymlinkType::File};
    REQUIRE(hasExactEntry(fixture.renamed));
    REQUIRE_FALSE(hasExactEntry(fixture.original));
    REQUIRE(std::filesystem::is_symlink(fixture.original));
    auto plan = fixture.plan();

    auto const& conflict = requireItem(plan, kOriginalUri);
    CHECK(conflict.classification == ScanClassification::Error);
    CHECK_FALSE(conflict.errorMessage.empty());
    CHECK(requireItem(plan, kCopyUri).classification == ScanClassification::New);
    CHECK(plan.count(ScanClassification::Moved) == 0);
    CHECK(plan.count(ScanClassification::SpellingChanged) == 0);
    auto const result = fixture.apply(std::move(plan));
    CHECK(result.failureCount > 0);
    CHECK(result.relinkedIds.empty());
    CHECK(result.missingCount == 0);
    auto transaction = fixture.library.readTransaction();
    auto const manifest = fixture.library.manifest().reader(transaction);
    auto const optRow = manifest.get(kOriginalUri);
    REQUIRE(optRow);
    CHECK(optRow->trackId() == trackId);
    CHECK(optRow->status() == library::FileStatus::Available);
    CHECK_FALSE(manifest.get(kRenamedUri));
    auto const optTrack = fixture.library.tracks().reader(transaction).get(trackId);
    REQUIRE(optTrack);
    CHECK(optTrack->property().uri() == kOriginalUri);
  }

  TEST_CASE("Scan spelling - rekeys CUE members only with exact carrier facts in a case-insensitive directory")
  {
    auto fixture = SpellingFixture{};

    if (!fixture.isCaseInsensitive())
    {
      SKIP("The temporary music directory is case-sensitive");
    }

    auto const originalSheet = fixture.musicRoot / "A.cue";
    {
      auto sheet = std::ofstream{originalSheet};
      sheet << "FILE \"rip.wav\" WAVE\nTRACK 01 AUDIO\nTITLE \"First\"\nINDEX 01 00:00:00\n"
               "TRACK 03 AUDIO\nTITLE \"Third\"\nINDEX 01 00:01:00\n";
      REQUIRE(sheet.good());
    }
    auto const imported = fixture.apply(fixture.plan());
    REQUIRE(imported.insertedIds.size() == 2);
    REQUIRE(imported.failureCount == 0);
    library::test::updateTrackSpec(fixture.library,
                                   imported.insertedIds[0],
                                   [](library::test::TrackSpec& spec) { spec.title = "Curated cue member"; });

    renameCase(fixture.original, fixture.renamed);
    renameCase(originalSheet, fixture.musicRoot / "a.cue");
    constexpr auto kPeerUri = "peer.flac";
    bool hasExactExtent = true;

    SECTION("unchanged carrier")
    {
      // Keep the exact STREAMINFO sample count.
    }

    SECTION("carrier extent becomes unknown")
    {
      hasExactExtent = false;
      writeScanFlacMetadataFixture(fixture.renamed, {"TITLE=Embedded title"}, 0);
      pinMtime(fixture.renamed, kChangedMtime);
      std::filesystem::copy_file(audio::test::requireAudioFixture("hires.flac"), fixture.musicRoot / kPeerUri);
    }

    auto plan = fixture.plan();
    auto const revision = fixture.library.libraryRevision(fixture.library.readTransaction());

    if (hasExactExtent)
    {
      requireSpellingChange(plan, kInvalidTrackId);
    }
    else
    {
      auto const& error = requireItem(plan, kRenamedUri);
      CHECK(error.classification == ScanClassification::Error);
      CHECK(error.cueDiagnostic == ScanCueDiagnostic::CarrierFactsMismatch);
      CHECK(error.errorMessage.contains("exact frame count is unavailable"));
      CHECK(requireItem(plan, kPeerUri).classification == ScanClassification::New);
    }

    auto const result = fixture.apply(std::move(plan));

    CHECK(result.staleCount == 0);

    if (hasExactExtent)
    {
      CHECK(result.insertedIds.empty());
      CHECK(result.relinkedIds == imported.insertedIds);
      CHECK(result.relinkedFileCount == 1);
      CHECK(result.failureCount == 0);
      checkUnchangedSpelling(fixture.plan());
    }
    else
    {
      CHECK(result.relinkedIds.empty());
      CHECK(result.mutatedIds.empty());
      CHECK(result.missingCount == 0);
      CHECK(result.failureCount == 1);
      REQUIRE(result.insertedIds.size() == 1);
    }

    auto const* const expectedUri = hasExactExtent ? kRenamedUri : kOriginalUri;
    {
      auto transaction = fixture.library.readTransaction();
      auto const manifest = fixture.library.manifest().reader(transaction);

      if (!hasExactExtent)
      {
        CHECK(fixture.library.libraryRevision(transaction) == revision + 1U);
        auto const optPeer = manifest.get(kPeerUri);
        REQUIRE(optPeer);
        CHECK(optPeer->trackId() == result.insertedIds.front());
        CHECK(optPeer->status() == library::FileStatus::Available);
      }

      CHECK_FALSE(manifest.get(hasExactExtent ? kOriginalUri : kRenamedUri));
      auto const optRow = manifest.get(expectedUri);
      REQUIRE(optRow);
      CHECK(optRow->status() == library::FileStatus::Available);
      CHECK(optRow->totalFrames() == 88200);
      CHECK(std::ranges::equal(optRow->segmentTrackIds(), imported.insertedIds));
      auto const tracks = fixture.library.tracks().reader(transaction);
      auto const optFirst = tracks.get(imported.insertedIds[0]);
      auto const optThird = tracks.get(imported.insertedIds[1]);
      REQUIRE(optFirst);
      REQUIRE(optThird);
      CHECK(optFirst->property().uri() == expectedUri);
      CHECK(optThird->property().uri() == expectedUri);
      REQUIRE(optFirst->property().segment());
      REQUIRE(optThird->property().segment());
      CHECK(optFirst->property().segment()->ordinal == 1);
      CHECK(optFirst->property().segment()->startFrame == 0);
      CHECK(optFirst->property().segment()->frameCount == 44100);
      CHECK(optThird->property().segment()->ordinal == 3);
      CHECK(optThird->property().segment()->startFrame == 44100);
      CHECK(optThird->property().segment()->frameCount == 44100);
      CHECK(optFirst->metadata().title() == "Curated cue member");
      CHECK(optThird->metadata().title() == "Third");
    }
  }

  TEST_CASE("Scan spelling - keeps coexisting names independent in a case-sensitive directory")
  {
    auto fixture = SpellingFixture{};

    if (fixture.isCaseInsensitive())
    {
      SKIP("The temporary music directory cannot contain both spellings");
    }

    auto const trackId = fixture.importTrack();

    SECTION("different files")
    {
      std::filesystem::copy_file(audio::test::requireAudioFixture("hires.flac"), fixture.renamed);
      REQUIRE_FALSE(std::filesystem::equivalent(fixture.original, fixture.renamed));
    }

    SECTION("hardlinks to the same file")
    {
      std::filesystem::create_hard_link(fixture.original, fixture.renamed);
      REQUIRE(std::filesystem::equivalent(fixture.original, fixture.renamed));
    }

    REQUIRE(hasExactEntry(fixture.original));
    REQUIRE(hasExactEntry(fixture.renamed));
    auto plan = fixture.plan();
    REQUIRE(plan.size() == 2);
    CHECK(requireItem(plan, kOriginalUri).classification == ScanClassification::Unchanged);
    CHECK(requireItem(plan, kRenamedUri).classification == ScanClassification::New);

    auto const result = fixture.apply(std::move(plan));

    REQUIRE(result.insertedIds.size() == 1);
    auto const newId = result.insertedIds.front();
    CHECK(newId != trackId);
    CHECK(result.relinkedIds.empty());
    CHECK(result.failureCount == 0);
    auto transaction = fixture.library.readTransaction();
    auto const manifest = fixture.library.manifest().reader(transaction);
    auto const optOld = manifest.get(kOriginalUri);
    auto const optNew = manifest.get(kRenamedUri);
    REQUIRE(optOld);
    REQUIRE(optNew);
    CHECK(optOld->trackId() == trackId);
    CHECK(optNew->trackId() == newId);
    CHECK(optOld->status() == library::FileStatus::Available);
    CHECK(optNew->status() == library::FileStatus::Available);
  }

  TEST_CASE("Scan spelling - follows audio identity despite an exact source symlink in a case-sensitive directory")
  {
    auto fixture = SpellingFixture{};

    if (fixture.isCaseInsensitive())
    {
      SKIP("The temporary music directory cannot contain both spellings");
    }

    auto const trackId = fixture.importTrack(AudioIdentityPolicy::Eager);
    renameCase(fixture.original, fixture.renamed);
    auto const link = ao::test::SymlinkFixture{fixture.renamed, fixture.original, ao::test::SymlinkType::File};
    REQUIRE(hasExactEntry(fixture.original));
    REQUIRE(std::filesystem::equivalent(fixture.original, fixture.renamed));
    auto plan = fixture.plan();

    REQUIRE(plan.size() == 1);
    auto const& moved = requireItem(plan, kRenamedUri);
    CHECK(moved.classification == ScanClassification::Moved);
    CHECK(moved.oldUri == kOriginalUri);
    CHECK(moved.trackId == trackId);
    CHECK(hasAudioIdentity(moved));
    auto const result = fixture.apply(std::move(plan));

    CHECK(result.failureCount == 0);
    CHECK(result.insertedIds.empty());
    CHECK(result.relinkedIds == std::vector{trackId});
    checkWholeFileRekey(fixture, trackId, kInitialMtime);
    checkUnchangedSpelling(fixture.plan());
  }

  TEST_CASE("Scan spelling - leaves a case-only rename unresolved without audio identity in a case-sensitive directory")
  {
    auto fixture = SpellingFixture{};

    if (fixture.isCaseInsensitive())
    {
      SKIP("The temporary music directory resolves case variants to the same entry");
    }

    auto const trackId = fixture.importTrack();
    renameCase(fixture.original, fixture.renamed);
    REQUIRE_FALSE(std::filesystem::exists(fixture.original));
    auto const plan = fixture.plan();

    REQUIRE(plan.size() == 2);
    auto const& missing = requireItem(plan, kOriginalUri);
    CHECK(missing.classification == ScanClassification::Missing);
    CHECK(missing.trackId == trackId);
    CHECK_FALSE(hasAudioIdentity(missing));
    auto const& added = requireItem(plan, kRenamedUri);
    CHECK(added.classification == ScanClassification::New);
    CHECK(added.trackId == kInvalidTrackId);
  }
} // namespace ao::rt::test
