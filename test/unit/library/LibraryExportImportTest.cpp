// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_all.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

#include <ao/library/DictionaryStore.h>
#include <ao/library/Exporter.h>
#include <ao/library/Importer.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <test/unit/lmdb/TestUtils.h>

#include <algorithm>
#include <fstream>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

TEST_CASE("Library Export/Import Cycle", "[app][core][yaml]")
{
  auto temp1 = TempDir{};
  auto ml1 = ao::library::MusicLibrary{temp1.path()};
  auto const smartListName = std::string("Smart List ") + std::string(256, 'S');
  auto const smartFilter = std::string("@duration > 60 and ") + std::string(256, 'x');
  auto const manualListName = std::string("Manual List ") + std::string(256, 'M');
  auto const manualListDescription = std::string("Manual Description ") + std::string(256, 'D');

  // 1. Setup initial library
  {
    auto txn = ml1.writeTransaction();
    auto& dict = ml1.dictionary();

    auto resWriter = ml1.resources().writer(txn);
    auto resId = resWriter.create(createTestData(100));
    std::ignore = resWriter.create(createTestData(64));

    auto trackBuilder = ao::library::TrackBuilder::createNew();
    trackBuilder.property().uri("song.flac").durationMs(180000);
    trackBuilder.metadata().title("Test Title").artist("Test Artist").coverArtId(resId.value());
    trackBuilder.tags().add("rock").add("favorite");
    trackBuilder.custom().add("mood", "happy");

    auto [preparedHot, preparedCold] = trackBuilder.prepare(txn, dict, ml1.resources());
    auto trackWriter = ml1.tracks().writer(txn);
    auto [trackId, view] =
      trackWriter.createHotCold(preparedHot.size(),
                                preparedCold.size(),
                                // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
                                [&](ao::TrackId, std::span<std::byte> hot, std::span<std::byte> cold)
                                {
                                  preparedHot.writeTo(hot);
                                  preparedCold.writeTo(cold);
                                });

    auto smartListBuilder = ao::library::ListBuilder::createNew().name(smartListName).filter(smartFilter);
    ml1.lists().writer(txn).create(smartListBuilder.serialize());

    auto manualListBuilder =
      ao::library::ListBuilder::createNew().name(manualListName).description(manualListDescription);
    manualListBuilder.tracks().add(trackId);
    ml1.lists().writer(txn).create(manualListBuilder.serialize());

    txn.commit();
  }

  // 2. Export to YAML
  std::filesystem::path yamlPath = std::filesystem::path(temp1.path()) / "backup.yaml";
  ao::library::Exporter exporter(ml1);
  REQUIRE_NOTHROW(exporter.exportToYaml(yamlPath, ao::library::ExportMode::Full));

  // 3. Import into a new library
  auto temp2 = TempDir{};
  auto ml2 = ao::library::MusicLibrary{temp2.path()};

  // Pre-create the track in ml2 to test overlay (since physical file song.flac doesn't exist)
  {
    auto txn = ml2.writeTransaction();
    auto& dict = ml2.dictionary();
    auto trackBuilder = ao::library::TrackBuilder::createNew();
    trackBuilder.property().uri("song.flac"); // technical properties are missing initially
    auto [preparedHot, preparedCold] = trackBuilder.prepare(txn, dict, ml2.resources());
    ml2.tracks().writer(txn).createHotCold(preparedHot.size(),
                                           preparedCold.size(),
                                           // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
                                           [&](ao::TrackId, std::span<std::byte> hot, std::span<std::byte> cold)
                                           {
                                             preparedHot.writeTo(hot);
                                             preparedCold.writeTo(cold);
                                           });
    txn.commit();
  }

  ao::library::Importer importer(ml2);
  REQUIRE_NOTHROW(importer.importFromYaml(yamlPath));

  // 4. Verify
  {
    auto const txn = ml2.readTransaction();
    auto const reader = ml2.tracks().reader(txn);
    auto const listReader = ml2.lists().reader(txn);
    auto& dict = ml2.dictionary();

    // Check tracks
    std::vector<std::pair<ao::TrackId, ao::library::TrackView>> tracks;
    for (auto const& item : reader)
    {
      tracks.push_back(item);
    }
    REQUIRE(tracks.size() == 1);
    auto const& view = tracks[0].second;
    REQUIRE(std::string(view.property().uri()) == "song.flac");
    REQUIRE(std::string(view.metadata().title()) == "Test Title");
    REQUIRE(std::string(dict.get(view.metadata().artistId())) == "Test Artist");

    // Check tags
    auto tags = view.tags();
    std::vector<std::string> tagNames;
    for (auto tid : tags)
    {
      tagNames.push_back(std::string(dict.get(tid)));
    }
    REQUIRE(std::ranges::contains(tagNames, "rock"));
    REQUIRE(std::ranges::contains(tagNames, "favorite"));

    // Check custom
    auto custom = view.custom();
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
        REQUIRE(std::string(lview.name()) == smartListName);
        REQUIRE(std::string(lview.filter()) == smartFilter);
      }
      else
      {
        manualCount++;
        REQUIRE(std::string(lview.name()) == manualListName);
        REQUIRE(std::string(lview.description()) == manualListDescription);
        REQUIRE(lview.tracks().size() == 1);
        REQUIRE(lview.tracks()[0] == tracks[0].first);
      }
    }
    REQUIRE(smartCount == 1);
    REQUIRE(manualCount == 1);
  }
}

TEST_CASE("Library import remaps list parents regardless of YAML order", "[core][yaml]")
{
  auto temp = TempDir{};
  auto ml = ao::library::MusicLibrary{temp.path()};

  ao::TrackId trackId;
  {
    auto txn = ml.writeTransaction();
    auto& dict = ml.dictionary();
    auto trackBuilder = ao::library::TrackBuilder::createNew();
    trackBuilder.property().uri("song.flac");
    auto [preparedHot, preparedCold] = trackBuilder.prepare(txn, dict, ml.resources());
    std::tie(trackId, std::ignore) =
      ml.tracks().writer(txn).createHotCold(preparedHot.size(),
                                            preparedCold.size(),
                                            // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
                                            [&](ao::TrackId, std::span<std::byte> hot, std::span<std::byte> cold)
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

  ao::library::Importer importer(ml);
  REQUIRE_NOTHROW(importer.importFromYaml(yamlPath));

  {
    auto const txn = ml.readTransaction();
    auto const listReader = ml.lists().reader(txn);

    std::optional<ao::library::ListView> parent;
    std::optional<ao::library::ListView> child;
    ao::ListId parentId;
    ao::ListId childId;

    for (auto const& [listId, view] : listReader)
    {
      if (std::string(view.name()) == "Parent")
      {
        parentId = listId;
        parent = view;
      }
      if (std::string(view.name()) == "Child")
      {
        childId = listId;
        child = view;
      }
    }

    REQUIRE(parent);
    REQUIRE(child);
    REQUIRE(parent->parentId() == ao::ListId{0});
    REQUIRE(child->parentId() == parentId);
    REQUIRE(childId != parentId);
    REQUIRE(child->tracks().size() == 1);
    REQUIRE(child->tracks()[0] == trackId);
  }
}
