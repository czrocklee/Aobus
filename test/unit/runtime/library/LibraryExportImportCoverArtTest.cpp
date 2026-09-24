// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "runtime/library/LibraryYamlExporter.h"
#include "runtime/library/LibraryYamlImporter.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/lmdb/LmdbTestSupport.h"
#include "test/unit/media/file/TestFile.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/PictureType.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceLayout.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/rt/library/LibraryTransfer.h>
#include <ao/utility/Sha256.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  using namespace ao::library;

  namespace
  {
    std::string readFileText(std::filesystem::path const& path)
    {
      auto stream = std::ifstream{path, std::ios::binary};
      return std::string{std::istreambuf_iterator{stream}, std::istreambuf_iterator<char>{}};
    }

    void writeFileText(std::filesystem::path const& path, std::string_view const text)
    {
      auto stream = std::ofstream{path, std::ios::binary};
      stream << text;
    }

    std::unordered_map<std::string, TrackView> tracksByUri(MusicLibrary const& ml, ReadTransaction const& transaction)
    {
      auto tracks = std::unordered_map<std::string, TrackView>{};

      for (auto const& [id, view] : ml.tracks().reader(transaction))
      {
        tracks.emplace(view.property().uri(), view);
      }

      return tracks;
    }

    /// Creates a track at @p uri, referencing @p coverBytes when any are given.
    void seedTrack(MusicLibrary& ml, std::string_view const uri, std::span<std::byte const> const coverBytes)
    {
      auto transaction = library::test::writeTransaction(ml);
      auto builder = TrackBuilder::makeEmpty();
      builder.property().uri(uri);
      builder.metadata().title("Existing");

      if (!coverBytes.empty())
      {
        auto resIdRes = library::test::physicalWriter(ml.resources(), transaction).create(coverBytes);
        REQUIRE(resIdRes);
        builder.coverArt().add(PictureType::FrontCover, *resIdRes);
      }

      REQUIRE(transaction.apply([&](LibraryWrite& write)
                                { return write.tracks().create(builder, FileManifestBuilder::makeEmpty()); }));
      REQUIRE(transaction.commit());
    }

    struct CoverSnapshot final
    {
      PictureType type{};
      utility::Sha256Digest digest{};
      std::uint32_t byteLength = 0;

      bool operator==(CoverSnapshot const&) const = default;
    };

    /// The descriptor each of @p uri's cover references names, in record order.
    std::vector<CoverSnapshot> coverSnapshots(MusicLibrary& ml, std::string_view const uri)
    {
      auto transaction = ml.readTransaction();
      auto const tracks = tracksByUri(ml, transaction);
      auto const iterator = tracks.find(std::string{uri});
      REQUIRE(iterator != tracks.end());

      auto const covers = iterator->second.coverArt();
      auto const resourceReader = ml.resources().reader(transaction);
      auto snapshots = std::vector<CoverSnapshot>{};

      for (std::uint16_t index = 0; index < covers.count(); ++index)
      {
        auto const optDescriptor = resourceReader.get(covers.at(index).resourceId);
        REQUIRE(optDescriptor);
        snapshots.push_back({covers.at(index).type, optDescriptor->digest, optDescriptor->byteLength});
      }

      return snapshots;
    }

    void seedSharedCovers(MusicLibrary& library,
                          std::span<std::byte const> frontBytes,
                          std::span<std::byte const> backBytes)
    {
      auto transaction = library::test::writeTransaction(library);
      auto writer = library::test::physicalWriter(library.resources(), transaction);
      auto const frontRes = writer.create(frontBytes);
      auto const backRes = writer.create(backBytes);
      REQUIRE(frontRes);
      REQUIRE(backRes);
      auto first = TrackBuilder::makeEmpty();
      first.property().uri("song1.flac");
      first.metadata().title("Song 1");
      first.coverArt().add(PictureType::BackCover, *backRes).add(PictureType::FrontCover, *frontRes);
      auto second = TrackBuilder::makeEmpty();
      second.property().uri("song2.flac");
      second.metadata().title("Song 2");
      second.coverArt().add(PictureType::FrontCover, *frontRes);
      REQUIRE(transaction.apply(
        [&](LibraryWrite& write) -> Result<>
        {
          auto tracks = write.tracks();
          REQUIRE(tracks.create(first, FileManifestBuilder::makeEmpty()));
          REQUIRE(tracks.create(second, FileManifestBuilder::makeEmpty()));
          return {};
        }));
      REQUIRE(transaction.commit());
    }

    void requireRejected(MusicLibrary& ml, std::filesystem::path const& path, ImportMode const mode)
    {
      auto importer = LibraryYamlImporter{ml};
      auto const res = importer.importFromYamlOffline(path, mode);
      auto const message = res ? std::string{} : res.error().message;
      INFO(message);
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
    }
  } // namespace

  TEST_CASE("LibraryYaml - a full document names each distinct cover once, by digest",
            "[runtime][integration][import-export][cover-art]")
  {
    auto const temp1 = ao::test::TempDir{};
    auto ml1 = library::test::makeTestMusicLibrary(temp1.path(), temp1.path());

    auto const coverData = lmdb::test::createTestData(1024);
    auto const backCoverData = lmdb::test::createTestData(257);
    auto const coverDigest = utility::computeSha256(coverData);
    auto const backCoverDigest = utility::computeSha256(backCoverData);
    seedSharedCovers(ml1, coverData, backCoverData);

    auto const yamlPath = std::filesystem::path{temp1.path()} / "covers.yaml";
    auto exporter = LibraryYamlExporter{ml1};
    REQUIRE(exporter.exportToYaml(yamlPath, ExportMode::Full));
    auto const content = readFileText(yamlPath);

    SECTION("the document carries digests and no cover bytes")
    {
      auto const coverText = utility::sha256Hex(coverDigest);
      auto const backCoverText = utility::sha256Hex(backCoverDigest);

      CHECK(content.contains("resources:"));
      CHECK(content.contains(coverText));
      CHECK(content.contains(backCoverText));

      // Bytes are gone, and with them the anchor and alias machinery that
      // expressed sharing: the table names each distinct cover once instead.
      CHECK_FALSE(content.contains("data:"));
      CHECK_FALSE(content.contains("&cover_"));
      CHECK_FALSE(content.contains("*cover_"));

      // One row per distinct cover, however many tracks name it.
      std::size_t occurrences = 0;

      for (auto position = content.find(coverText); position != std::string::npos;
           position = content.find(coverText, position + 1))
      {
        ++occurrences;
      }

      // Once in the table, then once for each of the two tracks that name it.
      CHECK(occurrences == 3);
      std::size_t backOccurrences = 0;

      for (auto position = content.find(backCoverText); position != std::string::npos;
           position = content.find(backCoverText, position + 1))
      {
        ++backOccurrences;
      }

      CHECK(backOccurrences == 2);
    }

    SECTION("resources are emitted in ascending digest order")
    {
      auto const firstText = utility::sha256Hex(std::min(coverDigest, backCoverDigest));
      auto const secondText = utility::sha256Hex(std::max(coverDigest, backCoverDigest));
      auto const tablePosition = content.find("resources:");
      REQUIRE(tablePosition != std::string::npos);

      CHECK(content.find(firstText, tablePosition) < content.find(secondText, tablePosition));
    }

    SECTION("two exports of one unchanged library are byte-identical")
    {
      auto const repeatPath = std::filesystem::path{temp1.path()} / "covers-again.yaml";
      REQUIRE(exporter.exportToYaml(repeatPath, ExportMode::Full));
      CHECK(readFileText(repeatPath) == content);
    }
  }

  TEST_CASE("LibraryYaml - full restore rebuilds the cover reference graph and sharing",
            "[runtime][integration][import-export][cover-art]")
  {
    auto const sourceTemp = ao::test::TempDir{};
    auto source = library::test::makeTestMusicLibrary(sourceTemp.path(), sourceTemp.path());
    auto const coverData = lmdb::test::createTestData(1024);
    auto const backCoverData = lmdb::test::createTestData(257);
    auto const coverDigest = utility::computeSha256(coverData);
    auto const backCoverDigest = utility::computeSha256(backCoverData);
    seedSharedCovers(source, coverData, backCoverData);
    auto const yamlPath = sourceTemp.path() / "covers.yaml";
    REQUIRE(LibraryYamlExporter{source}.exportToYaml(yamlPath, ExportMode::Full));

    auto const targetTemp = ao::test::TempDir{};
    auto target = library::test::makeTestMusicLibrary(targetTemp.path(), targetTemp.path());
    REQUIRE(LibraryYamlImporter{target}.importFromYamlOffline(yamlPath));

    auto transaction = target.readTransaction();
    auto const tracks = tracksByUri(target, transaction);
    REQUIRE(tracks.size() == 2);
    auto const& track1 = tracks.at("song1.flac");
    auto const& track2 = tracks.at("song2.flac");
    auto const optPrimary1 = track1.coverArt().primary();
    auto const optPrimary2 = track2.coverArt().primary();
    REQUIRE(optPrimary1);
    REQUIRE(optPrimary2);
    CHECK(optPrimary1->resourceId == optPrimary2->resourceId);
    REQUIRE(track1.coverArt().count() == 2);
    CHECK(track1.coverArt().at(0).type == PictureType::BackCover);
    CHECK(track1.coverArt().at(1).type == PictureType::FrontCover);
    REQUIRE(track2.coverArt().count() == 1);
    CHECK(track2.coverArt().at(0).type == PictureType::FrontCover);

    auto const reader = target.resources().reader(transaction);
    auto const optFront = reader.get(optPrimary1->resourceId);
    REQUIRE(optFront);
    CHECK(optFront->digest == coverDigest);
    CHECK(optFront->byteLength == coverData.size());
    auto const optBack = reader.get(track1.coverArt().at(0).resourceId);
    REQUIRE(optBack);
    CHECK(optBack->digest == backCoverDigest);
    CHECK(optBack->byteLength == backCoverData.size());
    CHECK(deriveResourceId(coverDigest) == optPrimary1->resourceId);
  }

  TEST_CASE("LibraryYaml - a full merge leaves the document's reference graph",
            "[runtime][integration][import-export][cover-art]")
  {
    auto const temp = ao::test::TempDir{};
    auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path());
    auto const uri = std::string{"song.flac"};
    auto const replacementData = lmdb::test::createTestData(3);
    auto const replacementText = utility::sha256Hex(utility::computeSha256(replacementData));

    {
      auto transaction = library::test::writeTransaction(ml);
      auto resWriter = library::test::physicalWriter(ml.resources(), transaction);
      auto frontIdRes = resWriter.create(lmdb::test::createTestData(8));
      REQUIRE(frontIdRes);
      auto backIdRes = resWriter.create(lmdb::test::createTestData(9));
      REQUIRE(backIdRes);

      auto builder = TrackBuilder::makeEmpty();
      builder.property().uri(uri);
      builder.coverArt().add(PictureType::FrontCover, *frontIdRes);
      builder.coverArt().add(PictureType::BackCover, *backIdRes);
      REQUIRE(transaction.apply([&](LibraryWrite& write)
                                { return write.tracks().create(builder, FileManifestBuilder::makeEmpty()); }));
      REQUIRE(transaction.commit());
    }

    auto const yamlPath = std::filesystem::path{temp.path()} / "covers.yaml";
    writeFileText(yamlPath,
                  std::format(R"(version: 5
export_mode: full
library:
  resources:
    - digest: {}
      length: 3
  tracks:
    - uri: song.flac
      covers:
        - type: 4
          resource: {}
  lists: []
)",
                              replacementText,
                              replacementText));

    auto importer = LibraryYamlImporter{ml};
    REQUIRE(importer.importFromYamlOffline(yamlPath, ImportMode::Merge));

    auto transaction = ml.readTransaction();
    auto const optManifest = ml.manifest().reader(transaction).get(uri);
    REQUIRE(optManifest);
    auto const optView =
      ml.tracks().reader(transaction).get(optManifest->trackId(), TrackStore::Reader::LoadMode::Both);
    REQUIRE(optView);
    REQUIRE(optView->coverArt().count() == 1);
    CHECK(optView->coverArt().at(0).type == PictureType::BackCover);

    auto const optDescriptor = ml.resources().reader(transaction).get(optView->coverArt().at(0).resourceId);
    REQUIRE(optDescriptor);
    CHECK(utility::sha256Hex(optDescriptor->digest) == replacementText);

    // The declared length created the row, because nothing in the database knew
    // this content yet and any figure beats none.
    CHECK(optDescriptor->byteLength == 3);
  }

  TEST_CASE("LibraryYaml - a declared length never overwrites a counted one",
            "[runtime][integration][import-export][cover-art]")
  {
    auto const temp = ao::test::TempDir{};
    auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path());
    auto const coverData = lmdb::test::createTestData(64);
    auto const coverText = utility::sha256Hex(utility::computeSha256(coverData));
    auto resId = kInvalidResourceId;

    {
      auto transaction = library::test::writeTransaction(ml);
      auto resIdRes = library::test::physicalWriter(ml.resources(), transaction).create(coverData);
      REQUIRE(resIdRes);
      resId = *resIdRes;

      auto builder = TrackBuilder::makeEmpty();
      builder.property().uri("song.flac");
      builder.coverArt().add(PictureType::FrontCover, resId);
      REQUIRE(transaction.apply([&](LibraryWrite& write)
                                { return write.tracks().create(builder, FileManifestBuilder::makeEmpty()); }));
      REQUIRE(transaction.commit());
    }

    auto const yamlPath = std::filesystem::path{temp.path()} / "hint.yaml";
    writeFileText(yamlPath,
                  std::format(R"(version: 5
export_mode: full
library:
  resources:
    - digest: {}
      length: 999999
  tracks:
    - uri: song.flac
      covers:
        - type: 3
          resource: {}
  lists: []
)",
                              coverText,
                              coverText));

    auto importer = LibraryYamlImporter{ml};
    REQUIRE(importer.importFromYamlOffline(yamlPath, ImportMode::Merge));

    auto transaction = ml.readTransaction();
    auto const tracks = tracksByUri(ml, transaction);
    REQUIRE(tracks.contains("song.flac"));
    auto const covers = tracks.at("song.flac").coverArt();
    REQUIRE(covers.count() == 1);
    CHECK(covers.at(0).type == PictureType::FrontCover);
    CHECK(covers.at(0).resourceId == resId);
    auto const optDescriptor = ml.resources().reader(transaction).get(covers.at(0).resourceId);
    REQUIRE(optDescriptor);
    CHECK(utility::sha256Hex(optDescriptor->digest) == coverText);
    CHECK(optDescriptor->byteLength == coverData.size());
  }

  TEST_CASE("LibraryYaml - the resource table's shape and closure are exact",
            "[runtime][unit][import-export][cover-art]")
  {
    auto const temp = ao::test::TempDir{};
    auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path());
    auto const yamlPath = std::filesystem::path{temp.path()} / "shape.yaml";
    auto const digestText = utility::sha256Hex(utility::computeSha256(lmdb::test::createTestData(16)));
    auto const otherText = utility::sha256Hex(utility::computeSha256(lmdb::test::createTestData(17)));

    SECTION("a cover reference naming no row rejects the document")
    {
      writeFileText(yamlPath,
                    std::format(R"(version: 5
export_mode: full
library:
  resources: []
  tracks:
    - uri: song.flac
      covers:
        - type: 3
          resource: {}
  lists: []
)",
                                digestText));
      requireRejected(ml, yamlPath, ImportMode::Restore);
    }

    SECTION("a row no track references rejects the document")
    {
      writeFileText(yamlPath,
                    std::format(R"(version: 5
export_mode: full
library:
  resources:
    - digest: {}
      length: 16
  tracks:
    - uri: song.flac
  lists: []
)",
                                digestText));
      requireRejected(ml, yamlPath, ImportMode::Restore);
    }

    SECTION("two rows carrying one digest reject the document")
    {
      writeFileText(yamlPath,
                    std::format(R"(version: 5
export_mode: full
library:
  resources:
    - digest: {}
      length: 16
    - digest: {}
      length: 32
  tracks:
    - uri: song.flac
      covers:
        - type: 3
          resource: {}
  lists: []
)",
                                digestText,
                                digestText,
                                digestText));
      requireRejected(ml, yamlPath, ImportMode::Restore);
    }

    SECTION("an uppercase digest rejects the document")
    {
      auto upper = digestText;

      for (auto& character : upper)
      {
        character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
      }

      writeFileText(yamlPath,
                    std::format(R"(version: 5
export_mode: full
library:
  resources:
    - digest: {}
      length: 16
  tracks:
    - uri: song.flac
      covers:
        - type: 3
          resource: {}
  lists: []
)",
                                upper,
                                upper));
      requireRejected(ml, yamlPath, ImportMode::Restore);
    }

    SECTION("a short digest rejects the document")
    {
      writeFileText(yamlPath, R"(version: 5
export_mode: full
library:
  resources:
    - digest: abcdef
      length: 16
  tracks:
    - uri: song.flac
      covers:
        - type: 3
          resource: abcdef
  lists: []
)");
      requireRejected(ml, yamlPath, ImportMode::Restore);
    }

    SECTION("a length above the 32-bit field rejects the document")
    {
      writeFileText(yamlPath,
                    std::format(R"(version: 5
export_mode: full
library:
  resources:
    - digest: {}
      length: 4294967296
  tracks:
    - uri: song.flac
      covers:
        - type: 3
          resource: {}
  lists: []
)",
                                digestText,
                                digestText));
      requireRejected(ml, yamlPath, ImportMode::Restore);
    }

    SECTION("a negative length rejects the document")
    {
      writeFileText(yamlPath,
                    std::format(R"(version: 5
export_mode: full
library:
  resources:
    - digest: {}
      length: -1
  tracks:
    - uri: song.flac
      covers:
        - type: 3
          resource: {}
  lists: []
)",
                                digestText,
                                digestText));
      requireRejected(ml, yamlPath, ImportMode::Restore);
    }

    SECTION("an unknown key inside a resource row rejects the document")
    {
      writeFileText(yamlPath,
                    std::format(R"(version: 5
export_mode: full
library:
  resources:
    - digest: {}
      length: 16
      data: AAAA
  tracks:
    - uri: song.flac
      covers:
        - type: 3
          resource: {}
  lists: []
)",
                                digestText,
                                digestText));
      requireRejected(ml, yamlPath, ImportMode::Restore);
    }

    SECTION("a full document without the table rejects")
    {
      writeFileText(yamlPath, R"(version: 5
export_mode: full
library:
  tracks:
    - uri: song.flac
  lists: []
)");
      requireRejected(ml, yamlPath, ImportMode::Restore);
    }

    SECTION("a full document with an empty table is accepted")
    {
      writeFileText(yamlPath, R"(version: 5
export_mode: full
library:
  resources: []
  tracks:
    - uri: song.flac
  lists: []
)");
      auto importer = LibraryYamlImporter{ml};
      REQUIRE(importer.importFromYamlOffline(yamlPath, ImportMode::Restore));
      CHECK(coverSnapshots(ml, "song.flac").empty());
    }

    SECTION("a table in a metadata payload rejects")
    {
      writeFileText(yamlPath,
                    std::format(R"(version: 5
export_mode: metadata
library:
  resources:
    - digest: {}
      length: 16
  tracks:
    - uri: song.flac
  lists: []
)",
                                digestText));
      requireRejected(ml, yamlPath, ImportMode::Restore);
    }

    SECTION("a cover reference in a delta payload rejects")
    {
      writeFileText(yamlPath,
                    std::format(R"(version: 5
export_mode: delta
library:
  tracks:
    - uri: song.flac
      covers:
        - type: 3
          resource: {}
  lists: []
)",
                                otherText));
      requireRejected(ml, yamlPath, ImportMode::Merge);
    }

    SECTION("a version-3 document is refused outright")
    {
      writeFileText(yamlPath, R"(version: 3
export_mode: full
library:
  tracks:
    - uri: song.flac
      covers:
        - type: 3
          data: BAUG
  lists: []
)");
      requireRejected(ml, yamlPath, ImportMode::Restore);
    }
  }

  TEST_CASE("LibraryYaml - a metadata document records no embedded cover",
            "[runtime][integration][import-export][cover-art]")
  {
    auto const temp = ao::test::TempDir{};
    auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path());

    {
      auto transaction = library::test::writeTransaction(ml);
      auto resIdRes = library::test::physicalWriter(ml.resources(), transaction).create(lmdb::test::createTestData(32));
      REQUIRE(resIdRes);

      auto builder = TrackBuilder::makeEmpty();
      builder.property().uri("song.flac");
      builder.metadata().title("Curated");
      builder.coverArt().add(PictureType::FrontCover, *resIdRes);
      REQUIRE(transaction.apply([&](LibraryWrite& write)
                                { return write.tracks().create(builder, FileManifestBuilder::makeEmpty()); }));
      REQUIRE(transaction.commit());
    }

    auto const yamlPath = std::filesystem::path{temp.path()} / "metadata.yaml";
    auto exporter = LibraryYamlExporter{ml};
    REQUIRE(exporter.exportToYaml(yamlPath, ExportMode::Metadata));
    auto const content = readFileText(yamlPath);

    CHECK(content.contains("Curated"));
    CHECK_FALSE(content.contains("covers:"));
    CHECK_FALSE(content.contains("resources:"));
    CHECK_FALSE(content.contains("data:"));
  }

  TEST_CASE("LibraryYaml - each import mode leaves the covers its contract names",
            "[runtime][integration][import-export][cover-art]")
  {
    auto const temp = ao::test::TempDir{};
    auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path());
    auto const yamlPath = std::filesystem::path{temp.path()} / "modes.yaml";
    auto const carrierPath = std::filesystem::path{temp.path()} / "song.flac";
    std::filesystem::copy_file(audio::test::requireAudioFixture("with_cover.flac"), carrierPath);

    // Three distinct identities: what the file carries, what the target track
    // already references, and what a document might name. A mode's contract is
    // only visible when they cannot be confused.
    auto const fileBytes = media::file::test::requireSoleEmbeddedPicture(carrierPath);
    auto const fileCover = CoverSnapshot{
      PictureType::Other, utility::computeSha256(fileBytes), static_cast<std::uint32_t>(fileBytes.size())};
    auto const curatedBytes = lmdb::test::createTestData(48);
    auto const curatedCover = CoverSnapshot{
      PictureType::FrontCover, utility::computeSha256(curatedBytes), static_cast<std::uint32_t>(curatedBytes.size())};
    auto const documentBytes = lmdb::test::createTestData(64);
    auto const documentCover = CoverSnapshot{
      PictureType::FrontCover, utility::computeSha256(documentBytes), static_cast<std::uint32_t>(documentBytes.size())};

    SECTION("a metadata restore leaves the file's current art, whatever the database held")
    {
      seedTrack(ml, "song.flac", curatedBytes);
      writeFileText(yamlPath, R"(version: 5
export_mode: metadata
library:
  tracks:
    - uri: song.flac
      title: Restored
  lists: []
)");
      auto importer = LibraryYamlImporter{ml};
      REQUIRE(importer.importFromYamlOffline(yamlPath, ImportMode::Restore));

      CHECK(coverSnapshots(ml, "song.flac") == std::vector{fileCover});
    }

    SECTION("a metadata restore of an unreadable file yields no cover and no properties")
    {
      writeFileText(yamlPath, R"(version: 5
export_mode: metadata
library:
  tracks:
    - uri: gone.flac
      title: Restored
  lists: []
)");
      auto importer = LibraryYamlImporter{ml};
      REQUIRE(importer.importFromYamlOffline(yamlPath, ImportMode::Restore));

      CHECK(coverSnapshots(ml, "gone.flac").empty());
      auto transaction = ml.readTransaction();
      auto const tracks = tracksByUri(ml, transaction);
      REQUIRE(tracks.contains("gone.flac"));
      CHECK(tracks.at("gone.flac").property().duration() == std::chrono::milliseconds{0});
    }

    SECTION("a metadata merge leaves the target track's covers alone")
    {
      seedTrack(ml, "song.flac", curatedBytes);
      writeFileText(yamlPath, R"(version: 5
export_mode: metadata
library:
  tracks:
    - uri: song.flac
      title: Merged
  lists: []
)");
      auto importer = LibraryYamlImporter{ml};
      REQUIRE(importer.importFromYamlOffline(yamlPath, ImportMode::Merge));

      CHECK(coverSnapshots(ml, "song.flac") == std::vector{curatedCover});
      auto transaction = ml.readTransaction();
      CHECK(tracksByUri(ml, transaction).at("song.flac").metadata().title() == "Merged");
    }

    SECTION("a delta restore leaves the file's current art, and none when the file cannot be read")
    {
      writeFileText(yamlPath, R"(version: 5
export_mode: delta
library:
  tracks:
    - uri: song.flac
      title: Restored
    - uri: gone.flac
      title: Restored
  lists: []
)");
      auto importer = LibraryYamlImporter{ml};
      REQUIRE(importer.importFromYamlOffline(yamlPath, ImportMode::Restore));

      CHECK(coverSnapshots(ml, "song.flac") == std::vector{fileCover});
      CHECK(coverSnapshots(ml, "gone.flac").empty());
    }

    SECTION("a delta merge keeps the target's covers and fills only an empty set from the file")
    {
      seedTrack(ml, "song.flac", curatedBytes);
      std::filesystem::copy_file(carrierPath, std::filesystem::path{temp.path()} / "bare.flac");
      seedTrack(ml, "bare.flac", {});
      writeFileText(yamlPath, R"(version: 5
export_mode: delta
library:
  tracks:
    - uri: song.flac
      title: Merged
    - uri: bare.flac
      title: Merged
  lists: []
)");
      auto importer = LibraryYamlImporter{ml};
      REQUIRE(importer.importFromYamlOffline(yamlPath, ImportMode::Merge));

      CHECK(coverSnapshots(ml, "song.flac") == std::vector{curatedCover});
      CHECK(coverSnapshots(ml, "bare.flac") == std::vector{fileCover});
    }

    SECTION("a listOnly import leaves every track's covers untouched")
    {
      seedTrack(ml, "song.flac", curatedBytes);
      {
        auto transaction = library::test::writeTransaction(ml);
        auto listBuilder = ListBuilder::makeEmpty().name("Favorites");
        REQUIRE(transaction.apply([&](LibraryWrite& write) { return write.lists().create(listBuilder); }));
        REQUIRE(transaction.commit());
      }

      auto exporter = LibraryYamlExporter{ml};
      REQUIRE(exporter.exportToYaml(yamlPath, ExportMode::ListOnly));

      auto const mode = GENERATE(ImportMode::Restore, ImportMode::Merge);
      auto importer = LibraryYamlImporter{ml};
      REQUIRE(importer.importFromYamlOffline(yamlPath, mode));

      CHECK(coverSnapshots(ml, "song.flac") == std::vector{curatedCover});
    }

    SECTION("a full restore takes the document's art without opening the file")
    {
      seedTrack(ml, "song.flac", curatedBytes);
      writeFileText(yamlPath,
                    std::format(R"(version: 5
export_mode: full
library:
  resources:
    - digest: {}
      length: 64
  tracks:
    - uri: song.flac
      title: Restored
      covers:
        - type: 3
          resource: {}
  lists: []
)",
                                utility::sha256Hex(documentCover.digest),
                                utility::sha256Hex(documentCover.digest)));
      auto importer = LibraryYamlImporter{ml};
      REQUIRE(importer.importFromYamlOffline(yamlPath, ImportMode::Restore));

      // The file at that URI carries a different picture, and a full document is
      // self-contained: the reference graph is the document's alone.
      CHECK(coverSnapshots(ml, "song.flac") == std::vector{documentCover});
      auto transaction = ml.readTransaction();
      CHECK(tracksByUri(ml, transaction).at("song.flac").property().duration() == std::chrono::milliseconds{0});
    }
  }
} // namespace ao::rt::test
