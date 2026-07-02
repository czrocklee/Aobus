// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/library/LibraryYamlExporter.h>
#include <ao/rt/library/LibraryYamlImporter.h>
#include <ao/yaml/Utils.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ao::rt::test
{
  using namespace ao::library;
  namespace yaml = ao::yaml;

  namespace
  {
    ryml::Tree loadTree(std::filesystem::path const& path, std::vector<char>& buffer)
    {
      buffer = yaml::readFile(path);
      auto tree = ryml::Tree{yaml::callbacks(path.string().c_str())};
      ryml::parse_in_place(ryml::to_substr(buffer), &tree);
      return tree;
    }

    ListId createList(ListStore::Writer writer, std::span<std::byte const> payload)
    {
      auto result = writer.create(payload);
      REQUIRE(result);
      return result->first;
    }
  } // namespace

  TEST_CASE("LibraryYaml list-only export restores lists by track URI",
            "[runtime][workflow][import-export][yaml][list]")
  {
    auto const temp1 = ao::test::TempDir{};
    auto ml1 = MusicLibrary{temp1.path(), temp1.path()};

    auto trackId = kInvalidTrackId;
    auto const* const uri = "special-list-song.flac";

    // 1. Setup initial library
    {
      trackId = library::test::addTrack(ml1, library::test::makeEmptyTrackSpec(uri));
      auto txn = ml1.writeTransaction();

      auto manifestWriter = ml1.manifest().writer(txn);
      auto builder = FileManifestBuilder::createNew();
      builder.trackId(trackId);
      REQUIRE(manifestWriter.put(uri, builder.serialize()));

      auto listBuilder = ListBuilder::createNew().name("My URI List");
      listBuilder.tracks().add(trackId);
      createList(ml1.lists().writer(txn), listBuilder.serialize());

      REQUIRE(txn.commit());
    }

    // 2. Export in ListOnly mode
    auto const yamlPath = std::filesystem::path{temp1.path()} / "list-only.yaml";
    auto exporter = LibraryYamlExporter{ml1};
    REQUIRE(exporter.exportToYaml(yamlPath, ExportMode::ListOnly));

    // 3. Verify YAML content
    {
      auto buffer = std::vector<char>{};
      auto tree = loadTree(yamlPath, buffer);
      auto root = tree.rootref();
      CHECK_FALSE(root["library"]["tracks"].readable());
      CHECK(root["library"]["lists"].readable());
    }

    // 4. Import into a library that has the SAME track but DIFFERENT TrackId
    auto const temp2 = ao::test::TempDir{};
    auto ml2 = MusicLibrary{temp2.path(), temp2.path()};

    auto targetTrackId = kInvalidTrackId;
    {
      auto txn = ml2.writeTransaction();

      // Create junk track first to ensure IDs don't match
      REQUIRE(ml2.tracks().writer(txn).createHotCold(0, 0, [](auto, auto, auto) {}));
      REQUIRE(txn.commit());
    }

    {
      targetTrackId = library::test::addTrack(ml2, library::test::makeEmptyTrackSpec(uri));
      auto txn = ml2.writeTransaction();

      auto manifestWriter = ml2.manifest().writer(txn);
      auto builder = FileManifestBuilder::createNew();
      builder.trackId(targetTrackId);
      REQUIRE(manifestWriter.put(uri, builder.serialize()));

      REQUIRE(txn.commit());
    }

    auto importer = LibraryYamlImporter{ml2};
    REQUIRE(importer.importFromYaml(yamlPath, rt::ImportMode::Restore));

    // 5. Verify list was restored and track remapped
    {
      auto txn = ml2.readTransaction();
      auto const listReader = ml2.lists().reader(txn);

      std::int32_t listCount = 0;

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

  TEST_CASE("LibraryYaml import remaps list parents regardless of YAML order",
            "[runtime][workflow][import-export][yaml][list][regression]")
  {
    auto temp = ao::test::TempDir{};
    auto ml = MusicLibrary{temp.path(), temp.path()};

    auto trackId = kInvalidTrackId;
    {
      trackId = library::test::addTrack(ml, library::test::makeEmptyTrackSpec("song.flac"));
    }

    auto const yamlPath = std::filesystem::path{temp.path()} / "child-first.yaml";
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

    auto importer = LibraryYamlImporter{ml};
    REQUIRE(importer.importFromYaml(yamlPath));

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
      CHECK(optParent->parentId() == kInvalidListId);
      CHECK(optChild->parentId() == parentId);
      CHECK(childId != parentId);
      REQUIRE(optChild->tracks().size() == 1);
      CHECK(optChild->tracks()[0] == trackId);
    }
  }

  TEST_CASE("LibraryYaml import drops dangling list references", "[runtime][workflow][import-export][yaml][list]")
  {
    auto const temp = ao::test::TempDir{};
    auto ml = MusicLibrary{temp.path(), temp.path()};
    auto importer = LibraryYamlImporter{ml};
    auto const yamlPath = std::filesystem::path{temp.path()} / "list-edges.yaml";

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

    REQUIRE(importer.importFromYaml(yamlPath));

    auto txn = ml.readTransaction();
    auto const listReader = ml.lists().reader(txn);

    std::int32_t listCount = 0;

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
} // namespace ao::rt::test
