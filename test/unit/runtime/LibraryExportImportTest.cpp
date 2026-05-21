// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ao/Type.h"
#include "ao/library/DictionaryStore.h"
#include "ao/library/FileManifestStore.h"
#include "ao/library/ListBuilder.h"
#include "ao/library/ListStore.h"
#include "ao/library/ListView.h"
#include "ao/library/MusicLibrary.h"
#include "ao/library/ResourceStore.h"
#include "ao/library/TrackBuilder.h"
#include "ao/library/TrackStore.h"
#include "ao/library/TrackView.h"
#include "runtime/LibraryExporter.h"
#include "runtime/LibraryImporter.h"
#include "test/unit/lmdb/TestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ao::library::test
{
  using namespace ao::lmdb::test;

  TEST_CASE("Library Export/Import Cycle", "[app][core][yaml]")
  {
    auto const temp1 = TempDir{};
    auto ml1 = MusicLibrary{temp1.path(), temp1.path()};
    auto const smartListName = std::string("Smart List ") + std::string(256, 'S');
    auto const smartFilter = std::string("@duration > 60 and ") + std::string(256, 'x');
    auto const manualListName = std::string("Manual List ") + std::string(256, 'M');
    auto const manualListDescription = std::string("Manual Description ") + std::string(256, 'D');

    // 1. Setup initial library
    {
      auto txn = ml1.writeTransaction();
      auto& dict = ml1.dictionary();

      auto resWriter = ml1.resources().writer(txn);
      auto const resId = resWriter.create(createTestData(100));
      std::ignore = resWriter.create(createTestData(64));

      auto trackBuilder = TrackBuilder::createNew();
      trackBuilder.property().uri("song.flac").durationMs(180000);
      trackBuilder.metadata().title("Test Title").artist("Test Artist").coverArtId(resId.raw());
      trackBuilder.tags().add("rock").add("favorite");
      trackBuilder.custom().add("mood", "happy");

      auto const [preparedHot, preparedCold] = trackBuilder.prepare(txn, dict, ml1.resources());
      auto trackWriter = ml1.tracks().writer(txn);
      auto const [trackId, view] =
        trackWriter.createHotCold(preparedHot.size(),
                                  preparedCold.size(),
                                  [&](TrackId, std::span<std::byte> hot, std::span<std::byte> cold)
                                  {
                                    preparedHot.writeTo(hot);
                                    preparedCold.writeTo(cold);
                                  });

      auto smartListBuilder = ListBuilder::createNew().name(smartListName).filter(smartFilter);
      ml1.lists().writer(txn).create(smartListBuilder.serialize());

      auto manualListBuilder = ListBuilder::createNew().name(manualListName).description(manualListDescription);
      manualListBuilder.tracks().add(trackId);
      ml1.lists().writer(txn).create(manualListBuilder.serialize());

      txn.commit();
    }

    // 2. Export to YAML
    auto const yamlPath = std::filesystem::path(temp1.path()) / "backup.yaml";
    auto exporter = rt::LibraryExporter{ml1};
    REQUIRE_NOTHROW(exporter.exportToYaml(yamlPath, rt::ExportMode::Full));

    // 3. Import into a new library
    auto const temp2 = TempDir{};
    auto ml2 = MusicLibrary{temp2.path(), temp2.path()};

    // Pre-create the track in ml2 to test overlay (since physical file song.flac doesn't exist)
    {
      auto txn = ml2.writeTransaction();
      auto& dict = ml2.dictionary();
      auto trackBuilder = TrackBuilder::createNew();
      trackBuilder.property().uri("song.flac"); // technical properties are missing initially
      auto const [preparedHot, preparedCold] = trackBuilder.prepare(txn, dict, ml2.resources());
      ml2.tracks().writer(txn).createHotCold(preparedHot.size(),
                                             preparedCold.size(),
                                             [&](TrackId, std::span<std::byte> hot, std::span<std::byte> cold)
                                             {
                                               preparedHot.writeTo(hot);
                                               preparedCold.writeTo(cold);
                                             });
      txn.commit();
    }

    auto importer = rt::LibraryImporter{ml2};
    REQUIRE_NOTHROW(importer.importFromYaml(yamlPath));

    // 4. Verify
    {
      auto txn = ml2.readTransaction();
      auto reader = ml2.tracks().reader(txn);
      auto const listReader = ml2.lists().reader(txn);
      auto& dict = ml2.dictionary();

      // Check tracks
      auto tracks = std::vector<std::pair<TrackId, TrackView>>{};

      for (auto const& item : reader)
      {
        tracks.push_back(item);
      }

      REQUIRE(tracks.size() == 1);
      auto const& view = tracks[0].second;
      REQUIRE(view.property().uri() == "song.flac");
      REQUIRE(view.metadata().title() == "Test Title");
      REQUIRE(dict.get(view.metadata().artistId()) == "Test Artist");

      // Check tags
      auto const tags = view.tags();
      auto tagNames = std::vector<std::string>{};

      for (auto tid : tags)
      {
        tagNames.emplace_back(dict.get(tid));
      }

      REQUIRE(std::ranges::contains(tagNames, "rock"));
      REQUIRE(std::ranges::contains(tagNames, "favorite"));

      // Check custom
      auto const custom = view.custom();
      bool foundMood = false;

      for (auto [k, v] : custom)
      {
        if (std::string(dict.get(k)) == "mood" && std::string(v) == "happy")
        {
          foundMood = true;
        }
      }

      REQUIRE(foundMood);

      // Check lists
      int smartCount = 0;
      int manualCount = 0;

      for (auto const& [lid, lview] : listReader)
      {
        if (lview.isSmart())
        {
          smartCount++;
          REQUIRE(lview.name() == smartListName);
          REQUIRE(lview.filter() == smartFilter);
        }
        else
        {
          manualCount++;
          REQUIRE(lview.name() == manualListName);
          REQUIRE(lview.description() == manualListDescription);
          REQUIRE(lview.tracks().size() == 1);
          REQUIRE(lview.tracks()[0] == tracks[0].first);
        }
      }

      REQUIRE(smartCount == 1);
      REQUIRE(manualCount == 1);
    }
  }

  TEST_CASE("Library Export/Import Phase 1 Fields", "[app][core][yaml]")
  {
    auto const temp1 = TempDir{};
    auto ml1 = MusicLibrary{temp1.path(), temp1.path()};

    // 1. Setup initial library with new fields
    {
      auto txn = ml1.writeTransaction();
      auto& dict = ml1.dictionary();

      auto trackBuilder = TrackBuilder::createNew();
      trackBuilder.property()
        .uri("full-fields.flac")
        .durationMs(240000)
        .fileSize(1024ULL * 1024ULL * 50ULL)
        .mtime(123456789);
      trackBuilder.metadata().title("Test Title").artist("Test Artist").composer("Test Composer").work("Test Work");

      auto const [preparedHot, preparedCold] = trackBuilder.prepare(txn, dict, ml1.resources());
      auto const [trackId, view] =
        ml1.tracks().writer(txn).createHotCold(preparedHot.size(),
                                               preparedCold.size(),
                                               [&](TrackId, std::span<std::byte> hot, std::span<std::byte> cold)
                                               {
                                                 preparedHot.writeTo(hot);
                                                 preparedCold.writeTo(cold);
                                               });

      auto manifestWriter = ml1.manifest().writer(txn);
      auto entry = ManifestEntry{.trackId = trackId};
      entry.fileSize(1024ULL * 1024ULL * 50ULL);
      entry.mtime(123456789);
      manifestWriter.put("full-fields.flac", entry);

      txn.commit();
    }

    // 2. Export to YAML (Full mode)
    auto const yamlPath = std::filesystem::path(temp1.path()) / "phase1.yaml";
    auto exporter = rt::LibraryExporter{ml1};
    REQUIRE_NOTHROW(exporter.exportToYaml(yamlPath, rt::ExportMode::Full));

    // 3. Import into a new library (Restore mode)
    auto const temp2 = TempDir{};
    auto ml2 = MusicLibrary{temp2.path(), temp2.path()};
    auto importer = rt::LibraryImporter{ml2};

    // Use Restore mode (default) - should not try to read physical file "full-fields.flac"
    REQUIRE_NOTHROW(importer.importFromYaml(yamlPath, rt::ImportMode::Restore));

    // 4. Verify in ml2
    {
      auto txn = ml2.readTransaction();
      auto const manifestReader = ml2.manifest().reader(txn);
      auto reader = ml2.tracks().reader(txn);
      reader.setManifestReader(manifestReader);
      auto& dict = ml2.dictionary();

      auto tracks = std::vector<std::pair<TrackId, TrackView>>{};

      for (auto const& item : reader)
      {
        tracks.push_back(item);
      }

      REQUIRE(tracks.size() == 1);
      auto const& view = tracks[0].second;

      REQUIRE(std::string(view.property().uri()) == "full-fields.flac");
      REQUIRE(view.property().fileSize() == 1024ULL * 1024ULL * 50ULL);
      REQUIRE(view.property().mtime() == 123456789);
      REQUIRE(view.property().durationMs() == 240000);

      REQUIRE(std::string(view.metadata().title()) == "Test Title");
      REQUIRE(std::string(dict.get(view.metadata().artistId())) == "Test Artist");
      REQUIRE(std::string(dict.get(view.metadata().composerId())) == "Test Composer");
      REQUIRE(std::string(dict.get(view.metadata().workId())) == "Test Work");
    }
  }

  TEST_CASE("Library Export/Import Base64 Cover Art", "[app][core][yaml][base64]")
  {
    auto const temp1 = TempDir{};
    auto ml1 = MusicLibrary{temp1.path(), temp1.path()};

    auto const coverData = createTestData(1024);
    auto resId = kInvalidResourceId;

    // 1. Setup initial library with shared cover art
    {
      auto txn = ml1.writeTransaction();
      auto& dict = ml1.dictionary();

      resId = ml1.resources().writer(txn).create(coverData);

      auto trackBuilder1 = TrackBuilder::createNew();
      trackBuilder1.property().uri("song1.flac");
      trackBuilder1.metadata().title("Song 1").coverArtId(resId.raw());

      auto trackBuilder2 = TrackBuilder::createNew();
      trackBuilder2.property().uri("song2.flac");
      trackBuilder2.metadata().title("Song 2").coverArtId(resId.raw());

      auto trackWriter = ml1.tracks().writer(txn);

      auto const [p1h, p1c] = trackBuilder1.prepare(txn, dict, ml1.resources());
      trackWriter.createHotCold(p1h.size(),
                                p1c.size(),
                                [&](TrackId, auto h, auto c)
                                {
                                  p1h.writeTo(h);
                                  p1c.writeTo(c);
                                });

      auto const [p2h, p2c] = trackBuilder2.prepare(txn, dict, ml1.resources());
      trackWriter.createHotCold(p2h.size(),
                                p2c.size(),
                                [&](TrackId, auto h, auto c)
                                {
                                  p2h.writeTo(h);
                                  p2c.writeTo(c);
                                });

      txn.commit();
    }

    // 2. Export to YAML
    auto const yamlPath = std::filesystem::path(temp1.path()) / "covers.yaml";
    auto exporter = rt::LibraryExporter{ml1};
    exporter.exportToYaml(yamlPath, rt::ExportMode::Full);

    // 3. Verify YAML contains anchor and alias (textual check)
    {
      auto ifs = std::ifstream{yamlPath};
      auto const content = std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
      // Should contain at least one anchor &cover_ and one alias *cover_
      CHECK(content.find("&cover_") != std::string::npos);
      CHECK(content.find("*cover_") != std::string::npos);
    }

    // 4. Import into new library
    auto const temp2 = TempDir{};
    auto ml2 = MusicLibrary{temp2.path(), temp2.path()};
    auto importer = rt::LibraryImporter{ml2};
    importer.importFromYaml(yamlPath);

    // 5. Verify deduplication and content
    {
      auto txn = ml2.readTransaction();
      auto reader = ml2.tracks().reader(txn);
      auto& resources = ml2.resources();

      auto tracks = std::vector<TrackView>{};

      for (auto const& [id, view] : reader)
      {
        tracks.push_back(view);
      }

      REQUIRE(tracks.size() == 2);
      auto const rid1 = ResourceId{tracks[0].metadata().coverArtId()};
      auto const rid2 = ResourceId{tracks[1].metadata().coverArtId()};

      REQUIRE(rid1 != kInvalidResourceId);
      REQUIRE(rid1 == rid2); // Deduplicated by CAS ResourceStore

      auto const optImportedData = resources.reader(txn).get(rid1.raw());
      REQUIRE(optImportedData);
      REQUIRE(optImportedData->size() == coverData.size());
      REQUIRE(std::ranges::equal(*optImportedData, coverData));
    }
  }

  TEST_CASE("Library Export/Import List Only", "[app][core][yaml][list]")
  {
    auto const temp1 = TempDir{};
    auto ml1 = MusicLibrary{temp1.path(), temp1.path()};

    auto trackId = kInvalidTrackId;
    const auto *const uri = "special-list-song.flac";

    // 1. Setup initial library
    {
      auto txn = ml1.writeTransaction();
      auto& dict = ml1.dictionary();

      auto trackBuilder = TrackBuilder::createNew();
      trackBuilder.property().uri(uri);
      auto const [p1h, p1c] = trackBuilder.prepare(txn, dict, ml1.resources());
      std::tie(trackId, std::ignore) = ml1.tracks().writer(txn).createHotCold(p1h.size(),
                                                                              p1c.size(),
                                                                              [&](TrackId, auto h, auto c)
                                                                              {
                                                                                p1h.writeTo(h);
                                                                                p1c.writeTo(c);
                                                                              });

      auto manifestWriter = ml1.manifest().writer(txn);
      manifestWriter.put(uri, ManifestEntry{.trackId = trackId});

      auto listBuilder = ListBuilder::createNew().name("My URI List");
      listBuilder.tracks().add(trackId);
      ml1.lists().writer(txn).create(listBuilder.serialize());

      txn.commit();
    }

    // 2. Export in ListOnly mode
    auto const yamlPath = std::filesystem::path(temp1.path()) / "list-only.yaml";
    auto exporter = rt::LibraryExporter{ml1};
    exporter.exportToYaml(yamlPath, rt::ExportMode::ListOnly);

    // 3. Verify YAML content
    {
      auto root = YAML::LoadFile(yamlPath.string());
      CHECK_FALSE(root["library"]["tracks"]);
      CHECK(root["library"]["lists"]);
    }

    // 4. Import into a library that has the SAME track but DIFFERENT TrackId
    auto const temp2 = TempDir{};
    auto ml2 = MusicLibrary{temp2.path(), temp2.path()};

    auto targetTrackId = kInvalidTrackId;
    {
      auto txn = ml2.writeTransaction();
      auto& dict = ml2.dictionary();

      // Create junk track first to ensure IDs don't match
      std::ignore = ml2.tracks().writer(txn).createHotCold(0, 0, [](auto, auto, auto) {});

      auto trackBuilder = TrackBuilder::createNew();
      trackBuilder.property().uri(uri);
      auto const [p1h, p1c] = trackBuilder.prepare(txn, dict, ml2.resources());
      std::tie(targetTrackId, std::ignore) = ml2.tracks().writer(txn).createHotCold(p1h.size(),
                                                                                    p1c.size(),
                                                                                    [&](TrackId, auto h, auto c)
                                                                                    {
                                                                                      p1h.writeTo(h);
                                                                                      p1c.writeTo(c);
                                                                                    });

      auto manifestWriter = ml2.manifest().writer(txn);
      manifestWriter.put(uri, ManifestEntry{.trackId = targetTrackId});

      txn.commit();
    }

    auto importer = rt::LibraryImporter{ml2};
    REQUIRE_NOTHROW(importer.importFromYaml(yamlPath, rt::ImportMode::Restore));

    // 5. Verify list was restored and track remapped
    {
      auto txn = ml2.readTransaction();
      auto const listReader = ml2.lists().reader(txn);

      int listCount = 0;

      for (auto const& [lid, lview] : listReader)
      {
        listCount++;
        CHECK(lview.name() == "My URI List");
        REQUIRE(lview.tracks().size() == 1);
        CHECK(lview.tracks()[0] == targetTrackId); // Remapped!
      }

      CHECK(listCount == 1);

      // Verify tracks were NOT cleared
      CHECK(ml2.tracks().reader(txn).begin() != ml2.tracks().reader(txn).end());
    }
  }

  TEST_CASE("Library Import Merge Mode", "[app][core][yaml][merge]")
  {
    auto const temp = TempDir{};
    auto ml = MusicLibrary{temp.path(), temp.path()};

    const auto *const uri1 = "track1.flac";
    const auto *const uri2 = "track2.flac";

    // 1. Setup initial library with track 1
    {
      auto txn = ml.writeTransaction();
      auto& dict = ml.dictionary();
      auto trackBuilder = TrackBuilder::createNew();
      trackBuilder.property().uri(uri1);
      trackBuilder.metadata().title("Original Title").rating(1);
      auto [preparedHot, preparedCold] = trackBuilder.prepare(txn, dict, ml.resources());
      auto [tid, view] = ml.tracks().writer(txn).createHotCold(preparedHot.size(),
                                                               preparedCold.size(),
                                                               [&](auto, auto h, auto c)
                                                               {
                                                                 preparedHot.writeTo(h);
                                                                 preparedCold.writeTo(c);
                                                               });
      ml.manifest().writer(txn).put(uri1, ManifestEntry{.trackId = tid});
      txn.commit();
    }

    // 2. Prepare YAML with update for track 1 and addition of track 2
    auto const yamlPath = std::filesystem::path(temp.path()) / "merge.yaml";
    {
      auto yaml = std::ofstream{yamlPath};
      yaml << R"(version: 1
export_mode: delta
library:
  tracks:
    - uri: track1.flac
      title: "Updated Title"
      rating: 5
    - uri: track2.flac
      title: "New Track"
  lists: []
)";
    }

    // 3. Perform Merge Import
    auto importer = rt::LibraryImporter{ml};
    REQUIRE_NOTHROW(importer.importFromYaml(yamlPath, rt::ImportMode::Merge));

    // 4. Verify results
    {
      auto txn = ml.readTransaction();
      auto reader = ml.tracks().reader(txn);

      auto tracks = std::unordered_map<std::string, TrackView>{};

      for (auto const& [id, view] : reader)
      {
        tracks.emplace(view.property().uri(), view);
      }

      REQUIRE(tracks.size() == 2);

      // Track 1 should be updated
      auto const& v1 = tracks.at(uri1);
      CHECK(v1.metadata().title() == "Updated Title");
      CHECK(v1.metadata().rating() == 5);

      // Track 2 should be added
      auto const& v2 = tracks.at(uri2);
      CHECK(std::string(v2.metadata().title()) == "New Track");
    }
  }

  TEST_CASE("Library import remaps list parents regardless of YAML order", "[core][yaml]")
  {
    auto temp = TempDir{};
    auto ml = MusicLibrary{temp.path(), temp.path()};

    auto trackId = kInvalidTrackId;
    {
      auto txn = ml.writeTransaction();
      auto& dict = ml.dictionary();
      auto trackBuilder = TrackBuilder::createNew();
      trackBuilder.property().uri("song.flac");
      auto [preparedHot, preparedCold] = trackBuilder.prepare(txn, dict, ml.resources());
      std::tie(trackId, std::ignore) =
        ml.tracks().writer(txn).createHotCold(preparedHot.size(),
                                              preparedCold.size(),
                                              [&](TrackId, std::span<std::byte> hot, std::span<std::byte> cold)
                                              {
                                                preparedHot.writeTo(hot);
                                                preparedCold.writeTo(cold);
                                              });
      txn.commit();
    }

    auto const yamlPath = std::filesystem::path(temp.path()) / "child-first.yaml";
    {
      auto yaml = std::ofstream{yamlPath};
      yaml << R"(version: 1
library:
  tracks:
    - id: 10
      uri: song.flac
  lists:
    - id: 2
      parentId: 1
      name: Child
      tracks:
        - 10
    - id: 1
      parentId: 0
      name: Parent
)";
    }

    auto importer = rt::LibraryImporter{ml};
    REQUIRE_NOTHROW(importer.importFromYaml(yamlPath));

    {
      auto txn = ml.readTransaction();
      auto const listReader = ml.lists().reader(txn);

      auto optParent = std::optional<ListView>{};
      auto optChild = std::optional<ListView>{};
      auto parentId = kInvalidListId;
      auto childId = kInvalidListId;

      for (auto const& [listId, view] : listReader)
      {
        if (view.name() == "Parent")
        {
          parentId = listId;
          optParent = view;
        }

        if (view.name() == "Child")
        {
          childId = listId;
          optChild = view;
        }
      }

      REQUIRE(optParent);
      REQUIRE(optChild);
      REQUIRE(optParent->parentId() == kInvalidListId);
      REQUIRE(optChild->parentId() == parentId);
      REQUIRE(childId != parentId);
      REQUIRE(optChild->tracks().size() == 1);
      REQUIRE(optChild->tracks()[0] == trackId);
    }
  }

  TEST_CASE("Library Import Validation and Error Handling", "[app][core][yaml][error]")
  {
    auto const temp = TempDir{};
    auto ml = MusicLibrary{temp.path(), temp.path()};
    auto importer = rt::LibraryImporter{ml};
    auto const yamlPath = std::filesystem::path(temp.path()) / "bad.yaml";

    auto testError = [&](std::string_view yamlContent, std::string_view expectedErrorFragment)
    {
      {
        auto yaml = std::ofstream{yamlPath};
        yaml << yamlContent;
      }
      REQUIRE_THROWS_WITH(
        importer.importFromYaml(yamlPath), Catch::Matchers::ContainsSubstring(std::string(expectedErrorFragment)));
    };

    SECTION("Missing version")
    {
      testError(R"(
library:
  tracks: []
  lists: []
)",
                "Missing 'version'");
    }

    SECTION("Unsupported version")
    {
      testError(R"(
version: 2
library:
  tracks: []
)",
                "Unsupported YAML version");
    }

    SECTION("Missing library section")
    {
      testError(R"(
version: 1
)",
                "Missing 'library' section");
    }

    SECTION("Track missing URI")
    {
      testError(R"(
version: 1
library:
  tracks:
    - id: 1
      title: "No URI"
  lists: []
)",
                "missing required 'uri'");
    }

    SECTION("Track empty URI")
    {
      testError(R"(
version: 1
library:
  tracks:
    - id: 1
      uri: ""
  lists: []
)",
                "empty 'uri'");
    }

    SECTION("Duplicate track ID")
    {
      testError(R"(
version: 1
library:
  tracks:
    - id: 1
      uri: "song1.flac"
    - id: 1
      uri: "song2.flac"
  lists: []
)",
                "Duplicate track YAML id");
    }

    SECTION("List missing ID")
    {
      testError(R"(
version: 1
library:
  tracks: []
  lists:
    - name: "No ID"
)",
                "missing required 'id'");
    }

    SECTION("List ID 0 (Reserved)")
    {
      testError(R"(
version: 1
library:
  tracks: []
  lists:
    - id: 0
      name: "Root List"
)",
                "List id 0 is reserved");
    }

    SECTION("Duplicate list ID")
    {
      testError(R"(
version: 1
library:
  tracks: []
  lists:
    - id: 1
      name: "List 1"
    - id: 1
      name: "List 2"
)",
                "Duplicate list YAML id");
    }

    SECTION("List missing name")
    {
      testError(R"(
version: 1
library:
  tracks: []
  lists:
    - id: 1
)",
                "missing required 'name'");
    }

    SECTION("Malformed Base64 cover art is skipped gracefully")
    {
      {
        auto yaml = std::ofstream{yamlPath};
        yaml << R"(
version: 1
library:
  tracks:
    - uri: "song1.flac"
      coverArtBase64: "Not!Valid@Base#64$"
  lists: []
)";
      }
      REQUIRE_NOTHROW(importer.importFromYaml(yamlPath));
      auto txn = ml.readTransaction();
      auto const reader = ml.tracks().reader(txn);
      auto const it = reader.begin();
      REQUIRE(it != reader.end());
      auto const& [tid, view] = *it;
      // Cover art ID should be 0 because it was skipped
      CHECK(view.metadata().coverArtId() == 0);
    }
  }

  TEST_CASE("Library Delta Export Mode Edge Cases", "[app][core][yaml][delta]")
  {
    auto const temp = TempDir{};
    auto ml = MusicLibrary{temp.path(), temp.path()};

    auto trackId1 = kInvalidTrackId;
    auto trackId2 = kInvalidTrackId;
    {
      auto txn = ml.writeTransaction();
      auto& dict = ml.dictionary();

      auto trackBuilder1 = TrackBuilder::createNew();
      trackBuilder1.property().uri("no-file.flac");
      trackBuilder1.metadata().title("Should Export Fully");
      auto const [p1h, p1c] = trackBuilder1.prepare(txn, dict, ml.resources());
      std::tie(trackId1, std::ignore) = ml.tracks().writer(txn).createHotCold(p1h.size(),
                                                                              p1c.size(),
                                                                              [&](TrackId, auto h, auto c)
                                                                              {
                                                                                p1h.writeTo(h);
                                                                                p1c.writeTo(c);
                                                                              });

      auto trackBuilder2 = TrackBuilder::createNew();
      trackBuilder2.property().uri("dummy.flac");
      trackBuilder2.metadata().title("Will fallback to full export because TagFile fails");
      auto const [p2h, p2c] = trackBuilder2.prepare(txn, dict, ml.resources());
      std::tie(trackId2, std::ignore) = ml.tracks().writer(txn).createHotCold(p2h.size(),
                                                                              p2c.size(),
                                                                              [&](TrackId, auto h, auto c)
                                                                              {
                                                                                p2h.writeTo(h);
                                                                                p2c.writeTo(c);
                                                                              });

      txn.commit();
    }

    auto const yamlPath = std::filesystem::path(temp.path()) / "delta.yaml";
    auto exporter = rt::LibraryExporter{ml};
    REQUIRE_NOTHROW(exporter.exportToYaml(yamlPath, rt::ExportMode::Delta));

    {
      auto root = YAML::LoadFile(yamlPath.string());
      auto tracks = root["library"]["tracks"];
      REQUIRE(tracks.IsSequence());
      REQUIRE(tracks.size() == 2);

      CHECK(tracks[0]["title"].as<std::string>() == "Should Export Fully");
      CHECK(tracks[1]["title"].as<std::string>() == "Will fallback to full export because TagFile fails");
    }
  }

  TEST_CASE("Library List Integrity Edge Cases", "[app][core][yaml][list]")
  {
    auto const temp = TempDir{};
    auto ml = MusicLibrary{temp.path(), temp.path()};
    auto importer = rt::LibraryImporter{ml};
    auto const yamlPath = std::filesystem::path(temp.path()) / "list-edges.yaml";

    {
      auto yaml = std::ofstream{yamlPath};
      yaml << R"(
version: 1
library:
  tracks:
    - id: 10
      uri: valid.flac
  lists:
    - id: 1
      name: Parent
      tracks:
        - 10
        - 999
        - uri: invalid-uri.flac
    - id: 2
      parentId: 999
      name: Dangling Parent
)";
    }

    REQUIRE_NOTHROW(importer.importFromYaml(yamlPath));

    auto txn = ml.readTransaction();
    auto const listReader = ml.lists().reader(txn);

    int listCount = 0;

    for (auto const& [lid, view] : listReader)
    {
      listCount++;

      if (view.name() == "Parent")
      {
        REQUIRE(view.tracks().size() == 1);
        CHECK(view.parentId() == kInvalidListId);
      }
      else if (view.name() == "Dangling Parent")
      {
        CHECK(view.parentId() == kInvalidListId);
      }
    }

    CHECK(listCount == 2);
  }
} // namespace ao::library::test
