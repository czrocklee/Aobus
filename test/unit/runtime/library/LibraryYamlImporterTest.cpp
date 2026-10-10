// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/library/LibraryYamlImporter.h"

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/Error.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/rt/library/LibraryTransfer.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <utility>

namespace ao::rt::test
{
  using namespace ao::library;

  namespace
  {
    // Wraps one track record's extra fields in a minimal current-version
    // document, so fixtures do not pin a version literal the planned version
    // bump would rewrite.
    std::string trackDocument(std::string_view trackFields)
    {
      return std::format("version: {}\n"
                         "export_mode: full\n"
                         "library:\n"
                         "  resources: []\n"
                         "  tracks:\n"
                         "    - uri: song.flac\n"
                         "{}"
                         "  lists: []\n",
                         kYamlFormatVersion,
                         trackFields);
    }

    std::string documentWith(std::string_view trackOneFields, std::string_view trackTwoFields)
    {
      return std::format("version: {}\n"
                         "export_mode: full\n"
                         "library:\n"
                         "  resources: []\n"
                         "  tracks:\n"
                         "    - uri: first.flac\n"
                         "{}"
                         "    - uri: second.flac\n"
                         "{}"
                         "  lists: []\n",
                         kYamlFormatVersion,
                         trackOneFields,
                         trackTwoFields);
    }

    Error previewFailure(std::string const& yaml)
    {
      auto const temp = ao::test::TempDir{};
      auto library = library::test::makeTestMusicLibrary(temp.path(), temp.path());
      auto const yamlPath = temp.path() / "document.yaml";

      {
        auto output = std::ofstream{yamlPath};
        output << yaml;
      }

      auto const res = LibraryYamlImporter{library}.previewImportFromYamlOffline(yamlPath, ImportMode::Restore);
      REQUIRE_FALSE(res);
      return res.error();
    }

    void requireAccepted(std::string const& yaml)
    {
      auto const temp = ao::test::TempDir{};
      auto library = library::test::makeTestMusicLibrary(temp.path(), temp.path());
      auto const yamlPath = temp.path() / "document.yaml";

      {
        auto output = std::ofstream{yamlPath};
        output << yaml;
      }

      auto const res = LibraryYamlImporter{library}.previewImportFromYamlOffline(yamlPath, ImportMode::Restore);
      INFO(yaml);
      REQUIRE(res);
    }
  } // namespace

  TEST_CASE("LibraryYaml - recording-date admits only canonical partial dates",
            "[runtime][unit][import-export][schema]")
  {
    SECTION("canonical literals are accepted")
    {
      requireAccepted(trackDocument("      title: Song\n      recording-date: 1981\n"));
      requireAccepted(trackDocument("      title: Song\n      recording-date: 1981-05\n"));
      requireAccepted(trackDocument("      title: Song\n      recording-date: 1981-05-12\n"));
    }

    SECTION("present empty scalar text is accepted as the clear form")
    {
      // The clear form is empty scalar text, not a quoted-only form and not a
      // YAML null. A present empty plain scalar and both quoted empties clear.
      requireAccepted(trackDocument("      title: Song\n      recording-date:\n"));
      requireAccepted(trackDocument("      title: Song\n      recording-date: ''\n"));
      requireAccepted(trackDocument("      title: Song\n      recording-date: \"\"\n"));
    }

    SECTION("literal null and tilde reject as nonempty date text")
    {
      for (auto const token : std::array<std::string_view, 2>{"null", "~"})
      {
        CAPTURE(token);
        auto const error =
          previewFailure(trackDocument(std::format("      title: Song\n      recording-date: {}\n", token)));
        CHECK(error.code == Error::Code::FormatRejected);
        CHECK_THAT(
          error.message, Catch::Matchers::ContainsSubstring("recording-date must be an empty scalar or a canonical"));
      }
    }

    SECTION("quoted whitespace rejects and is not a clear")
    {
      for (auto const scalar : std::array<std::string_view, 3>{"' '", "\"      \"", R"("\t")"})
      {
        CAPTURE(scalar);
        auto const error =
          previewFailure(trackDocument(std::format("      title: Song\n      recording-date: {}\n", scalar)));
        CHECK(error.code == Error::Code::FormatRejected);
        CHECK_THAT(
          error.message, Catch::Matchers::ContainsSubstring("recording-date must be an empty scalar or a canonical"));
      }
    }

    SECTION("noncanonical dates reject the payload")
    {
      constexpr auto kRejectedDates = std::to_array<std::string_view>({"1981-5",
                                                                       "1981-05-1",
                                                                       " 1981",
                                                                       "1981 ",
                                                                       "0000",
                                                                       "1981-13",
                                                                       "1981-00",
                                                                       "1981-05-00",
                                                                       "1981-02-30",
                                                                       "1981-05-12T00:00",
                                                                       "1981/05/12"});

      // Each scalar is single-quoted so its literal text, including the
      // leading and trailing whitespace cases, reaches the strict parser:
      // an unquoted plain scalar is trimmed by the YAML lexer first and
      // would arrive as valid text instead.
      for (auto const date : kRejectedDates)
      {
        CAPTURE(date);
        auto const yaml = trackDocument(std::format("      title: Song\n      recording-date: '{}'\n", date));
        auto const error = previewFailure(yaml);
        CHECK(error.code == Error::Code::FormatRejected);
        CHECK_THAT(error.message, Catch::Matchers::ContainsSubstring("recording-date"));
      }
    }

    SECTION("a non-scalar date rejects the payload")
    {
      auto const error = previewFailure(trackDocument("      title: Song\n      recording-date: [1981]\n"));
      CHECK(error.code == Error::Code::FormatRejected);
      CHECK_THAT(error.message, Catch::Matchers::ContainsSubstring("recording-date must be a scalar"));
    }
  }

