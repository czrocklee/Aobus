// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CliTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/audio/AudioFixtureSupport.h"
#include "test/library/MusicLibraryTestSupport.h"
#include "test/media/wav/TestWav.h"
#include <ao/CoreIds.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/library/LibraryPaths.h>
#include <ao/utility/Hash128.h>
#include <ao/utility/Uuid.h>
#include <ao/yaml/RymlAdapter.h>

#include <catch2/catch_test_macros.hpp>
#include <ryml.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <vector>

namespace ao::cli::test
{
  namespace
  {
    namespace fs = std::filesystem;

    struct ManifestSnapshot final
    {
      utility::UuidBytes libraryId{};
      TrackId trackId{};
      std::uint64_t revision = 0;
      std::uint64_t payloadLength = 0;
      utility::Hash128 signature{};

      bool operator==(ManifestSnapshot const&) const = default;
    };

    ManifestSnapshot readSnapshot(CliFixture const& fixture, std::string_view uri)
    {
      auto const musicLibrary =
        library::test::makeTestMusicLibrary(fixture.root(), rt::LibraryPaths{fixture.root()}.databasePath());
      auto const read = musicLibrary.readTransaction();
      auto const optRow = musicLibrary.manifest().reader(read).get(uri);
      REQUIRE(optRow);
      return {.libraryId = musicLibrary.metadataHeader(read).libraryId,
              .trackId = optRow->trackId(),
              .revision = musicLibrary.libraryRevision(read),
              .payloadLength = optRow->audioPayloadLength(),
              .signature = optRow->audioSignature()};
    }

    void writeWav(fs::path const& path, std::uint8_t sampleByte)
    {
      auto const bytes =
        ao::test::wav::makeWav({.sampleRate = 8000, .audioData = std::vector<std::uint8_t>(1600, sampleByte)});
      auto out = std::ofstream{path, std::ios::binary};
      out.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      out.close();
      REQUIRE(out.good());
    }

    ryml::Tree requireReport(CliResult const& result, std::string_view uri, std::string_view status, bool updated)
    {
      REQUIRE(result.status == 0);
      auto tree = parseYaml(result.out);
      auto const report = tree.rootref();
      REQUIRE(report.is_map());
      REQUIRE(report.has_child("uri"));
      REQUIRE(report.has_child("status"));
      REQUIRE(report.has_child("updated"));
      REQUIRE(report.has_child("observed"));
      CHECK_FALSE(report["updated"].is_val_quoted());
      CHECK(yaml::scalarView(report["uri"]) == uri);
      CHECK(yaml::scalarView(report["status"]) == status);
      CHECK(yaml::scalarView(report["updated"]) == (updated ? "true" : "false"));
      REQUIRE(report.has_child("stored") == (status != "pending"));
      REQUIRE(report.has_child("confirmation") == (!updated && status != "match"));

      if (report.has_child("confirmation"))
      {
        REQUIRE_FALSE(yaml::scalarView(report["confirmation"]).empty());
      }

      return tree;
    }

    void checkIdentity(ryml::ConstNodeRef identity, ManifestSnapshot const& stored)
    {
      REQUIRE(identity.is_map());
      REQUIRE(identity.has_child("signature"));
      REQUIRE(identity.has_child("payloadLength"));
      CHECK(yaml::scalarView(identity["payloadLength"]) == std::to_string(stored.payloadLength));
      CHECK(yaml::scalarView(identity["signature"]) == utility::hash128Hex(stored.signature));
    }
  } // namespace

  TEST_CASE("CLI - targeted fingerprint verification leaves a fresh root uninitialized")
  {
    auto const fixture = CliFixture{};
    auto const managedPath = rt::LibraryPaths{fixture.root()}.managedDataPath();
    REQUIRE_FALSE(fs::exists(managedPath));

    checkDomainFailure(fixture.run({"lib", "fingerprint", "--verify", "song.wav"}), "no existing library");
    CHECK_FALSE(fs::exists(managedPath));

    checkDomainFailure(
      fixture.run({"lib", "fingerprint", "--verify", "song.wav", "--confirm", "unused"}), "no existing library");
    CHECK_FALSE(fs::exists(managedPath));
  }

