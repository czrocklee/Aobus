// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/library/LibraryYamlExporter.h"

#include "ScanApplyTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/Credits.h>
#include <ao/library/RecordingDate.h>
#include <ao/library/TrackBuilder.h>
#include <ao/rt/library/LibraryTransfer.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  using namespace ao::library;

  namespace
  {
    // Creates one track carrying the supplied performance credits, so export
    // fixtures do not depend on a scanned media file.
    TrackId addPerformanceTrack(MusicLibrary& ml, RecordingDate date, std::vector<CreditView> credits)
    {
      auto const trackId =
        library::test::addTrackWithUniqueFixtureUri(ml, library::test::makeEmptyTrackSpec("song.flac"));

      library::test::mutateTrack(
        ml, trackId, [&](library::TrackBuilder& builder) { builder.metadata().recordingDate(date).credits(credits); });

      return trackId;
    }

    std::string exportMetadataText(MusicLibrary& ml)
    {
      auto const temp = ao::test::TempDir{};
      auto const yamlPath = temp.path() / "export.yaml";
      REQUIRE(LibraryYamlExporter{ml}.exportToYaml(yamlPath, ExportMode::Metadata));
      return ao::test::readFile(yamlPath);
    }

    std::string exportDeltaText(MusicLibrary& ml)
    {
      auto const temp = ao::test::TempDir{};
      auto const yamlPath = temp.path() / "export.yaml";
      REQUIRE(LibraryYamlExporter{ml}.exportToYaml(yamlPath, ExportMode::Delta));
      return ao::test::readFile(yamlPath);
    }

    // A library whose root carries one readable media file, so a delta export
    // builds a real file-derived baseline.
    MusicLibrary makeFileBackedLibrary(std::filesystem::path const& root, std::filesystem::path const& databasePath)
    {
      std::filesystem::copy_file(audio::test::requireAudioFixture("basic_metadata.flac"), root / "song.flac");
      return library::test::makeTestMusicLibrary(root, databasePath);
    }
  } // namespace

  TEST_CASE("LibraryYamlExporter - a present recording date is recorded, an absent one is omitted",
            "[runtime][unit][import-export][yaml]")
  {
    auto const temp = ao::test::TempDir{};
    auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path());
    auto const trackId = addPerformanceTrack(ml, RecordingDate{}, {});

    auto const exported = exportMetadataText(ml);
    CHECK_THAT(exported, Catch::Matchers::ContainsSubstring("uri: song.flac"));
    CHECK_THAT(exported, !Catch::Matchers::ContainsSubstring("recording-date"));

    library::test::mutateTrack(
      ml,
      trackId,
      [](library::TrackBuilder& builder)
      { builder.metadata().recordingDate(RecordingDate{.year = 1981, .month = 5, .day = 12}); });

    auto const withDate = exportMetadataText(ml);
    CHECK_THAT(withDate, Catch::Matchers::ContainsSubstring("recording-date:"));
    // The exact canonical value the date round trips through is pinned by the
    // import tests; quoting style is the emitter's, not the contract's.
    CHECK_THAT(withDate, Catch::Matchers::ContainsSubstring("1981"));
  }

  TEST_CASE("LibraryYamlExporter - credits record stored names kinds roles and order",
            "[runtime][unit][import-export][yaml]")
  {
    SECTION("entries without a role omit the role key")
    {
      auto const temp = ao::test::TempDir{};
      auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path());
      addPerformanceTrack(ml, RecordingDate{}, {{.name = "Keith"}});

      auto const exported = exportMetadataText(ml);
      CHECK_THAT(exported, Catch::Matchers::ContainsSubstring("credits:"));
      CHECK_THAT(exported, Catch::Matchers::ContainsSubstring("name: Keith"));
      CHECK_THAT(exported, !Catch::Matchers::ContainsSubstring("role:"));
    }

    SECTION("entries with roles keep them beside their names")
    {
      auto const temp = ao::test::TempDir{};
      auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path());
      addPerformanceTrack(ml, RecordingDate{}, {{.name = "Keith", .role = "Guitar"}});

      auto const exported = exportMetadataText(ml);
      CHECK_THAT(exported, Catch::Matchers::ContainsSubstring("name: Keith"));
      CHECK_THAT(exported, Catch::Matchers::ContainsSubstring("role: Guitar"));
    }

    SECTION("repeated names and roles are emitted as stored")
    {
      auto const temp = ao::test::TempDir{};
      auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path());
      addPerformanceTrack(ml,
                          RecordingDate{},
                          {{.name = "Keith", .role = "Guitar"}, {.name = "Anne"}, {.name = "Keith", .role = "Violin"}});

      auto const exported = exportMetadataText(ml);
      auto const firstName = exported.find("name: Keith");
      REQUIRE(firstName != std::string::npos);
      auto const secondName = exported.find("name: Keith", firstName + 1);
      REQUIRE(secondName != std::string::npos);
      CHECK_THAT(exported, Catch::Matchers::ContainsSubstring("name: Anne"));
      CHECK_THAT(exported, Catch::Matchers::ContainsSubstring("role: Guitar"));
      CHECK_THAT(exported, Catch::Matchers::ContainsSubstring("role: Violin"));
    }

    SECTION("an empty list is explicit")
    {
      auto const temp = ao::test::TempDir{};
      auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path());
      addPerformanceTrack(ml, RecordingDate{.year = 1981}, {});

      auto const exported = exportMetadataText(ml);
      CHECK_THAT(exported, Catch::Matchers::ContainsSubstring("credits: []"));
    }
  }

  TEST_CASE("LibraryYamlExporter - delta records a stored date against the file baseline",
            "[runtime][unit][import-export][yaml]")
  {
    // No media reader populates a recording date, so a delta baseline always
    // carries an absent date. The reachable delta paths are a stored date
    // against that absent baseline, and absence against absence; the clear
    // form (a baseline date versus a stored absent date) has no producible
    // producer fixture today and is covered by the import-side clear tests.
    SECTION("a stored date and list are recorded against the absent baseline")
    {
      auto const temp = ao::test::TempDir{};
      auto ml = makeFileBackedLibrary(temp.path(), temp.path() / "db");
      addPerformanceTrack(
        ml, RecordingDate{.year = 1981, .month = 5, .day = 12}, {{.name = "Keith", .role = "Guitar"}});

      auto const exported = exportDeltaText(ml);
      CHECK_THAT(exported, Catch::Matchers::ContainsSubstring("recording-date:"));
      CHECK_THAT(exported, Catch::Matchers::ContainsSubstring("name: Keith"));
    }

    SECTION("an absent date emits no recording-date key")
    {
      auto const temp = ao::test::TempDir{};
      auto ml = makeFileBackedLibrary(temp.path(), temp.path() / "db");
      addPerformanceTrack(ml, RecordingDate{}, {});

      auto const exported = exportDeltaText(ml);
      CHECK_THAT(exported, !Catch::Matchers::ContainsSubstring("recording-date"));
    }
  }

  TEST_CASE("LibraryYamlExporter - every mode carries the intended credits scope",
            "[runtime][unit][import-export][yaml]")
  {
    auto const temp = ao::test::TempDir{};
    auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto const trackId = addPerformanceTrack(ml, RecordingDate{}, {});
    auto const yamlPath = temp.path() / "export.yaml";

    for (auto const mode : {ExportMode::Full, ExportMode::Metadata, ExportMode::Delta, ExportMode::ListOnly})
    {
      REQUIRE(LibraryYamlExporter{ml}.exportToYaml(yamlPath, mode));
      auto const emptyExport = ao::test::readFile(yamlPath);
      CHECK(emptyExport.contains("credits: []") == (mode == ExportMode::Full || mode == ExportMode::Metadata));
    }

    auto const credits = std::vector<Credit>{{.name = "Performer", .role = "role"},
                                             {.name = "Soloist", .kind = CreditKind::Soloist},
                                             {.name = "Conductor", .kind = CreditKind::Conductor},
                                             {.name = "Ensemble", .kind = CreditKind::Ensemble}};
    library::test::mutateTrack(ml, trackId, [&](TrackBuilder& builder) { builder.metadata().credits(credits); });

    for (auto const mode : {ExportMode::Full, ExportMode::Metadata, ExportMode::Delta, ExportMode::ListOnly})
    {
      REQUIRE(LibraryYamlExporter{ml}.exportToYaml(yamlPath, mode));
      auto const exported = ao::test::readFile(yamlPath);
      CHECK(exported.contains("credits:") == (mode != ExportMode::ListOnly));

      if (mode != ExportMode::ListOnly)
      {
        CHECK(exported.find("kind: conductor") < exported.find("kind: ensemble"));
        CHECK(exported.find("kind: ensemble") < exported.find("kind: soloist"));
        CHECK(exported.find("kind: soloist") < exported.find("kind: performer"));
      }
    }
  }

  TEST_CASE("LibraryYamlExporter - delta compares complete canonical credits with the media baseline",
            "[runtime][unit][import-export][yaml]")
  {
    auto const temp = ao::test::TempDir{};
    writeScanFlacMetadataFixture(
      temp.path() / "song.flac",
      {"PERFORMER=  Jose\xcc\x81 ( Guitar )  ", "CONDUCTOR= Leader ", "PERFORMER=Second", "PERFORMER=Second"});
    auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto const canonical = std::vector<Credit>{{.name = "Leader", .kind = CreditKind::Conductor},
                                               {.name = "Jos\xc3\xa9", .role = "Guitar"},
                                               {.name = "Second"},
                                               {.name = "Second"}};
    auto const trackId =
      library::test::addTrack(ml, library::test::TrackSpec{.credits = canonical, .uri = "song.flac"});
    CHECK_FALSE(exportDeltaText(ml).contains("credits:"));

    auto changed = canonical;

    SECTION("role change")
    {
      changed[1].role = "Violin";
    }

    SECTION("kind change")
    {
      changed[1].kind = CreditKind::Soloist;
    }

    SECTION("within-kind order change")
    {
      std::swap(changed[1], changed[2]);
    }

    SECTION("duplicate multiplicity change")
    {
      changed.pop_back();
    }

    SECTION("explicit clear")
    {
      changed.clear();
    }

    library::test::mutateTrack(ml, trackId, [&](TrackBuilder& builder) { builder.metadata().credits(changed); });
    auto const exported = exportDeltaText(ml);
    CHECK(exported.contains("credits:"));

    if (changed.empty())
    {
      CHECK(exported.contains("credits: []"));
    }
  }
} // namespace ao::rt::test