  TEST_CASE("LibraryYaml - credits require closed name kind and optional role maps",
            "[runtime][unit][import-export][schema]")
  {
    for (auto const* kind : {"conductor", "ensemble", "soloist", "performer"})
    {
      requireAccepted(trackDocument(std::format("      credits: [{{name: Keith, kind: {}}}]\n", kind)));
    }

    for (auto const* role : {"", "''", "' '", "null", "~"})
    {
      requireAccepted(trackDocument(std::format("      credits:\n"
                                                "        - name: Keith\n"
                                                "          kind: performer\n"
                                                "          role: {}\n",
                                                role)));
    }

    requireAccepted(trackDocument("      credits: []\n"));

    constexpr auto kRejectedPayloads = std::to_array<std::pair<std::string_view, std::string_view>>({
      {"      credits: Keith\n", "must be a sequence"},
      {"      credits:\n", "must be a sequence"},
      {"      credits: null\n", "must be a sequence"},
      {"      credits: ~\n", "must be a sequence"},
      {"      credits: {}\n", "must be a sequence"},
      {"      credits: ['Keith']\n", "must be a map"},
      {"      credits: [{kind: performer}]\n", "missing required 'name'"},
      {"      credits: [{name: Keith}]\n", "missing required 'kind'"},
      {"      credits: [{name: Keith, kind: Performer}]\n", "Unknown credit kind"},
      {"      credits: [{name: Keith, kind: musician}]\n", "Unknown credit kind"},
      {"      credits: [{name: Keith, kind: all}]\n", "Unknown credit kind"},
      {"      credits: [{name: Keith, kind: null}]\n", "Unknown credit kind"},
      {"      credits: [{name: Keith, kind: performer, instrument: Guitar}]\n", "unknown field 'instrument'"},
      {"      credits: [{name: Keith, name: Anne, kind: performer}]\n", "duplicate field 'name'"},
      {"      credits: [{name: Keith, kind: performer, kind: soloist}]\n", "duplicate field 'kind'"},
      {"      credits: [{name: Keith, kind: performer, role: Guitar, role: Violin}]\n", "duplicate field 'role'"},
      {"      credits: [{name: [Keith], kind: performer}]\n", "name must be a scalar"},
      {"      credits: [{name: Keith, kind: [performer]}]\n", "kind must be a scalar"},
      {"      credits: [{name: Keith, kind: performer, role: {}}]\n", "role must be a scalar"},
      {"      credits: [{name: '', kind: performer}]\n", "blank"},
      {"      credits: [{name: ' ', kind: performer}]\n", "blank"},
      {"      conductor: Keith\n", "unknown field 'conductor'"},
      {"      ensemble: Keith\n", "unknown field 'ensemble'"},
      {"      soloist: Keith\n", "unknown field 'soloist'"},
      {"      musicians: []\n", "unknown field 'musicians'"},
      {"      custom: {credits: X}\n", "reserved metadata key"},
    });

    for (auto const& [fields, expected] : kRejectedPayloads)
    {
      CAPTURE(fields);
      auto const error = previewFailure(trackDocument(fields));
      CHECK(error.code == Error::Code::FormatRejected);
      CHECK_THAT(error.message, Catch::Matchers::ContainsSubstring(std::string{expected}));
    }
  }

