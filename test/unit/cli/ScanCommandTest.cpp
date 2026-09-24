// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CliTestSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include <ao/library/AudioIdentity.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/library/LibraryPaths.h>
#include <ao/utility/Path.h>
#include <ao/yaml/RymlAdapter.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

namespace ao::cli::test
{
  namespace
  {
    namespace fs = std::filesystem;

    bool hasManifestAudioIdentity(CliFixture const& fixture, std::string_view uri)
    {
      auto musicLibrary =
        library::test::makeTestMusicLibrary(fixture.root(), rt::LibraryPaths{fixture.root()}.databasePath());
      auto transaction = musicLibrary.readTransaction();
      auto optManifest = musicLibrary.manifest().reader(transaction).get(uri);
      REQUIRE(optManifest);
      return library::hasAudioIdentity(optManifest->audioPayloadLength(), optManifest->audioSignature());
    }
  } // namespace

  TEST_CASE("CLI - scan applies new files and dry run preserves planned changes", "[cli][integration][scan]")
  {
    auto fixture = CliFixture{};
    auto const trackPath = fixture.root() / "track.flac";
    fixture.copyAudio("basic_metadata.flac", trackPath.filename().string());

    auto result = fixture.run({"scan", "--dry-run"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "new 1  changed 0  moved 0  missing 0  unchanged 0  errors 0"));
    CHECK(contains(result.out, "new track.flac"));

    result = fixture.run({"track", "show"});
    REQUIRE(result.status == 0);
    CHECK(result.out.empty());

    result = fixture.run({"scan", "--verbose"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.err, "scan:"));
    CHECK(contains(result.err, "apply:"));
    CHECK(contains(result.err, "fingerprint:"));
    CHECK(contains(result.out, "new 1  changed 0  moved 0  missing 0  unchanged 0  errors 0"));

    result = fixture.run({"track", "show"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Test Title"));

    auto const oldMtimeTime = fs::last_write_time(trackPath);
    fs::last_write_time(trackPath, oldMtimeTime + std::chrono::seconds{5});

    result = fixture.run({"scan", "--dry-run"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "new 0  changed 1  moved 0  missing 0  unchanged 0  errors 0"));
    CHECK(contains(result.out, "changed track.flac"));

    result = fixture.run({"scan", "--dry-run"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "new 0  changed 1  moved 0  missing 0  unchanged 0  errors 0"));

    fs::remove(trackPath);

    result = fixture.run({"scan", "--dry-run"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "new 0  changed 0  moved 0  missing 1  unchanged 0  errors 0"));
    CHECK(contains(result.out, "missing track.flac"));

    result = fixture.run({"scan"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "new 0  changed 0  moved 0  missing 1  unchanged 0  errors 0"));
    CHECK(contains(result.out, "1 missing file needs review"));
  }

  TEST_CASE("CLI - deferred scan fingerprints pending manifest rows on demand", "[cli][integration][scan]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "track.flac");

    auto result = fixture.run({"scan", "--defer-fingerprint", "--verbose"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "new 1  changed 0  moved 0  missing 0  unchanged 0  errors 0"));
    CHECK(contains(result.out, "Audio identity fingerprinting was deferred"));
    CHECK(contains(result.err, "scan:"));
    CHECK(contains(result.err, "apply:"));
    CHECK_FALSE(contains(result.err, "fingerprint:"));
    CHECK_FALSE(hasManifestAudioIdentity(fixture, "track.flac"));

    result = fixture.run({"lib", "fingerprint", "--pending", "--verbose"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "fingerprinted 1  skipped 0  failed 0"));
    CHECK(contains(result.err, "fingerprint:"));
    CHECK(hasManifestAudioIdentity(fixture, "track.flac"));

    result = fixture.run({"-O", "json", "lib", "fingerprint", "--pending"});
    REQUIRE(result.status == 0);
    auto tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["completed"]) == "0");
    CHECK(yaml::scalarView(tree.rootref()["skipped"]) == "0");
    CHECK(yaml::scalarView(tree.rootref()["failures"]) == "0");
    CHECK_FALSE(tree.rootref().has_child("cancelled"));

    checkDomainFailure(fixture.run({"lib", "fingerprint"}), "lib fingerprint requires --pending");
  }

  TEST_CASE("CLI - verbose scan reports UTF-8 paths", "[cli][integration][scan]")
  {
    auto const expected = std::string{"\xE8\xAA\xB0\xE3\x81\x8B\xE3\x80\x81\xE6\xB5\xB7\xE3\x82\x92\xE3\x80\x82.flac"};
    auto fixture = CliFixture{};
    std::filesystem::copy_file(
      audio::test::requireAudioFixture("basic_metadata.flac"), fixture.root() / utility::pathFromUtf8(expected));

    auto const result = fixture.run({"scan", "--defer-fingerprint", "--verbose"});

    REQUIRE(result.status == 0);
    CHECK(contains(result.err, "scan:"));
    CHECK(contains(result.err, expected));
    CHECK(contains(result.err, "apply:"));
  }

  TEST_CASE("CLI - scan reports moved files", "[cli][integration][scan]")
  {
    auto fixture = CliFixture{};
    auto const originalPath = fixture.root() / "track.flac";
    auto const movedPath = fixture.root() / "renamed.flac";
    fixture.copyAudio("basic_metadata.flac", originalPath.filename().string());

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    fs::rename(originalPath, movedPath);

    result = fixture.run({"scan", "--dry-run"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "new 0  changed 0  moved 1  missing 0  unchanged 0  errors 0"));
    CHECK(contains(result.out, "moved renamed.flac"));

    result = fixture.run({"scan"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "new 0  changed 0  moved 1  missing 0  unchanged 0  errors 0"));
    CHECK(contains(result.out, "Relinked 1 moved file"));

    result = fixture.run({"scan", "--dry-run"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "new 0  changed 0  moved 0  missing 0  unchanged 1  errors 0"));
  }
} // namespace ao::cli::test
