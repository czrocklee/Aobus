// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "runtime/library/LibraryYamlImporter.h"
#include "test/unit/FilesystemTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/FileTimestamp.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibraryUri.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/library/LibraryTransfer.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>

namespace ao::rt::test
{
  using namespace ao::library;

  namespace
  {
    struct RejectedPayload final
    {
      std::string_view label;
      std::string_view yaml;
      std::string_view error;
    };

    void checkRejectedPayload(RejectedPayload const& payload)
    {
      CAPTURE(payload.label);
      auto const temp = ao::test::TempDir{};
      auto library = library::test::makeTestMusicLibrary(temp.path(), temp.path());
      auto const yamlPath = temp.path() / "rejected.yaml";
      {
        auto output = std::ofstream{yamlPath};
        output << payload.yaml;
      }

      auto const res = LibraryYamlImporter{library}.previewImportFromYamlOffline(yamlPath, ImportMode::Restore);
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
      CHECK_THAT(res.error().message, Catch::Matchers::ContainsSubstring(std::string{payload.error}));

      if (payload.error.starts_with("Unsupported YAML version"))
      {
        CHECK(res.error().message.contains("(current 7)"));
        CHECK(res.error().message.contains("No automatic conversion"));
        CHECK(res.error().message.contains("keep this document"));
        CHECK(res.error().message.contains("build supporting its version"));
      }
    }
  } // namespace

  TEST_CASE("LibraryYaml - unsupported versions are rejected before interpreting their payload",
            "[runtime][unit][import-export][schema]")
  {
    checkRejectedPayload({
      .label = "older version 4",
      .yaml = "version: 4\nlibrary: malformed\n",
      .error = "Unsupported YAML version 4",
    });

    // Version 5 kept the mtime scalar, so its documents are not this format's;
    // there is no migration and no fallback, only the one version this build
    // writes.
    checkRejectedPayload({
      .label = "previous version 5",
      .yaml = "version: 5\nlibrary: malformed\n",
      .error = "Unsupported YAML version 5",
    });

    // Version 6 predates the performance-information fields: it carries no
    // recording date and no Credits list, so its documents are not this
    // format's either.
    checkRejectedPayload({
      .label = "previous version 6",
      .yaml = "version: 6\nlibrary: malformed\n",
      .error = "Unsupported YAML version 6",
    });

    checkRejectedPayload({
      .label = "future version 8",
      .yaml = "version: 8\nlibrary: malformed\n",
      .error = "Unsupported YAML version 8",
    });
  }