  TEST_CASE("CLI - fingerprint confirmation adopts only the previewed replacement audio")
  {
    auto const fixture = CliFixture{};
    auto const* const uri = "song.wav";
    auto const path = fixture.root() / uri;
    writeWav(path, 1);
    auto const modificationTime = fs::last_write_time(path);
    auto const size = fs::file_size(path);
    REQUIRE(fixture.run({"scan"}).status == 0);
    auto const id = std::to_string(readSnapshot(fixture, uri).trackId.raw());
    REQUIRE(fixture.run({"track", "update", id, "--title", "Curated", "--add-tag", "favorite"}).status == 0);
    auto const original = readSnapshot(fixture, uri);
    REQUIRE(original.payloadLength == 1600);

    writeWav(path, 2);
    fs::last_write_time(path, modificationTime);
    REQUIRE((fs::last_write_time(path) == modificationTime));
    REQUIRE(fs::file_size(path) == size);
    auto const preview =
      requireReport(fixture.run({"-O", "json", "lib", "fingerprint", "--verify", uri}), uri, "mismatch", false);
    auto const report = preview.rootref();
    checkIdentity(report["stored"], original);
    auto const observed = report["observed"];
    CHECK(yaml::scalarView(observed["payloadLength"]) == "1600");
    REQUIRE(yaml::scalarView(observed["signature"]) != utility::hash128Hex(original.signature));
    CHECK(readSnapshot(fixture, uri) == original);
    auto const token = yaml::scalarView(report["confirmation"]);

    SECTION("different bytes after preview invalidate the confirmation even with identical file facts")
    {
      writeWav(path, 3);
      fs::last_write_time(path, modificationTime);
      REQUIRE(fs::file_size(path) == size);
      REQUIRE((fs::last_write_time(path) == modificationTime));
      checkDomainFailure(
        fixture.run({"lib", "fingerprint", "--verify", uri, "--confirm", token}), "confirmation is stale");
      CHECK(readSnapshot(fixture, uri) == original);
    }

    SECTION("restoring the original audio makes the token inapplicable at the same revision")
    {
      writeWav(path, 1);
      fs::last_write_time(path, modificationTime);
      auto const matched =
        requireReport(fixture.run({"-O", "json", "lib", "fingerprint", "--verify", uri}), uri, "match", false);
      checkIdentity(matched.rootref()["stored"], original);
      checkIdentity(matched.rootref()["observed"], original);
      REQUIRE(readSnapshot(fixture, uri) == original);

      checkDomainFailure(fixture.run({"lib", "fingerprint", "--verify", uri, "--confirm", token}), "already matches");
      CHECK(readSnapshot(fixture, uri) == original);
    }

    SECTION("confirmed audio wins over the old audio in a later scan")
    {
      auto const applied = requireReport(
        fixture.run({"-O", "json", "lib", "fingerprint", "--verify", uri, "--confirm", token}), uri, "mismatch", true);
      auto const adopted = readSnapshot(fixture, uri);
      CHECK(adopted.trackId == original.trackId);
      CHECK(adopted.revision > original.revision);
      checkIdentity(observed, adopted);
      checkIdentity(applied.rootref()["stored"], original);
      checkIdentity(applied.rootref()["observed"], adopted);

      auto const matched =
        requireReport(fixture.run({"-O", "json", "lib", "fingerprint", "--verify", uri}), uri, "match", false);
      checkIdentity(matched.rootref()["stored"], adopted);
      checkIdentity(matched.rootref()["observed"], adopted);
      CHECK(readSnapshot(fixture, uri) == adopted);

      fs::rename(path, fixture.root() / "renamed.wav");
      writeWav(fixture.root() / "old-copy.wav", 1);
      REQUIRE(fixture.run({"scan"}).status == 0);
      CHECK(readSnapshot(fixture, "old-copy.wav").trackId != original.trackId);
      CHECK(readSnapshot(fixture, "renamed.wav").trackId == original.trackId);
      auto const favorite = fixture.run({"-O", "json", "track", "show", "--filter", "#favorite"});
      REQUIRE(favorite.status == 0);
      auto const row = parseYaml(favorite.out);
      CHECK(yaml::scalarView(row.rootref()["id"]) == id);
      CHECK(yaml::scalarView(row.rootref()["uri"]) == "renamed.wav");
      CHECK(yaml::scalarView(row.rootref()["title"]) == "Curated");
    }
  }

