// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CliTestSupport.h"
#include "runtime/resource/ResourceByteDiskCache.h"
#include "test/unit/FilesystemTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <ao/rt/library/LibraryDurability.h>
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

    void writeCueSheet(fs::path const& path, bool const isMalformed = false)
    {
      auto sheet = std::ofstream{path, std::ios::binary};
      sheet << (isMalformed ? "FILE ignored.wav WAVE\nTRACK invalid AUDIO\n"
                            : "FILE ignored.wav WAVE\nTRACK 03 AUDIO\nINDEX 01 00:00:00\n"
                              "TRACK 09 AUDIO\nINDEX 01 00:00:01\n");
      REQUIRE(sheet.good());
    }
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

  TEST_CASE("CLI - lib dump --manifest prints a scanned modification time as exact seconds and nanoseconds",
            "[cli][integration][lib][manifest]")
  {
    auto fixture = CliFixture{};
    auto const trackPath = fixture.root() / "track.flac";
    fixture.copyAudio("basic_metadata.flac", trackPath.filename().string());

    // The stamped instant is 2024-07-01T12:00:59.5Z: the scan stores it as the
    // exact Unix instant, and the plain dump maps both halves verbatim.
    auto const modificationTime =
      std::chrono::sys_days{std::chrono::year{2024} / std::chrono::July / std::chrono::day{1}} +
      std::chrono::hours{12} + std::chrono::seconds{59} + std::chrono::milliseconds{500};
    auto const stampedTime = ao::test::fileTimeFromSystemTime(modificationTime);
    fs::last_write_time(trackPath, stampedTime);
    REQUIRE(fs::last_write_time(trackPath) == stampedTime);

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    result = fixture.run({"lib", "dump", "--manifest"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "URI: track.flac"));
    CHECK(contains(result.out, "MTime: seconds=1719835259 nanoseconds=500000000"));
  }

  TEST_CASE("CLI - lib dump --manifest reports an imported mtime verbatim and unknown for a stored null",
            "[cli][integration][lib][manifest]")
  {
    auto fixture = CliFixture{};
    // Both files exist, so a filesystem stat cannot explain either row: the
    // document's explicit `mtime` is the only source the dump reflects.
    fixture.copyAudio("basic_metadata.flac", "mapped.flac");
    fixture.copyAudio("basic_metadata.flac", "song.flac");

    auto const documentPath = fixture.root() / "mtime-overlay.yaml";
    {
      auto document = std::ofstream{documentPath};
      document << R"yaml(version: 1
export-mode: full
library:
  resources: []
  tracks:
    - uri: mapped.flac
      title: Mapped Mtime
      mtime:
        seconds: 1719835259
        nanoseconds: 500000000
    - uri: song.flac
      title: Unknown Mtime
      mtime: null
  lists: []
)yaml";
    }

    auto result = fixture.run({"lib", "import", documentPath.string()});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());

    result = fixture.run({"lib", "dump", "--manifest"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "URI: mapped.flac"));
    CHECK(contains(result.out, "MTime: seconds=1719835259 nanoseconds=500000000"));
    CHECK(contains(result.out, "URI: song.flac"));
    CHECK(contains(result.out, "MTime: unknown"));
  }

  TEST_CASE("CLI - lib dump --manifest machine output nests a known mtime and omits an absent one",
            "[cli][integration][lib][manifest]")
  {
    auto fixture = CliFixture{};
    // Both files exist, so a filesystem stat cannot explain either row: the
    // document's explicit `mtime` values are the only source the dump reflects.
    fixture.copyAudio("basic_metadata.flac", "mapped.flac");
    fixture.copyAudio("basic_metadata.flac", "song.flac");

    auto const documentPath = fixture.root() / "mtime-overlay.yaml";
    {
      auto document = std::ofstream{documentPath};
      document << R"yaml(version: 1
export-mode: full
library:
  resources: []
  tracks:
    - uri: mapped.flac
      title: Mapped Mtime
      mtime:
        seconds: 1719835259
        nanoseconds: 500000000
    - uri: song.flac
      title: Unknown Mtime
      mtime: null
  lists: []
)yaml";
    }

    auto result = fixture.run({"lib", "import", documentPath.string()});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());

    auto const assertManifestRecords = [](ryml::ConstNodeRef const& manifest)
    {
      REQUIRE(manifest.is_seq());
      REQUIRE(manifest.num_children() == 2);

      // The manifest reader iterates in URI byte order, so the records appear
      // in the order the import document named them.
      auto const& mapped = manifest[0];
      REQUIRE(yaml::scalarView(mapped["uri"]) == "mapped.flac");
      CHECK(yaml::scalarView(mapped["mtime"]["seconds"]) == "1719835259");
      CHECK(yaml::scalarView(mapped["mtime"]["nanoseconds"]) == "500000000");
      // The persisted key is `mtime`, not the DTO member name.
      CHECK_FALSE(mapped.has_child("optMtime"));

      // An absent modification time omits the key entirely rather than
      // serializing a null.
      auto const& unknown = manifest[1];
      REQUIRE(yaml::scalarView(unknown["uri"]) == "song.flac");
      CHECK_FALSE(unknown.has_child("mtime"));
    };

    result = fixture.run({"-O", "yaml", "lib", "dump", "--manifest"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    auto tree = parseYaml(result.out);
    assertManifestRecords(tree.rootref()["manifest"]);

    result = fixture.run({"-O", "json", "lib", "dump", "--manifest"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    requireJsonLineParses(result.out);
    auto jsonTree = parseYaml(result.out);
    assertManifestRecords(jsonTree.rootref()["manifest"]);
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
    auto fixture = CliFixture{rt::LibraryDurability::Durable};
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

  TEST_CASE("CLI - lib verify lists accompanying CUE warnings without failing or replacing classifications",
            "[cli][unit][lib][verify]")
  {
    auto fixture = CliFixture{};
    auto const path = fixture.root() / "track.flac";
    fixture.copyAudio("basic_metadata.flac", "track.flac");
    REQUIRE(fixture.run({"init"}).status == 0);
    auto const healthy = fixture.run({"lib", "verify"});
    REQUIRE(healthy.status == 0);
    CHECK(healthy.out == "ok\n");
    writeCueSheet(fixture.root() / "track.cue");

    auto const unchanged = fixture.run({"lib", "verify"});
    CHECK(unchanged.status == 0);
    CHECK(contains(unchanged.out, "unchanged track.flac Imported as a whole file"));
    CHECK(unchanged.err.empty());
    auto const structured = fixture.run({"-O", "json", "lib", "verify"});
    CHECK(structured.status == 0);
    auto tree = parseYaml(structured.out);
    CHECK(yaml::scalarView(tree.rootref()["ok"]) == "true");
    auto const issues = tree.rootref()["issues"];
    REQUIRE(issues.num_children() == 1);
    CHECK(yaml::scalarView(issues[0]["type"]) == "unchanged");
    CHECK(yaml::scalarView(issues[0]["uri"]) == "track.flac");
    CHECK(contains(yaml::scalarView(issues[0]["message"]), "Imported as a whole file"));

    fs::last_write_time(path, fs::last_write_time(path) + std::chrono::seconds{5});
    auto const changed = fixture.run({"lib", "verify"});
    CHECK(changed.status == 0);
    CHECK(contains(changed.out, "changed track.flac Imported as a whole file"));

    fs::rename(path, fixture.root() / "renamed.flac");
    fs::rename(fixture.root() / "track.cue", fixture.root() / "renamed.cue");
    auto const moved = fixture.run({"lib", "verify"});
    CHECK(moved.status == 0);
    CHECK(contains(moved.out, "moved renamed.flac Imported as a whole file"));
  }

  TEST_CASE("CLI - lib verify retains unchanged carrier diagnostics when its sheet is missing",
            "[cli][unit][lib][verify]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "image.flac");
    writeCueSheet(fixture.root() / "image.cue");
    REQUIRE(fixture.run({"init"}).status == 0);
    auto const healthy = fixture.run({"lib", "verify"});
    REQUIRE(healthy.status == 0);
    CHECK(healthy.out == "ok\n");
    fs::remove(fixture.root() / "image.cue");

    auto const report = fixture.run({"-O", "yaml", "lib", "verify"});
    CHECK(report.status == 0);
    CHECK(report.err.empty());
    auto tree = parseYaml(report.out);
    CHECK(yaml::scalarView(tree.rootref()["ok"]) == "true");
    auto const issues = tree.rootref()["issues"];
    REQUIRE(issues.num_children() == 1);
    CHECK(yaml::scalarView(issues[0]["type"]) == "unchanged");
    CHECK(yaml::scalarView(issues[0]["uri"]) == "image.flac");
    CHECK(contains(yaml::scalarView(issues[0]["message"]), "No sheet divides this file"));
    auto const shown = fixture.run({"track", "show"});
    REQUIRE(shown.status == 0);
    CHECK(contains(shown.out, "CUE Track Number: 3"));
    CHECK(contains(shown.out, "CUE Track Number: 9"));
  }

  TEST_CASE("CLI - lib relink exposes fingerprinted malformed-sheet destinations and preserves explicit identity",
            "[cli][integration][lib][relink]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "first.flac");
    fixture.copyAudio("basic_metadata.flac", "second.flac");
    REQUIRE(fixture.run({"init"}).status == 0);
    auto const original = fixture.run({"-O", "yaml", "track", "show"});
    REQUIRE(original.status == 0);
    auto originalTree = parseYaml(original.out);
    auto originalId = std::string{};

    for (auto const track : originalTree.rootref()["tracks"].children())
    {
      if (yaml::scalarView(track["uri"]) == "first.flac")
      {
        originalId = yaml::scalarView(track["id"]);
        break;
      }
    }

    REQUIRE_FALSE(originalId.empty());
    fs::rename(fixture.root() / "first.flac", fixture.root() / "moved-first.flac");
    fs::rename(fixture.root() / "second.flac", fixture.root() / "moved-second.flac");
    writeCueSheet(fixture.root() / "moved-first.cue", true);
    writeCueSheet(fixture.root() / "orphan.cue");
    writeCueSheet(fixture.root() / "broken.cue");

    {
      auto broken = std::ofstream{fixture.root() / "broken.flac", std::ios::binary};
      broken << "not an audio file";
      REQUIRE(broken.good());
    }

    auto const listed = fixture.run({"lib", "relink"});
    REQUIRE(listed.status == 0);
    CHECK(contains(listed.out, "new moved-first.flac"));
    CHECK(contains(listed.out, "candidate first.flac -> moved-first.flac"));
    CHECK_FALSE(contains(listed.out, "new orphan.cue"));
    CHECK_FALSE(contains(listed.out, "new broken.flac"));
    auto const structured = fixture.run({"-O", "json", "lib", "relink"});
    REQUIRE(structured.status == 0);
    auto listingTree = parseYaml(structured.out);
    CHECK(listingTree.rootref()["newFiles"].num_children() == 2);
    CHECK(listingTree.rootref()["candidates"].num_children() == 4);
    checkDomainFailure(fixture.run({"lib", "relink", "--from", "first.flac", "--to", "broken.flac"}),
                       "new file is not unresolved: broken.flac");
    checkDomainFailure(fixture.run({"lib", "relink", "--from", "first.flac", "--to", "orphan.cue"}),
                       "new file is not unresolved: orphan.cue");

    auto const preview =
      fixture.run({"lib", "relink", "--dry-run", "--from", "first.flac", "--to", "moved-first.flac"});
    REQUIRE(preview.status == 0);
    CHECK(contains(preview.out, "relinked first.flac -> moved-first.flac (dry-run)"));
    auto const beforeApply = fixture.run({"-O", "yaml", "track", "show", originalId});
    REQUIRE(beforeApply.status == 0);
    auto beforeTree = parseYaml(beforeApply.out);
    REQUIRE(beforeTree.rootref()["tracks"].num_children() == 1);
    CHECK(yaml::scalarView(beforeTree.rootref()["tracks"][0]["id"]) == originalId);
    CHECK(yaml::scalarView(beforeTree.rootref()["tracks"][0]["uri"]) == "first.flac");

    auto const applied = fixture.run({"lib", "relink", "--from", "first.flac", "--to", "moved-first.flac"});
    // The move commits, and the malformed sheet beside the whole file is a warning, not a failure.
    CHECK(applied.status == 0);
    CHECK(contains(applied.err, "warning: moved-first.flac: "));
    CHECK_FALSE(contains(applied.err, "relink failed"));
    auto const afterApply = fixture.run({"-O", "yaml", "track", "show", originalId});
    REQUIRE(afterApply.status == 0);
    auto afterTree = parseYaml(afterApply.out);
    REQUIRE(afterTree.rootref()["tracks"].num_children() == 1);
    CHECK(yaml::scalarView(afterTree.rootref()["tracks"][0]["id"]) == originalId);
    CHECK(yaml::scalarView(afterTree.rootref()["tracks"][0]["uri"]) == "moved-first.flac");
  }

  TEST_CASE("CLI - lib relink applies an explicit two-member carrier when the sheet matches",
            "[cli][integration][lib][relink]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "first.flac");
    fixture.copyAudio("basic_metadata.flac", "second.flac");
    writeCueSheet(fixture.root() / "first.cue");
    REQUIRE(fixture.run({"init"}).status == 0);

    auto const original = fixture.run({"-O", "yaml", "track", "show"});
    REQUIRE(original.status == 0);
    auto originalTree = parseYaml(original.out);
    auto memberIds = std::vector<std::string>{};

    for (auto const track : originalTree.rootref()["tracks"].children())
    {
      if (yaml::scalarView(track["uri"]) == "first.flac")
      {
        memberIds.emplace_back(yaml::scalarView(track["id"]));
      }
    }

    REQUIRE(memberIds.size() == 2);
    REQUIRE(memberIds[0] != memberIds[1]);

    // Two missing copies of the same audio leave the move ambiguous, so only
    // the explicit source may be selected. The destination sheet is the stored one.
    fs::remove(fixture.root() / "second.flac");
    fs::rename(fixture.root() / "first.flac", fixture.root() / "moved-first.flac");
    fs::rename(fixture.root() / "first.cue", fixture.root() / "moved-first.cue");

    auto const applied = fixture.run({"lib", "relink", "--from", "first.flac", "--to", "moved-first.flac"});
    REQUIRE(applied.status == 0);
    CHECK(applied.err.empty());
    CHECK(contains(applied.out, "relinked first.flac -> moved-first.flac"));

    auto const after = fixture.run({"-O", "yaml", "track", "show"});
    REQUIRE(after.status == 0);
    auto afterTree = parseYaml(after.out);
    auto preserved = std::vector<std::string>{};

    for (auto const track : afterTree.rootref()["tracks"].children())
    {
      auto const id = std::string{yaml::scalarView(track["id"])};

      if (id != memberIds[0] && id != memberIds[1])
      {
        continue;
      }

      preserved.push_back(id);
      CHECK(yaml::scalarView(track["uri"]) == "moved-first.flac");
    }

    REQUIRE(preserved.size() == 2);
    CHECK(preserved[0] != preserved[1]);
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
    result = fixture.run({"lib", "resource", "export", idText, "--output-file", outputPath.string()});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "exported resource:"));

    auto in = std::ifstream{outputPath, std::ios::binary};
    REQUIRE(in);
    auto const exported = std::vector<char>{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    CHECK(std::to_string(exported.size()) == sizeText);

    // Those bytes are the resource because they hash to the digest the row
    // names, which the dump prints.
    auto const digest = utility::computeSha256(std::as_bytes(std::span<char const>{exported}));
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
      fixture.run({"lib", "resource", "export", "999999", "--output-file", (fixture.root() / "missing.bin").string()}),
      "resource not found: 999999");

    // The bytes are installed through the same replacement the library export
    // uses, so a destination directory the user did not create stays an error
    // rather than becoming a tree the command invented.
    auto const missingDirectory = fixture.root() / "missing";
    checkDomainFailure(
      fixture.run({"lib", "resource", "export", idText, "--output-file", (missingDirectory / "cover.bin").string()}),
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
      fixture.run(
        {"lib", "resource", "export", std::to_string(resourceId.raw()), "--output-file", outputPath.string()}),
      std::format("resource not available: {}", resourceId.raw()));
    CHECK_FALSE(fs::exists(outputPath));
  }

  TEST_CASE("CLI - lib export reserves --output for the global format option", "[cli][integration][lib][import-export]")
  {
    auto source = CliFixture{};
    source.copyAudio("basic_metadata.flac", "track.flac");
    REQUIRE(source.run({"init"}).status == 0);

    // --output-file names the destination; the short form keeps that meaning.
    auto const exportPath = source.root() / "library.yaml";
    auto result = source.run({"lib", "export", "--output-file", exportPath.string()});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(fs::exists(exportPath));

    auto const shortPath = source.root() / "short.yaml";
    result = source.run({"lib", "export", "-o", shortPath.string()});
    REQUIRE(result.status == 0);
    CHECK(fs::exists(shortPath));

    // --output reaches the global format option and never names a file.
    result = source.run({"lib", "export", exportPath.string(), "--output", "json"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    requireJsonLineParses(result.out);
    auto tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["action"]) == "export");
    CHECK(yaml::scalarView(tree.rootref()["path"]) == exportPath.string());
    CHECK_FALSE(fs::exists(source.root() / "json"));
  }

  TEST_CASE("CLI - lib resource export reserves --output for the global format option",
            "[cli][integration][lib][resource]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("with_cover.flac", "cover.flac");
    REQUIRE(fixture.run({"init"}).status == 0);

    auto result = fixture.run({"-O", "yaml", "lib", "resource", "list"});
    REQUIRE(result.status == 0);
    auto const listing = parseYaml(result.out);
    REQUIRE(listing.rootref()["resources"].num_children() == 1);
    auto const idText = std::string{yaml::scalarView(listing.rootref()["resources"][0]["id"])};

    auto const outputPath = fixture.root() / "cover.bin";
    result = fixture.run({"lib", "resource", "export", idText, "--output-file", outputPath.string()});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "exported resource:"));
    CHECK(fs::exists(outputPath));

    // The short form -o keeps the destination meaning.
    auto const shortPath = fixture.root() / "short.bin";
    result = fixture.run({"lib", "resource", "export", idText, "-o", shortPath.string()});
    REQUIRE(result.status == 0);
    CHECK(fs::exists(shortPath));

    // --output selects the report format; --output-file stays the destination.
    auto const againPath = fixture.root() / "again.bin";
    result =
      fixture.run({"lib", "resource", "export", idText, "--output-file", againPath.string(), "--output", "json"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    requireJsonLineParses(result.out);
    auto tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["id"]) == idText);
    CHECK(yaml::scalarView(tree.rootref()["output"]) == againPath.string());
    CHECK(fs::exists(againPath));
    CHECK_FALSE(fs::exists(fixture.root() / "json"));
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
    CHECK(yaml::scalarView(tree.rootref()["payloadVersion"]) == "1");
    CHECK(yaml::scalarView(tree.rootref()["payloadMode"]) == "full");
    CHECK(yaml::scalarView(tree.rootref()["targetScope"]) == "library");
    CHECK(yaml::scalarView(tree.rootref()["tracksCreated"]) == "1");
    CHECK(yaml::scalarView(tree.rootref()["invalidFiltersKept"]) == "0");

    result = target.run({"lib", "import", "--dry-run", "--mode", "restore", exportPath.string()});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Payload: YAML v1, mode 'full', target scope 'library'."));
    CHECK(contains(
      result.out, "Changes: tracks +1/~0/-0, lists +0/-0, dangling references ignored 0, invalid filters kept 0."));

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

  TEST_CASE("CLI - lib export --mode list-only carries the hyphenated mode token through import",
            "[cli][integration][lib][import-export]")
  {
    auto source = CliFixture{};
    source.copyAudio("basic_metadata.flac", "track.flac");
    REQUIRE(source.run({"init"}).status == 0);
    REQUIRE(source.run({"list", "create", "--name", "Saved"}).status == 0);

    auto const exportPath = source.root() / "lists.yaml";
    // The retired camel-case token is not an alias for the YAML token.
    checkDomainFailure(source.run({"lib", "export", exportPath.string(), "--mode", "listOnly"}),
                       "invalid export mode 'listOnly'. Valid modes are: delta, metadata, full, list-only.");
    CHECK_FALSE(fs::exists(exportPath));

    auto result = source.run({"lib", "export", exportPath.string(), "--mode", "list-only"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    auto const document = parseYaml(ao::test::readFile(exportPath));
    CHECK(yaml::scalarView(document.rootref()["export-mode"]) == "list-only");

    auto target = CliFixture{};
    result = target.run({"-O", "json", "lib", "import", "--dry-run", exportPath.string()});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    auto const tree = parseYaml(result.out);
    // The report's mode value follows the YAML token; its own keys stay camel case.
    CHECK(yaml::scalarView(tree.rootref()["payloadMode"]) == "list-only");
    CHECK(yaml::scalarView(tree.rootref()["listsCreated"]) == "1");
    CHECK_FALSE(yaml::findChild(tree.rootref(), "payload-mode").readable());

    result = target.run({"lib", "import", "--dry-run", exportPath.string()});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Payload: YAML v1, mode 'list-only'"));
  }

  TEST_CASE("CLI - lib import counts kept invalid List filters in every output and phase",
            "[cli][integration][lib][import-export]")
  {
    auto fixture = CliFixture{};
    auto const documentPath = fixture.root() / "invalid-filter.yaml";
    {
      auto document = std::ofstream{documentPath};
      document << R"yaml(version: 1
export-mode: list-only
library:
  lists:
    - id: 1
      name: Invalid
      filter: "("
    - id: 2
      name: Valid
      filter: '$genre = "Rock"'
)yaml";
    }

    auto result = fixture.run({"-O", "json", "lib", "import", "--dry-run", documentPath.string()});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    auto tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "true");
    CHECK(yaml::scalarView(tree.rootref()["listsCreated"]) == "2");
    CHECK(yaml::scalarView(tree.rootref()["invalidFiltersKept"]) == "1");

    result = fixture.run({"lib", "import", "--dry-run", documentPath.string()});
    REQUIRE(result.status == 0);
    CHECK(contains(
      result.out, "Changes: tracks +0/~0/-0, lists +2/-0, dangling references ignored 0, invalid filters kept 1."));

    result = fixture.run({"-O", "json", "lib", "import", documentPath.string()});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "false");
    CHECK(yaml::scalarView(tree.rootref()["invalidFiltersKept"]) == "1");

    result =
      fixture.run({"lib", "import", "--mode", "restore", "--confirm-destructive-restore", documentPath.string()});
    REQUIRE(result.status == 0);
    CHECK(contains(
      result.out, "Changes: tracks +0/~0/-0, lists +2/-2, dangling references ignored 0, invalid filters kept 1."));
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