  TEST_CASE("LibraryYaml - an invalid performance field rejects the payload before any mutation",
            "[runtime][unit][import-export][schema]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = library::test::makeTestMusicLibrary(temp.path(), temp.path());
    auto const existingTrackId =
      library::test::addTrackWithUniqueFixtureUri(library, library::test::makeEmptyTrackSpec("kept.flac"));

    auto const yamlPath = temp.path() / "document.yaml";
    auto const yaml = documentWith("      title: First\n"
                                   "      recording-date: 1981-05\n"
                                   "      credits:\n"
                                   "        - name: Keith\n"
                                   "          kind: performer\n"
                                   "          role: Guitar\n",
                                   "      title: Second\n"
                                   "      recording-date: 1981-2-30\n");

    {
      auto output = std::ofstream{yamlPath};
      output << yaml;
    }

    auto const res = LibraryYamlImporter{library}.importFromYamlOffline(yamlPath, ImportMode::Restore);
    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::FormatRejected);

    // The valid peer record and the pre-existing track both stay untouched:
    // the failure happened in the document preflight, before preparation.
    auto transaction = library.readTransaction();
    auto reader = library.tracks().reader(transaction);
    REQUIRE(reader.entryCount() == 1);
    auto const optView = reader.get(existingTrackId, TrackStore::Reader::LoadMode::Both);
    REQUIRE(optView);
    CHECK(optView->property().uri() == "kept.flac");
    CHECK_FALSE(optView->performance().recordingDate().isPresent());
    CHECK(optView->performance().credits().empty());
  }

  TEST_CASE("LibraryYaml - invalid credits preflight leaves all library state untouched",
            "[runtime][unit][import-export][schema]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = library::test::makeTestMusicLibrary(temp.path(), temp.path());
    auto const trackId = library::test::addTrack(
      library,
      library::test::TrackSpec{
        .title = "Kept", .credits = {{.name = "Original", .role = "Piano"}}, .uri = "kept.flac"});
    auto const yamlPath = temp.path() / "invalid.yaml";
    auto const revision = library.libraryRevision(library.readTransaction());
    auto const generation = library.dictionary().generation();
    auto const dictionarySize = library.dictionary().size();

    for (auto const mode : {ImportMode::Restore, ImportMode::Merge})
    {
      for (auto const& invalid :
           {std::string{"{name: ' ', kind: soloist}"},
            std::string{"{name: Invalid, kind: unknown}"},
            std::string{"{name: '"} + static_cast<char>(0xff) + "', kind: performer}",
            std::string{"{name: Invalid, kind: performer, role: '"} + static_cast<char>(0xff) + "'}"})
      {
        {
          auto output = std::ofstream{yamlPath, std::ios::binary};
          output << documentWith("      title: Never interned\n      credits: [{name: New, kind: conductor}]\n",
                                 std::format("      credits: [{}]\n", invalid));
        }

        auto const previewRes = LibraryYamlImporter{library}.previewImportFromYamlOffline(yamlPath, mode);
        REQUIRE_FALSE(previewRes);
        CHECK(previewRes.error().code == Error::Code::FormatRejected);
        auto const importRes = LibraryYamlImporter{library}.importFromYamlOffline(yamlPath, mode);
        REQUIRE_FALSE(importRes);
        CHECK(importRes.error().code == Error::Code::FormatRejected);
        auto transaction = library.readTransaction();
        CHECK(library.libraryRevision(transaction) == revision);
        CHECK(library.dictionary().generation() == generation);
        CHECK(library.dictionary().size() == dictionarySize);
        auto const reader = library.tracks().reader(transaction);
        CHECK(reader.entryCount() == 1);
        auto const optView = reader.get(trackId);
        REQUIRE(optView);
        CHECK(optView->metadata().title() == "Kept");
        auto const credits = optView->performance().credits();
        REQUIRE(credits.size() == 1);
        CHECK(library.dictionary().get(credits.front().nameId) == "Original");
        CHECK(library.dictionary().get(credits.front().roleId) == "Piano");
      }
    }
  }

  TEST_CASE("LibraryYaml - a future version rejects before the performance fields are interpreted",
            "[runtime][unit][import-export][schema]")
  {
    auto const yaml = std::format("version: {}\n"
                                  "export_mode: full\n"
                                  "library:\n"
                                  "  resources: []\n"
                                  "  tracks:\n"
                                  "    - uri: song.flac\n"
                                  "      title: Song\n"
                                  "      recording-date: not-a-date\n"
                                  "  lists: []\n",
                                  kYamlFormatVersion + 1);

    auto const error = previewFailure(yaml);
    CHECK(error.code == Error::Code::FormatRejected);
    auto const expectedVersion = std::format("Unsupported YAML version {}", kYamlFormatVersion + 1);
    CHECK_THAT(error.message, Catch::Matchers::ContainsSubstring(expectedVersion));
  }
} // namespace ao::rt::test
