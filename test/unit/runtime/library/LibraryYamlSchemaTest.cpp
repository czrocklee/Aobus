// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/FilesystemTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include <ao/Error.h>
#include <ao/library/LibraryUri.h>
#include <ao/rt/library/LibraryYamlImporter.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

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

      auto const result = LibraryYamlImporter{library}.previewImportFromYamlOffline(yamlPath, ImportMode::Restore);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
      CHECK_THAT(result.error().message, Catch::Matchers::ContainsSubstring(std::string{payload.error}));
    }
  } // namespace

  TEST_CASE("LibraryYaml - version 4 rejects ambiguous or forward-unknown records",
            "[runtime][workflow][import-export][schema]")
  {
    constexpr auto kRejectedPayloads = std::to_array<RejectedPayload>({
      {.label = "library field",
       .yaml = R"(version: 4
export_mode: full
library:
  resources: []
  future: true
  tracks: []
  lists: []
)",
       .error = "library contains unknown field 'future'"},
      {.label = "track field",
       .yaml = R"(version: 4
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
       .yaml = R"(version: 4
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
       .yaml = R"(version: 4
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
       .yaml = R"(version: 4
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
       .yaml = R"(version: 4
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
       .yaml = R"(version: 4
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
       .yaml = R"(version: 4
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
       .yaml = R"(version: 4
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
       .yaml = R"(version: 4
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

  TEST_CASE("LibraryYaml - version 4 requires explicit scope and root-contained URIs",
            "[runtime][workflow][import-export][schema]")
  {
    constexpr auto kRejectedPayloads = std::to_array<RejectedPayload>({
      {.label = "missing export mode",
       .yaml = R"(version: 4
library:
  resources: []
  tracks: []
  lists: []
)",
       .error = "missing required 'export_mode'"},
      {.label = "missing tracks",
       .yaml = R"(version: 4
export_mode: full
library:
  resources: []
  lists: []
)",
       .error = "missing required 'tracks'"},
      {.label = "missing lists",
       .yaml = R"(version: 4
export_mode: full
library:
  resources: []
  tracks: []
)",
       .error = "missing required 'lists'"},
      {.label = "tracks in list-only payload",
       .yaml = R"(version: 4
export_mode: listOnly
library:
  tracks: []
  lists: []
)",
       .error = "library.tracks is forbidden"},
      {.label = "missing list-only lists",
       .yaml = R"(version: 4
export_mode: listOnly
library: {}
)",
       .error = "missing required 'lists'"},
      {.label = "absolute track URI",
       .yaml = R"(version: 4
export_mode: full
library:
  resources: []
  tracks:
    - uri: /outside.flac
  lists: []
)",
       .error = "must be root-relative"},
      {.label = "parent traversal",
       .yaml = R"(version: 4
export_mode: full
library:
  resources: []
  tracks:
    - uri: ../outside.flac
  lists: []
)",
       .error = "escapes the library root"},
      {.label = "absolute list URI",
       .yaml = R"(version: 4
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
            "[runtime][workflow][import-export][uri]")
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
      output << R"(version: 4
export_mode: metadata
library:
  tracks:
    - uri: alias/song.flac
  lists: []
)";
    }

    auto const result = LibraryYamlImporter{library}.previewImportFromYamlOffline(yamlPath, ImportMode::Restore);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::FormatRejected);
    CHECK(result.error().message.contains("resolves outside the library root"));
  }

  TEST_CASE("LibraryYaml - version 4 rejects duplicate semantic keys", "[runtime][workflow][import-export][schema]")
  {
    constexpr auto kRejectedPayloads = std::to_array<RejectedPayload>({
      {.label = "canonical track URI",
       .yaml = R"(version: 4
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
       .yaml = R"(version: 4
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

  TEST_CASE("LibraryYaml - version 4 rejects invalid list semantics", "[runtime][workflow][import-export][schema]")
  {
    constexpr auto kRejectedPayloads = std::to_array<RejectedPayload>({
      {.label = "invalid List filter",
       .yaml = R"(version: 4
export_mode: listOnly
library:
  lists:
    - id: 1
      name: Invalid filter
      filter: "("
)",
       .error = "filter is invalid"},
      {.label = "parent cycle",
       .yaml = R"(version: 4
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

  TEST_CASE("LibraryYaml - version 4 rejects values beyond supported limits",
            "[runtime][workflow][import-export][schema]")
  {
    auto overlongUri = std::string(LibraryUri::kMaxLength + 1U, 'a');
    checkRejectedPayload(RejectedPayload{
      .label = "overlong URI",
      .yaml = std::string{"version: 4\nexport_mode: full\nlibrary:\n  tracks:\n    - uri: "} + overlongUri +
              "\n  lists: []\n",
      .error = "exceeds the maximum",
    });

    auto const overlongListName = std::string(65536, 'n');
    checkRejectedPayload(RejectedPayload{
      .label = "overlong List name",
      .yaml = std::string{"version: 4\nexport_mode: listOnly\nlibrary:\n  lists:\n    - id: 1\n      name: "} +
              overlongListName + "\n",
      .error = "exceeds the 65535-byte product limit",
    });

    constexpr auto kRejectedResourceRows = std::to_array<RejectedPayload>({
      {.label = "resource row without a length",
       .yaml = R"(version: 4
export_mode: full
library:
  resources:
    - digest: 9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08
  tracks: []
  lists: []
)",
       .error = "Resource record missing required 'length' field"},
      {.label = "resource row without a digest",
       .yaml = R"(version: 4
export_mode: full
library:
  resources:
    - length: 3
  tracks: []
  lists: []
)",
       .error = "Resource record missing required 'digest' field"},
      {.label = "non-numeric length",
       .yaml = R"(version: 4
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
       .yaml = R"(version: 4
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
      .yaml = R"(version: 4
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
} // namespace ao::rt::test
