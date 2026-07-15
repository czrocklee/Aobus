// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/ListStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/library/LibraryYamlImporter.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>

namespace ao::rt::test
{
  using namespace ao::library;

  TEST_CASE("LibraryYaml - import reports invalid input errors", "[runtime][workflow][import-export][error]")
  {
    auto const temp = ao::test::TempDir{};
    auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path());
    auto importer = LibraryYamlImporter{ml};
    auto const yamlPath = std::filesystem::path{temp.path()} / "bad.yaml";

    auto testError = [&](std::string_view yamlContent, Error::Code expectedCode, std::string_view expectedErrorFragment)
    {
      {
        auto yaml = std::ofstream{yamlPath};
        yaml << yamlContent;
      }
      auto const result = importer.importFromYaml(yamlPath);
      REQUIRE(!result);
      CHECK(result.error().code == expectedCode);
      CHECK_THAT(result.error().message, Catch::Matchers::ContainsSubstring(std::string{expectedErrorFragment}));
    };

    SECTION("IO Error: File not found")
    {
      auto const nonExistentPath = std::filesystem::path{temp.path()} / "ghost.yaml";
      auto const result = importer.importFromYaml(nonExistentPath);
      REQUIRE(!result);
      CHECK(result.error().code == Error::Code::IoError);
    }

