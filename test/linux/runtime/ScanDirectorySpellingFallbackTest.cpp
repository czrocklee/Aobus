// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/library/ScanApplyOperation.h"
#include "runtime/library/ScanSpelling.h"
#include "test/FilesystemTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/library/MusicLibraryTestSupport.h"
#include "test/library/WritableLibraryTestSupport.h"
#include "test/runtime/library/ScanApplyTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/FileStatus.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackWriter.h>
#include <ao/rt/library/LibraryScan.h>
#include <ao/rt/library/ScanPlan.h>

#include <catch2/catch_test_macros.hpp>
#include <linux/limits.h>
#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  extern "C" int nativeLstat(char const* path, struct ::stat* status) noexcept asm("__real_lstat");

  namespace
  {
    constexpr auto kMarker = std::string_view{"/aobus-scan-spelling-no-file-id"};
    constexpr auto kOldDirectory = std::string_view{"/Album"};
    constexpr auto kNewDirectory = std::string_view{"/album"};
    constexpr auto kLeaf = std::string_view{"/x.flac"};
    constexpr auto kSecondLeaf = std::string_view{"/y.flac"};
    constexpr auto kRecreateMarker = std::string_view{"/.recreate-old-directory"};
    constexpr auto kRedirectMarker = std::string_view{"/.redirect/Album"};
    constexpr auto kOldUri = std::string_view{"Album/x.flac"};
    constexpr auto kNewUri = std::string_view{"album/x.flac"};
    constexpr auto kSecondOldUri = std::string_view{"Album/y.flac"};
    constexpr auto kSecondNewUri = std::string_view{"album/y.flac"};

    void recreateOldDirectoryIfArmed(std::array<char, PATH_MAX>& translated,
                                     std::size_t const rootSize,
                                     std::string_view const leaf) noexcept
    {
      auto marker = std::array<char, PATH_MAX>{};

      if (rootSize + kRecreateMarker.size() + leaf.size() >= marker.size())
      {
        return;
      }

      std::memcpy(marker.data(), translated.data(), rootSize);
      std::memcpy(marker.data() + rootSize, kRecreateMarker.data(), kRecreateMarker.size());
      std::memcpy(marker.data() + rootSize + kRecreateMarker.size(), leaf.data(), leaf.size());
      using FileStatus = struct ::stat;
      auto status = FileStatus{};
      auto const savedErrno = errno;

      if (nativeLstat(marker.data(), &status) == 0 && S_ISDIR(status.st_mode))
      {
        translated[rootSize + 1] = kOldDirectory[1];
        translated[rootSize + kOldDirectory.size()] = '\0';
        // Leave the completed raw lookup's result intact. The test checks
        // that this created a distinct directory, not a translated alias.
        std::ignore = ::mkdir(translated.data(), 0700);
      }

      errno = savedErrno;
    }

    void redirectOldBlockerIfArmed(char const* path, std::size_t rootSize) noexcept
    {
      auto marker = std::array<char, PATH_MAX>{};

      if (rootSize + kRedirectMarker.size() >= marker.size())
      {
        return;
      }

      std::memcpy(marker.data(), path, rootSize);
      std::memcpy(marker.data() + rootSize, kRedirectMarker.data(), kRedirectMarker.size());
      auto const savedErrno = errno;
      // Atomically substitute the staged symlink after sampling the regular
      // file. The spellings in the probed directory remain exactly the same.
      std::ignore = ::rename(marker.data(), path);
      errno = savedErrno;
    }
  } // namespace

  // Only this ELF test executable wraps lstat. Inside its marked TempDir,
  // emulate direct case-insensitive parent lookup with no regular-file ID.
  // The directory ID, enumeration, file contents and other metadata are real.
  // This tests scan wiring, not the capabilities of the host filesystem.
  extern "C"
  {
    int spellingLstat(char const* path, struct ::stat* status) noexcept asm("__wrap_lstat");

    int spellingLstat(char const* path, struct ::stat* status) noexcept
    {
      auto const request = std::string_view{path};
      auto const primaryLeaf = request.ends_with(kLeaf) ? kLeaf : std::string_view{};
      auto const leaf = request.ends_with(kSecondLeaf) ? kSecondLeaf : primaryLeaf;
      auto const parent = request.substr(0, request.size() - leaf.size());
      auto const isOld = parent.ends_with(kOldDirectory);

      if ((!isOld && !parent.ends_with(kNewDirectory)) ||
          !parent.substr(0, parent.size() - kOldDirectory.size()).ends_with(kMarker))
      {
        return nativeLstat(path, status);
      }

      auto result = nativeLstat(path, status);

      if (isOld && leaf.empty() && result == 0 && S_ISREG(status->st_mode))
      {
        redirectOldBlockerIfArmed(path, parent.size() - kOldDirectory.size());
      }

      if (isOld && result != 0 && errno == ENOENT && request.size() < PATH_MAX)
      {
        auto translated = std::array<char, PATH_MAX>{};
        std::memcpy(translated.data(), path, request.size() + 1);
        auto const rootSize = parent.size() - kOldDirectory.size();
        translated[rootSize + 1] = kNewDirectory[1];
        result = nativeLstat(translated.data(), status);

        if (!leaf.empty() && result == 0 && S_ISREG(status->st_mode))
        {
          recreateOldDirectoryIfArmed(translated, rootSize, leaf);
        }
      }

      if (!leaf.empty() && result == 0 && S_ISREG(status->st_mode))
      {
        status->st_ino = 0;
      }

      return result;
    }
  }

  namespace
  {
    namespace fs = std::filesystem;

    fs::path makeRoot(fs::path const& temp)
    {
      auto root = temp / kMarker.substr(1);
      fs::create_directories(root / kOldDirectory.substr(1));
      return root;
    }

    struct FallbackFixture final
    {
      ao::test::TempDir temp;
      fs::path root = makeRoot(temp.path());
      library::MusicLibrary library = library::test::makeTestMusicLibrary(root, temp.path() / "db");

      FallbackFixture() { writeScanFlacMetadataFixture(root / kOldUri, {"TITLE=Before the rename"}); }

      ScanPlan plan() const { return ao::test::requireValue(LibraryScan{library}.buildPlan()); }

      TrackId importAndRename(AudioIdentityPolicy policy = AudioIdentityPolicy::Eager)
      {
        auto const imported =
          ao::test::requireValue(ScanApplyOperation{library, plan(), {}, {}, {.audioIdentityPolicy = policy}}.run());
        REQUIRE(imported.failureCount == 0);
        REQUIRE(imported.insertedIds.size() == 1);
        renameDirectory();
        return imported.insertedIds.front();
      }

      void renameDirectory() const
      {
        auto const intermediate = root / "intermediate";
        fs::rename(root / kOldDirectory.substr(1), intermediate);
        fs::rename(intermediate, root / kNewDirectory.substr(1));
      }

      TrackId addOwner(std::string_view uri)
      {
        auto transaction = library::test::writeTransaction(library);
        auto const id = ao::test::requireValue(transaction.apply(
          [uri](library::LibraryWrite& write)
          {
            auto writer = write.tracks();
            auto const optSource = writer.manifest(kOldUri);
            REQUIRE(optSource);
            auto track = library::TrackBuilder::makeEmpty();
            track.property().uri(uri);
            return writer.create(track, library::FileManifestBuilder::fromView(*optSource));
          }));
        REQUIRE(transaction.commit());
        return id;
      }

      std::uint64_t revision() const { return library.libraryRevision(library.readTransaction()); }

      void checkBinding(std::string_view uri, TrackId id) const
      {
        auto const transaction = library.readTransaction();
        auto const optManifest = library.manifest().reader(transaction).get(uri);
        REQUIRE(optManifest);
        CHECK(optManifest->trackId() == id);
        CHECK(optManifest->status() == library::FileStatus::Available);
        auto const optTrack = library.tracks().reader(transaction).get(id);
        REQUIRE(optTrack);
        CHECK(optTrack->property().uri() == uri);
      }

      void checkUnbound(std::string_view uri) const
      {
        auto const transaction = library.readTransaction();
        CHECK_FALSE(library.manifest().reader(transaction).get(uri));
      }
    };

    ScanItem const& requireItem(ScanPlan const& plan, std::string_view uri)
    {
      auto const found = std::ranges::find(plan.items(), uri, &ScanItem::uri);
      REQUIRE(found != plan.items().end());
      return *found;
    }

    ScanPlan requireFallbackMove(FallbackFixture const& fixture, TrackId id)
    {
      auto plan = fixture.plan();
      REQUIRE(plan.size() == 1);
      auto const& destination = requireItem(plan, kNewUri);
      REQUIRE(destination.classification == ScanClassification::Moved);
      CHECK(destination.oldUri == kOldUri);
      CHECK(destination.trackId == id);
      return plan;
    }
  } // namespace

  TEST_CASE("Scan directory spelling fallback - rebinds a unique fingerprint without a native file ID")
  {
    auto fixture = FallbackFixture{};
    auto const id = fixture.importAndRename();
    auto const result =
      ao::test::requireValue(ScanApplyOperation{fixture.library, requireFallbackMove(fixture, id), {}, {}}.run());
    CHECK(result.failureCount == 0);
    CHECK(result.relinkedIds == std::vector{id});
    CHECK(result.insertedIds.empty());
    fixture.checkBinding(kNewUri, id);
    fixture.checkUnbound(kOldUri);
  }

  TEST_CASE("Scan directory spelling fallback - rechecks the listing after the second raw alias lookup")
  {
    auto fixture = FallbackFixture{};
    auto const secondFile = fixture.root / kSecondOldUri;
    writeScanFlacMetadataFixture(secondFile, {"TITLE=Second track"});
    {
      // Different payload lengths give the two tracks unique fingerprints.
      auto payload = std::ofstream{secondFile, std::ios::binary | std::ios::app};
      payload.put('y');
      payload.close();
      REQUIRE(payload.good());
    }

    auto const imported = ao::test::requireValue(ScanApplyOperation{fixture.library, fixture.plan(), {}, {}}.run());
    REQUIRE(imported.failureCount == 0);
    REQUIRE(imported.insertedIds.size() == 2);
    fixture.renameDirectory();

    auto plan = fixture.plan();
    REQUIRE(plan.size() == 2);
    auto const& firstTrack = requireItem(plan, kNewUri);
    auto const& secondTrack = requireItem(plan, kSecondNewUri);
    REQUIRE(firstTrack.classification == ScanClassification::Moved);
    REQUIRE(secondTrack.classification == ScanClassification::Moved);
    REQUIRE(firstTrack.oldUri == kOldUri);
    REQUIRE(secondTrack.oldUri == kSecondOldUri);
    auto const firstId = firstTrack.trackId;
    auto const secondId = secondTrack.trackId;
    REQUIRE(firstId != secondId);
    fixture.checkBinding(kOldUri, firstId);
    fixture.checkBinding(kSecondOldUri, secondId);

    // Use plan order rather than relying on the filesystem's iteration order.
    auto const lastUri = plan.items().back().uri;
    auto failures = std::vector<ScanFailure>{};
    auto operation = ScanApplyOperation{
      fixture.library, std::move(plan), {}, [&failures](ScanFailure const& failure) { failures.push_back(failure); }};
    requirePrepared(operation);
    auto const oldDirectory = fixture.root / kOldDirectory.substr(1);
    REQUIRE_FALSE(fs::exists(oldDirectory));
    REQUIRE(fs::create_directories(fixture.root / kRecreateMarker.substr(1) / fs::path{lastUri}.filename()));
    auto const revision = fixture.revision();
    auto const result = ao::test::requireValue(operation.run());

    REQUIRE(fs::is_directory(oldDirectory));
    CHECK_FALSE(fs::equivalent(oldDirectory, fixture.root / kNewDirectory.substr(1)));
    CHECK(result.failureCount == 1);
    CHECK(result.relinkedIds.empty());
    CHECK(result.insertedIds.empty());
    CHECK(fixture.revision() == revision);
    REQUIRE(failures.size() == 1);
    CHECK(failures.front().uri == lastUri);
    CHECK(failures.front().stage == "validate spelling");
    fixture.checkBinding(kOldUri, firstId);
    fixture.checkBinding(kSecondOldUri, secondId);
    fixture.checkUnbound(kNewUri);
    fixture.checkUnbound(kSecondNewUri);
  }

  TEST_CASE("Scan directory spelling fallback - rejects an exact regular blocker replaced by a redirect")
  {
    auto const temp = ao::test::TempDir{};
    auto const root = temp.path() / kMarker.substr(1);
    auto const oldEntry = root / kOldDirectory.substr(1);
    auto const newDirectory = root / kNewDirectory.substr(1);
    fs::create_directories(newDirectory);

    if (fs::exists(oldEntry))
    {
      SKIP("The temporary directory cannot contain both exact spellings");
    }

    {
      auto blocker = std::ofstream{oldEntry};
      blocker.put('x');
      REQUIRE(blocker.good());
    }
    writeScanFlacMetadataFixture(root / kNewUri, {"TITLE=Destination"});
    auto const unarmedRes = ScanSpellingInspection{root, {}}.inspectDirectory(kOldUri, kNewUri);
    REQUIRE(unarmedRes);
    REQUIRE(std::holds_alternative<DistinctScanSpellings>(*unarmedRes));
    auto const marker = root / kRedirectMarker.substr(1);
    fs::create_directory(marker.parent_path());
    auto const redirect = ao::test::SymlinkFixture{newDirectory, marker, ao::test::SymlinkType::Directory};

    auto const racedRes = ScanSpellingInspection{root, {}}.inspectDirectory(kOldUri, kNewUri);

    REQUIRE(fs::is_symlink(fs::symlink_status(oldEntry)));
    REQUIRE(fs::equivalent(root / kOldUri, root / kNewUri));
    REQUIRE_FALSE(racedRes);
    CHECK(racedRes.error().code == Error::Code::Conflict);
  }

  TEST_CASE("Scan directory spelling fallback - refuses a new binding when the fingerprint is pending")
  {
    auto fixture = FallbackFixture{};
    auto const id = fixture.importAndRename(AudioIdentityPolicy::DeferNew);
    auto const revision = fixture.revision();
    auto plan = fixture.plan();
    REQUIRE(plan.size() == 2);

    for (auto const uri : {kOldUri, kNewUri})
    {
      auto const& item = requireItem(plan, uri);
      CHECK(item.classification == ScanClassification::Error);
      CHECK(item.errorMessage.contains("Native spelling identity is incomplete"));
    }

    auto const result = ao::test::requireValue(ScanApplyOperation{fixture.library, std::move(plan), {}, {}}.run());
    CHECK(result.failureCount == 2);
    CHECK(fixture.revision() == revision);
    fixture.checkBinding(kOldUri, id);
    fixture.checkUnbound(kNewUri);
  }

  TEST_CASE("Scan directory spelling fallback - reports the historical alias without touching its exact owner")
  {
    auto fixture = FallbackFixture{};
    auto const oldId = fixture.importAndRename();
    auto const exactId = fixture.addOwner(kNewUri);
    auto const revision = fixture.revision();
    auto plan = fixture.plan();
    REQUIRE(plan.size() == 2);
    auto const& alias = requireItem(plan, kOldUri);
    CHECK(alias.classification == ScanClassification::Error);
    CHECK(alias.errorMessage.contains("Native identity is unavailable"));
    CHECK(alias.errorMessage.contains("already has a manifest owner"));
    CHECK(requireItem(plan, kNewUri).classification == ScanClassification::Unchanged);
    auto const result = ao::test::requireValue(ScanApplyOperation{fixture.library, std::move(plan), {}, {}}.run());
    CHECK(result.failureCount == 1);
    CHECK(fixture.revision() == revision);
    fixture.checkBinding(kOldUri, oldId);
    fixture.checkBinding(kNewUri, exactId);
  }

  TEST_CASE("Scan directory spelling fallback - refuses a prepared move when its spelling evidence changes")
  {
    auto fixture = FallbackFixture{};
    auto const id = fixture.importAndRename();
    auto stage = std::string_view{"validate spelling"};
    auto failedStage = std::string{};
    auto operation = ScanApplyOperation{fixture.library,
                                        requireFallbackMove(fixture, id),
                                        {},
                                        [&failedStage](ScanFailure const& failure) { failedStage = failure.stage; }};
    requirePrepared(operation);
    constexpr auto kPeerUri = "ALBUM/x.flac";
    auto peerId = kInvalidTrackId;

    SECTION("the directory is replaced but the same file is moved back")
    {
      auto const destination = fixture.root / kNewUri;
      auto const directory = destination.parent_path();
      auto const backup = fixture.temp.path() / "backup";
      fs::rename(directory, backup);
      fs::create_directory(directory);
      fs::rename(backup / destination.filename(), destination);
      REQUIRE_FALSE(fs::equivalent(directory, backup));
    }

    SECTION("a library writer adds a competing directory spelling")
    {
      peerId = fixture.addOwner(kPeerUri);
      stage = "validate library evidence";
    }

    auto const revision = fixture.revision();
    auto const result = ao::test::requireValue(operation.run());
    CHECK(result.failureCount == 1);
    CHECK(failedStage == stage);
    CHECK(result.relinkedIds.empty());
    CHECK(fixture.revision() == revision);
    fixture.checkBinding(kOldUri, id);
    fixture.checkUnbound(kNewUri);

    if (peerId != kInvalidTrackId)
    {
      fixture.checkBinding(kPeerUri, peerId);
    }
  }
} // namespace ao::rt::test
