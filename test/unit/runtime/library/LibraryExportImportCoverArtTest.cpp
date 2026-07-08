// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/lmdb/TestUtils.h"
#include <ao/CoreIds.h>
#include <ao/library/CoverArt.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/library/TrackWrite.h>
#include <ao/lmdb/Transaction.h>
#include <ao/rt/library/LibraryYamlExporter.h>
#include <ao/rt/library/LibraryYamlImporter.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  using namespace ao::library;

  namespace
  {
    std::pair<TrackBuilder::PreparedHot, TrackBuilder::PreparedCold> prepareTrack(TrackBuilder& builder,
                                                                                  lmdb::WriteTransaction& txn,
                                                                                  DictionaryStore& dict,
                                                                                  ResourceStore& resources)
    {
      auto result = builder.prepare(txn, dict, resources);
      REQUIRE(result);
      return *result;
    }

    std::pair<TrackId, TrackView> createPreparedTrack(TrackStore::Writer& writer,
                                                      TrackBuilder::PreparedHot const& preparedHot,
                                                      TrackBuilder::PreparedCold const& preparedCold)
    {
      auto result = createPreparedTrackData(writer, preparedHot, preparedCold);
      REQUIRE(result);
      return *result;
    }
  } // namespace

  TEST_CASE("LibraryYaml - round trip deduplicates shared cover art resources",
            "[runtime][workflow][import-export][cover]")
  {
    auto const temp1 = ao::test::TempDir{};
    auto ml1 = MusicLibrary{temp1.path(), temp1.path()};

    auto const coverData = lmdb::test::createTestData(1024);
    auto const backCoverData = lmdb::test::createTestData(257);
    auto resId = kInvalidResourceId;
    auto backResId = kInvalidResourceId;

    // 1. Setup initial library with shared cover art
    {
      auto txn = ml1.writeTransaction();
      auto& dict = ml1.dictionary();

      auto resIdResult = ml1.resources().writer(txn).create(coverData);
      REQUIRE(resIdResult);
      resId = *resIdResult;
      auto backResIdResult = ml1.resources().writer(txn).create(backCoverData);
      REQUIRE(backResIdResult);
      backResId = *backResIdResult;

      auto trackBuilder1 = TrackBuilder::createNew();
      trackBuilder1.property().uri("song1.flac");
      trackBuilder1.metadata().title("Song 1");
      trackBuilder1.coverArt().add(PictureType::BackCover, backResId);
      trackBuilder1.coverArt().add(PictureType::FrontCover, resId);

      auto trackBuilder2 = TrackBuilder::createNew();
      trackBuilder2.property().uri("song2.flac");
      trackBuilder2.metadata().title("Song 2");
      trackBuilder2.coverArt().add(PictureType::FrontCover, resId);

      auto trackWriter = ml1.tracks().writer(txn);

      auto const [p1h, p1c] = prepareTrack(trackBuilder1, txn, dict, ml1.resources());
      createPreparedTrack(trackWriter, p1h, p1c);

      auto const [p2h, p2c] = prepareTrack(trackBuilder2, txn, dict, ml1.resources());
      createPreparedTrack(trackWriter, p2h, p2c);

      REQUIRE(txn.commit());
    }

    // 2. Export to YAML
    auto const yamlPath = std::filesystem::path{temp1.path()} / "covers.yaml";
    auto exporter = LibraryYamlExporter{ml1};
    REQUIRE(exporter.exportToYaml(yamlPath, ExportMode::Full));

    // 3. Verify YAML contains anchor and alias (textual check)
    {
      auto ifs = std::ifstream{yamlPath};
      auto const content = std::string((std::istreambuf_iterator{ifs}), std::istreambuf_iterator<char>{});
      // Should contain at least one anchor &cover_ and one alias *cover_
      CHECK(content.find("&cover_") != std::string::npos);
      CHECK(content.find("*cover_") != std::string::npos);
    }

    // 4. Import into new library
    auto const temp2 = ao::test::TempDir{};
    auto ml2 = MusicLibrary{temp2.path(), temp2.path()};
    auto importer = LibraryYamlImporter{ml2};
    REQUIRE(importer.importFromYaml(yamlPath));

    // 5. Verify deduplication and content
    {
      auto txn = ml2.readTransaction();
      auto reader = ml2.tracks().reader(txn);
      auto& resources = ml2.resources();

      auto tracks = std::unordered_map<std::string, TrackView>{};

      for (auto const& [id, view] : reader)
      {
        tracks.emplace(view.property().uri(), view);
      }

      REQUIRE(tracks.size() == 2);
      auto const& track1 = tracks.at("song1.flac");
      auto const& track2 = tracks.at("song2.flac");
      auto const optPrimary1 = track1.coverArt().primary();
      auto const optPrimary2 = track2.coverArt().primary();

      REQUIRE(optPrimary1);
      REQUIRE(optPrimary2);
      CHECK(optPrimary1->resourceId == optPrimary2->resourceId); // Deduplicated by CAS ResourceStore
      REQUIRE(track1.coverArt().count() == 2);
      CHECK(track1.coverArt().at(0).type == PictureType::BackCover);
      CHECK(track1.coverArt().at(1).type == PictureType::FrontCover);

      auto const optImportedData = resources.reader(txn).get(optPrimary1->resourceId);
      REQUIRE(optImportedData);
      CHECK(optImportedData->size() == coverData.size());
      CHECK(std::ranges::equal(*optImportedData, coverData));

      auto const optBackData = resources.reader(txn).get(track1.coverArt().at(0).resourceId);
      REQUIRE(optBackData);
      CHECK(std::ranges::equal(*optBackData, backCoverData));
    }
  }

  TEST_CASE("LibraryYaml - merge replaces and removes cover art", "[runtime][workflow][import-export][cover]")
  {
    auto const temp = ao::test::TempDir{};
    auto ml = MusicLibrary{temp.path(), temp.path()};
    auto const uri = std::string{"song.flac"};

    {
      auto txn = ml.writeTransaction();
      auto& dict = ml.dictionary();
      auto resWriter = ml.resources().writer(txn);
      auto frontIdResult = resWriter.create(lmdb::test::createTestData(8));
      REQUIRE(frontIdResult);
      auto const frontId = *frontIdResult;
      auto backIdResult = resWriter.create(lmdb::test::createTestData(9));
      REQUIRE(backIdResult);
      auto const backId = *backIdResult;

      auto builder = TrackBuilder::createNew();
      builder.property().uri(uri);
      builder.coverArt().add(PictureType::FrontCover, frontId);
      builder.coverArt().add(PictureType::BackCover, backId);
      auto const [hot, cold] = prepareTrack(builder, txn, dict, ml.resources());
      auto trackWriter = ml.tracks().writer(txn);
      auto const trackId = createPreparedTrack(trackWriter, hot, cold).first;

      auto manifest = FileManifestBuilder::createNew();
      manifest.trackId(trackId);
      REQUIRE(ml.manifest().writer(txn).put(uri, manifest.serialize()));
      REQUIRE(txn.commit());
    }

    auto const yamlPath = std::filesystem::path{temp.path()} / "covers.yaml";
    {
      auto yaml = std::ofstream{yamlPath};
      yaml << R"(version: 1
export_mode: full
library:
  tracks:
    - uri: song.flac
      covers:
        - type: 4
          data: BAUG
  lists: []
)";
    }

    auto importer = LibraryYamlImporter{ml};
    REQUIRE(importer.importFromYaml(yamlPath, ImportMode::Merge));

    {
      auto txn = ml.readTransaction();
      auto const manifestResult = ml.manifest().reader(txn).get(uri);
      REQUIRE(manifestResult);
      auto const optView = ml.tracks().reader(txn).get(manifestResult->trackId(), TrackStore::Reader::LoadMode::Both);
      REQUIRE(optView);
      REQUIRE(optView->coverArt().count() == 1);
      CHECK(optView->coverArt().at(0).type == PictureType::BackCover);

      auto const optData = ml.resources().reader(txn).get(optView->coverArt().at(0).resourceId);
      REQUIRE(optData);
      REQUIRE(optData->size() == 3);
      CHECK((*optData)[0] == std::byte{4});
      CHECK((*optData)[1] == std::byte{5});
      CHECK((*optData)[2] == std::byte{6});
    }

    {
      auto yaml = std::ofstream{yamlPath};
      yaml << R"(version: 1
export_mode: delta
library:
  tracks:
    - uri: song.flac
      covers: []
  lists: []
)";
    }

    REQUIRE(importer.importFromYaml(yamlPath, ImportMode::Merge));

    {
      auto txn = ml.readTransaction();
      auto const manifestResult = ml.manifest().reader(txn).get(uri);
      REQUIRE(manifestResult);
      auto const optView = ml.tracks().reader(txn).get(manifestResult->trackId(), TrackStore::Reader::LoadMode::Both);
      REQUIRE(optView);
      CHECK(optView->coverArt().count() == 0);
    }
  }
} // namespace ao::rt::test