    SECTION("Missing version")
    {
      testError(R"(
library:
  tracks: []
  lists: []
)",
                Error::Code::FormatRejected,
                "Missing 'version'");
    }

    SECTION("Unsupported version")
    {
      testError(R"(
version: 2
library:
  tracks: []
)",
                Error::Code::FormatRejected,
                "Unsupported YAML version");
    }

    SECTION("Missing library section")
    {
      testError(R"(
version: 1
)",
                Error::Code::FormatRejected,
                "Missing 'library' section");
    }

    SECTION("Track missing URI")
    {
      testError(R"(
version: 1
library:
  tracks:
    - id: 1
      title: "No URI"
  lists: []
)",
                Error::Code::FormatRejected,
                "missing required 'uri'");
    }

    SECTION("Track empty URI")
    {
      testError(R"(
version: 1
library:
  tracks:
    - id: 1
      uri: ""
  lists: []
)",
                Error::Code::FormatRejected,
                "empty 'uri'");
    }

    SECTION("Duplicate track ID")
    {
      testError(R"(
version: 1
library:
  tracks:
    - id: 1
      uri: "song1.flac"
    - id: 1
      uri: "song2.flac"
  lists: []
)",
                Error::Code::FormatRejected,
                "Duplicate track YAML id");
    }

    SECTION("List missing ID")
    {
      testError(R"(
version: 1
library:
  tracks: []
  lists:
    - name: "No ID"
)",
                Error::Code::FormatRejected,
                "missing required 'id'");
    }

    SECTION("List ID 0 (Reserved)")
    {
      testError(R"(
version: 1
library:
  tracks: []
  lists:
    - id: 0
      name: "Root List"
)",
                Error::Code::FormatRejected,
                "List id 0 is reserved");
    }

    SECTION("Duplicate list ID")
    {
      testError(R"(
version: 1
library:
  tracks: []
  lists:
    - id: 1
      name: "List 1"
    - id: 1
      name: "List 2"
)",
                Error::Code::FormatRejected,
                "Duplicate list YAML id");
    }

    SECTION("List missing name")
    {
      testError(R"(
version: 1
library:
  tracks: []
  lists:
    - id: 1
)",
                Error::Code::FormatRejected,
                "missing required 'name'");
    }

    SECTION("Malformed Base64 cover art rejects the import")
    {
      testError(R"(
version: 1
library:
  tracks:
    - uri: "song1.flac"
      covers:
        - type: 3
          data: "Not!Valid@Base#64$"
  lists: []
)",
                Error::Code::FormatRejected,
                "cover data");
    }

    SECTION("Unknown export mode is rejected")
    {
      testError(R"(
version: 1
export_mode: mystery
library:
  tracks: []
  lists: []
)",
                Error::Code::FormatRejected,
                "Unknown export_mode");
    }

    SECTION("Malformed numeric version is rejected")
    {
      testError(R"(
version: 1x
library:
  tracks: []
  lists: []
)",
                Error::Code::FormatRejected,
                "version");
    }

    SECTION("Malformed track ID is rejected")
    {
      testError(R"(
version: 1
library:
  tracks:
    - id: 1x
      uri: "song1.flac"
  lists: []
)",
                Error::Code::FormatRejected,
                "Track record.id");
    }
  }

  TEST_CASE("LibraryYaml - import handles structural corruption cases", "[runtime][workflow][import-export][error]")
  {
    auto const temp = ao::test::TempDir{};
    auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path());
    auto importer = LibraryYamlImporter{ml};
    auto const yamlPath = std::filesystem::path{temp.path()} / "corrupt.yaml";

    SECTION("Tracks is scalar instead of sequence")
    {
      {
        auto yaml = std::ofstream{yamlPath};
        yaml << R"(
version: 1
library:
  tracks: "not-a-sequence"
  lists: []
)";
      }
      auto const result = importer.importFromYaml(yamlPath);
      REQUIRE(!result);
      CHECK(result.error().code == Error::Code::FormatRejected);
      CHECK(result.error().message.contains("library.tracks must be a sequence"));
    }

    SECTION("List missing mandatory ID")
    {
      {
        auto yaml = std::ofstream{yamlPath};
        yaml << R"(
version: 1
library:
  tracks: []
  lists:
    - name: "No ID"
)";
      }
      auto const result = importer.importFromYaml(yamlPath);
      REQUIRE(!result);
      CHECK(result.error().code == Error::Code::FormatRejected);
      CHECK(result.error().message.contains("missing required 'id'"));
    }

    SECTION("List entry points to non-existent track")
    {
      {
        auto yaml = std::ofstream{yamlPath};
        yaml << R"(
version: 1
library:
  tracks: []
  lists:
    - id: 1
      name: "Ghost List"
      tracks:
        - 999
)";
      }
      // Import should succeed but the list will be empty (or the ghost track ignored)
      auto const result = importer.importFromYaml(yamlPath);
      REQUIRE(result);

      auto transaction = ml.readTransaction();
      auto const optList = ml.lists().reader(transaction).get(ListId{1});
      REQUIRE(optList);
      CHECK(optList->tracks().empty());
    }
  }

  TEST_CASE("LibraryYaml - restore rolls back every mutation when a later track fails",
            "[runtime][workflow][import-export][regression]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = library::test::makeTestMusicLibrary(temp.path(), temp.path());
    auto const originalTrackId = library::test::addTrack(
      library,
      library::test::TrackSpec{.title = "Original Track", .artist = "Original Artist", .uri = "original.flac"});
    auto const originalDictionaryGeneration = library.dictionary().generation();
    std::uint64_t originalRevision = 0;

    {
      auto transaction = library.readTransaction();
      originalRevision = library.libraryRevision(transaction);
    }

    constexpr auto kUint16Overflow = std::size_t{std::numeric_limits<std::uint16_t>::max()} + 1;
    auto const oversizedTitle = std::string(kUint16Overflow, 'x');
    auto const yamlPath = temp.path() / "rollback.yaml";

    {
      auto yaml = std::ofstream{yamlPath};
      yaml << R"(version: 1
export_mode: full
library:
  tracks:
    - id: 1
      uri: "transient.flac"
      title: "Transient Track"
      artist: "Transient Artist"
    - id: 2
      uri: "oversized.flac"
      title: ")"
           << oversizedTitle << R"("
  lists: []
)";
    }

    auto importer = LibraryYamlImporter{library};
    auto const result = importer.importFromYaml(yamlPath, ImportMode::Restore);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::ValueTooLarge);
    CHECK(library.dictionary().generation() == originalDictionaryGeneration);
    CHECK_FALSE(library.dictionary().findId("Transient Artist"));

    auto transaction = library.readTransaction();
    CHECK(library.libraryRevision(transaction) == originalRevision);
    std::size_t trackCount = 0;

    for (auto const& [trackId, view] : library.tracks().reader(transaction))
    {
      ++trackCount;
      CHECK(trackId == originalTrackId);
      CHECK(view.metadata().title() == "Original Track");
      CHECK(view.property().uri() == "original.flac");
    }

    CHECK(trackCount == 1);
    CHECK_FALSE(library.manifest().reader(transaction).get("transient.flac"));
  }
} // namespace ao::rt::test
