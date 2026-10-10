// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/library/LibraryYamlExporter.h"
#include "runtime/library/LibraryYamlImporter.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/library/Credits.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/RecordingDate.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/rt/library/LibraryTransfer.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>

namespace ao::rt::test
{
  using namespace ao::library;

  namespace
  {
    void checkStoredCredits(MusicLibrary& library, library::test::TrackSpec const& expected)
    {
      auto const transaction = library.readTransaction();
      auto const reader = library.tracks().reader(transaction);
      REQUIRE(reader.entryCount() == 1);

      for (auto const& [id, view] : reader)
      {
        REQUIRE(view.isColdValid());
        CHECK(view.performance().credits().size() == 8185);
        auto const stored = library::test::trackSpecFromView(library, view);
        CHECK(stored.uri == expected.uri);
        CHECK(stored.recordingDate == expected.recordingDate);
        CHECK(stored.credits == expected.credits);
      }
    }
  } // namespace

  TEST_CASE("LibraryYaml - maximum short-URI credits survive persisted reopen and metadata round trip",
            "[runtime][integration][import-export][yaml]")
  {
    auto expected = library::test::makeEmptyTrackSpec("x.wav");
    expected.recordingDate = RecordingDate{.year = 1981, .month = 5};

    SECTION("all entries use the inferred Performer segment")
    {
    }

    SECTION("all four canonical segments share the maximum capacity")
    {
      expected.credits = {{.name = "Conductor: second", .kind = CreditKind::Conductor, .role = "Guest; leader"},
                          {.name = "Conductor: first", .kind = CreditKind::Conductor},
                          {.name = "Ensemble, A=B", .kind = CreditKind::Ensemble},
                          {.name = "Soloist", .kind = CreditKind::Soloist, .role = "Piano (solo)"}};
    }

    // The supported URI x.wav fits 8,185 entries within the whole cold-record bound without Work,
    // custom metadata, or covers. The date uses the existing prefix.
    auto const performers = std::array{Credit{.name = "José: A=B", .role = "Guitar; voice (guest)"},
                                       Credit{.name = "Player, second"},
                                       Credit{.name = "José: A=B", .role = "Guitar; voice (guest)"},
                                       Credit{.name = "Player [fourth]", .role = "null"}};
    expected.credits.reserve(8185);
    std::size_t performerIndex = 0;

    while (expected.credits.size() < 8185)
    {
      expected.credits.push_back(performers[performerIndex % performers.size()]);
      ++performerIndex;
    }

    auto const sourceTemp = ao::test::TempDir{};
    auto const sourceDatabase = sourceTemp.path() / "db";
    auto const yamlPath = sourceTemp.path() / "boundary.yaml";

    {
      auto source = library::test::makeTestMusicLibrary(sourceTemp.path(), sourceDatabase);
      library::test::addTrack(source, expected);
      checkStoredCredits(source, expected);
    }

    // Ordinary open performs deep validation; never overlap owners of this path.
    {
      auto sourceRes = library::test::openTestMusicLibrary(sourceTemp.path(), sourceDatabase);
      REQUIRE(sourceRes);
      checkStoredCredits(*sourceRes, expected);
      REQUIRE(LibraryYamlExporter{*sourceRes}.exportToYaml(yamlPath, ExportMode::Metadata));
    }

    auto const destinationTemp = ao::test::TempDir{};
    auto const destinationDatabase = destinationTemp.path() / "db";

    {
      auto destination = library::test::makeTestMusicLibrary(destinationTemp.path(), destinationDatabase);
      auto const importRes = LibraryYamlImporter{destination}.importFromYamlOffline(yamlPath, ImportMode::Restore);
      REQUIRE(importRes);
      CHECK(importRes->tracksCreated == 1);
      checkStoredCredits(destination, expected);
    }

    {
      auto destinationRes = library::test::openTestMusicLibrary(destinationTemp.path(), destinationDatabase);
      REQUIRE(destinationRes);
      checkStoredCredits(*destinationRes, expected);
    }
  }
} // namespace ao::rt::test
