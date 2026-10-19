// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "runtime/library/LibraryYamlExporter.h"
#include "runtime/library/LibraryYamlImporter.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/query/Parser.h>
#include <ao/query/QueryCompilation.h>
#include <ao/rt/library/LibraryTransfer.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/yaml/RymlAdapter.h>

#include <c4/yml/tree.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  using namespace ao::library;
  namespace yaml = ao::yaml;

  namespace
  {
    ryml::Tree loadTree(std::filesystem::path const& path, std::vector<char>& buffer)
    {
      auto bufferRes = yaml::readFileResult(path);
      REQUIRE(bufferRes);
      buffer = std::move(*bufferRes);
      auto state = yaml::ErrorCallbackState{path.string()};
      auto tree = ryml::Tree{yaml::callbacks()};
      REQUIRE(yaml::parseInPlace(tree, buffer, state));
      return tree;
    }

    ListId createList(WriteTransaction& transaction, ListBuilder const& list)
    {
      auto res = transaction.apply([&list](LibraryWrite& write) { return write.lists().create(list); });
      REQUIRE(res);
      return *res;
    }

    std::optional<std::vector<std::string>> listOrderUris(library::MusicLibrary& ml, std::string_view listName)
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
        result.reserve(view.orderTrackIds().size());

        for (auto const trackId : view.orderTrackIds())
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

  TEST_CASE("LibraryYaml - list-only export restores lists by track URI", "[runtime][integration][import-export][list]")
  {
    auto const temp1 = ao::test::TempDir{};
    auto ml1 = library::test::makeTestMusicLibrary(temp1.path(), temp1.path());
    auto const sourceLibraryId = [&]
    {
      auto transaction = ml1.readTransaction();
      return ml1.metadataHeader(transaction).libraryId;
    }();

    auto trackId = kInvalidTrackId;
    auto const* const uri = "special-list-song.flac";

    // 1. Setup initial library
    {
      trackId = library::test::addTrackWithUniqueFixtureUri(ml1, library::test::makeEmptyTrackSpec(uri));
      auto transaction = library::test::writeTransaction(ml1);

      auto listBuilder = ListBuilder::makeEmpty().name("My URI List");
      listBuilder.orderTrackIds().add(trackId);
      createList(transaction, listBuilder);

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
    auto const targetLibraryId = [&]
    {
      auto transaction = ml2.readTransaction();
      return ml2.metadataHeader(transaction).libraryId;
    }();
    REQUIRE(targetLibraryId != sourceLibraryId);

    // Create a valid junk track first to ensure IDs don't match.
    auto const junkTrackId = library::test::addTrackWithUniqueFixtureUri(
      ml2, library::test::makeEmptyTrackSpec("library-export-import-junk.flac"));
    REQUIRE(junkTrackId != kInvalidTrackId);

    auto targetTrackId = kInvalidTrackId;
    {
      targetTrackId = library::test::addTrackWithUniqueFixtureUri(ml2, library::test::makeEmptyTrackSpec(uri));
    }

    REQUIRE(targetTrackId != trackId);
    auto importer = LibraryYamlImporter{ml2};
    REQUIRE(importer.importFromYamlOffline(yamlPath, rt::ImportMode::Restore));

    // 5. Verify list was restored and track remapped
    {
      auto transaction = ml2.readTransaction();
      auto const listReader = ml2.lists().reader(transaction);

      std::int32_t listCount = 0;

      for (auto const& [lid, lview] : listReader)
      {
        listCount++;
        CHECK(lview.name() == "My URI List");
        REQUIRE(lview.orderTrackIds().size() == 1);
        CHECK(lview.orderTrackIds()[0] == targetTrackId); // Remapped!
      }

      CHECK(listCount == 1);

      // Both the remapped target and the unrelated track survive list-only restore.
      auto const trackReader = ml2.tracks().reader(transaction);
      auto const optTarget = trackReader.get(targetTrackId);
      auto const optJunk = trackReader.get(junkTrackId);
      REQUIRE(optTarget);
      REQUIRE(optJunk);
      CHECK(optTarget->property().uri() == uri);
      CHECK(optJunk->property().uri() == "library-export-import-junk.flac");
      CHECK(ml2.metadataHeader(transaction).libraryId == targetLibraryId);
    }
  }

  TEST_CASE("LibraryYaml - import remaps list parents regardless of YAML order",
            "[runtime][integration][import-export][list]")
  {
    auto temp = ao::test::TempDir{};
    auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path());

    std::ignore = library::test::addTrackWithUniqueFixtureUri(ml, library::test::makeEmptyTrackSpec("song.flac"));

    auto const yamlPath = std::filesystem::path{temp.path()} / "child-first.yaml";
    {
      auto yaml = std::ofstream{yamlPath};
      yaml << R"(version: 1
export-mode: full
library:
  resources: []
  tracks:
    - id: 10
      uri: song.flac
  lists:
    - id: 2
      parent-id: 1
      name: Child
      order:
        - 10
    - id: 1
      parent-id: 0
      name: Parent
)";
    }

    auto importer = LibraryYamlImporter{ml};
    REQUIRE(importer.importFromYamlOffline(yamlPath));

    {
      auto transaction = ml.readTransaction();
      auto const listReader = ml.lists().reader(transaction);
      // Restore recreates the Track under a fresh id, so the order must name that one.
      auto const optManifest = ml.manifest().reader(transaction).get("song.flac");
      REQUIRE(optManifest);
      auto const trackId = optManifest->trackId();

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
      REQUIRE(optChild->orderTrackIds().size() == 1);
      CHECK(optChild->orderTrackIds()[0] == trackId);
    }
  }

  TEST_CASE("LibraryYaml - import drops dangling list references", "[runtime][integration][import-export][list]")
  {
    auto const temp = ao::test::TempDir{};
    auto ml = library::test::makeTestMusicLibrary(temp.path(), temp.path());
    auto importer = LibraryYamlImporter{ml};
    auto const yamlPath = std::filesystem::path{temp.path()} / "list-edges.yaml";

    {
      auto yaml = std::ofstream{yamlPath};
      yaml << R"(
version: 1
export-mode: full
library:
  resources: []
  tracks:
    - id: 10
      uri: valid.flac
  lists:
    - id: 1
      name: Parent
      order:
        - 10
        - 999
        - uri: invalid-uri.flac
    - id: 2
      parent-id: 999
      name: Dangling Parent
)";
    }

    auto const reportRes = importer.importFromYamlOffline(yamlPath);
    REQUIRE(reportRes);
    CHECK(reportRes->payloadVersion == 1);
    CHECK(reportRes->payloadMode == ExportMode::Full);
    CHECK(reportRes->targetScope == ImportTargetScope::Library);
    CHECK(reportRes->danglingReferencesIgnored == 3);

    auto transaction = ml.readTransaction();
    auto const listReader = ml.lists().reader(transaction);

    std::int32_t listCount = 0;
    bool foundParent = false;
    bool foundDanglingParent = false;
    auto const trackReader = ml.tracks().reader(transaction);

    for (auto const& [lid, view] : listReader)
    {
      listCount++;

      if (view.name() == "Parent")
      {
        CHECK_FALSE(foundParent);
        foundParent = true;
        REQUIRE(view.orderTrackIds().size() == 1);
        auto const optTrack = trackReader.get(view.orderTrackIds()[0]);
        REQUIRE(optTrack);
        CHECK(optTrack->property().uri() == "valid.flac");
        CHECK(view.parentId() == kInvalidListId);
      }
      else if (view.name() == "Dangling Parent")
      {
        CHECK_FALSE(foundDanglingParent);
        foundDanglingParent = true;
        CHECK(view.parentId() == kInvalidListId);
        CHECK(view.orderTrackIds().empty());
      }
    }

    CHECK(listCount == 2);
    CHECK(foundParent);
    CHECK(foundDanglingParent);
  }

  TEST_CASE("LibraryYaml - a List filter that does not parse or compile survives export and restore",
            "[runtime][integration][import-export][list]")
  {
    // Written raw, as the database admits any filter text. One fails to parse,
    // one parses but fails to compile, and one carries decomposed text and
    // surrounding whitespace that neither side may normalize or trim.
    constexpr auto kUnparsable = std::string_view{"($genre = \"Rock\""};
    constexpr auto kUncompilable = std::string_view{"@duration ~ \"three\""};
    constexpr auto kDecomposed = std::string_view{"  $artist ~ \"Café\" and  "};
    constexpr auto kValid = std::string_view{"$genre = \"Rock\""};
    REQUIRE_FALSE(query::parse(kUnparsable));
    REQUIRE_FALSE(query::parse(kDecomposed));
    auto const uncompilableRes = query::parse(kUncompilable);
    REQUIRE(uncompilableRes);
    REQUIRE_FALSE(query::compileQuery(*uncompilableRes));

    auto const sourceTemp = ao::test::TempDir{};
    auto source = library::test::makeTestMusicLibrary(sourceTemp.path(), sourceTemp.path());

    {
      auto transaction = library::test::writeTransaction(source);
      createList(transaction, ListBuilder::makeEmpty().name("Unparsable").filter(kUnparsable));
      createList(transaction, ListBuilder::makeEmpty().name("Uncompilable").filter(kUncompilable));
      createList(transaction, ListBuilder::makeEmpty().name("Decomposed").filter(kDecomposed));
      createList(transaction, ListBuilder::makeEmpty().name("Valid").filter(kValid));
      REQUIRE(transaction.commit());
    }

    auto const yamlPath = std::filesystem::path{sourceTemp.path()} / "invalid-filters.yaml";
    REQUIRE(LibraryYamlExporter{source}.exportToYaml(yamlPath, ExportMode::Full));

    auto const targetTemp = ao::test::TempDir{};
    auto target = library::test::makeTestMusicLibrary(targetTemp.path(), targetTemp.path());
    auto importer = LibraryYamlImporter{target};

    auto const previewRes = importer.previewImportFromYamlOffline(yamlPath);
    REQUIRE(previewRes);
    CHECK(previewRes->listsCreated == 4);
    CHECK(previewRes->invalidFiltersKept == 3);

    auto const reportRes = importer.importFromYamlOffline(yamlPath);
    REQUIRE(reportRes);
    CHECK(reportRes->listsCreated == 4);
    CHECK(reportRes->invalidFiltersKept == 3);

    auto restored = std::vector<std::tuple<std::string, std::string, ListId>>{};

    {
      auto transaction = target.readTransaction();

      for (auto const& [listId, view] : target.lists().reader(transaction))
      {
        restored.emplace_back(view.name(), view.filter(), listId);
      }
    }

    std::ranges::sort(restored);
    REQUIRE(restored.size() == 4);
    CHECK(std::get<0>(restored[0]) == "Decomposed");
    CHECK(std::get<1>(restored[0]) == kDecomposed);
    CHECK(std::get<0>(restored[1]) == "Uncompilable");
    CHECK(std::get<1>(restored[1]) == kUncompilable);
    CHECK(std::get<0>(restored[2]) == "Unparsable");
    CHECK(std::get<1>(restored[2]) == kUnparsable);
    CHECK(std::get<0>(restored[3]) == "Valid");
    CHECK(std::get<1>(restored[3]) == kValid);

    // Each counted List reports the error its own text gives, and the valid
    // one reports none, so the count matches what the Lists show.
    auto const expressionError = [](std::string_view filter)
    {
      auto expressionRes = query::parse(filter);

      if (!expressionRes)
      {
        return expressionRes.error().message;
      }

      auto planRes = query::compileQuery(*expressionRes);
      REQUIRE_FALSE(planRes);
      return planRes.error().message;
    };

    auto changes = makeStateOnlyLibraryChanges(target);
    auto cache = TrackSourceCache{target, changes};
    cache.reloadAllTracks();

    for (auto const& [name, filter, listId] : restored)
    {
      CAPTURE(name);
      auto const lease = ao::test::requireValue(cache.acquire(listId));
      auto const optError = cache.sourceError(lease);

      if (filter == kValid)
      {
        CHECK_FALSE(optError);
        continue;
      }

      REQUIRE(optError);
      CHECK(optError->message == std::format("List {} stored filter: {}", listId.raw(), expressionError(filter)));
    }
  }

  TEST_CASE("LibraryYaml - mixed list order references preserve first-occurrence order across round-trip",
            "[runtime][integration][import-export][list]")
  {
    auto const sourceTemp = ao::test::TempDir{};
    auto sourceLibrary = library::test::makeTestMusicLibrary(sourceTemp.path(), sourceTemp.path());
    auto const inputPath = std::filesystem::path{sourceTemp.path()} / "mixed-list-references.yaml";

    {
      auto yaml = std::ofstream{inputPath};
      yaml << R"(version: 1
export-mode: full
library:
  resources: []
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
      order:
        - uri: second.flac
        - 10
        - id: 20
        - uri: third.flac
        - uri: first.flac
        - 30
)";
    }

    auto sourceImporter = LibraryYamlImporter{sourceLibrary};
    REQUIRE(sourceImporter.importFromYamlOffline(inputPath));

    auto const expectedUris = std::vector<std::string>{"second.flac", "first.flac", "third.flac"};
    auto const optSourceUris = listOrderUris(sourceLibrary, "Mixed References");
    REQUIRE(optSourceUris);
    CHECK(*optSourceUris == expectedUris);

    auto const exportedPath = std::filesystem::path{sourceTemp.path()} / "mixed-list-round-trip.yaml";
    auto sourceExporter = LibraryYamlExporter{sourceLibrary};
    REQUIRE(sourceExporter.exportToYaml(exportedPath, ExportMode::Full));

    auto const targetTemp = ao::test::TempDir{};
    auto targetLibrary = library::test::makeTestMusicLibrary(targetTemp.path(), targetTemp.path());
    auto targetImporter = LibraryYamlImporter{targetLibrary};
    REQUIRE(targetImporter.importFromYamlOffline(exportedPath));

    auto const optTargetUris = listOrderUris(targetLibrary, "Mixed References");
    REQUIRE(optTargetUris);
    CHECK(*optTargetUris == expectedUris);
  }
} // namespace ao::rt::test
