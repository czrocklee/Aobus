// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "ScanApplyTestSupport.h"
#include "runtime/library/LibraryYamlExporter.h"
#include "runtime/library/LibraryYamlImporter.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/library/Credits.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/RecordingDate.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/rt/library/LibraryTransfer.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  using namespace ao::library;

  namespace
  {
    template<typename Inspect>
    void withTrack(MusicLibrary& ml, std::string_view uri, Inspect&& inspect)
    {
      auto transaction = ml.readTransaction();

      for (auto const& [id, view] : ml.tracks().reader(transaction))
      {
        if (view.property().uri() == uri)
        {
          std::forward<Inspect>(inspect)(view);
          return;
        }
      }

      FAIL("Missing expected track");
    }

    std::string trackDocument(std::string_view trackFields, ExportMode mode = ExportMode::Metadata)
    {
      return std::format("version: {}\nexport_mode: {}\nlibrary:\n{}"
                         "  tracks:\n    - uri: song.flac\n{}  lists: []\n",
                         kYamlFormatVersion,
                         exportModeName(mode),
                         mode == ExportMode::Full ? "  resources: []\n" : "",
                         trackFields);
    }

    void writeDocument(std::filesystem::path const& path, std::string_view document)
    {
      auto output = std::ofstream{path, std::ios::binary};
      output << document;
      REQUIRE(output.good());
    }

    struct LibraryStamp final
    {
      std::uint64_t revision = 0;
      std::uint64_t dictionaryGeneration = 0;
      std::size_t dictionarySize = 0;

      bool operator==(LibraryStamp const&) const = default;
    };

    LibraryStamp libraryStamp(MusicLibrary& ml)
    {
      auto const transaction = ml.readTransaction();
      return LibraryStamp{.revision = ml.libraryRevision(transaction),
                          .dictionaryGeneration = ml.dictionary().generation(),
                          .dictionarySize = ml.dictionary().size()};
    }
  } // namespace

  TEST_CASE("LibraryYaml - round trip preserves date precision and complete canonical credits",
            "[runtime][integration][import-export][yaml]")
  {
    auto date = RecordingDate{};

    SECTION("complete-date precision")
    {
      date = RecordingDate{.year = 1981, .month = 5, .day = 12};
    }

    SECTION("month precision")
    {
      date = RecordingDate{.year = 1981, .month = 5};
    }

    SECTION("year precision")
    {
      date = RecordingDate{.year = 1981};
    }

    auto const input = std::vector<Credit>{{.name = " Keith ", .role = " Guitar "},
                                           {.name = "Anne", .kind = CreditKind::Soloist},
                                           {.name = " Keith ", .role = " Guitar "},
                                           {.name = "Boulez", .kind = CreditKind::Conductor},
                                           {.name = "Orchestra", .kind = CreditKind::Ensemble},
                                           {.name = "Jose\xcc\x81", .kind = CreditKind::Soloist, .role = " \t "}};
    auto const expected = std::vector<Credit>{{.name = "Boulez", .kind = CreditKind::Conductor},
                                              {.name = "Orchestra", .kind = CreditKind::Ensemble},
                                              {.name = "Anne", .kind = CreditKind::Soloist},
                                              {.name = "Jos\xc3\xa9", .kind = CreditKind::Soloist},
                                              {.name = "Keith", .role = "Guitar"},
                                              {.name = "Keith", .role = "Guitar"}};

    for (auto const mode : {ExportMode::Full, ExportMode::Metadata, ExportMode::Delta})
    {
      auto const temp = ao::test::TempDir{};
      auto source = library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db");
      library::test::addTrack(
        source, library::test::TrackSpec{.recordingDate = date, .credits = input, .uri = "song.flac"});
      auto const yamlPath = temp.path() / "roundtrip.yaml";
      REQUIRE(LibraryYamlExporter{source}.exportToYaml(yamlPath, mode));
      auto const target = ao::test::TempDir{};
      auto destination = library::test::makeTestMusicLibrary(target.path(), target.path());
      auto const reportRes = LibraryYamlImporter{destination}.importFromYamlOffline(yamlPath, ImportMode::Restore);
      REQUIRE(reportRes);
      CHECK(reportRes->tracksCreated == 1);
      withTrack(destination,
                "song.flac",
                [&](TrackView const& view)
                {
                  CHECK(view.performance().recordingDate() == date);
                  CHECK(library::test::trackSpecFromView(destination, view).credits == expected);
                });
    }
  }

  TEST_CASE("LibraryYaml - role absence and literal null text survive round trip",
            "[runtime][integration][import-export][yaml]")
  {
    auto const temp = ao::test::TempDir{};
    auto destination = library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto const yamlPath = temp.path() / "roles.yaml";
    writeDocument(yamlPath,
                  trackDocument("      credits:\n"
                                "        - {name: Omitted, kind: conductor}\n"
                                "        - name: Empty\n          kind: ensemble\n          role:\n"
                                "        - {name: Quoted, kind: soloist, role: ''}\n"
                                "        - {name: Blank, kind: performer, role: ' '}\n"
                                "        - {name: Null, kind: performer, role: null}\n"
                                "        - {name: Tilde, kind: performer, role: ~}\n"
                                "        - {name: NBSP, kind: performer, role: '\xc2\xa0'}\n"));
    REQUIRE(LibraryYamlImporter{destination}.importFromYamlOffline(yamlPath, ImportMode::Restore));
    auto const expected = std::vector<Credit>{{.name = "Omitted", .kind = CreditKind::Conductor},
                                              {.name = "Empty", .kind = CreditKind::Ensemble},
                                              {.name = "Quoted", .kind = CreditKind::Soloist},
                                              {.name = "Blank"},
                                              {.name = "Null", .role = "null"},
                                              {.name = "Tilde", .role = "~"},
                                              {.name = "NBSP", .role = "\xc2\xa0"}};
    withTrack(destination,
              "song.flac",
              [&](TrackView const& view)
              { CHECK(library::test::trackSpecFromView(destination, view).credits == expected); });
    auto const exportedPath = temp.path() / "export.yaml";
    REQUIRE(LibraryYamlExporter{destination}.exportToYaml(exportedPath, ExportMode::Metadata));
    REQUIRE(LibraryYamlImporter{destination}.importFromYamlOffline(exportedPath, ImportMode::Restore));
    withTrack(destination,
              "song.flac",
              [&](TrackView const& view)
              { CHECK(library::test::trackSpecFromView(destination, view).credits == expected); });
  }

  TEST_CASE("LibraryYaml - every metadata mode preserves omitted merge credits and replaces present credits",
            "[runtime][integration][import-export][yaml]")
  {
    auto const original = std::vector<Credit>{
      {.name = "Curated", .kind = CreditKind::Soloist, .role = "Piano"}, {.name = "Other", .role = "Guitar"}};

    for (auto const mode : {ExportMode::Full, ExportMode::Metadata, ExportMode::Delta})
    {
      for (auto const* overlay : {"", "      credits: []\n", "      credits: [{name: Replacement, kind: ensemble}]\n"})
      {
        CAPTURE(mode, overlay);
        auto const temp = ao::test::TempDir{};
        writeScanFlacMetadataFixture(temp.path() / "song.flac", {"CONDUCTOR=File", "PERFORMER=File performer"});
        auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db");
        library::test::addTrack(ml,
                                library::test::TrackSpec{.work = "Work",
                                                         .movement = "Movement",
                                                         .recordingDate = {.year = 1955},
                                                         .credits = original,
                                                         .uri = "song.flac",
                                                         .movementNumber = 2,
                                                         .movementTotal = 4});
        auto const yamlPath = temp.path() / "merge.yaml";
        writeDocument(yamlPath, trackDocument(std::string{"      title: Retitled\n"} + overlay, mode));
        REQUIRE(LibraryYamlImporter{ml}.importFromYamlOffline(yamlPath, ImportMode::Merge));
        withTrack(ml,
                  "song.flac",
                  [&](TrackView const& view)
                  {
                    auto const spec = library::test::trackSpecFromView(ml, view);
                    CHECK(spec.title == "Retitled");
                    CHECK(spec.work == "Work");
                    CHECK(spec.movement == "Movement");
                    CHECK(spec.movementNumber == 2);
                    CHECK(spec.movementTotal == 4);
                    CHECK(spec.recordingDate == RecordingDate{.year = 1955});

                    if (std::string_view{overlay}.empty())
                    {
                      CHECK(spec.credits == original);
                    }
                    else if (std::string_view{overlay}.contains("[]"))
                    {
                      CHECK(spec.credits.empty());
                    }
                    else
                    {
                      CHECK(spec.credits == std::vector<Credit>{{.name = "Replacement", .kind = CreditKind::Ensemble}});
                    }
                  });
      }
    }
  }

  TEST_CASE("LibraryYaml - creation and restore choose credits baselines by payload mode",
            "[runtime][integration][import-export][yaml]")
  {
    for (auto const mode : {ExportMode::Full, ExportMode::Metadata, ExportMode::Delta})
    {
      for (auto const importMode : {ImportMode::Restore, ImportMode::Merge})
      {
        for (std::string_view const fileState : {"missing", "readable", "malformed"})
        {
          for (bool const clear : {false, true})
          {
            CAPTURE(mode, importMode, fileState, clear);
            auto const temp = ao::test::TempDir{};

            if (fileState == "readable")
            {
              writeScanFlacMetadataFixture(temp.path() / "song.flac", {"CONDUCTOR=File", "PERFORMER=Player (Piano)"});
            }
            else if (fileState == "malformed")
            {
              writeDocument(temp.path() / "song.flac", "Not FLAC");
            }

            auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db");
            auto const yamlPath = temp.path() / "create.yaml";
            writeDocument(yamlPath, trackDocument(clear ? "      credits: []\n" : "", mode));
            REQUIRE(LibraryYamlImporter{ml}.importFromYamlOffline(yamlPath, importMode));
            withTrack(ml,
                      "song.flac",
                      [&](TrackView const& view)
                      {
                        auto const spec = library::test::trackSpecFromView(ml, view);
                        CHECK_FALSE(spec.recordingDate.isPresent());

                        if (mode == ExportMode::Delta && fileState == "readable" && !clear)
                        {
                          CHECK(spec.credits == std::vector<Credit>{{.name = "File", .kind = CreditKind::Conductor},
                                                                    {.name = "Player", .role = "Piano"}});
                        }
                        else
                        {
                          CHECK(spec.credits.empty());
                        }
                      });
          }
        }
      }
    }
  }

  TEST_CASE("LibraryYaml - empty performance metadata round trips explicitly",
            "[runtime][integration][import-export][yaml]")
  {
    auto const temp = ao::test::TempDir{};
    auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db");
    library::test::addTrack(ml, library::test::makeEmptyTrackSpec("song.flac"));
    auto const yamlPath = temp.path() / "empty.yaml";

    for (auto const mode : {ExportMode::Full, ExportMode::Metadata, ExportMode::Delta})
    {
      REQUIRE(LibraryYamlExporter{ml}.exportToYaml(yamlPath, mode));
      REQUIRE(LibraryYamlImporter{ml}.importFromYamlOffline(yamlPath, ImportMode::Restore));
      withTrack(ml,
                "song.flac",
                [](TrackView const& view)
                {
                  CHECK_FALSE(view.performance().recordingDate().isPresent());
                  CHECK(view.performance().credits().empty());
                });
    }
  }

  TEST_CASE("LibraryYaml - list-only transfer leaves credits untouched", "[runtime][integration][import-export][yaml]")
  {
    auto const temp = ao::test::TempDir{};
    auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto const original = std::vector<Credit>{{.name = "Kept", .kind = CreditKind::Ensemble}};
    library::test::addTrack(ml, library::test::TrackSpec{.credits = original, .uri = "song.flac"});
    auto const yamlPath = temp.path() / "lists.yaml";
    REQUIRE(LibraryYamlExporter{ml}.exportToYaml(yamlPath, ExportMode::ListOnly));
    CHECK_FALSE(ao::test::readFile(yamlPath).contains("credits"));

    for (auto const mode : {ImportMode::Restore, ImportMode::Merge})
    {
      REQUIRE(LibraryYamlImporter{ml}.importFromYamlOffline(yamlPath, mode));
      withTrack(ml,
                "song.flac",
                [&](TrackView const& view) { CHECK(library::test::trackSpecFromView(ml, view).credits == original); });
    }
  }

  TEST_CASE("LibraryYaml - recording-date empty scalar text clears on merge and invalid text has no effect",
            "[runtime][integration][import-export][yaml]")
  {
    auto const temp = ao::test::TempDir{};
    auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto const original = std::vector<Credit>{{.name = "Glenn", .kind = CreditKind::Soloist, .role = "Piano"}};
    library::test::addTrack(
      ml, library::test::TrackSpec{.recordingDate = {.year = 1955}, .credits = original, .uri = "song.flac"});
    auto const yamlPath = temp.path() / "grammar.yaml";
    constexpr auto kDateGrammar = "recording-date must be an empty scalar or a canonical";

    auto expectStoredDate = [&](RecordingDate const date)
    {
      withTrack(ml,
                "song.flac",
                [&](TrackView const& view)
                {
                  CHECK(view.performance().recordingDate() == date);
                  CHECK(library::test::trackSpecFromView(ml, view).credits == original);
                });
    };

    SECTION("present empty scalar forms clear only the date")
    {
      auto scalar = std::string{};

      SECTION("plain empty")
      {
        scalar = "";
      }

      SECTION("single-quoted empty")
      {
        scalar = "''";
      }

      SECTION("double-quoted empty")
      {
        scalar = "\"\"";
      }

      writeDocument(yamlPath, trackDocument(std::format("      recording-date: {}\n", scalar)));
      auto const before = libraryStamp(ml);
      REQUIRE(LibraryYamlImporter{ml}.previewImportFromYamlOffline(yamlPath, ImportMode::Merge));
      CHECK(libraryStamp(ml) == before);
      REQUIRE(LibraryYamlImporter{ml}.importFromYamlOffline(yamlPath, ImportMode::Merge));
      auto const after = libraryStamp(ml);
      CHECK(after.revision == before.revision + 1);
      CHECK(after.dictionaryGeneration == before.dictionaryGeneration);
      CHECK(after.dictionarySize == before.dictionarySize);
      expectStoredDate({});
    }

    SECTION("an omitted recording-date preserves the stored date")
    {
      writeDocument(yamlPath, trackDocument(""));
      auto const before = libraryStamp(ml);
      REQUIRE(LibraryYamlImporter{ml}.previewImportFromYamlOffline(yamlPath, ImportMode::Merge));
      CHECK(libraryStamp(ml) == before);
      REQUIRE(LibraryYamlImporter{ml}.importFromYamlOffline(yamlPath, ImportMode::Merge));
      CHECK(libraryStamp(ml).dictionaryGeneration == before.dictionaryGeneration);
      CHECK(libraryStamp(ml).dictionarySize == before.dictionarySize);
      expectStoredDate({.year = 1955});
    }

    SECTION("literal null tilde quoted whitespace and noncanonical precision change nothing")
    {
      for (auto const [fields, token] : std::to_array<std::pair<std::string_view, std::string_view>>(
             {{"      recording-date: null\n", "null"},
              {"      recording-date: ~\n", "~"},
              {"      recording-date: ' '\n", " "},
              {"      recording-date: \"      \"\n", "      "},
              {"      recording-date: \"\\t\"\n", "\t"},
              {"      recording-date: '1981-5'\n", "1981-5"},
              {"      recording-date: ' 1981'\n", " 1981"}}))
      {
        CAPTURE(fields);
        writeDocument(yamlPath, trackDocument(fields));
        auto const before = libraryStamp(ml);
        auto const previewRes = LibraryYamlImporter{ml}.previewImportFromYamlOffline(yamlPath, ImportMode::Merge);
        REQUIRE_FALSE(previewRes);
        CHECK(previewRes.error().code == Error::Code::FormatRejected);
        CHECK_THAT(previewRes.error().message, Catch::Matchers::ContainsSubstring(kDateGrammar));
        CHECK(libraryStamp(ml) == before);
        auto const mergeRes = LibraryYamlImporter{ml}.importFromYamlOffline(yamlPath, ImportMode::Merge);
        REQUIRE_FALSE(mergeRes);
        CHECK(mergeRes.error().code == Error::Code::FormatRejected);
        CHECK_THAT(mergeRes.error().message, Catch::Matchers::ContainsSubstring(kDateGrammar));
        CHECK(libraryStamp(ml) == before);
        expectStoredDate({.year = 1955});
        CHECK_FALSE(ml.dictionary().findId(token));
      }
    }
  }
} // namespace ao::rt::test
