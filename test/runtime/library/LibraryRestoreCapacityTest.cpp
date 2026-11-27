// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/library/LibraryYamlExporter.h"
#include "test/TestFixtureSupport.h"
#include "test/runtime/ExecutorTestSupport.h"
#include "test/runtime/RuntimeLibraryTestSupport.h"
#include <ao/Error.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibrarySchemaPolicy.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryImportPlan.h>
#include <ao/rt/library/LibraryJobs.h>
#include <ao/rt/library/LibraryTransfer.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    struct RestoreTarget final
    {
      RestoreTarget(std::filesystem::path const& directory,
                    std::uint64_t mapBytes,
                    library::LibrarySchemaPolicy schemaPolicy = library::LibrarySchemaPolicy::InitializeIfEmpty)
        : storage{ao::test::requireValue(library::MusicLibrary::open(
            directory,
            directory / "db",
            {.pinnedMapBytes = mapBytes, .flushCommits = false, .schemaPolicy = schemaPolicy}))}
      {
      }

      LibraryImportPlan preview(std::filesystem::path const& path)
      {
        return ao::test::requireValue(
          runtime.runTask(runtime.jobs().prepareLibraryImportAsync(path, ImportMode::Restore)));
      }

      Result<ImportReport> apply(LibraryImportPlan plan)
      {
        return runtime.runTask(runtime.jobs().applyLibraryImportPlanAsync(std::move(plan)));
      }

      library::MusicLibrary storage;
      QueuedExecutor executor;
      LibraryChanges changes = makeLibraryChanges(executor, storage);
      LibraryCommandsFixture runtime{storage, changes, executor};
    };

    struct RestoreState final
    {
      std::array<std::byte, 16> libraryId{};
      std::uint64_t revision = 0;
      std::string yaml;
      std::size_t dictionaryEntries = 0;
      std::size_t resources = 0;
      std::vector<std::pair<std::string, std::vector<std::byte>>> manifests;
    };

    RestoreState snapshot(library::MusicLibrary const& storage, std::filesystem::path const& exportPath)
    {
      REQUIRE(LibraryYamlExporter{storage}.exportToYaml(exportPath));
      auto const read = storage.readTransaction();
      auto state = RestoreState{.libraryId = storage.metadataHeader(read).libraryId,
                                .revision = storage.libraryRevision(read),
                                .yaml = ao::test::readFile(exportPath),
                                .dictionaryEntries = storage.dictionary().size(),
                                .resources = 0,
                                .manifests = {}};

      for (auto const& [uri, view] : storage.manifest().reader(read))
      {
        auto const bytes = view.rawData();
        state.manifests.emplace_back(uri, std::vector<std::byte>{bytes.begin(), bytes.end()});
      }

      for ([[maybe_unused]] auto const& descriptor : storage.resources().reader(read))
      {
        ++state.resources;
      }

      return state;
    }

    void checkUnchanged(std::string_view phase, RestoreState const& actual, RestoreState const& before)
    {
      CAPTURE(phase);
      CHECK(actual.libraryId == before.libraryId);
      CHECK(actual.revision == before.revision);
      CHECK(actual.yaml == before.yaml);
      CHECK(actual.dictionaryEntries == before.dictionaryEntries);
      CHECK(actual.resources == before.resources);
      CHECK(actual.manifests == before.manifests);
    }
  } // namespace

  TEST_CASE("LibraryJobs - Restore rolls back when the map fills after a successful preview")
  {
    auto const seed = ao::test::testInputPath("test/data/library/v1-scalars.yaml");
    auto const replacement = ao::test::testInputPath("test/data/library/v1.yaml");
    constexpr std::uint64_t kRoomyMapBytes = 4ULL * 1024 * 1024;
    std::uint64_t tightMapBytes = 0;
    auto expected = ImportReport{};
    auto expectedYaml = std::string{};

    {
      auto wide = ao::test::TempDir{};
      auto target = RestoreTarget{wide.path(), kRoomyMapBytes};
      REQUIRE(target.apply(target.preview(seed)));
      expected = ao::test::requireValue(target.apply(target.preview(replacement)));
      REQUIRE(expected.tracksCreated > 1);
      REQUIRE(expected.tracksDeleted > 0);
      REQUIRE(expected.listsCreated > 0);
      // At this fixture's committed extent the replay can stage the preview, but lacks
      // headroom to commit. Measuring that boundary avoids assuming a database page size.
      tightMapBytes = target.storage.storageCapacity().highWaterBytes;
      auto restored = snapshot(target.storage, wide.path() / "snapshot.yaml");
      REQUIRE(restored.resources > 0);
      expectedYaml = std::move(restored.yaml);
    }
    CAPTURE(tightMapBytes);

    auto tight = ao::test::TempDir{};
    auto const exportPath = tight.path() / "snapshot.yaml";
    auto before = RestoreState{};
    {
      auto target = RestoreTarget{tight.path(), tightMapBytes};
      REQUIRE(target.storage.storageCapacity().mapBytes == tightMapBytes);
      auto publications = std::vector<LibraryChangeSet>{};
      auto const sub =
        target.changes.onChanged([&](LibraryChangeSet const& change) { publications.push_back(change); });
      REQUIRE(target.apply(target.preview(seed)));
      REQUIRE(publications.size() == 1);
      REQUIRE(publications.front().libraryReset);
      publications.clear();
      before = snapshot(target.storage, exportPath);
      REQUIRE_FALSE(before.manifests.empty());
      REQUIRE(before.resources == 0);
      auto const generation = target.storage.dictionary().generation();
      auto plan = target.preview(replacement);
      REQUIRE(plan.report() == expected);
      checkUnchanged("after preview", snapshot(target.storage, exportPath), before);
      REQUIRE(publications.empty());

      auto const res = target.apply(std::move(plan));
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::StorageFull);
      CHECK(publications.empty());
      checkUnchanged("after failed apply", snapshot(target.storage, exportPath), before);
      CHECK(target.storage.dictionary().generation() == generation);
      auto const availability = target.runtime.library().authoringAvailability();
      CHECK(availability.state == LibraryAuthoringState::Available);
      CHECK(availability.libraryRevision == before.revision);
      CHECK(target.preview(replacement).report() == expected);
      CHECK(publications.empty());
    }

    {
      auto reopened = RestoreTarget{tight.path(), tightMapBytes, library::LibrarySchemaPolicy::RequireExisting};
      checkUnchanged("after reopen", snapshot(reopened.storage, exportPath), before);
      REQUIRE(reopened.preview(replacement).report() == expected);
    }

    auto retry = RestoreTarget{tight.path(), kRoomyMapBytes, library::LibrarySchemaPolicy::RequireExisting};
    auto plan = retry.preview(replacement);
    REQUIRE(plan.report() == expected);
    auto publications = std::vector<LibraryChangeSet>{};
    auto const sub =
      retry.changes.onChanged([&publications](LibraryChangeSet const& change) { publications.push_back(change); });
    auto const report = ao::test::requireValue(retry.apply(std::move(plan)));
    CHECK(report == expected);
    auto const restored = snapshot(retry.storage, exportPath);
    CHECK(restored.libraryId == before.libraryId);
    CHECK(restored.revision == before.revision + 1);
    CHECK(restored.yaml == expectedYaml);
    REQUIRE(publications.size() == 1);
    CHECK(publications.front().libraryReset);
    CHECK(publications.front().libraryRevision == restored.revision);
  }
} // namespace ao::rt::test
