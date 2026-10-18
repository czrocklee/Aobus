// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CueYamlTestSupport.h"
#include "runtime/library/LibraryYamlImportOperation.h"
#include "runtime/library/LibraryYamlImporter.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/FileManifestView.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/library/TrackWriter.h>
#include <ao/rt/library/LibraryTransfer.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string_view>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    struct CueYamlPreparationFixture final
    {
      ao::test::TempDir temp{};
      library::MusicLibrary library;
      std::filesystem::path documentPath{};
      std::vector<TrackId> ids{};
      TrackId keptId = kInvalidTrackId;

      CueYamlPreparationFixture()
        : library{library::test::makeTestMusicLibrary(temp.path() / "missing-media", temp.path() / "db")}
        , documentPath{temp.path() / "merge.yaml"}
      {
        REQUIRE(importCue(library,
                          documentPath,
                          cueDocument(cueRecord(2, 0, 44100, 176400, 44100, "      title: Opening\n") +
                                      cueRecord(9, 132300, 44100, 176400, 44100, "      title: Tail\n"))));
        keptId = library::test::addTrack(library, library::test::TrackSpec{.title = "Kept", .uri = "kept.flac"});
        ids = carrierIds(library);
        REQUIRE(ids.size() == 2);
      }

      void removeCarrier()
      {
        auto transaction = library::test::writeTransaction(library);
        REQUIRE(transaction.apply(
          [&](library::LibraryWrite& write) -> Result<>
          {
            for (auto const id : ids)
            {
              auto const removedRes = write.tracks().remove(id);
              REQUIRE(removedRes);
              REQUIRE(*removedRes);
            }

            return {};
          }));
        REQUIRE(transaction.commit());
      }

      void writePayload(bool appendOnly) const
      {
        auto const record = appendOnly ? cueRecord(7, 88200, 44100, 176400, 44100, "      title: Added\n")
                                       : cueRecord(2, 0, 44100, 176400, 44100, "      title: Updated\n");
        auto output = std::ofstream{documentPath, std::ios::binary};
        output << cueDocument("    - uri: kept.flac\n      title: Never committed\n" + record);
      }
    };

    void checkRejectedPreparedMerge(CueYamlPreparationFixture& fixture,
                                    LibraryYamlImportOperation& operation,
                                    LibraryYamlImportOperation::PreparedImport const& prepared,
                                    std::vector<TrackId> const& expectedIds)
    {
      auto const revision = fixture.library.libraryRevision(fixture.library.readTransaction());
      auto const dictionarySize = fixture.library.dictionary().size();

      {
        auto transaction = library::test::writeTransaction(fixture.library);
        auto const res =
          transaction.apply([&](library::LibraryWrite& write) { return operation.apply(prepared, write); });
        REQUIRE_FALSE(res);
        CHECK(res.error().code == Error::Code::InvalidState);
      }

      auto transaction = fixture.library.readTransaction();
      auto const optManifest = fixture.library.manifest().reader(transaction).get("disc.flac");

      if (expectedIds.empty())
      {
        CHECK_FALSE(optManifest);
      }
      else
      {
        REQUIRE(optManifest);
        CHECK(std::ranges::equal(optManifest->segmentTrackIds(), expectedIds));
      }

      CHECK(fixture.library.libraryRevision(transaction) == revision);
      CHECK(fixture.library.dictionary().size() == dictionarySize);
      auto const reader = fixture.library.tracks().reader(transaction);
      CHECK(reader.entryCount() == expectedIds.size() + 1);
      auto const optKept = reader.get(fixture.keptId);
      REQUIRE(optKept);
      CHECK(optKept->metadata().title() == "Kept");

      if (std::ranges::contains(expectedIds, fixture.ids.front()))
      {
        auto const optFirst = reader.get(fixture.ids.front());
        REQUIRE(optFirst);
        CHECK(optFirst->metadata().title() == "Opening");
      }
    }
  } // namespace

  TEST_CASE("CueYaml preparation - deleting an untouched carrier ordinal invalidates the whole offline merge",
            "[runtime][unit][import-export][concurrency][cue-yaml-preparation]")
  {
    for (bool const appendOnly : {false, true})
    {
      CAPTURE(appendOnly);
      auto fixture = CueYamlPreparationFixture{};
      fixture.writePayload(appendOnly);
      auto importer = LibraryYamlImporter{fixture.library};
      auto operation = LibraryYamlImportOperation{importer};
      auto const preparedRes = operation.prepare(fixture.documentPath, ImportMode::Merge, false);
      REQUIRE(preparedRes);

      {
        auto transaction = library::test::writeTransaction(fixture.library);
        auto const removedRes =
          transaction.apply([&](library::LibraryWrite& write) { return write.tracks().remove(fixture.ids.back()); });
        REQUIRE(removedRes);
        REQUIRE(*removedRes);
        REQUIRE(transaction.commit());
      }

      checkRejectedPreparedMerge(fixture, operation, *preparedRes, {fixture.ids.front()});
    }
  }

  TEST_CASE("CueYaml preparation - appending an untouched carrier ordinal invalidates the whole offline merge",
            "[runtime][unit][import-export][concurrency][cue-yaml-preparation]")
  {
    for (bool const appendOnly : {false, true})
    {
      CAPTURE(appendOnly);
      auto fixture = CueYamlPreparationFixture{};
      fixture.writePayload(appendOnly);
      auto importer = LibraryYamlImporter{fixture.library};
      auto operation = LibraryYamlImportOperation{importer};
      auto const preparedRes = operation.prepare(fixture.documentPath, ImportMode::Merge, false);
      REQUIRE(preparedRes);
      auto expectedIds = fixture.ids;

      {
        auto transaction = library::test::writeTransaction(fixture.library);
        auto const addedRes = transaction.apply(
          [&](library::LibraryWrite& write) -> Result<std::vector<TrackId>>
          {
            auto builder = library::TrackBuilder::makeEmpty();
            builder.property()
              .uri("disc.flac")
              .sampleRate(SampleRate{44100})
              .segment(library::TrackSegment{.startFrame = 44100, .frameCount = 44100, .ordinal = 4});
            auto const members = std::array{builder};
            return write.tracks().addCarrierMembers("disc.flac", members);
          });
        REQUIRE(addedRes);
        REQUIRE(addedRes->size() == 1);
        expectedIds.insert(expectedIds.begin() + 1, addedRes->front());
        REQUIRE(transaction.commit());
      }

      checkRejectedPreparedMerge(fixture, operation, *preparedRes, expectedIds);
    }
  }

  TEST_CASE("CueYaml preparation - carrier appearance or complete removal invalidates an append-only merge",
            "[runtime][unit][import-export][concurrency][cue-yaml-preparation]")
  {
    auto fixture = CueYamlPreparationFixture{};
    bool hadCarrierAtPreparation = true;

    SECTION("a carrier appeared at a previously absent URI")
    {
      fixture.removeCarrier();
      hadCarrierAtPreparation = false;
    }

    SECTION("the complete carrier was removed")
    {
    }

    fixture.writePayload(true);
    auto importer = LibraryYamlImporter{fixture.library};
    auto operation = LibraryYamlImportOperation{importer};
    auto const preparedRes = operation.prepare(fixture.documentPath, ImportMode::Merge, false);
    REQUIRE(preparedRes);
    auto expectedIds = std::vector<TrackId>{};

    if (hadCarrierAtPreparation)
    {
      fixture.removeCarrier();
    }
    else
    {
      auto transaction = library::test::writeTransaction(fixture.library);
      auto const createdRes = transaction.apply(
        [&](library::LibraryWrite& write) -> Result<std::vector<TrackId>>
        {
          auto builder = library::TrackBuilder::makeEmpty();
          builder.property()
            .uri("disc.flac")
            .sampleRate(SampleRate{44100})
            .segment(library::TrackSegment{.startFrame = 44100, .frameCount = 44100, .ordinal = 4});
          auto const members = std::array{builder};
          return write.tracks().createCarrier(members, library::FileManifestBuilder::makeEmpty(), 176400);
        });
      REQUIRE(createdRes);
      expectedIds = *createdRes;
      REQUIRE(transaction.commit());
    }

    checkRejectedPreparedMerge(fixture, operation, *preparedRes, expectedIds);
  }

  TEST_CASE("CueYaml preparation - unrelated offline edits do not invalidate unchanged carrier membership",
            "[runtime][unit][import-export][concurrency][cue-yaml-preparation]")
  {
    auto fixture = CueYamlPreparationFixture{};

    {
      auto output = std::ofstream{fixture.documentPath, std::ios::binary};
      output << cueDocument(cueRecord(2, 0, 44100, 176400, 44100, "      title: Updated\n"));
    }

    auto importer = LibraryYamlImporter{fixture.library};
    auto operation = LibraryYamlImportOperation{importer};
    auto const preparedRes = operation.prepare(fixture.documentPath, ImportMode::Merge, false);
    REQUIRE(preparedRes);
    library::test::mutateTrack(fixture.library,
                               fixture.keptId,
                               [](library::TrackBuilder& builder)
                               { builder.metadata().title("Concurrent unrelated edit"); });

    {
      auto transaction = library::test::writeTransaction(fixture.library);
      auto const res =
        transaction.apply([&](library::LibraryWrite& write) { return operation.apply(*preparedRes, write); });
      REQUIRE(res);
      CHECK(res->tracksUpdated == 1);
      CHECK(res->tracksCreated == 0);
      REQUIRE(transaction.commit());
    }

    CHECK(carrierIds(fixture.library) == fixture.ids);
    auto transaction = fixture.library.readTransaction();
    auto const reader = fixture.library.tracks().reader(transaction);
    auto const optKept = reader.get(fixture.keptId);
    auto const optFirst = reader.get(fixture.ids.front());
    REQUIRE(optKept);
    REQUIRE(optFirst);
    CHECK(optKept->metadata().title() == "Concurrent unrelated edit");
    CHECK(optFirst->metadata().title() == "Updated");
    CHECK(reader.entryCount() == 3);
  }
} // namespace ao::rt::test
