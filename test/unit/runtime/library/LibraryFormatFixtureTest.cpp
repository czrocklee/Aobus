// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/library/LibraryYamlExporter.h"
#include "runtime/library/LibraryYamlImporter.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/library/LibraryTransfer.h>
#include <ao/utility/Path.h>
#include <ao/utility/Raii.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

// Each directory under test/data/library holds the data.mdb of one platform's
// version-1 library. Every fixture was made on its platform with that
// platform's CLI, from an empty directory <root>:
//
//   aobus -C <root> lib import v1.yaml --mode restore --confirm-destructive-restore
//   aobus -C <root> track create <root>/identity-history.flac   (a copy of test/data/audio/basic_metadata.flac)
//   aobus -C <root> track delete 264
//   aobus -C <root> list create --name "Identity history"
//   aobus -C <root> list delete 6
//
// and copying <root>/.aobus/library/data.mdb. The created and deleted Track
// and List give the library an identity history: its highest ids are gone, and
// a later write must not reuse them. v1.yaml is the full export of the result,
// so exporting any of the fixtures reproduces it byte for byte. The fixtures
// are frozen at the first release; a later format change adds fixtures of its
// own version instead of regenerating these.

namespace ao::rt::test
{
  using namespace ao::library;

  namespace
  {
    struct PlatformFixture final
    {
      std::string_view directory;
      std::uint32_t pageBytes = 0;
    };

    // LMDB keeps the page size of the host that created an environment, so the
    // Apple Silicon fixture is the one that carries 16 KiB pages to the others.
    constexpr auto kFixtures = std::array{
      PlatformFixture{.directory = "linux-x86_64", .pageBytes = 4096},
      PlatformFixture{.directory = "macos-arm64", .pageBytes = 16384},
      PlatformFixture{.directory = "windows-x64", .pageBytes = 4096},
    };

    // What the procedure above leaves behind. The next ids follow the deleted
    // Track 264 and List 6, not the 263 Tracks and 5 Lists that remain.
    constexpr std::size_t kFixtureTrackCount = 263;
    constexpr auto kNextTrackId = TrackId{265};
    constexpr auto kNextListId = ListId{7};
    constexpr auto kWrittenTag = std::string_view{"written-after-copy"};

    std::filesystem::path fixtureRoot()
    {
      return std::filesystem::path{LIBRARY_TEST_DATA_DIR};
    }

    struct FixtureLayout final
    {
      std::uint32_t pageBytes = 0;
      std::uint32_t tracksHotDepth = 0;
      std::uint32_t tracksColdDepth = 0;
      std::uint32_t manifestDepth = 0;
      std::uint64_t listOverflowPages = 0;
    };

    // Read natively, because the adapter reports no per-database tree shape.
    FixtureLayout layoutOf(std::filesystem::path const& databasePath)
    {
      auto const requireNative = [](int const code) { REQUIRE(code == MDB_SUCCESS); };

      ::MDB_env* environment = nullptr;
      requireNative(::mdb_env_create(&environment));
      auto const environmentPtr = utility::makeUniquePtr<::mdb_env_close>(environment);
      requireNative(::mdb_env_set_maxdbs(environment, 8));
      auto const utf8Path = utility::pathToUtf8(databasePath);
      requireNative(::mdb_env_open(environment, utf8Path.c_str(), MDB_RDONLY | MDB_NOTLS, 0644));
      ::MDB_txn* transaction = nullptr;
      requireNative(::mdb_txn_begin(environment, nullptr, MDB_RDONLY, &transaction));
      auto const transactionPtr = utility::makeUniquePtr<::mdb_txn_abort>(transaction);

      auto const statOf = [&requireNative, transaction](char const* const name)
      {
        ::MDB_dbi database = 0;
        requireNative(::mdb_dbi_open(transaction, name, 0, &database));
        auto stat = ::MDB_stat{};
        requireNative(::mdb_stat(transaction, database, &stat));
        return stat;
      };

      auto const tracksHot = statOf("tracks_hot");
      return FixtureLayout{
        .pageBytes = tracksHot.ms_psize,
        .tracksHotDepth = tracksHot.ms_depth,
        .tracksColdDepth = statOf("tracks_cold").ms_depth,
        .manifestDepth = statOf("file_manifest").ms_depth,
        .listOverflowPages = statOf("lists").ms_overflow_pages,
      };
    }

    std::string exportFull(MusicLibrary const& library, std::filesystem::path const& path)
    {
      REQUIRE(LibraryYamlExporter{library}.exportToYaml(path, ExportMode::Full));
      return ao::test::readFile(path);
    }

    std::size_t trackCount(MusicLibrary& library)
    {
      std::size_t count = 0;
      auto transaction = library.readTransaction();

      for ([[maybe_unused]] auto const& item : library.tracks().reader(transaction))
      {
        ++count;
      }

      return count;
    }

    // Before the first release a format is at version 1. After it, the current
    // version is the last released one or exactly one past it, because a format
    // changes version at most once per release.
    consteval bool matchesReleaseRule(std::uint32_t const current, std::uint32_t const lastReleased)
    {
      return lastReleased == 0 ? current == 1 : current == lastReleased || current == lastReleased + 1;
    }
  } // namespace

