// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/lmdb/LmdbTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackView.h>
#include <ao/rt/library/LibraryYamlExporter.h>
#include <ao/rt/library/LibraryYamlImporter.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
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
    ListId createList(ListStore::Writer writer, std::span<std::byte const> payload)
    {
      auto result = writer.create(payload);
      REQUIRE(result);
      return result->first;
    }

    std::size_t trackCount(MusicLibrary& ml)
    {
      std::size_t count = 0;
      auto transaction = ml.readTransaction();

      for ([[maybe_unused]] auto const& item : ml.tracks().reader(transaction))
      {
        ++count;
      }

      return count;
    }

    std::size_t listCount(MusicLibrary& ml)
    {
      std::size_t count = 0;
      auto transaction = ml.readTransaction();

      for ([[maybe_unused]] auto const& item : ml.lists().reader(transaction))
      {
        ++count;
      }

      return count;
    }

    std::string trackTitleForUri(MusicLibrary& ml, std::string_view uri)
    {
      auto transaction = ml.readTransaction();

      for (auto const& [id, view] : ml.tracks().reader(transaction))
      {
        if (view.property().uri() == uri)
        {
          return std::string{view.metadata().title()};
        }
      }

      return {};
    }
  } // namespace

  TEST_CASE("LibraryYaml - round trip preserves tracks, covers, and lists", "[runtime][workflow][import-export][yaml]")
  {
    auto const temp1 = ao::test::TempDir{};
    auto ml1 = MusicLibrary{temp1.path(), temp1.path()};
    auto const smartListName = std::string{"Smart List "} + std::string(256, 'S');
    auto const smartFilter = std::string{"@duration > 60 and "} + std::string(256, 'x');
    auto const manualListName = std::string{"Manual List "} + std::string(256, 'M');
    auto const manualListDescription = std::string{"Manual Description "} + std::string(256, 'D');

    // 1. Setup initial library
    {
      auto transaction = ml1.writeTransaction();

      auto resWriter = ml1.resources().writer(transaction);
      auto resIdResult = resWriter.create(lmdb::test::createTestData(100));
      REQUIRE(resIdResult);
      auto const resId = *resIdResult;
      REQUIRE(resWriter.create(lmdb::test::createTestData(64)));
      REQUIRE(transaction.commit());
      auto const trackId = library::test::addTrack(ml1,
                                                   library::test::TrackSpec{.title = "Test Title",
                                                                            .artist = "Test Artist",
                                                                            .album = "",
                                                                            .uri = "song.flac",
                                                                            .tags = {"rock", "favorite"},
                                                                            .customMetadata = {{"mood", "happy"}},
                                                                            .coverArtId = resId,
                                                                            .year = 0,
                                                                            .discNumber = 0,
                                                                            .trackNumber = 0,
                                                                            .duration = std::chrono::minutes{3},
                                                                            .bitrate = Bitrate{},
                                                                            .sampleRate = SampleRate{96000},
                                                                            .channels = Channels{},
                                                                            .bitDepth = BitDepth{24},
                                                                            .codec = AudioCodec::Flac});
      auto listTransaction = ml1.writeTransaction();

      auto smartListBuilder = ListBuilder::makeEmpty().name(smartListName).filter(smartFilter);
      createList(ml1.lists().writer(listTransaction), smartListBuilder.serialize());

      auto manualListBuilder = ListBuilder::makeEmpty().name(manualListName).description(manualListDescription);
      manualListBuilder.tracks().add(trackId);
      createList(ml1.lists().writer(listTransaction), manualListBuilder.serialize());

      REQUIRE(listTransaction.commit());
    }

    // 2. Export to YAML
    auto const yamlPath = std::filesystem::path{temp1.path()} / "backup.yaml";
    auto exporter = LibraryYamlExporter{ml1};
    REQUIRE(exporter.exportToYaml(yamlPath, rt::ExportMode::Full));

    {
      auto ifs = std::ifstream{yamlPath};
      auto const begin = std::istreambuf_iterator{ifs};
      auto const end = decltype(begin){};
      auto const exported = std::string{begin, end};
      REQUIRE_THAT(exported, Catch::Matchers::ContainsSubstring("codec: FLAC"));
    }

    // 3. Import into a new library
    auto const temp2 = ao::test::TempDir{};
    auto ml2 = MusicLibrary{temp2.path(), temp2.path()};

    // Pre-create the track in ml2 to test overlay (since physical file song.flac doesn't exist)
    {
      library::test::addTrack(ml2, library::test::makeEmptyTrackSpec("song.flac"));
    }

    auto importer = LibraryYamlImporter{ml2};
    REQUIRE(importer.importFromYaml(yamlPath));

    // 4. Verify
    {
      auto transaction = ml2.readTransaction();
      auto reader = ml2.tracks().reader(transaction);
      auto const listReader = ml2.lists().reader(transaction);
      auto& dictionary = ml2.dictionary();

      // Check tracks
      auto tracks = std::vector<std::pair<TrackId, TrackView>>{};

      for (auto const& item : reader)
      {
        tracks.push_back(item);
      }

      REQUIRE(tracks.size() == 1);
      auto const& view = tracks[0].second;
      CHECK(view.property().uri() == "song.flac");
      CHECK(view.property().sampleRate() == 96000);
      CHECK(view.property().bitDepth() == 24);
      CHECK(view.property().codec() == AudioCodec::Flac);
      CHECK(view.metadata().title() == "Test Title");
      CHECK(dictionary.get(view.metadata().artistId()) == "Test Artist");

      // Check tags
      auto const tags = view.tags();
      auto tagNames = std::vector<std::string>{};

      for (auto tid : tags)
      {
        tagNames.emplace_back(dictionary.get(tid));
      }

      CHECK(std::ranges::contains(tagNames, std::string_view{"rock"}));
      CHECK(std::ranges::contains(tagNames, std::string_view{"favorite"}));

      // Check custom
      auto const custom = view.customMetadata();
      bool foundMood = false;

      for (auto [k, v] : custom)
      {
        if (std::string{dictionary.get(k)} == "mood" && std::string{v} == "happy")
        {
          foundMood = true;
        }
      }

      CHECK(foundMood);

      // Check lists
      std::int32_t smartCount = 0;
      std::int32_t manualCount = 0;

      for (auto const& [lid, lview] : listReader)
      {
        if (lview.isSmart())
        {
          smartCount++;
          CHECK(lview.name() == smartListName);
          CHECK(lview.filter() == smartFilter);
        }
        else
        {
          manualCount++;
          CHECK(lview.name() == manualListName);
          CHECK(lview.description() == manualListDescription);
          REQUIRE(lview.tracks().size() == 1);
          CHECK(lview.tracks()[0] == tracks[0].first);
        }
      }

      CHECK(smartCount == 1);
      CHECK(manualCount == 1);
    }
  }

  TEST_CASE("LibraryYaml - restore preserves classical metadata fields", "[runtime][workflow][import-export][yaml]")
  {
    auto const temp1 = ao::test::TempDir{};
    auto ml1 = MusicLibrary{temp1.path(), temp1.path()};

    // 1. Setup initial library with new fields
    {
      auto const trackId = library::test::addTrack(ml1,
                                                   library::test::TrackSpec{.title = "Test Title",
                                                                            .artist = "Test Artist",
                                                                            .album = "",
                                                                            .composer = "Test Composer",
                                                                            .conductor = "Test Conductor",
                                                                            .ensemble = "Test Ensemble",
                                                                            .work = "Test Work",
                                                                            .movement = "Test Movement",
                                                                            .soloist = "Test Soloist",
                                                                            .uri = "full-fields.flac",
                                                                            .year = 0,
                                                                            .discNumber = 0,
                                                                            .trackNumber = 0,
                                                                            .movementNumber = 2,
                                                                            .movementTotal = 4,
                                                                            .duration = std::chrono::minutes{4},
                                                                            .bitrate = Bitrate{},
                                                                            .sampleRate = SampleRate{},
                                                                            .channels = Channels{},
                                                                            .bitDepth = BitDepth{}});
      auto transaction = ml1.writeTransaction();
      auto manifestWriter = ml1.manifest().writer(transaction);
      auto builder = FileManifestBuilder::makeEmpty();
      builder.trackId(trackId).mtime(123456789);
      REQUIRE(manifestWriter.put("full-fields.flac", builder.serialize()));

      REQUIRE(transaction.commit());
    }

    // 2. Export to YAML (Full mode)
    auto const yamlPath = std::filesystem::path{temp1.path()} / "phase1.yaml";
    auto exporter = LibraryYamlExporter{ml1};
    REQUIRE(exporter.exportToYaml(yamlPath, rt::ExportMode::Full));

    // 3. Import into a new library (Restore mode)
    auto const temp2 = ao::test::TempDir{};
    auto ml2 = MusicLibrary{temp2.path(), temp2.path()};
    auto importer = LibraryYamlImporter{ml2};

    // Use Restore mode (default) - should not try to read physical file "full-fields.flac"
    REQUIRE(importer.importFromYaml(yamlPath, rt::ImportMode::Restore));

    // 4. Verify in ml2
    {
      auto transaction = ml2.readTransaction();

      auto reader = ml2.tracks().reader(transaction);
      auto& dictionary = ml2.dictionary();

      auto tracks = std::vector<std::pair<TrackId, TrackView>>{};

      for (auto const& item : reader)
      {
        tracks.push_back(item);
      }

      REQUIRE(tracks.size() == 1);
      auto const& view = tracks[0].second;

      CHECK(std::string{view.property().uri()} == "full-fields.flac");

      CHECK(view.property().duration() == std::chrono::minutes{4});

      CHECK(std::string{view.metadata().title()} == "Test Title");
      CHECK(std::string{dictionary.get(view.metadata().artistId())} == "Test Artist");
      CHECK(std::string{dictionary.get(view.metadata().composerId())} == "Test Composer");
      CHECK(std::string{dictionary.get(view.classical().conductorId())} == "Test Conductor");
      CHECK(std::string{dictionary.get(view.classical().ensembleId())} == "Test Ensemble");
      CHECK(std::string{dictionary.get(view.classical().workId())} == "Test Work");
      CHECK(std::string{dictionary.get(view.classical().movementId())} == "Test Movement");
      CHECK(std::string{dictionary.get(view.classical().soloistId())} == "Test Soloist");
      CHECK(view.classical().movementNumber() == 2);
      CHECK(view.classical().movementTotal() == 4);
    }
  }

  TEST_CASE("LibraryYaml - merge updates existing tracks and adds new tracks",
            "[runtime][workflow][import-export][merge]")
  {
    auto const temp = ao::test::TempDir{};
    auto ml = MusicLibrary{temp.path(), temp.path()};

    auto const* const uri1 = "track1.flac";
    auto const* const uri2 = "track2.flac";

    // 1. Setup initial library with track 1
    {
      auto const tid = library::test::addTrack(ml,
                                               library::test::TrackSpec{.title = "Original Title",
                                                                        .artist = "",
                                                                        .album = "",
                                                                        .uri = uri1,
                                                                        .year = 0,
                                                                        .discNumber = 0,
                                                                        .trackNumber = 0,
                                                                        .duration = std::chrono::milliseconds{0},
                                                                        .bitrate = Bitrate{},
                                                                        .sampleRate = SampleRate{},
                                                                        .channels = Channels{},
                                                                        .bitDepth = BitDepth{}});
      auto transaction = ml.writeTransaction();
      auto builder = FileManifestBuilder::makeEmpty();
      builder.trackId(tid);
      REQUIRE(ml.manifest().writer(transaction).put(uri1, builder.serialize()));
      REQUIRE(transaction.commit());
    }

    // 2. Prepare YAML with update for track 1 and addition of track 2
    auto const yamlPath = std::filesystem::path{temp.path()} / "merge.yaml";
    {
      auto yaml = std::ofstream{yamlPath};
      yaml << R"(version: 1
export_mode: delta
library:
  tracks:
    - uri: track1.flac
      title: "Updated Title"
    - uri: track2.flac
      title: "New Track"
  lists: []
)";
    }

    // 3. Perform Merge Import
    auto importer = LibraryYamlImporter{ml};
    REQUIRE(importer.importFromYaml(yamlPath, rt::ImportMode::Merge));

    // 4. Verify results
    {
      auto transaction = ml.readTransaction();
      auto reader = ml.tracks().reader(transaction);

      auto tracks = std::unordered_map<std::string, TrackView>{};

      for (auto const& [id, view] : reader)
      {
        tracks.emplace(view.property().uri(), view);
      }

      REQUIRE(tracks.size() == 2);

      // Track 1 should be updated
      auto const& v1 = tracks.at(uri1);
      CHECK(v1.metadata().title() == "Updated Title");

      // Track 2 should be added
      auto const& v2 = tracks.at(uri2);
      CHECK(std::string{v2.metadata().title()} == "New Track");
    }
  }

  TEST_CASE("LibraryYaml - import reports counts and dry-run leaves target unchanged",
            "[runtime][workflow][import-export][dry-run]")
  {
    SECTION("restore into empty target")
    {
      auto const sourceTemp = ao::test::TempDir{};
      auto source = MusicLibrary{sourceTemp.path(), sourceTemp.path()};
      auto const trackId =
        library::test::addTrack(source, library::test::TrackSpec{.title = "Source", .uri = "source.flac"});

      auto listTransaction = source.writeTransaction();
      auto listBuilder = ListBuilder::makeEmpty().name("Source List");
      listBuilder.tracks().add(trackId);
      createList(source.lists().writer(listTransaction), listBuilder.serialize());
      REQUIRE(listTransaction.commit());

      auto const yamlPath = std::filesystem::path{sourceTemp.path()} / "restore.yaml";
      REQUIRE(LibraryYamlExporter{source}.exportToYaml(yamlPath, ExportMode::Full));

      auto const targetTemp = ao::test::TempDir{};
      auto target = MusicLibrary{targetTemp.path(), targetTemp.path()};
      auto importer = LibraryYamlImporter{target};
      auto const report = importer.previewImportFromYaml(yamlPath, ImportMode::Restore);

      REQUIRE(report);
      CHECK(report->tracksCreated == 1);
      CHECK(report->tracksUpdated == 0);
      CHECK(report->tracksDeleted == 0);
      CHECK(report->listsCreated == 1);
      CHECK(report->listsDeleted == 0);
      CHECK(trackCount(target) == 0);
      CHECK(listCount(target) == 0);
    }

    SECTION("merge reports updates and creates without committing")
    {
      auto const temp = ao::test::TempDir{};
      auto ml = MusicLibrary{temp.path(), temp.path()};
      auto const existingTrackId =
        library::test::addTrack(ml, library::test::TrackSpec{.title = "Original", .uri = "track1.flac"});

      {
        auto transaction = ml.writeTransaction();
        auto builder = FileManifestBuilder::makeEmpty();
        builder.trackId(existingTrackId);
        REQUIRE(ml.manifest().writer(transaction).put("track1.flac", builder.serialize()));
        REQUIRE(transaction.commit());
      }

      auto const yamlPath = std::filesystem::path{temp.path()} / "merge-report.yaml";
      {
        auto yaml = std::ofstream{yamlPath};
        yaml << R"(version: 1
export_mode: delta
library:
  tracks:
    - uri: track1.flac
      title: "Updated"
    - uri: track2.flac
      title: "New"
  lists:
    - id: 1
      parentId: 0
      name: "Imported"
      tracks:
        - uri: track2.flac
)";
      }

      auto importer = LibraryYamlImporter{ml};
      auto const dryRunReport = importer.previewImportFromYaml(yamlPath, ImportMode::Merge);

      REQUIRE(dryRunReport);
      CHECK(dryRunReport->tracksCreated == 1);
      CHECK(dryRunReport->tracksUpdated == 1);
      CHECK(dryRunReport->tracksDeleted == 0);
      CHECK(dryRunReport->listsCreated == 1);
      CHECK(dryRunReport->listsDeleted == 0);
      CHECK(trackCount(ml) == 1);
      CHECK(listCount(ml) == 0);
      CHECK(trackTitleForUri(ml, "track1.flac") == "Original");

      auto const commitReport = importer.importFromYaml(yamlPath, ImportMode::Merge);
      REQUIRE(commitReport);
      CHECK(*commitReport == *dryRunReport);
      CHECK(trackCount(ml) == 2);
      CHECK(listCount(ml) == 1);
      CHECK(trackTitleForUri(ml, "track1.flac") == "Updated");
      CHECK(trackTitleForUri(ml, "track2.flac") == "New");
    }
  }

  TEST_CASE("LibraryYaml - import canonicalizes track URIs and recovers file sizes",
            "[runtime][workflow][import-export][uri]")
  {
    auto const temp = ao::test::TempDir{};
    auto ml = MusicLibrary{temp.path(), temp.path()};
    auto importer = LibraryYamlImporter{ml};
    auto const yamlPath = std::filesystem::path{temp.path()} / "canonization.yaml";

    auto const songPath = std::filesystem::path{temp.path()} / "song.flac";
    {
      auto out = std::ofstream{songPath};
      out << "dummy content"; // 13 bytes
    }

    {
      auto yaml = std::ofstream{yamlPath};
      yaml << R"(
version: 1
library:
  tracks:
    - id: 1
      uri: ./song.flac
    - id: 2
      uri: .\song2.flac
    - id: 3
      uri: nested\..\song3.flac
  lists:
    - id: 1
      name: Test List
      tracks:
        - uri: ./song.flac
        - uri: .\song2.flac
        - uri: nested\..\song3.flac
)";
    }

    REQUIRE(importer.importFromYaml(yamlPath, rt::ImportMode::Restore));

    auto transaction = ml.readTransaction();
    auto const trackReader = ml.tracks().reader(transaction);
    auto const manifestReader = ml.manifest().reader(transaction);

    std::int32_t count = 0;

    for (auto const& [id, view] : trackReader)
    {
      auto builder = TrackBuilder::fromView(view, ml.dictionary());
      CHECK(builder.property().uri().find("./") == std::string_view::npos);
      CHECK(builder.property().uri().find('\\') == std::string_view::npos);
      CHECK(builder.property().uri().find("..") == std::string_view::npos);
      count++;
    }

    CHECK(count == 3);

    auto manifestResult = manifestReader.get("song.flac");
    REQUIRE(manifestResult);
    CHECK(manifestResult->fileSize() == 13);
    CHECK(manifestResult->mtime() > 0);

    auto manifestResult2 = manifestReader.get("song2.flac");
    REQUIRE(manifestResult2);
    CHECK(manifestResult2->fileSize() == 0);

    auto manifestResult3 = manifestReader.get("song3.flac");
    REQUIRE(manifestResult3);
    CHECK(manifestResult3->fileSize() == 0);

    auto const listReader = ml.lists().reader(transaction);
    auto optList = listReader.get(ListId{1});
    REQUIRE(optList);
    REQUIRE(optList->tracks().size() == 3);
  }

  TEST_CASE("LibraryYaml - import accepts metadata and delta-mode YAML examples",
            "[runtime][workflow][import-export][yaml]")
  {
    auto const temp = ao::test::TempDir{};
    auto ml = MusicLibrary{temp.path(), temp.path()};
    auto importer = LibraryYamlImporter{ml};
    auto const yamlPath = std::filesystem::path{temp.path()} / "coverage.yaml";

    // Create symlink to valid flac
    auto const songPath = std::filesystem::path{temp.path()} / "A.flac";
    std::filesystem::copy_file(
      std::filesystem::current_path() / "test/integration/tag/test_data/basic_metadata.flac", songPath);

    {
      auto yaml = std::ofstream{yamlPath};
      yaml << "version: 1\n";
      yaml << "libraryId: \"AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE\"\n";
      yaml << "export_mode: metadata\n";
      yaml << "library:\n";
      yaml << "  tracks:\n";
      yaml << "    - uri: \"A.flac\"\n";
      yaml << "      year: 2024\n";
      yaml << "      track_number: 1\n";
      yaml << "      disc_number: 1\n";

      // Test hex decode
      std::filesystem::copy_file(songPath, std::filesystem::path{temp.path()} / "J.flac");
      yaml << "    - uri: \"J.flac\"\n";
      yaml << "  lists:\n";
      yaml << "    - id: 1\n";
      yaml << "      name: \"Coverage List\"\n";
      yaml << "      type: manual\n";
      yaml << "      tracks:\n";
      yaml << "        - id: 1\n";
      yaml << "        - 2\n";
    }

    auto result = importer.importFromYaml(yamlPath);

    if (!result)
    {
      INFO("Import failed: " << result.error().message);
    }

    REQUIRE(result);

    // Delta mode coverage
    auto const yamlPathDelta = std::filesystem::path{temp.path()} / "coverage_delta.yaml";
    {
      auto yaml = std::ofstream{yamlPathDelta};
      yaml << "version: 1\n";
      yaml << "libraryId: \"AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE\"\n";
      yaml << "export_mode: delta\n";
      yaml << "library:\n";
      yaml << "  tracks:\n";
      yaml << "    - uri: \"A.flac\"\n";
      yaml << "      year: 2024\n";
      yaml << "      track_number: 1\n";
      yaml << "      disc_number: 1\n";
      yaml << "  lists:\n";
      yaml << "    - id: 1\n";
      yaml << "      name: \"Coverage List\"\n";
      yaml << "      type: manual\n";
      yaml << "      tracks:\n";
      yaml << "        - id: 1\n";
    }

    auto resultDelta = importer.importFromYaml(yamlPathDelta, ImportMode::Merge);
    REQUIRE(resultDelta);
  }

  TEST_CASE("LibraryYaml - metadata import clears absent classical fields from file tags",
            "[runtime][regression][import-export][yaml]")
  {
    auto const temp = ao::test::TempDir{};
    auto ml = MusicLibrary{temp.path(), temp.path()};
    auto const uri = std::string{"tagged.flac"};
    auto const songPath = std::filesystem::path{temp.path()} / uri;
    std::filesystem::copy_file(std::filesystem::path{TAG_TEST_DATA_DIR} / "classical_metadata.flac", songPath);

    auto const yamlPath = std::filesystem::path{temp.path()} / "metadata.yaml";
    {
      auto yaml = std::ofstream{yamlPath};
      yaml << "version: 1\n";
      yaml << "libraryId: \"AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE\"\n";
      yaml << "export_mode: metadata\n";
      yaml << "library:\n";
      yaml << "  tracks:\n";
      yaml << "    - uri: \"tagged.flac\"\n";
      yaml << "      title: \"YAML Title\"\n";
    }

    auto importer = LibraryYamlImporter{ml};
    auto result = importer.importFromYaml(yamlPath);
    REQUIRE(result);

    auto transaction = ml.readTransaction();
    auto reader = ml.tracks().reader(transaction);
    auto const optView = reader.get(TrackId{1}, TrackStore::Reader::LoadMode::Both);
    REQUIRE(optView);
    CHECK(optView->property().uri() == uri);
    CHECK(optView->metadata().title() == "YAML Title");
    CHECK(optView->classical().conductorId() == kInvalidDictionaryId);
    CHECK(optView->classical().ensembleId() == kInvalidDictionaryId);
    CHECK(optView->classical().soloistId() == kInvalidDictionaryId);
  }
} // namespace ao::rt::test