  TEST_CASE("LibraryYaml - version 7 rejects ambiguous or forward-unknown records",
            "[runtime][unit][import-export][schema]")
  {
    constexpr auto kRejectedPayloads = std::to_array<RejectedPayload>({
      {.label = "library field",
       .yaml = R"(version: 7
export_mode: full
library:
  resources: []
  future: true
  tracks: []
  lists: []
)",
       .error = "library contains unknown field 'future'"},
      {.label = "track field",
       .yaml = R"(version: 7
export_mode: full
library:
  resources: []
  tracks:
    - uri: song.flac
      future: true
  lists: []
)",
       .error = "Track record contains unknown field 'future'"},
      {.label = "duplicate track field",
       .yaml = R"(version: 7
export_mode: full
library:
  resources: []
  tracks:
    - uri: first.flac
      uri: second.flac
  lists: []
)",
       .error = "Track record contains duplicate field 'uri'"},
      {.label = "cover field",
       .yaml = R"(version: 7
export_mode: full
library:
  resources: []
  tracks:
    - uri: song.flac
      covers:
        - type: 3
          resource: 9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08
          future: true
  lists: []
)",
       .error = "Track cover contains unknown field 'future'"},
      {.label = "cover payload",
       .yaml = R"(version: 7
export_mode: full
library:
  resources: []
  tracks:
    - uri: song.flac
      covers:
        - type: 3
          data: YQ==
  lists: []
)",
       .error = "Track cover contains unknown field 'data'"},
      {.label = "list field",
       .yaml = R"(version: 7
export_mode: full
library:
  resources: []
  tracks: []
  lists:
    - id: 1
      name: List
      type: manual
)",
       .error = "List record contains unknown field 'type'"},
      {.label = "list-reference field",
       .yaml = R"(version: 7
export_mode: full
library:
  resources: []
  tracks:
    - id: 1
      uri: song.flac
  lists:
    - id: 1
      name: List
      order:
        - id: 1
          future: true
)",
       .error = "List order reference contains unknown field 'future'"},
      {.label = "ambiguous list reference",
       .yaml = R"(version: 7
export_mode: full
library:
  resources: []
  tracks:
    - id: 1
      uri: song.flac
  lists:
    - id: 1
      name: List
      order:
        - id: 1
          uri: song.flac
)",
       .error = "exactly one of 'id' or 'uri'"},
      {.label = "unknown codec",
       .yaml = R"(version: 7
export_mode: full
library:
  resources: []
  tracks:
    - uri: song.flac
      codec: VORBIS
  lists: []
)",
       .error = "Unknown codec 'VORBIS'"},
      {.label = "unknown cover type",
       .yaml = R"(version: 7
export_mode: full
library:
  resources:
    - digest: 9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08
      length: 3
  tracks:
    - uri: song.flac
      covers:
        - type: 21
          resource: 9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08
  lists: []
)",
       .error = "Unknown cover type 21"},
    });

    for (auto const& payload : kRejectedPayloads)
    {
      checkRejectedPayload(payload);
    }
  }

  TEST_CASE("LibraryYaml - version 7 requires explicit scope and root-contained URIs",
            "[runtime][unit][import-export][schema]")
  {
    constexpr auto kRejectedPayloads = std::to_array<RejectedPayload>({
      {.label = "missing export mode",
       .yaml = R"(version: 7
library:
  resources: []
  tracks: []
  lists: []
)",
       .error = "missing required 'export_mode'"},
      {.label = "missing tracks",
       .yaml = R"(version: 7
export_mode: full
library:
  resources: []
  lists: []
)",
       .error = "missing required 'tracks'"},
      {.label = "missing lists",
       .yaml = R"(version: 7
export_mode: full
library:
  resources: []
  tracks: []
)",
       .error = "missing required 'lists'"},
      {.label = "tracks in list-only payload",
       .yaml = R"(version: 7
export_mode: listOnly
library:
  tracks: []
  lists: []
)",
       .error = "library.tracks is forbidden"},
      {.label = "missing list-only lists",
       .yaml = R"(version: 7
export_mode: listOnly
library: {}
)",
       .error = "missing required 'lists'"},
      {.label = "absolute track URI",
       .yaml = R"(version: 7
export_mode: full
library:
  resources: []
  tracks:
    - uri: /outside.flac
  lists: []
)",
       .error = "must be root-relative"},
      {.label = "parent traversal",
       .yaml = R"(version: 7
export_mode: full
library:
  resources: []
  tracks:
    - uri: ../outside.flac
  lists: []
)",
       .error = "escapes the library root"},
      {.label = "absolute list URI",
       .yaml = R"(version: 7
export_mode: listOnly
library:
  lists:
    - id: 1
      name: List
      order:
        - uri: C:/outside.flac
)",
       .error = "must be root-relative"},
    });

    for (auto const& payload : kRejectedPayloads)
    {
      checkRejectedPayload(payload);
    }
  }

  TEST_CASE("LibraryYaml - import rejects a URI resolving through a root-escaping symlink",
            "[runtime][unit][import-export][uri]")
  {
    auto const temp = ao::test::TempDir{};
    auto const musicRoot = temp.path() / "music";
    auto const outsideRoot = temp.path() / "outside";
    std::filesystem::create_directories(musicRoot);
    std::filesystem::create_directories(outsideRoot);
    auto const symlink = ao::test::SymlinkFixture{outsideRoot, musicRoot / "alias", ao::test::SymlinkType::Directory};
    auto library = library::test::makeTestMusicLibrary(musicRoot, temp.path() / "db");
    auto const yamlPath = temp.path() / "outside.yaml";
    {
      auto output = std::ofstream{yamlPath};
      output << R"(version: 7
export_mode: metadata
library:
  tracks:
    - uri: alias/song.flac
  lists: []
)";
    }

    auto const res = LibraryYamlImporter{library}.previewImportFromYamlOffline(yamlPath, ImportMode::Restore);
    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::FormatRejected);
    CHECK(res.error().message.contains("resolves outside the library root"));
  }

  TEST_CASE("LibraryYaml - import rejects a track URI that is not a supported audio file",
            "[runtime][unit][import-export][uri]")
  {
    auto const mode = GENERATE(ImportMode::Restore, ImportMode::Merge);
    CAPTURE(mode);
    auto const temp = ao::test::TempDir{};
    auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path());
    auto importer = LibraryYamlImporter{ml};
    auto const yamlPath = temp.path() / "unsupported.yaml";

    auto const writePayload = [&yamlPath](std::string_view const tracksYaml)
    {
      auto output = std::ofstream{yamlPath};
      output << "version: " << kYamlFormatVersion << "\nexport_mode: metadata\nlibrary:\n  tracks:\n"
             << tracksYaml << "  lists: []\n";
    };

    // Both referenced files exist on disk. Rejection of the unsupported
    // extension therefore proves admission policy rather than a missing file.
    auto const supportedPath = temp.path() / "song.flac";
    auto const unsupportedPath = temp.path() / "notes.txt";
    {
      auto output = std::ofstream{supportedPath};
      output << "dummy";
    }
    {
      auto output = std::ofstream{unsupportedPath};
      output << "text";
    }
    REQUIRE(std::filesystem::is_regular_file(supportedPath));
    REQUIRE(std::filesystem::is_regular_file(unsupportedPath));

    SECTION("mixed payload is refused whole and admits no track")
    {
      writePayload(R"(    - uri: song.flac
      title: Supported
    - uri: notes.txt
      title: Unsupported
)");

      auto const res = importer.importFromYamlOffline(yamlPath, mode);

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
      CHECK_THAT(res.error().message, Catch::Matchers::ContainsSubstring("notes.txt"));
      CHECK_THAT(res.error().message, Catch::Matchers::ContainsSubstring("unsupported media file extension"));

      auto transaction = ml.readTransaction();
      CHECK(ml.tracks().reader(transaction).entryCount() == 0);
      CHECK_FALSE(ml.manifest().reader(transaction).get("song.flac"));
      CHECK_FALSE(ml.manifest().reader(transaction).get("notes.txt"));
    }

    SECTION("payload with only the supported track imports it")
    {
      writePayload(R"(    - uri: song.flac
      title: Supported
)");

      auto const res = importer.importFromYamlOffline(yamlPath, mode);

      REQUIRE(res);
      CHECK(res->tracksCreated == 1);

      auto transaction = ml.readTransaction();
      CHECK(ml.tracks().reader(transaction).entryCount() == 1);
      REQUIRE(ml.manifest().reader(transaction).get("song.flac"));
    }

    SECTION("mixed payload leaves a populated baseline untouched")
    {
      writePayload(R"(    - uri: song.flac
      title: Baseline
)");
      REQUIRE(importer.importFromYamlOffline(yamlPath, mode));

      auto const [baselineTrackId, baselineFileSize, baselineMtime, baselineRevision] = [&ml]
      {
        auto transaction = ml.readTransaction();
        auto const optBaseline = ml.manifest().reader(transaction).get("song.flac");
        REQUIRE(optBaseline);
        return std::tuple<TrackId, std::uint64_t, std::optional<FileTimestamp>, std::uint64_t>{
          optBaseline->trackId(), optBaseline->fileSize(), optBaseline->mtime(), ml.libraryRevision(transaction)};
      }();
      // Own the baseline facts before importing again; the read transaction
      // and its borrowed manifest view must not become the post-import oracle.
      CHECK(baselineFileSize == 5);
      CHECK(baselineMtime);
      REQUIRE(baselineRevision > 0);

      writePayload(R"(    - uri: song.flac
      title: Supported
    - uri: notes.txt
      title: Unsupported
)");

      auto const res = importer.importFromYamlOffline(yamlPath, mode);

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
      CHECK_THAT(res.error().message, Catch::Matchers::ContainsSubstring("notes.txt"));
      CHECK_THAT(res.error().message, Catch::Matchers::ContainsSubstring("unsupported media file extension"));

      auto transaction = ml.readTransaction();
      CHECK(ml.libraryRevision(transaction) == baselineRevision);
      CHECK(ml.tracks().reader(transaction).entryCount() == 1);
      auto const optManifest = ml.manifest().reader(transaction).get("song.flac");
      REQUIRE(optManifest);
      CHECK(optManifest->trackId() == baselineTrackId);
      CHECK(optManifest->fileSize() == baselineFileSize);
      CHECK(optManifest->mtime() == baselineMtime);
      auto const optView = ml.tracks().reader(transaction).get(baselineTrackId, TrackStore::Reader::LoadMode::Both);
      REQUIRE(optView);
      CHECK(optView->property().uri() == "song.flac");
      CHECK(optView->metadata().title() == "Baseline");
      CHECK_FALSE(ml.manifest().reader(transaction).get("notes.txt"));
    }

    SECTION("supported extension with an absent file imports and commits its facts")
    {
      auto const absentPath = temp.path() / "absent.flac";
      REQUIRE_FALSE(std::filesystem::exists(absentPath));
      writePayload(R"(    - uri: absent.flac
      title: Absent
      fileSize: 345
)");

      auto const res = importer.importFromYamlOffline(yamlPath, mode);

      REQUIRE(res);
      CHECK(res->tracksCreated == 1);

      CHECK_FALSE(std::filesystem::exists(absentPath));

      auto transaction = ml.readTransaction();
      auto const optManifest = ml.manifest().reader(transaction).get("absent.flac");
      REQUIRE(optManifest);
      CHECK(optManifest->fileSize() == 345);
      CHECK_FALSE(optManifest->mtime());
      auto const absentTrackId = optManifest->trackId();
      auto const optView = ml.tracks().reader(transaction).get(absentTrackId, TrackStore::Reader::LoadMode::Both);
      REQUIRE(optView);
      CHECK(optView->property().uri() == "absent.flac");
      CHECK(optView->metadata().title() == "Absent");
    }

    SECTION("uppercase extension is admitted and preserved verbatim")
    {
      writePayload(R"(    - uri: song.FLAC
      title: Upper
)");

      auto const res = importer.importFromYamlOffline(yamlPath, mode);

      REQUIRE(res);
      CHECK(res->tracksCreated == 1);

      auto transaction = ml.readTransaction();
      CHECK(ml.tracks().reader(transaction).entryCount() == 1);
      auto const optManifest = ml.manifest().reader(transaction).get("song.FLAC");
      REQUIRE(optManifest);
      auto const optView =
        ml.tracks().reader(transaction).get(optManifest->trackId(), TrackStore::Reader::LoadMode::Both);
      REQUIRE(optView);
      CHECK(optView->property().uri() == "song.FLAC");
      CHECK(optView->metadata().title() == "Upper");
    }

    SECTION("backslash-spelled unsupported URI is rejected under its canonical form")
    {
      writePayload(R"(    - uri: .\notes.txt
      title: Unsupported
)");

      auto const res = importer.importFromYamlOffline(yamlPath, mode);

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
      // Admission sees the canonical URI, so the raw backslash spelling never
      // reaches the error or a manifest key.
      CHECK_THAT(
        res.error().message, Catch::Matchers::ContainsSubstring("'notes.txt' has an unsupported media file extension"));

      auto transaction = ml.readTransaction();
      CHECK(ml.tracks().reader(transaction).entryCount() == 0);
      CHECK_FALSE(ml.manifest().reader(transaction).get("notes.txt"));
    }
  }

  TEST_CASE("LibraryYaml - version 7 rejects duplicate semantic keys", "[runtime][unit][import-export][schema]")
  {
    constexpr auto kRejectedPayloads = std::to_array<RejectedPayload>({
      {.label = "canonical track URI",
       .yaml = R"(version: 7
export_mode: full
library:
  resources: []
  tracks:
    - uri: albums/live/../song.flac
    - uri: albums/song.flac
  lists: []
)",
       .error = "Duplicate canonical track URI 'albums/song.flac'"},
      {.label = "custom metadata key",
       .yaml = R"(version: 7
export_mode: full
library:
  resources: []
  tracks:
    - uri: song.flac
      custom:
        mood: calm
        mood: loud
  lists: []
)",
       .error = "custom contains duplicate field 'mood'"},
    });

    for (auto const& payload : kRejectedPayloads)
    {
      checkRejectedPayload(payload);
    }
  }

  TEST_CASE("LibraryYaml - version 7 rejects invalid list semantics", "[runtime][unit][import-export][schema]")
  {
    constexpr auto kRejectedPayloads = std::to_array<RejectedPayload>({
      {.label = "invalid List filter",
       .yaml = R"(version: 7
export_mode: listOnly
library:
  lists:
    - id: 1
      name: Invalid filter
      filter: "("
)",
       .error = "filter is invalid"},
      {.label = "parent cycle",
       .yaml = R"(version: 7
export_mode: listOnly
library:
  lists:
    - id: 1
      parentId: 2
      name: First
    - id: 2
      parentId: 1
      name: Second
)",
       .error = "parent graph contains a cycle"},
    });

    for (auto const& payload : kRejectedPayloads)
    {
      checkRejectedPayload(payload);
    }
  }

  TEST_CASE("LibraryYaml - version 7 rejects values beyond supported limits", "[runtime][unit][import-export][schema]")
  {
    auto overlongUri = std::string(LibraryUri::kMaxLength + 1U, 'a');
    checkRejectedPayload(RejectedPayload{
      .label = "overlong URI",
      .yaml = std::string{"version: 7\nexport_mode: full\nlibrary:\n  tracks:\n    - uri: "} + overlongUri +
              "\n  lists: []\n",
      .error = "exceeds the maximum",
    });

    auto const overlongListName = std::string(65536, 'n');
    checkRejectedPayload(RejectedPayload{
      .label = "overlong List name",
      .yaml = std::string{"version: 7\nexport_mode: listOnly\nlibrary:\n  lists:\n    - id: 1\n      name: "} +
              overlongListName + "\n",
      .error = "exceeds the 65535-byte product limit",
    });

    constexpr auto kRejectedResourceRows = std::to_array<RejectedPayload>({
      {.label = "resource row without a length",
       .yaml = R"(version: 7
export_mode: full
library:
  resources:
    - digest: 9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08
  tracks: []
  lists: []
)",
       .error = "Resource record missing required 'length' field"},
      {.label = "resource row without a digest",
       .yaml = R"(version: 7
export_mode: full
library:
  resources:
    - length: 3
  tracks: []
  lists: []
)",
       .error = "Resource record missing required 'digest' field"},
      {.label = "non-numeric length",
       .yaml = R"(version: 7
export_mode: full
library:
  resources:
    - digest: 9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08
      length: abc
  tracks: []
  lists: []
)",
       .error = "Resource record.length must be a valid scalar"},
      {.label = "negative length",
       .yaml = R"(version: 7
export_mode: full
library:
  resources:
    - digest: 9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08
      length: -1
  tracks: []
  lists: []
)",
       .error = "Resource record.length must be a valid scalar"},
    });

    for (auto const& payload : kRejectedResourceRows)
    {
      checkRejectedPayload(payload);
    }

    checkRejectedPayload(RejectedPayload{
      .label = "digest spelling",
      .yaml = R"(version: 7
export_mode: full
library:
  resources:
    - digest: 9F86D081884C7D659A2FEAA0C55AD015A3BF4F1B2B0B822CD15D6C15B0F00A08
      length: 3
  tracks:
    - uri: song.flac
      covers:
        - type: 3
          resource: 9F86D081884C7D659A2FEAA0C55AD015A3BF4F1B2B0B822CD15D6C15B0F00A08
  lists: []
)",
      .error = "64 lowercase hexadecimal characters",
    });
  }

  TEST_CASE("LibraryYaml - version 7 rejects malformed mtime instants", "[runtime][unit][import-export][schema]")
  {
    constexpr auto kRejectedPayloads = std::to_array<RejectedPayload>({
      {.label = "nanoseconds beyond one second",
       .yaml = R"(version: 7
export_mode: full
library:
  resources: []
  tracks:
    - uri: song.flac
      mtime:
        seconds: 1719835259
        nanoseconds: 1000000000
  lists: []
)",
       .error = "mtime.nanoseconds must be below 1000000000"},
      {.label = "negative nanoseconds",
       .yaml = R"(version: 7
export_mode: full
library:
  resources: []
  tracks:
    - uri: song.flac
      mtime:
        seconds: 1719835259
        nanoseconds: -1
  lists: []
)",
       .error = "mtime.nanoseconds must be a valid scalar"},
      {.label = "missing seconds",
       .yaml = R"(version: 7
export_mode: full
library:
  resources: []
  tracks:
    - uri: song.flac
      mtime:
        nanoseconds: 500000000
  lists: []
)",
       .error = "mtime missing required 'seconds' field"},
      {.label = "missing nanoseconds",
       .yaml = R"(version: 7
export_mode: full
library:
  resources: []
  tracks:
    - uri: song.flac
      mtime:
        seconds: 1719835259
  lists: []
)",
       .error = "mtime missing required 'nanoseconds' field"},
      {.label = "unknown mtime field",
       .yaml = R"(version: 7
export_mode: full
library:
  resources: []
  tracks:
    - uri: song.flac
      mtime:
        seconds: 1719835259
        nanoseconds: 500000000
        epoch: unix
  lists: []
)",
       .error = "mtime contains unknown field 'epoch'"},
      {.label = "duplicate mtime field",
       .yaml = R"(version: 7
export_mode: full
library:
  resources: []
  tracks:
    - uri: song.flac
      mtime:
        seconds: 1719835259
        seconds: 0
        nanoseconds: 500000000
  lists: []
)",
       .error = "mtime contains duplicate field 'seconds'"},
      {.label = "scalar mtime",
       .yaml = R"(version: 7
export_mode: full
library:
  resources: []
  tracks:
    - uri: song.flac
      mtime: 1719835259500000000
  lists: []
)",
       .error = "mtime must be a map or null"},
      {.label = "quoted null string",
       .yaml = R"(version: 7
export_mode: full
library:
  resources: []
  tracks:
    - uri: song.flac
      mtime: "null"
  lists: []
)",
       .error = "mtime must be a map or null"},
      {.label = "seconds beyond the signed width",
       .yaml = R"(version: 7
export_mode: full
library:
  resources: []
  tracks:
    - uri: song.flac
      mtime:
        seconds: 9223372036854775808
        nanoseconds: 0
  lists: []
)",
       .error = "mtime.seconds must be a valid scalar"},
      {.label = "nanoseconds beyond the unsigned width",
       .yaml = R"(version: 7
export_mode: full
library:
  resources: []
  tracks:
    - uri: song.flac
      mtime:
        seconds: 1719835259
        nanoseconds: 4294967296
  lists: []
)",
       .error = "mtime.nanoseconds must be a valid scalar"},
      {.label = "non-numeric seconds",
       .yaml = R"(version: 7
export_mode: full
library:
  resources: []
  tracks:
    - uri: song.flac
      mtime:
        seconds: never
        nanoseconds: 0
  lists: []
)",
       .error = "mtime.seconds must be a valid scalar"},
    });

    for (auto const& payload : kRejectedPayloads)
    {
      checkRejectedPayload(payload);
    }
  }

  TEST_CASE("LibraryYaml - a malformed mtime refuses the payload whole in both modes",
            "[runtime][unit][import-export][schema]")
  {
    auto const mode = GENERATE(ImportMode::Restore, ImportMode::Merge);
    CAPTURE(mode);
    auto const temp = ao::test::TempDir{};
    auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path());
    auto const originalMtime = FileTimestamp{.seconds = 1719835259, .nanoseconds = 500000000};
    auto const trackId =
      library::test::addTrackWithUniqueFixtureUri(ml, library::test::makeEmptyTrackSpec("existing.flac"));

    {
      auto transaction = library::test::writeTransaction(ml);
      auto builder = FileManifestBuilder::makeEmpty();
      builder.mtime(originalMtime);
      REQUIRE(transaction.apply([&](LibraryWrite& write) { return write.tracks().updateManifest(trackId, builder); }));
      REQUIRE(transaction.commit());
    }

    std::uint64_t originalRevision = 0;

    {
      auto transaction = ml.readTransaction();
      originalRevision = ml.libraryRevision(transaction);
    }

    auto const yamlPath = temp.path() / "malformed-mtime.yaml";
    {
      auto output = std::ofstream{yamlPath};
      output << R"(version: 7
export_mode: full
library:
  resources: []
  tracks:
    - uri: admitted.flac
      title: Admitted
    - uri: malformed.flac
      mtime:
        seconds: 1719835259
        nanoseconds: 1000000000
  lists: []
)";
    }

    auto const res = LibraryYamlImporter{ml}.importFromYamlOffline(yamlPath, mode);
    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::FormatRejected);
    CHECK(res.error().message.contains("nanoseconds must be below 1000000000"));

    // The refusal happens during the document's preflight, before any record,
    // list, or revision moves, whatever mode the caller asked for.
    auto transaction = ml.readTransaction();
    CHECK(ml.libraryRevision(transaction) == originalRevision);
    CHECK(ml.tracks().reader(transaction).entryCount() == 1);
    CHECK_FALSE(ml.manifest().reader(transaction).get("admitted.flac"));
    CHECK_FALSE(ml.manifest().reader(transaction).get("malformed.flac"));
    auto const optExisting = ml.manifest().reader(transaction).get("existing.flac");
    REQUIRE(optExisting);
    REQUIRE(optExisting->mtime());
    CHECK(*optExisting->mtime() == originalMtime);

    std::size_t listCount = 0;

    for ([[maybe_unused]] auto const& [listId, listView] : ml.lists().reader(transaction))
    {
      ++listCount;
    }

    CHECK(listCount == 0);
  }
} // namespace ao::rt::test
