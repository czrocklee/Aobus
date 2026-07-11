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
#include <ao/yaml/RymlAdapter.h>

#include <c4/yml/tree.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
      auto context = yaml::CallbackContext{path.string()};
      auto tree = ryml::Tree{yaml::callbacks(context)};
      yaml::parseInPlace(tree, buffer, context);
      tree.callbacks(yaml::callbacks());
      return tree;
    }

    ListId createList(ListStore::Writer writer, std::span<std::byte const> payload)
    {
      auto result = writer.create(payload);
      REQUIRE(result);
      return result->first;
    }

    std::optional<std::vector<std::string>> listTrackUris(library::MusicLibrary& ml, std::string_view listName)
    {
      auto transaction = ml.readTransaction();
      auto const listReader = ml.lists().reader(transaction);
      auto const trackReader = ml.tracks().reader(transaction);

      for (auto const& [listId, view] : listReader)
      {
        if (view.name() != listName)
        {
          continue;
        }

        auto result = std::vector<std::string>{};
        result.reserve(view.tracks().size());

        for (auto const trackId : view.tracks())
        {
          auto const optTrack = trackReader.get(trackId);

          if (!optTrack)
          {
            return std::nullopt;
          }

          result.emplace_back(optTrack->property().uri());
        }

        return result;
      }

      return std::nullopt;
    }
  } // namespace

  TEST_CASE("LibraryYaml - list-only export restores lists by track URI", "[runtime][workflow][import-export][list]")
  {
    auto const temp1 = ao::test::TempDir{};
    auto ml1 = library::test::makeTestMusicLibrary(temp1.path(), temp1.path());

    auto trackId = kInvalidTrackId;
    auto const* const uri = "special-list-song.flac";

    // 1. Setup initial library
    {
      trackId = library::test::addTrack(ml1, library::test::makeEmptyTrackSpec(uri));
      auto transaction = ml1.writeTransaction();

      auto manifestWriter = ml1.manifest().writer(transaction);
      auto builder = FileManifestBuilder::makeEmpty();
      builder.trackId(trackId);
      REQUIRE(manifestWriter.put(uri, builder.serialize()));

      auto listBuilder = ListBuilder::makeEmpty().name("My URI List");
      listBuilder.tracks().add(trackId);
      createList(ml1.lists().writer(transaction), listBuilder.serialize());

      REQUIRE(transaction.commit());
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
    auto ml2 = library::test::makeTestMusicLibrary(temp2.path(), temp2.path());

    auto targetTrackId = kInvalidTrackId;
    {
      auto transaction = ml2.writeTransaction();

      // Create junk track first to ensure IDs don't match
      REQUIRE(ml2.tracks().writer(transaction).createHotCold(0, 0, [](auto, auto, auto) {}));
      REQUIRE(transaction.commit());
    }

    {
      targetTrackId = library::test::addTrack(ml2, library::test::makeEmptyTrackSpec(uri));
      auto transaction = ml2.writeTransaction();

      auto manifestWriter = ml2.manifest().writer(transaction);
      auto builder = FileManifestBuilder::makeEmpty();
      builder.trackId(targetTrackId);
      REQUIRE(manifestWriter.put(uri, builder.serialize()));

      REQUIRE(transaction.commit());
    }

    auto importer = LibraryYamlImporter{ml2};
    REQUIRE(importer.importFromYaml(yamlPath, rt::ImportMode::Restore));

    // 5. Verify list was restored and track remapped
    {
      auto transaction = ml2.readTransaction();
      auto const listReader = ml2.lists().reader(transaction);

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
      CHECK(ml2.tracks().reader(transaction).begin() != ml2.tracks().reader(transaction).end());
    }
  }

  TEST_CASE("LibraryYaml - import remaps list parents regardless of YAML order",
            "[runtime][workflow][import-export][list]")
  {
    auto temp = ao::test::TempDir{};
    auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path());

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
      auto transaction = ml.readTransaction();
      auto const listReader = ml.lists().reader(transaction);

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

  TEST_CASE("LibraryYaml - import drops dangling list references", "[runtime][workflow][import-export][list]")
  {
    auto const temp = ao::test::TempDir{};
    auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path());
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

    auto transaction = ml.readTransaction();
    auto const listReader = ml.lists().reader(transaction);

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

  TEST_CASE("LibraryYaml - mixed manual list references preserve first-occurrence order across round-trip",
            "[runtime][workflow][import-export][list]")
  {
    auto const sourceTemp = ao::test::TempDir{};
    auto sourceLibrary = library::test::makeTestMusicLibrary(sourceTemp.path(), sourceTemp.path());
    auto const inputPath = std::filesystem::path{sourceTemp.path()} / "mixed-list-references.yaml";

    {
      auto yaml = std::ofstream{inputPath};
      yaml << R"(version: 1
library:
  tracks:
    - id: 10
      uri: first.flac
    - id: 20
      uri: second.flac
    - id: 30
      uri: third.flac
  lists:
    - id: 1
      name: Mixed References
      tracks:
        - uri: second.flac
        - 10
        - id: 20
        - uri: third.flac
        - uri: first.flac
        - 30
)";
    }

    auto sourceImporter = LibraryYamlImporter{sourceLibrary};
    REQUIRE(sourceImporter.importFromYaml(inputPath));

    auto const expectedUris = std::vector<std::string>{"second.flac", "first.flac", "third.flac"};
    auto const optSourceUris = listTrackUris(sourceLibrary, "Mixed References");
    REQUIRE(optSourceUris);
    CHECK(*optSourceUris == expectedUris);

    auto const exportedPath = std::filesystem::path{sourceTemp.path()} / "mixed-list-round-trip.yaml";
    auto sourceExporter = LibraryYamlExporter{sourceLibrary};
    REQUIRE(sourceExporter.exportToYaml(exportedPath, ExportMode::Full));

    auto const targetTemp = ao::test::TempDir{};
    auto targetLibrary = library::test::makeTestMusicLibrary(targetTemp.path(), targetTemp.path());
    auto targetImporter = LibraryYamlImporter{targetLibrary};
    REQUIRE(targetImporter.importFromYaml(exportedPath));

    auto const optTargetUris = listTrackUris(targetLibrary, "Mixed References");
    REQUIRE(optTargetUris);
    CHECK(*optTargetUris == expectedUris);
  }
} // namespace ao::rt::test