  TEST_CASE("Library format fixtures - each library format version changes at most once per release",
            "[runtime][unit][format-fixture]")
  {
    STATIC_REQUIRE(matchesReleaseRule(library::kLibraryVersion, library::kLastReleasedLibraryVersion));
    STATIC_REQUIRE(matchesReleaseRule(kYamlFormatVersion, kLastReleasedYamlFormatVersion));
  }

  TEST_CASE("Library format fixtures - a library from each platform reads back and accepts a write",
            "[runtime][integration][import-export][format-fixture]")
  {
    auto const expectedYaml = ao::test::readFile(fixtureRoot() / "v1.yaml");
    REQUIRE_FALSE(expectedYaml.empty());

    for (auto const& fixture : kFixtures)
    {
      DYNAMIC_SECTION(fixture.directory)
      {
        auto const temp = ao::test::TempDir{};
        auto const databasePath = temp.path() / "library";
        REQUIRE(std::filesystem::create_directory(databasePath));
        // A copy, because opening creates lock files beside the database and the
        // test writes the database; the committed fixture must stay as its
        // platform made it.
        REQUIRE(std::filesystem::copy_file(fixtureRoot() / fixture.directory / "data.mdb", databasePath / "data.mdb"));

        // The page size is the creating host's, and these shapes are what the
        // fixtures carry across it: Track and manifest trees with a branch level,
        // and a List record too large for its leaf page.
        auto const layout = layoutOf(databasePath);
        CHECK(layout.pageBytes == fixture.pageBytes);
        CHECK(layout.tracksHotDepth >= 2);
        CHECK(layout.tracksColdDepth >= 2);
        CHECK(layout.manifestDepth >= 2);
        CHECK(layout.listOverflowPages >= 1);

        {
          auto libraryRes = library::test::openTestMusicLibrary(temp.path(), databasePath);
          REQUIRE(libraryRes);
          CHECK(exportFull(*libraryRes, temp.path() / "export.yaml") == expectedYaml);

          // One write that reaches the Track, manifest, dictionary, List, and
          // identifier stores of a library another platform laid out. The tag is
          // text the fixture does not hold, so the write adds a dictionary entry.
          REQUIRE_FALSE(libraryRes->dictionary().findId(kWrittenTag));
          auto spec = library::test::makeEmptyTrackSpec("Written/after copy.flac");
          spec.tags = {std::string{kWrittenTag}};
          auto transaction = library::test::writeTransaction(*libraryRes);
          auto idsRes = transaction.apply(
            [&libraryRes, &spec](LibraryWrite& write) -> Result<std::pair<TrackId, ListId>>
            {
              auto const trackId = library::test::addTrack(*libraryRes, write, spec);
              auto list = ListBuilder::makeEmpty().name("Written after copy");
              list.orderTrackIds().add(trackId);
              auto listRes = write.lists().create(list);

              if (!listRes)
              {
                return std::unexpected{listRes.error()};
              }

              return std::pair{trackId, *listRes};
            });
          REQUIRE(idsRes);
          REQUIRE(transaction.commit());
          CHECK(idsRes->first == kNextTrackId);
          CHECK(idsRes->second == kNextListId);
        }

        auto libraryRes = library::test::openTestMusicLibrary(temp.path(), databasePath);
        REQUIRE(libraryRes);
        CHECK(trackCount(*libraryRes) == kFixtureTrackCount + 1);

        auto transaction = libraryRes->readTransaction();
        auto const optTrack = libraryRes->tracks().reader(transaction).get(kNextTrackId);
        REQUIRE(optTrack);
        CHECK(optTrack->property().uri() == "Written/after copy.flac");
        REQUIRE(optTrack->tags().count() == 1);
        CHECK(libraryRes->dictionary().get(*optTrack->tags().begin()) == kWrittenTag);
        auto const optList = libraryRes->lists().reader(transaction).get(kNextListId);
        REQUIRE(optList);
        CHECK(optList->name() == "Written after copy");
        REQUIRE(optList->orderTrackIds().size() == 1);
        CHECK(optList->orderTrackIds()[0] == kNextTrackId);
      }
    }
  }

  TEST_CASE("Library format fixtures - the version 1 document restores into an empty library and exports unchanged",
            "[runtime][integration][import-export][format-fixture]")
  {
    auto const yamlPath = fixtureRoot() / "v1.yaml";
    auto const temp = ao::test::TempDir{};
    auto const databasePath = temp.path() / "library";
    REQUIRE(std::filesystem::create_directory(databasePath));
    auto libraryRes = library::test::openTestMusicLibrary(temp.path(), databasePath);
    REQUIRE(libraryRes);

    // The music root holds none of the audio files the document names, so the
    // restore, CUE members and saved ranks included, comes from the document alone.
    auto importer = LibraryYamlImporter{*libraryRes};
    auto const reportRes = importer.importFromYamlOffline(yamlPath, ImportMode::Restore);
    REQUIRE(reportRes);
    CHECK(reportRes->payloadVersion == kYamlFormatVersion);
    CHECK(reportRes->tracksCreated == kFixtureTrackCount);
    CHECK(exportFull(*libraryRes, temp.path() / "export.yaml") == ao::test::readFile(yamlPath));
  }
} // namespace ao::rt::test
