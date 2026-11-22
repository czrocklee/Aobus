// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CueYamlTestSupport.h"
#include "runtime/library/LibraryYamlImportOperation.h"
#include "runtime/library/LibraryYamlImporter.h"
#include "test/TestFixtureSupport.h"
#include "test/library/MusicLibraryTestSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/library/WritableLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/FileManifestView.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/library/ListWriter.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/library/TrackWriter.h>
#include <ao/rt/library/LibraryTransfer.h>
#include <ao/utility/Hash128.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
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

      void writeDocument(std::string_view document) const
      {
        auto output = std::ofstream{documentPath, std::ios::binary};
        output << document;
        REQUIRE(output);
      }

      void writeCarrierMerge() const
      {
        writeDocument(cueDocument(cueRecord(2, 0, 44100, 176400, 44100, "      title: Imported\n")));
      }

      void checkTracks(std::string_view kept = "Kept",
                       std::string_view opening = "Opening",
                       std::string_view tail = "Tail") const
      {
        CHECK(carrierIds(library) == ids);
        auto transaction = library.readTransaction();
        auto const optManifest = library.manifest().reader(transaction).get("kept.flac");
        REQUIRE(optManifest);
        CHECK(optManifest->trackId() == keptId);
        auto const reader = library.tracks().reader(transaction);
        CHECK(reader.entryCount() == 3);
        auto const optKept = reader.get(keptId);
        auto const optOpening = reader.get(ids.front());
        auto const optTail = reader.get(ids.back());
        REQUIRE(optKept);
        REQUIRE(optOpening);
        REQUIRE(optTail);
        CHECK(optKept->metadata().title() == kept);
        CHECK(optOpening->metadata().title() == opening);
        CHECK(optTail->metadata().title() == tail);
      }
    };

    void checkRejected(library::MusicLibrary& library,
                       LibraryYamlImportOperation& operation,
                       LibraryYamlImportOperation::PreparedImport const& prepared,
                       bool preview = false)
    {
      auto const revision = library.libraryRevision(library.readTransaction());
      {
        auto transaction = library::test::writeTransaction(library);
        auto const res = transaction.apply(
          [&](library::LibraryWrite& write)
          { return preview ? operation.preview(prepared, write) : operation.apply(prepared, write); });
        REQUIRE_FALSE(res);
        CHECK(res.error().code == Error::Code::InvalidState);
      }
      CHECK(library.libraryRevision(library.readTransaction()) == revision);
    }
  } // namespace

  TEST_CASE("CueYaml preparation - edits outside the payload invalidate a prepared merge", "[concurrency]")
  {
    bool const sameCarrier = GENERATE(false, true);
    CAPTURE(sameCarrier);
    auto fixture = CueYamlPreparationFixture{};

    if (sameCarrier)
    {
      fixture.writeDocument(cueDocument(cueRecord(7, 88200, 44100, 176400, 44100, "      title: Added\n")));
    }
    else
    {
      fixture.writeCarrierMerge();
    }

    auto importer = LibraryYamlImporter{fixture.library};
    auto operation = LibraryYamlImportOperation{importer};
    auto const preparedRes = operation.prepare(fixture.documentPath, ImportMode::Merge, false);
    REQUIRE(preparedRes);

    library::test::mutateTrack(fixture.library,
                               sameCarrier ? fixture.ids.back() : fixture.keptId,
                               [](library::TrackBuilder& builder) { builder.metadata().title("Intervening edit"); });
    checkRejected(fixture.library, operation, *preparedRes);
    fixture.checkTracks(
      sameCarrier ? "Kept" : "Intervening edit", "Opening", sameCarrier ? "Intervening edit" : "Tail");
  }

  TEST_CASE("CueYaml preparation - removing an unnamed member invalidates a prepared carrier append", "[concurrency]")
  {
    auto fixture = CueYamlPreparationFixture{};
    fixture.writeDocument(cueDocument(cueRecord(7, 88200, 44100, 176400, 44100, "      title: Added\n")));
    auto importer = LibraryYamlImporter{fixture.library};
    auto operation = LibraryYamlImportOperation{importer};
    auto const preparedRes = operation.prepare(fixture.documentPath, ImportMode::Merge, false);
    REQUIRE(preparedRes);

    {
      auto transaction = library::test::writeTransaction(fixture.library);
      auto const removeRes =
        transaction.apply([&](library::LibraryWrite& write) { return write.tracks().remove(fixture.ids.back()); });
      REQUIRE(removeRes);
      REQUIRE(*removeRes);
      REQUIRE(transaction.commit());
    }

    checkRejected(fixture.library, operation, *preparedRes);
    CHECK(carrierIds(fixture.library) == std::vector<TrackId>{fixture.ids.front()});
    auto transaction = fixture.library.readTransaction();
    auto const reader = fixture.library.tracks().reader(transaction);
    CHECK(reader.entryCount() == 2);
    CHECK_FALSE(reader.get(fixture.ids.back()));
    auto const optOpening = reader.get(fixture.ids.front());
    auto const optKept = reader.get(fixture.keptId);
    REQUIRE(optOpening);
    REQUIRE(optKept);
    CHECK(optOpening->metadata().title() == "Opening");
    CHECK(optKept->metadata().title() == "Kept");
  }

  TEST_CASE("CueYaml preparation - restoring an edited title still invalidates a prepared merge", "[concurrency]")
  {
    auto fixture = CueYamlPreparationFixture{};
    fixture.writeCarrierMerge();
    auto importer = LibraryYamlImporter{fixture.library};
    auto operation = LibraryYamlImportOperation{importer};
    auto const preparedRes = operation.prepare(fixture.documentPath, ImportMode::Merge, false);
    REQUIRE(preparedRes);

    auto const retitle = [&](std::string_view title)
    {
      library::test::mutateTrack(
        fixture.library, fixture.ids.front(), [&](library::TrackBuilder& builder) { builder.metadata().title(title); });
    };
    retitle("Temporary edit");
    fixture.checkTracks("Kept", "Temporary edit");
    retitle("Opening");
    fixture.checkTracks();
    checkRejected(fixture.library, operation, *preparedRes);
    fixture.checkTracks();
  }

  TEST_CASE("CueYaml preparation - a title edit invalidates a prepared album-only merge", "[concurrency]")
  {
    auto fixture = CueYamlPreparationFixture{};
    auto const targetId = library::test::addTrack(
      fixture.library,
      library::test::TrackSpec{.title = "Original title", .album = "Original album", .uri = "song.flac"});
    fixture.writeDocument(cueDocument("    - uri: song.flac\n      album: Imported album\n"));
    auto importer = LibraryYamlImporter{fixture.library};
    auto operation = LibraryYamlImportOperation{importer};
    auto const preparedRes = operation.prepare(fixture.documentPath, ImportMode::Merge, false);
    REQUIRE(preparedRes);

    library::test::mutateTrack(
      fixture.library, targetId, [](library::TrackBuilder& builder) { builder.metadata().title("Intervening title"); });
    checkRejected(fixture.library, operation, *preparedRes);

    auto transaction = fixture.library.readTransaction();
    auto const optTrack = fixture.library.tracks().reader(transaction).get(targetId);
    REQUIRE(optTrack);
    CHECK(optTrack->metadata().title() == "Intervening title");
    CHECK(fixture.library.dictionary().get(optTrack->metadata().albumId()) == "Original album");
  }

  TEST_CASE("CueYaml preparation - a manifest-only commit invalidates a prepared merge", "[concurrency]")
  {
    auto fixture = CueYamlPreparationFixture{};
    fixture.writeCarrierMerge();
    auto importer = LibraryYamlImporter{fixture.library};
    auto operation = LibraryYamlImportOperation{importer};
    auto const preparedRes = operation.prepare(fixture.documentPath, ImportMode::Merge, false);
    REQUIRE(preparedRes);
    constexpr auto kSignature = utility::Hash128{.bytes = {std::byte{0x41}}};
    {
      auto transaction = library::test::writeTransaction(fixture.library);
      REQUIRE(transaction.apply(
        [&](library::LibraryWrite& write) -> Result<>
        {
          auto const optManifest = write.tracks().manifest("disc.flac");
          REQUIRE(optManifest);
          auto manifest = library::FileManifestBuilder::fromView(*optManifest);
          manifest.audioPayloadLength(9).audioSignature(kSignature);
          return write.tracks().updateManifest("disc.flac", manifest);
        }));
      REQUIRE(transaction.commit());
    }

    checkRejected(fixture.library, operation, *preparedRes);
    fixture.checkTracks();
    auto transaction = fixture.library.readTransaction();
    auto const optManifest = fixture.library.manifest().reader(transaction).get("disc.flac");
    REQUIRE(optManifest);
    CHECK(optManifest->audioPayloadLength() == 9);
    CHECK(optManifest->audioSignature() == kSignature);
  }

  TEST_CASE("CueYaml preparation - a list commit invalidates list-only restore and preview", "[concurrency]")
  {
    auto fixture = CueYamlPreparationFixture{};
    fixture.writeDocument(
      std::format("version: {}\nexport-mode: list-only\nlibrary:\n  lists: []\n", kYamlFormatVersion));
    auto importer = LibraryYamlImporter{fixture.library};
    auto operation = LibraryYamlImportOperation{importer};
    auto const preparedRes = operation.prepare(fixture.documentPath, ImportMode::Restore, false);
    REQUIRE(preparedRes);

    auto const interveningId = [&]
    {
      auto transaction = library::test::writeTransaction(fixture.library);
      auto list = library::ListBuilder::makeEmpty().name("Intervening list");
      list.orderTrackIds().add(fixture.keptId);
      auto const res = transaction.apply([&](library::LibraryWrite& write) { return write.lists().create(list); });
      REQUIRE(res);
      REQUIRE(transaction.commit());
      return *res;
    }();
    checkRejected(fixture.library, operation, *preparedRes, true);
    checkRejected(fixture.library, operation, *preparedRes);
    fixture.checkTracks();
    auto transaction = fixture.library.readTransaction();
    auto const optList = fixture.library.lists().reader(transaction).get(interveningId);
    REQUIRE(optList);
    CHECK(optList->name() == "Intervening list");
    REQUIRE(optList->orderTrackIds().size() == 1);
    CHECK(optList->orderTrackIds().front() == fixture.keptId);
  }

  TEST_CASE("CueYaml preparation - aborted previews leave a prepared merge usable")
  {
    auto fixture = CueYamlPreparationFixture{};
    fixture.writeCarrierMerge();
    auto importer = LibraryYamlImporter{fixture.library};
    auto operation = LibraryYamlImportOperation{importer};
    auto const preparedRes = operation.prepare(fixture.documentPath, ImportMode::Merge, false);
    REQUIRE(preparedRes);
    auto const revision = fixture.library.libraryRevision(fixture.library.readTransaction());

    for (std::uint32_t attempt = 0; attempt < 2; ++attempt)
    {
      auto transaction = library::test::writeTransaction(fixture.library);
      auto const res =
        transaction.apply([&](library::LibraryWrite& write) { return operation.preview(*preparedRes, write); });
      REQUIRE(res);
      CHECK(res->tracksUpdated == 1);
      transaction.abort();
      CHECK(fixture.library.libraryRevision(fixture.library.readTransaction()) == revision);
      fixture.checkTracks();
    }

    auto transaction = library::test::writeTransaction(fixture.library);
    auto const res =
      transaction.apply([&](library::LibraryWrite& write) { return operation.apply(*preparedRes, write); });
    REQUIRE(res);
    CHECK(res->tracksUpdated == 1);
    REQUIRE(transaction.commit());
    CHECK(fixture.library.libraryRevision(fixture.library.readTransaction()) == revision + 1);
    fixture.checkTracks("Kept", "Imported");
  }

  TEST_CASE("CueYaml preparation - a different library rejects a prepared merge at the same revision")
  {
    auto source = CueYamlPreparationFixture{};
    auto target = CueYamlPreparationFixture{};
    REQUIRE(source.library.libraryRevision(source.library.readTransaction()) ==
            target.library.libraryRevision(target.library.readTransaction()));
    REQUIRE(source.library.metadataHeader(source.library.readTransaction()).libraryId !=
            target.library.metadataHeader(target.library.readTransaction()).libraryId);
    REQUIRE(source.ids == target.ids);
    source.writeCarrierMerge();
    auto sourceImporter = LibraryYamlImporter{source.library};
    auto sourceOperation = LibraryYamlImportOperation{sourceImporter};
    auto const preparedRes = sourceOperation.prepare(source.documentPath, ImportMode::Merge, false);
    REQUIRE(preparedRes);

    auto targetImporter = LibraryYamlImporter{target.library};
    auto targetOperation = LibraryYamlImportOperation{targetImporter};
    checkRejected(target.library, targetOperation, *preparedRes);
    target.checkTracks();
  }
} // namespace ao::rt::test
