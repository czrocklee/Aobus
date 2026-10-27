// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CliTestSupport.h"
#include "test/audio/AudioFixtureSupport.h"
#include "test/library/MusicLibraryTestSupport.h"
#include <ao/library/AudioIdentity.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/library/LibraryPaths.h>
#include <ao/utility/Path.h>
#include <ao/yaml/RymlAdapter.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
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

  TEST_CASE("CLI - scan reports missing CUE segments as actionable and restores deleted ordinals",
            "[cli][integration][scan]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "image.flac");

    {
      auto sheet = std::ofstream{fixture.root() / "image.cue", std::ios::binary};
      sheet << "FILE ignored.wav WAVE\nTRACK 03 AUDIO\nTITLE First\nINDEX 01 00:00:00\n"
               "TRACK 09 AUDIO\nTITLE Second\nINDEX 01 00:00:01\n";
      REQUIRE(sheet.good());
    }

    REQUIRE(fixture.run({"scan"}).status == 0);
    auto const initial = fixture.run({"-O", "yaml", "track", "show"});
    REQUIRE(initial.status == 0);
    auto tree = parseYaml(initial.out);
    auto const tracks = yaml::findChild(tree.rootref(), "tracks");
    REQUIRE(tracks.num_children() == 2);
    auto const removedId = std::string{yaml::scalarView(yaml::findChild(tracks[0], "id"))};
    REQUIRE(fixture.run({"track", "delete", removedId}).status == 0);

    auto const planned = fixture.run({"scan", "--dry-run"});
    REQUIRE(planned.status == 0);
    CHECK(contains(planned.out, "missing-segments 1"));
    CHECK(contains(planned.out, "missing-segments image.flac"));
    auto const jsonPlan = fixture.run({"-O", "json", "scan", "--dry-run"});
    REQUIRE(jsonPlan.status == 0);
    auto planTree = parseYaml(jsonPlan.out);
    CHECK(yaml::scalarView(yaml::findChild(planTree.rootref(), "missingSegments")) == "1");
    auto const items = yaml::findChild(planTree.rootref(), "items");
    REQUIRE(items.num_children() == 1);
    CHECK(yaml::scalarView(yaml::findChild(items[0], "type")) == "missing-segments");

    REQUIRE(fixture.run({"scan"}).status == 0);
    auto const restored = fixture.run({"track", "show"});
    REQUIRE(restored.status == 0);
    CHECK(contains(restored.out, "CUE Track Number: 3"));
    CHECK(contains(restored.out, "CUE Track Number: 9"));
  }

  TEST_CASE("CLI - scan dry run lists accompanying CUE warnings while omitting healthy unchanged files",
            "[cli][unit][scan]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "image.flac");
    auto const writeSheet = [&](bool const hasChangedLayout)
    {
      auto sheet = std::ofstream{fixture.root() / "image.cue", std::ios::binary};
      sheet << "FILE ignored.wav WAVE\nTRACK 03 AUDIO\nINDEX 01 00:00:00\nTRACK 09 AUDIO\n"
            << (hasChangedLayout ? "INDEX 01 00:00:02\n" : "INDEX 01 00:00:01\n");
      REQUIRE(sheet.good());
    };
    bool isCarrier = false;

    SECTION("whole file gains a dividing sheet")
    {
      isCarrier = false;
    }

    SECTION("carrier sheet changes layout")
    {
      isCarrier = true;
      writeSheet(false);
    }

    REQUIRE(fixture.run({"scan"}).status == 0);
    auto const healthy = fixture.run({"-O", "json", "scan", "--dry-run"});
    REQUIRE(healthy.status == 0);
    auto healthyTree = parseYaml(healthy.out);
    CHECK(yaml::scalarView(healthyTree.rootref()["unchanged"]) == "1");
    CHECK(yaml::scalarView(healthyTree.rootref()["errors"]) == "0");
    CHECK(yaml::scalarView(healthyTree.rootref()["warnings"]) == "0");
    CHECK(healthyTree.rootref()["items"].num_children() == 0);
    auto const healthyPlain = fixture.run({"scan", "--dry-run"});
    REQUIRE(healthyPlain.status == 0);
    CHECK_FALSE(contains(healthyPlain.out, "unchanged image.flac"));
    writeSheet(isCarrier);
    auto const* const expected =
      isCarrier ? "CUE sheet differs from the imported segments: TRACK 3" : "Imported as a whole file";

    auto const plain = fixture.run({"scan", "--dry-run"});
    REQUIRE(plain.status == 0);
    CHECK(contains(plain.out, "unchanged 1  errors 0  missing-segments 0  warnings 1"));
    CHECK(contains(plain.out, std::string{"unchanged image.flac "} + expected));

    for (auto const* const format : {"json", "yaml"})
    {
      auto const report = fixture.run({"-O", format, "scan", "--dry-run"});
      REQUIRE(report.status == 0);
      auto tree = parseYaml(report.out);
      CHECK(yaml::scalarView(tree.rootref()["unchanged"]) == "1");
      CHECK(yaml::scalarView(tree.rootref()["errors"]) == "0");
      CHECK(yaml::scalarView(tree.rootref()["warnings"]) == "1");
      auto const items = tree.rootref()["items"];
      REQUIRE(items.num_children() == 1);
      CHECK(yaml::scalarView(items[0]["type"]) == "unchanged");
      CHECK(yaml::scalarView(items[0]["uri"]) == "image.flac");
      CHECK(contains(yaml::scalarView(items[0]["message"]), expected));
    }
  }

  TEST_CASE("CLI - scan and verify exit zero when only CUE warnings remain", "[cli][integration][scan]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "image.flac");
    REQUIRE(fixture.run({"scan"}).status == 0);

    for (auto const* const name : {"image.cue", "orphan.cue"})
    {
      auto sheet = std::ofstream{fixture.root() / name, std::ios::binary};
      sheet << "FILE ignored.wav WAVE\nTRACK 03 AUDIO\nINDEX 01 00:00:00\nTRACK 09 AUDIO\nINDEX 01 00:00:01\n";
      REQUIRE(sheet.good());
    }

    // A sheet beside a whole-file import and a sheet that names no audio file
    // both ask for review; neither leaves an item unapplied.
    for (std::int32_t pass = 0; pass < 2; ++pass)
    {
      CAPTURE(pass);
      auto const scanned = fixture.run({"scan"});
      CHECK(scanned.status == 0);
      CHECK(contains(scanned.out, "errors 0  missing-segments 0  warnings 2"));
      CHECK(contains(scanned.err, "warning: image.flac: Imported as a whole file"));
      CHECK(contains(scanned.err, "warning: orphan.cue: No supported audio file has this CUE sheet's name"));
      CHECK_FALSE(contains(scanned.err, "failed to"));
      CHECK_FALSE(contains(scanned.err, "scan apply failed"));
    }

    auto const verified = fixture.run({"lib", "verify"});
    CHECK(verified.status == 0);
    CHECK(contains(verified.out, "image.flac"));
    CHECK(contains(verified.out, "orphan.cue"));
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

  TEST_CASE("CLI - scan reports one moved file for a two-member CUE carrier", "[cli][integration][scan]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "image.flac");

    {
      auto sheet = std::ofstream{fixture.root() / "image.cue", std::ios::binary};
      sheet << "FILE ignored.wav WAVE\nTRACK 03 AUDIO\nTITLE First\nINDEX 01 00:00:00\n"
               "TRACK 09 AUDIO\nTITLE Second\nINDEX 01 00:00:01\n";
      REQUIRE(sheet.good());
    }

    REQUIRE(fixture.run({"scan"}).status == 0);
    auto const before = fixture.run({"-O", "yaml", "track", "show"});
    REQUIRE(before.status == 0);
    auto beforeTree = parseYaml(before.out);
    auto const beforeTracks = yaml::findChild(beforeTree.rootref(), "tracks");
    REQUIRE(beforeTracks.num_children() == 2);
    auto const firstId = std::string{yaml::scalarView(yaml::findChild(beforeTracks[0], "id"))};
    auto const secondId = std::string{yaml::scalarView(yaml::findChild(beforeTracks[1], "id"))};
    REQUIRE(firstId != secondId);

    // Move the audio and its matching sidecar together so the plan is a pure
    // carrier relink, not an orphan sheet or an unrelated diagnostic.
    fs::rename(fixture.root() / "image.flac", fixture.root() / "renamed.flac");
    fs::rename(fixture.root() / "image.cue", fixture.root() / "renamed.cue");

    auto const applied = fixture.run({"scan"});
    REQUIRE(applied.status == 0);
    CHECK(applied.err.empty());
    CHECK(contains(applied.out, "new 0  changed 0  moved 1  missing 0  unchanged 0  errors 0"));
    CHECK(contains(applied.out, "Relinked 1 moved file"));
    CHECK_FALSE(contains(applied.out, "Relinked 2 moved files"));

    auto const after = fixture.run({"-O", "yaml", "track", "show"});
    REQUIRE(after.status == 0);
    auto afterTree = parseYaml(after.out);
    auto const afterTracks = yaml::findChild(afterTree.rootref(), "tracks");
    REQUIRE(afterTracks.num_children() == 2);
    auto const afterFirstId = std::string{yaml::scalarView(yaml::findChild(afterTracks[0], "id"))};
    auto const afterSecondId = std::string{yaml::scalarView(yaml::findChild(afterTracks[1], "id"))};
    CHECK(afterFirstId != afterSecondId);
    CHECK(((afterFirstId == firstId && afterSecondId == secondId) ||
           (afterFirstId == secondId && afterSecondId == firstId)));
    CHECK(yaml::scalarView(yaml::findChild(afterTracks[0], "uri")) == "renamed.flac");
    CHECK(yaml::scalarView(yaml::findChild(afterTracks[1], "uri")) == "renamed.flac");
  }

  TEST_CASE("CLI - scan and init report apply failures and exit non-zero", "[cli][integration][scan]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "track.flac");

    // A supported extension whose bytes do not decode passes the plan as new,
    // then fails per item during apply.
    auto const corruptPath = fixture.root() / "corrupt.flac";
    {
      auto out = std::ofstream{corruptPath, std::ios::binary};
      out << "not an audio file";
    }

    auto result = fixture.run({"scan"});
    CHECK(result.status == 1);
    CHECK(contains(result.out, "new 2  changed 0  moved 0  missing 0  unchanged 0  errors 0"));
    CHECK(contains(result.err, "failed to read media file corrupt.flac"));
    CHECK(contains(result.out, "1 item failed to apply"));
    CHECK(contains(result.err, "scan apply failed: 1 item could not be applied"));

    // The decodable file still applied, so the failed scan is partial, not void.
    auto showResult = fixture.run({"track", "show"});
    REQUIRE(showResult.status == 0);
    CHECK(contains(showResult.out, "Test Title"));

    // The failed item left no manifest row, and init shares the same path.
    result = fixture.run({"init"});
    CHECK(result.status == 1);
    CHECK(contains(result.out, "new 1  changed 0  moved 0  missing 0  unchanged 1  errors 0"));
    CHECK(contains(result.out, "1 item failed to apply"));
    CHECK(contains(result.err, "scan apply failed: 1 item could not be applied"));

    // Structured output is emitted before apply, so the failure surfaces on
    // stderr and through the exit status without changing the document.
    result = fixture.run({"-O", "json", "scan"});
    CHECK(result.status == 1);
    requireJsonLineParses(result.out);
    auto tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["new"]) == "1");
    CHECK(contains(result.err, "scan apply failed: 1 item could not be applied"));
  }
} // namespace ao::cli::test