  TEST_CASE("CLI - fingerprint confirmation rejects a different prior identity at the same library revision")
  {
    auto const original = CliFixture{};
    auto const fork = CliFixture{};
    auto const* const uri = "song.wav";
    REQUIRE(original.run({"init"}).status == 0);

    // Fork the closed, empty database, not its locks or leases. A single scan
    // on each side will allocate the same track ID at the same revision.
    auto const originalDb = rt::LibraryPaths{original.root()}.databasePath();
    auto const forkDb = rt::LibraryPaths{fork.root()}.databasePath();
    REQUIRE(fs::create_directories(forkDb));
    REQUIRE(fs::copy_file(originalDb / "data.mdb", forkDb / "data.mdb"));
    auto const originalPath = original.root() / uri;
    auto const forkPath = fork.root() / uri;
    writeWav(originalPath, 1);
    writeWav(forkPath, 2);
    auto const modificationTime = fs::last_write_time(originalPath);
    fs::last_write_time(forkPath, modificationTime);
    REQUIRE(original.run({"scan"}).status == 0);
    REQUIRE(fork.run({"scan"}).status == 0);
    auto const first = readSnapshot(original, uri);
    auto const other = readSnapshot(fork, uri);
    REQUIRE(other.libraryId == first.libraryId);
    REQUIRE(other.trackId == first.trackId);
    REQUIRE(other.revision == first.revision);
    REQUIRE(other.payloadLength == first.payloadLength);
    REQUIRE(other.signature != first.signature);

    for (auto const& path : {originalPath, forkPath})
    {
      writeWav(path, 3);
      fs::last_write_time(path, modificationTime);
    }

    auto const foreign =
      requireReport(original.run({"-O", "json", "lib", "fingerprint", "--verify", uri}), uri, "mismatch", false);
    auto const token = yaml::scalarView(foreign.rootref()["confirmation"]);
    checkDomainFailure(fork.run({"lib", "fingerprint", "--verify", uri, "--confirm", token}), "confirmation is stale");
    REQUIRE(readSnapshot(fork, uri) == other);

    auto const local =
      requireReport(fork.run({"-O", "json", "lib", "fingerprint", "--verify", uri}), uri, "mismatch", false);
    auto const localToken = yaml::scalarView(local.rootref()["confirmation"]);
    REQUIRE(fork.run({"lib", "fingerprint", "--verify", uri, "--confirm", localToken}).status == 0);
    auto const adopted = readSnapshot(fork, uri);
    checkIdentity(foreign.rootref()["observed"], adopted);
    checkIdentity(local.rootref()["observed"], adopted);
  }

  TEST_CASE("CLI - pending fingerprint preview needs explicit confirmation to store its observation")
  {
    auto const fixture = CliFixture{};
    auto const* const uri = "deferred.flac";
    fixture.copyAudio("basic_metadata.flac", uri);
    REQUIRE(fixture.run({"scan", "--defer-fingerprint"}).status == 0);
    auto const pending = readSnapshot(fixture, uri);
    REQUIRE(pending.payloadLength == 0);
    auto const preview =
      requireReport(fixture.run({"-O", "json", "lib", "fingerprint", "--verify", uri}), uri, "pending", false);
    CHECK(readSnapshot(fixture, uri) == pending);
    auto const token = yaml::scalarView(preview.rootref()["confirmation"]);

    checkDomainFailure(
      fixture.run({"lib", "fingerprint", "--pending", "--verify", uri}), "requires --pending or --verify");
    checkDomainFailure(fixture.run({"lib", "fingerprint", "--confirm", token}), "requires --pending or --verify");
    checkDomainFailure(
      fixture.run({"lib", "fingerprint", "--pending", "--confirm", token}), "--confirm requires --verify");
    checkDomainFailure(
      fixture.run({"lib", "fingerprint", "--verify", uri, "--verbose"}), "--verbose requires --pending");
    checkDomainFailure(fixture.run({"lib", "fingerprint", "--verify", ""}), "--verify requires a non-empty URI");
    checkDomainFailure(
      fixture.run({"lib", "fingerprint", "--verify", uri, "--confirm", ""}), "--confirm requires a non-empty token");
    CHECK(readSnapshot(fixture, uri) == pending);

    auto const applied = requireReport(
      fixture.run({"-O", "json", "lib", "fingerprint", "--verify", uri, "--confirm", token}), uri, "pending", true);
    auto const adopted = readSnapshot(fixture, uri);
    REQUIRE(adopted.payloadLength > 0);
    CHECK(adopted.trackId == pending.trackId);
    checkIdentity(preview.rootref()["observed"], adopted);
    checkIdentity(applied.rootref()["observed"], adopted);
  }

  TEST_CASE("CLI - a tag-only FLAC edit matches without offering fingerprint adoption")
  {
    auto const fixture = CliFixture{};
    auto const* const uri = "tagged.flac";
    auto const path = fixture.root() / uri;
    auto bytes = ao::test::readFile(audio::test::requireAudioFixture("basic_metadata.flac"));
    auto const title = bytes.find("Test Title");
    REQUIRE(title != std::string::npos);
    REQUIRE((std::ofstream{path, std::ios::binary} << bytes).good());
    auto const modificationTime = fs::last_write_time(path);
    REQUIRE(fixture.run({"scan"}).status == 0);
    auto const original = readSnapshot(fixture, uri);

    bytes[title] = 'B'; // Same-width edit to the Vorbis comment, not the encoded frames.
    REQUIRE((std::ofstream{path, std::ios::binary} << bytes).good());
    fs::last_write_time(path, modificationTime);
    auto const match =
      requireReport(fixture.run({"-O", "json", "lib", "fingerprint", "--verify", uri}), uri, "match", false);
    checkIdentity(match.rootref()["stored"], original);
    checkIdentity(match.rootref()["observed"], original);
    CHECK(readSnapshot(fixture, uri) == original);
  }
} // namespace ao::cli::test
