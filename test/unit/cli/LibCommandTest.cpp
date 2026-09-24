// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CliTestSupport.h"
#include "runtime/resource/ResourceByteDiskCache.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <ao/rt/library/LibraryPaths.h>
#include <ao/utility/ByteView.h>
#include <ao/utility/FileAllocation.h>
#include <ao/utility/Sha256.h>
#include <ao/yaml/RymlAdapter.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::cli::test
{
  namespace
  {
    namespace fs = std::filesystem;
  } // namespace

  TEST_CASE("CLI - corrupt persisted manifest rejects the command before payload output", "[cli][unit][lib][integrity]")
  {
    auto fixture = CliFixture{};
    auto const databasePath = rt::LibraryPaths{fixture.root()}.databasePath();

    {
      std::ignore = library::test::makeTestMusicLibrary(fixture.root(), databasePath);
    }

    {
      auto environmentRes = lmdb::Environment::open(
        databasePath,
        {.flags = lmdb::kEnvNoTls, .maxDatabases = 8, .pinnedMapBytes = library::test::kTestMusicLibraryMapBytes});
      REQUIRE(environmentRes);
      auto environment = std::move(*environmentRes);
      auto transactionRes = lmdb::WriteTransaction::begin(environment);
      REQUIRE(transactionRes);
      auto transaction = std::move(*transactionRes);
      auto manifestRes = lmdb::ByteKeyDatabase::open(transaction, "file_manifest");
      REQUIRE(manifestRes);
      auto& manifest = *manifestRes;
      auto const malformedKey = utility::bytes::view(std::string_view{"bad"});
      auto const payload = library::FileManifestBuilder::makeEmpty().trackId(TrackId{1}).serialize();
      REQUIRE(manifest.writer(transaction).create(malformedKey, payload));
      REQUIRE(transaction.commit());
    }

    auto const result = fixture.run({"lib", "dump", "--manifest"});
    CHECK(result.status == 1);
    CHECK(result.out.empty());
    CHECK(contains(result.err, "failed to open library"));
    CHECK(contains(result.err, "File manifest key has an invalid size"));
  }

  TEST_CASE("CLI - lib stats reports known fixture counts", "[cli][integration][lib][stats]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "basic_metadata.flac");
    fixture.copyAudio("hires.flac", "hires.flac");
    fixture.copyAudio("with_cover.flac", "cover.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    result = fixture.run({"tag", "add", "fav", "1", "2"});
    REQUIRE(result.status == 0);

    result = fixture.run({"list", "create", "--name", "Pinned"});
    REQUIRE(result.status == 0);

    // The scanned cover is the only resource, and its described length is what
    // the resource listing reports for it.
    result = fixture.run({"-O", "yaml", "lib", "resource", "list"});
    REQUIRE(result.status == 0);
    auto const listing = parseYaml(result.out);
    REQUIRE(listing.rootref()["resources"].num_children() == 1);
    auto const coverBytes = std::string{yaml::scalarView(listing.rootref()["resources"][0]["size"])};

    result = fixture.run({"lib", "stats"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "tracks: 3"));
    CHECK(contains(result.out, "lists: 1"));
    CHECK(contains(result.out, "resources: 1"));
    CHECK(contains(result.out, std::format("resourceBytes: {}", coverBytes)));
    CHECK(contains(result.out, "manifest: 3"));
    CHECK(contains(result.out, "dictionary: "));
    CHECK(contains(result.out, "tags: 1"));
    CHECK(contains(result.out, "diskBytes: "));
    CHECK(contains(result.out, "highWaterBytes: "));
    CHECK(contains(result.out, std::format("mapBytes: {}", library::test::kTestMusicLibraryMapBytes)));

    result = fixture.run({"-O", "json", "lib", "stats"});
    REQUIRE(result.status == 0);
    requireJsonLineParses(result.out);
    auto tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["tracks"]) == "3");
    CHECK(yaml::scalarView(tree.rootref()["resources"]) == "1");
    CHECK(tree.rootref()["dictionary"].readable());
    CHECK(tree.rootref()["diskBytes"].readable());
    CHECK(tree.rootref()["highWaterBytes"].readable());
    CHECK(tree.rootref()["mapBytes"].readable());

    // Retagging the carrier to hold no art and rescanning drops the reference.
    // The described bytes fall with it, while the row stays: a descriptor is
    // append-only, and another library may still name the same content.
    fixture.copyAudio("basic_metadata.flac", "cover.flac");
    result = fixture.run({"scan"});
    REQUIRE(result.status == 0);

    result = fixture.run({"lib", "stats"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "resources: 1"));
    CHECK(contains(result.out, "resourceBytes: 0"));
  }

  TEST_CASE("CLI - database allocation excludes workspace and unrelated descendants", "[cli][unit][lib][stats]")
  {
    auto fixture = CliFixture{};
    REQUIRE(fixture.run({"init"}).status == 0);
    auto const before = fixture.run({"-O", "json", "lib", "stats"});
    REQUIRE(before.status == 0);
    auto const beforeTree = parseYaml(before.out);
    auto const expected = std::string{yaml::scalarView(beforeTree.rootref()["diskBytes"])};
    REQUIRE(std::stoull(expected) > 0);
    auto const databasePath = rt::LibraryPaths{fixture.root()}.databasePath();
    CHECK(std::stoull(expected) == utility::allocatedFileBytes(databasePath / "data.mdb") +
                                     utility::allocatedFileBytes(databasePath / "lock.mdb") +
                                     utility::allocatedFileBytes(databasePath / ".aobus-writer.lock"));
    std::filesystem::create_directories(databasePath / "unrelated");
    {
      auto workspace = std::ofstream{databasePath / "workspace.yaml"};
      workspace << std::string(65536, 'x');
      auto descendant = std::ofstream{databasePath / "unrelated" / "data.mdb"};
      descendant << std::string(65536, 'y');
    }
    auto const after = fixture.run({"-O", "json", "lib", "stats"});
    REQUIRE(after.status == 0);
    auto const afterTree = parseYaml(after.out);
    CHECK(yaml::scalarView(afterTree.rootref()["diskBytes"]) == expected);
  }

  TEST_CASE("CLI - lib verify reports missing files with failing exit", "[cli][unit][lib][verify]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "track.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    result = fixture.run({"lib", "verify"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "ok"));

    auto const trackPath = fixture.root() / "track.flac";
    auto const oldMtimeTime = fs::last_write_time(trackPath);
    fs::last_write_time(trackPath, oldMtimeTime + std::chrono::seconds{5});

    result = fixture.run({"lib", "verify"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "changed track.flac"));

    fs::remove(trackPath);

    result = fixture.run({"lib", "verify"});
    CHECK(result.status == 1);
    CHECK(contains(result.out, "missing track.flac"));
    CHECK(contains(result.err, "library verification failed"));
  }

  TEST_CASE("CLI - lib verify reports moved files without failing", "[cli][unit][lib][verify]")
  {
    auto fixture = CliFixture{};
    auto const originalPath = fixture.root() / "track.flac";
    auto const movedPath = fixture.root() / "renamed.flac";
    fixture.copyAudio("basic_metadata.flac", originalPath.filename().string());

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    fs::rename(originalPath, movedPath);

    result = fixture.run({"lib", "verify"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "moved renamed.flac"));
  }

  TEST_CASE("CLI - lib relink lists, previews, and applies explicit moved-file bindings",
            "[cli][integration][lib][relink]")
  {
    auto fixture = CliFixture{};
    auto const firstPath = fixture.root() / "first.flac";
    auto const secondPath = fixture.root() / "second.flac";
    auto const movedFirstPath = fixture.root() / "moved-first.flac";
    auto const movedSecondPath = fixture.root() / "moved-second.flac";
    fixture.copyAudio("basic_metadata.flac", firstPath.filename().string());
    fixture.copyAudio("basic_metadata.flac", secondPath.filename().string());

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    fs::rename(firstPath, movedFirstPath);
    fs::rename(secondPath, movedSecondPath);

    result = fixture.run({"lib", "relink"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "missing first.flac"));
    CHECK(contains(result.out, "new moved-first.flac"));
    CHECK(contains(result.out, "candidate first.flac -> moved-first.flac"));

    result = fixture.run({"lib", "relink", "--dry-run", "--from", "first.flac", "--to", "moved-first.flac"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "relinked first.flac -> moved-first.flac (dry-run)"));

    result = fixture.run({"-O", "json", "track", "show"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, R"("uri": "first.flac")"));
    CHECK_FALSE(contains(result.out, R"("uri": "moved-first.flac")"));

    result = fixture.run({"lib", "relink", "--from", "first.flac", "--to", "moved-first.flac"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "relinked first.flac -> moved-first.flac"));

    result = fixture.run({"-O", "json", "track", "show"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, R"("uri": "moved-first.flac")"));
  }

  TEST_CASE("CLI - lib relink rejects incomplete and invalid bindings", "[cli][unit][lib][relink]")
  {
    {
      auto fixture = CliFixture{};
      auto result = fixture.run({"lib", "relink", "--from", "missing.flac"});
      checkDomainFailure(result, "lib relink requires both --from and --to");
    }

    {
      auto fixture = CliFixture{};
      fixture.copyAudio("basic_metadata.flac", "track.flac");

      auto result = fixture.run({"init"});
      REQUIRE(result.status == 0);

      result = fixture.run({"lib", "relink", "--from", "track.flac", "--to", "track.flac"});
      checkDomainFailure(result, "missing manifest row is not unresolved: track.flac");
    }

    {
      auto fixture = CliFixture{};
      auto const missingPath = fixture.root() / "missing.flac";
      auto const mismatchPath = fixture.root() / "mismatch.flac";
      fixture.copyAudio("basic_metadata.flac", missingPath.filename().string());

      auto result = fixture.run({"init"});
      REQUIRE(result.status == 0);

      fs::remove(missingPath);
      fixture.copyAudio("hires.flac", mismatchPath.filename().string());

      result = fixture.run({"lib", "relink", "--from", "missing.flac", "--to", "mismatch.flac"});
      checkDomainFailure(result, "audio identity mismatch: missing.flac -> mismatch.flac");
    }
  }

  TEST_CASE("CLI - lib resource list and export read a cover by digest", "[cli][integration][lib][resource]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("with_cover.flac", "cover.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    result = fixture.run({"-O", "yaml", "lib", "resource", "list"});
    REQUIRE(result.status == 0);
    auto const listing = parseYaml(result.out);
    REQUIRE(listing.rootref()["resources"].num_children() == 1);
    auto const idText = std::string{yaml::scalarView(listing.rootref()["resources"][0]["id"])};
    auto const sizeText = std::string{yaml::scalarView(listing.rootref()["resources"][0]["size"])};

    result = fixture.run({"lib", "resource", "list"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, idText));
    CHECK(contains(result.out, sizeText));

    // The row holds no bytes, so export reads them from the file that references
    // the resource.
    auto const outputPath = fixture.root() / "cover.bin";
    result = fixture.run({"lib", "resource", "export", idText, "--output", outputPath.string()});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "exported resource:"));

    auto in = std::ifstream{outputPath, std::ios::binary};
    REQUIRE(in);
    auto const exported = std::vector<char>{std::istreambuf_iterator{in}, std::istreambuf_iterator<char>{}};
    CHECK(std::to_string(exported.size()) == sizeText);

    // Those bytes are the resource because they hash to the digest the row
    // names, which the dump prints.
    auto const digest = utility::computeSha256(std::as_bytes(std::span{exported}));
    result = fixture.run({"lib", "dump", "--resources"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, utility::sha256Hex(digest)));

    // What the verified read returned it also installed, under the cache root this
    // invocation was given. Reading it back through the cache is how the test
    // states which directory that is: an entry in the machine's own cache would
    // both survive the fixture and evict a user's covers to hold its budget.
    auto const cache = rt::ResourceByteDiskCache{rt::ResourceByteDiskCache::Config{
      .directory = rt::coverCacheDirectory(fixture.cacheDirectory()),
      .maximumEntryBytes = exported.size(),
    }};
    auto const optCached = cache.read(digest);
    REQUIRE(optCached);
    CHECK(optCached->size() == exported.size());

    checkDomainFailure(
      fixture.run({"lib", "resource", "export", "999999", "--output", (fixture.root() / "missing.bin").string()}),
      "resource not found: 999999");

    // The bytes are installed through the same replacement the library export
    // uses, so a destination directory the user did not create stays an error
    // rather than becoming a tree the command invented.
    auto const missingDirectory = fixture.root() / "missing";
    checkDomainFailure(
      fixture.run({"lib", "resource", "export", idText, "--output", (missingDirectory / "cover.bin").string()}),
      "failed to open resource output:");
    CHECK_FALSE(fs::exists(missingDirectory));
  }

  TEST_CASE("CLI - lib resource export reports absence when no source holds the content", "[cli][unit][lib][resource]")
  {
    auto fixture = CliFixture{};
    auto const orphanBytes = std::array{std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40}};
    auto const resourceId = fixture.addResource(orphanBytes);

    // The row exists and describes four bytes, but nothing references it, so no
    // file can reproduce them. The fixture's own cache root is what makes that
    // absence this test's to arrange: against the machine's cache, an entry left
    // by anything else would answer for these bytes.
    auto result = fixture.run({"lib", "resource", "list"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, std::to_string(resourceId.raw())));

    auto const outputPath = fixture.root() / "orphan.bin";
    checkDomainFailure(
      fixture.run({"lib", "resource", "export", std::to_string(resourceId.raw()), "--output", outputPath.string()}),
      std::format("resource not available: {}", resourceId.raw()));
    CHECK_FALSE(fs::exists(outputPath));
  }

  TEST_CASE("CLI - lib export and import round-trip library data", "[cli][integration][lib][import-export]")
  {
    auto source = CliFixture{};
    source.copyAudio("basic_metadata.flac", "track.flac");

    auto result = source.run({"init"});
    REQUIRE(result.status == 0);

    auto const exportPath = source.root() / "library.yaml";
    result = source.run({"lib", "export", exportPath.string()});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(fs::exists(exportPath));

    auto target = CliFixture{};
    result = target.run({"-O", "json", "lib", "import", "--dry-run", "--mode", "restore", exportPath.string()});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    auto tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["action"]) == "import");
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "true");
    CHECK(yaml::scalarView(tree.rootref()["payloadVersion"]) == "5");
    CHECK(yaml::scalarView(tree.rootref()["payloadMode"]) == "full");
    CHECK(yaml::scalarView(tree.rootref()["targetScope"]) == "library");
    CHECK(yaml::scalarView(tree.rootref()["tracksCreated"]) == "1");

    result = target.run({"lib", "import", "--dry-run", "--mode", "restore", exportPath.string()});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Payload: YAML v5, mode 'full', target scope 'library'."));
    CHECK(contains(result.out, "Changes: tracks +1/~0/-0, lists +0/-0, dangling references ignored 0."));

    result = target.run({"track", "show"});
    REQUIRE(result.status == 0);
    CHECK(result.out.empty());

    checkDomainFailure(target.run({"lib", "import", "--mode", "restore", exportPath.string()}),
                       "restore requires --confirm-destructive-restore");

    result = target.run(
      {"-O", "json", "lib", "import", "--mode", "restore", "--confirm-destructive-restore", exportPath.string()});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "false");

    result = target.run({"track", "show"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Test Title"));
  }

  TEST_CASE("CLI - lib import defaults to merge and preserves absent target tracks",
            "[cli][integration][lib][import-export]")
  {
    auto source = CliFixture{};
    source.copyAudio("basic_metadata.flac", "imported.flac");
    REQUIRE(source.run({"init"}).status == 0);

    auto const exportPath = source.root() / "library.yaml";
    REQUIRE(source.run({"lib", "export", exportPath.string()}).status == 0);

    auto target = CliFixture{};
    target.copyAudio("hires.flac", "kept.flac");
    REQUIRE(target.run({"init"}).status == 0);

    auto result = target.run({"lib", "import", exportPath.string()});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());

    result = target.run({"lib", "stats"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "tracks: 2"));
  }

  TEST_CASE("CLI - lib domain failures use stderr and exit non-zero", "[cli][unit][lib][contract]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "track.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    // Exercise transfer failures with both track and List content present.
    result = fixture.run({"list", "create", "--name", "Parent"});
    REQUIRE(result.status == 0);

    checkDomainFailure(fixture.run({"lib", "export", (fixture.root() / "export.yaml").string(), "--mode", "bad"}),
                       "invalid export mode");
    checkDomainFailure(
      fixture.run({"lib", "export", (fixture.root() / "missing" / "export.yaml").string()}), "export failed");
    checkDomainFailure(fixture.run({"lib", "import", (fixture.root() / "import.yaml").string(), "--mode", "bad"}),
                       "invalid import mode");
    checkDomainFailure(fixture.run({"lib", "import", (fixture.root() / "missing.yaml").string()}), "import failed");
  }
} // namespace ao::cli::test
