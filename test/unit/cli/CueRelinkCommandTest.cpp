// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CliTestSupport.h"
#include <ao/yaml/RymlAdapter.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
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
    void writeCue(std::filesystem::path const& path)
    {
      auto stream = std::ofstream{path, std::ios::binary};
      stream << "FILE ignored.wav WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n"
                "TRACK 02 AUDIO\nINDEX 01 00:00:01\n";
      REQUIRE(stream.good());
    }

    struct ShownSegment final
    {
      std::string id{};
      std::string ordinal{};
      std::string startFrame{};
      std::string frameCount{};
      std::string endFrame{};
    };

    std::vector<ShownSegment> segmentsForUri(ryml::ConstNodeRef const& tracks, std::string_view uri)
    {
      auto segments = std::vector<ShownSegment>{};

      for (auto const track : tracks.children())
      {
        if (yaml::scalarView(track["uri"]) != uri)
        {
          continue;
        }

        REQUIRE(track.has_child("segment"));
        auto const segment = track["segment"];
        segments.push_back(ShownSegment{.id = std::string{yaml::scalarView(track["id"])},
                                        .ordinal = std::string{yaml::scalarView(segment["ordinal"])},
                                        .startFrame = std::string{yaml::scalarView(segment["startFrame"])},
                                        .frameCount = std::string{yaml::scalarView(segment["frameCount"])},
                                        .endFrame = std::string{yaml::scalarView(segment["endFrame"])}});
      }

      std::ranges::sort(segments,
                        [](ShownSegment const& left, ShownSegment const& right)
                        { return std::stoull(left.ordinal) < std::stoull(right.ordinal); });
      return segments;
    }

    std::string wholeTrackId(ryml::ConstNodeRef const& tracks, std::string_view uri)
    {
      for (auto const track : tracks.children())
      {
        if (yaml::scalarView(track["uri"]) != uri)
        {
          continue;
        }

        REQUIRE_FALSE(track.has_child("segment"));
        return std::string{yaml::scalarView(track["id"])};
      }

      return {};
    }

    void checkTrackIds(ryml::ConstNodeRef const& ids, std::vector<ShownSegment> const& members)
    {
      REQUIRE(ids.is_seq());
      REQUIRE(ids.num_children() == members.size());

      for (std::size_t index = 0; index < members.size(); ++index)
      {
        CHECK(yaml::scalarView(ids[index]) == members[index].id);
      }
    }

    void checkCarrierRelinkDocument(ryml::ConstNodeRef const& root,
                                    std::vector<ShownSegment> const& members,
                                    bool dryRun)
    {
      CHECK(yaml::scalarView(root["dryRun"]) == (dryRun ? "true" : "false"));
      CHECK(yaml::scalarView(root["oldUri"]) == "disc.wav");
      CHECK(yaml::scalarView(root["newUri"]) == "new.wav");
      CHECK(yaml::scalarView(root["trackId"]) == "0");
      REQUIRE(root.has_child("trackIds"));
      checkTrackIds(root["trackIds"], members);
    }

    void checkWholeFileRecord(ryml::ConstNodeRef const& record, std::string_view trackId)
    {
      CHECK(yaml::scalarView(record["trackId"]) == trackId);
      CHECK(record.has_child("fileSize"));
      CHECK(record.has_child("status"));
      CHECK_FALSE(record.has_child("trackIds"));
      CHECK_FALSE(record.has_child("segmentCount"));
      CHECK_FALSE(record.has_child("totalFrames"));
    }

    void checkCarrierRecord(ryml::ConstNodeRef const& record,
                            std::vector<ShownSegment> const& members,
                            std::uint64_t totalFrames)
    {
      CHECK(yaml::scalarView(record["trackId"]) == "0");
      CHECK(record.has_child("fileSize"));
      CHECK(record.has_child("status"));
      REQUIRE(record.has_child("trackIds"));
      checkTrackIds(record["trackIds"], members);
      CHECK(yaml::scalarView(record["segmentCount"]) == std::to_string(members.size()));
      CHECK(yaml::scalarView(record["totalFrames"]) == std::to_string(totalFrames));
    }

    ryml::ConstNodeRef manifestRecord(ryml::ConstNodeRef const& manifest, std::string_view uri)
    {
      for (auto const record : manifest.children())
      {
        if (yaml::scalarView(record["uri"]) == uri)
        {
          return record;
        }
      }

      REQUIRE(false);
      return manifest[0];
    }

    void checkManifest(CliFixture const& fixture,
                       std::string_view carrierUri,
                       std::vector<ShownSegment> const& members,
                       std::uint64_t totalFrames,
                       std::string_view firstWholeUri,
                       std::string_view firstWholeId,
                       std::string_view secondWholeUri,
                       std::string_view secondWholeId)
    {
      auto const json = fixture.run({"-O", "json", "lib", "dump", "--manifest"});
      REQUIRE(json.status == 0);
      CHECK(json.err.empty());
      auto tree = parseYaml(json.out);
      auto const manifest = tree.rootref()["manifest"];
      REQUIRE(manifest.is_seq());
      checkCarrierRecord(manifestRecord(manifest, carrierUri), members, totalFrames);
      checkWholeFileRecord(manifestRecord(manifest, firstWholeUri), firstWholeId);
      checkWholeFileRecord(manifestRecord(manifest, secondWholeUri), secondWholeId);

      auto const plain = fixture.run({"lib", "dump", "--manifest"});
      REQUIRE(plain.status == 0);
      CHECK(plain.err.empty());
      auto const memberList = members[0].id + ", " + members[1].id;
      CHECK(contains(plain.out, std::string{"URI: "} + std::string{carrierUri}));
      CHECK(contains(plain.out, "Track ID: 0\n"));
      CHECK(contains(plain.out, "Track IDs: " + memberList + "\n"));
      CHECK(contains(plain.out, "Segment Count: " + std::to_string(members.size()) + "\n"));
      CHECK(contains(plain.out, "Total Frames: " + std::to_string(totalFrames) + "\n"));
      CHECK(countOccurrences(plain.out, "Track IDs:") == 1);
      CHECK(countOccurrences(plain.out, "Segment Count:") == 1);
      CHECK(countOccurrences(plain.out, "Total Frames:") == 1);
      CHECK(contains(plain.out, "Track ID: " + std::string{firstWholeId} + "\n"));
      CHECK(contains(plain.out, "Track ID: " + std::string{secondWholeId} + "\n"));
    }
  } // namespace

  TEST_CASE("CLI CUE relink - incompatible carriers are neither candidates nor successful previews",
            "[cli][integration][cue-relink-command]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.wav", "old.wav");
    writeCue(fixture.root() / "old.cue");
    bool isAmbiguous = false;

    SECTION("known rejected carrier")
    {
    }

    SECTION("ambiguous New destination")
    {
      fixture.copyAudio("basic_metadata.wav", "duplicate.wav");
      isAmbiguous = true;
    }

    REQUIRE(fixture.run({"init"}).status == 0);
    auto const before = fixture.run({"-O", "json", "track", "show"});
    REQUIRE(before.status == 0);
    auto const manifestBefore = fixture.run({"-O", "json", "lib", "dump", "--manifest"});
    REQUIRE(manifestBefore.status == 0);
    std::filesystem::remove(fixture.root() / "old.cue");
    std::filesystem::rename(fixture.root() / "old.wav", fixture.root() / "new.wav");

    if (isAmbiguous)
    {
      std::filesystem::remove(fixture.root() / "duplicate.wav");
    }

    {
      // Change only WAV rate headers; the encoded PCM identity stays identical.
      auto stream = std::fstream{fixture.root() / "new.wav", std::ios::binary | std::ios::in | std::ios::out};
      auto const rateHeader = std::to_array<unsigned char>({0x80, 0xbb, 0, 0, 0, 0xee, 2, 0});
      stream.seekp(24);
      stream.write(reinterpret_cast<char const*>(rateHeader.data()), rateHeader.size());
      REQUIRE(stream.good());
    }

    auto const listing = fixture.run({"-O", "json", "lib", "relink"});
    REQUIRE(listing.status == 0);
    auto tree = parseYaml(listing.out);
    CHECK(tree.rootref()["candidates"].num_children() == (isAmbiguous ? 1 : 0));

    for (auto const candidate : tree.rootref()["candidates"].children())
    {
      // A whole-file duplicate remains compatible; the carrier does not.
      CHECK(yaml::scalarView(candidate["oldUri"]) == "duplicate.wav");
      CHECK(yaml::scalarView(candidate["newUri"]) == "new.wav");
    }

    auto const preview = fixture.run({"lib", "relink", "--from", "old.wav", "--to", "new.wav", "--dry-run"});
    CHECK(preview.status == 1);
    CHECK_FALSE(contains(preview.out, "relinked"));
    auto const applied = fixture.run({"lib", "relink", "--from", "old.wav", "--to", "new.wav"});
    CHECK(applied.status == 1);
    CHECK(fixture.run({"-O", "json", "track", "show"}).out == before.out);
    CHECK(fixture.run({"-O", "json", "lib", "dump", "--manifest"}).out == manifestBefore.out);
  }

  TEST_CASE("CLI CUE relink - source-form warning accompanies a committed identity-preserving carrier move",
            "[cli][integration][cue-relink-command]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.wav", "old.wav");
    fixture.copyAudio("basic_metadata.wav", "duplicate.wav");
    writeCue(fixture.root() / "old.cue");
    REQUIRE(fixture.run({"init"}).status == 0);
    auto const original = fixture.run({"-O", "yaml", "track", "show"});
    REQUIRE(original.status == 0);
    auto originalTree = parseYaml(original.out);
    auto memberIds = std::vector<std::string>{};

    for (auto const track : originalTree.rootref()["tracks"].children())
    {
      if (yaml::scalarView(track["uri"]) == "old.wav")
      {
        memberIds.emplace_back(yaml::scalarView(track["id"]));
      }
    }

    REQUIRE(memberIds.size() == 2);
    std::filesystem::remove(fixture.root() / "duplicate.wav");
    std::filesystem::remove(fixture.root() / "old.cue");
    std::filesystem::rename(fixture.root() / "old.wav", fixture.root() / "new.wav");
    auto const listed = fixture.run({"lib", "relink"});
    REQUIRE(listed.status == 0);
    CHECK(contains(listed.out, "candidate old.wav -> new.wav"));
    auto const preview = fixture.run({"lib", "relink", "--from", "old.wav", "--to", "new.wav", "--dry-run"});
    REQUIRE(preview.status == 0);
    CHECK(fixture.run({"-O", "yaml", "track", "show"}).out == original.out);
    auto const applied = fixture.run({"lib", "relink", "--from", "old.wav", "--to", "new.wav"});
    // The committed move succeeds; its missing sheet is a warning for a person to settle.
    CHECK(applied.status == 0);
    CHECK(contains(applied.err, "warning: new.wav: No sheet divides this file; imported segments kept"));
    CHECK_FALSE(contains(applied.err, "relink failed"));
    CHECK(contains(applied.out, "old.wav -> new.wav"));

    for (auto const& id : memberIds)
    {
      auto const after = fixture.run({"-O", "yaml", "track", "show", id});
      REQUIRE(after.status == 0);
      auto afterTree = parseYaml(after.out);
      REQUIRE(afterTree.rootref()["tracks"].num_children() == 1);
      CHECK(yaml::scalarView(afterTree.rootref()["tracks"][0]["id"]) == id);
      CHECK(yaml::scalarView(afterTree.rootref()["tracks"][0]["uri"]) == "new.wav");
    }
  }

  TEST_CASE("CLI CUE relink - explicit carrier move preserves ranges and reports every member id",
            "[cli][integration][cue-relink-command]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.wav", "disc.wav");
    fixture.copyAudio("basic_metadata.wav", "duplicate.wav");
    fixture.copyAudio("basic_metadata.flac", "keeper.flac");
    writeCue(fixture.root() / "disc.cue");
    REQUIRE(fixture.run({"init"}).status == 0);
    auto const original = fixture.run({"-O", "yaml", "track", "show"});
    REQUIRE(original.status == 0);
    auto originalTree = parseYaml(original.out);
    auto const tracks = originalTree.rootref()["tracks"];
    auto const members = segmentsForUri(tracks, "disc.wav");
    REQUIRE(members.size() == 2);
    REQUIRE(members[0].id != members[1].id);
    REQUIRE(members[0].id != "0");
    REQUIRE(members[1].id != "0");
    REQUIRE(members[0].ordinal == "1");
    REQUIRE(members[1].ordinal == "2");
    REQUIRE(members[0].startFrame == "0");
    REQUIRE(members[0].startFrame != members[1].startFrame);
    REQUIRE(members[0].frameCount != "0");
    REQUIRE(members[1].frameCount != "0");
    REQUIRE(members[0].endFrame == members[1].startFrame);
    auto const totalFrames = std::stoull(members[1].endFrame);
    REQUIRE(totalFrames == std::stoull(members[0].frameCount) + std::stoull(members[1].frameCount));
    REQUIRE(totalFrames != 0);
    auto const duplicateId = wholeTrackId(tracks, "duplicate.wav");
    auto const keeperId = wholeTrackId(tracks, "keeper.flac");
    REQUIRE_FALSE(duplicateId.empty());
    REQUIRE_FALSE(keeperId.empty());
    REQUIRE(duplicateId != "0");
    REQUIRE(keeperId != "0");
    REQUIRE(duplicateId != keeperId);
    checkManifest(fixture, "disc.wav", members, totalFrames, "duplicate.wav", duplicateId, "keeper.flac", keeperId);

    // An identical missing whole file leaves automatic matching ambiguous.
    // The renamed sheet stays with the destination so the explicit move can commit.
    std::filesystem::remove(fixture.root() / "duplicate.wav");
    std::filesystem::rename(fixture.root() / "disc.wav", fixture.root() / "new.wav");
    std::filesystem::rename(fixture.root() / "disc.cue", fixture.root() / "new.cue");
    auto const memberList = members[0].id + ", " + members[1].id;
    auto const listed = fixture.run({"-O", "json", "lib", "relink"});
    REQUIRE(listed.status == 0);
    auto listTree = parseYaml(listed.out);
    REQUIRE(listTree.rootref()["newFiles"].num_children() == 1);
    CHECK(yaml::scalarView(listTree.rootref()["newFiles"][0]) == "new.wav");
    REQUIRE(listTree.rootref()["candidates"].num_children() == 2);
    bool sawCarrier = false;
    bool sawWholeFile = false;

    for (auto const candidate : listTree.rootref()["candidates"].children())
    {
      CHECK(yaml::scalarView(candidate["newUri"]) == "new.wav");
      CHECK(candidate.has_child("audioPayloadLength"));

      if (yaml::scalarView(candidate["oldUri"]) == "disc.wav")
      {
        sawCarrier = true;
        CHECK(yaml::scalarView(candidate["trackId"]) == "0");
        REQUIRE(candidate.has_child("trackIds"));
        checkTrackIds(candidate["trackIds"], members);
      }
      else if (yaml::scalarView(candidate["oldUri"]) == "duplicate.wav")
      {
        sawWholeFile = true;
        CHECK(yaml::scalarView(candidate["trackId"]) == duplicateId);
        CHECK_FALSE(candidate.has_child("trackIds"));
      }
    }

    REQUIRE(sawCarrier);
    REQUIRE(sawWholeFile);
    auto const plainList = fixture.run({"lib", "relink"});
    REQUIRE(plainList.status == 0);
    CHECK(contains(plainList.out, "candidate disc.wav -> new.wav trackIds " + memberList));
    CHECK(contains(plainList.out, "candidate duplicate.wav -> new.wav"));
    CHECK_FALSE(contains(plainList.out, "candidate duplicate.wav -> new.wav trackIds"));
    auto const preview =
      fixture.run({"-O", "json", "lib", "relink", "--from", "disc.wav", "--to", "new.wav", "--dry-run"});
    REQUIRE(preview.status == 0);
    auto previewTree = parseYaml(preview.out);
    checkCarrierRelinkDocument(previewTree.rootref(), members, true);
    auto const plainPreview = fixture.run({"lib", "relink", "--from", "disc.wav", "--to", "new.wav", "--dry-run"});
    REQUIRE(plainPreview.status == 0);
    CHECK(contains(plainPreview.out, "relinked disc.wav -> new.wav trackIds " + memberList + " (dry-run)"));
    CHECK(fixture.run({"-O", "yaml", "track", "show"}).out == original.out);
    auto const applied = fixture.run({"-O", "json", "lib", "relink", "--from", "disc.wav", "--to", "new.wav"});
    REQUIRE(applied.status == 0);
    CHECK(applied.err.empty());
    auto appliedTree = parseYaml(applied.out);
    checkCarrierRelinkDocument(appliedTree.rootref(), members, false);
    auto const after = fixture.run({"-O", "yaml", "track", "show"});
    REQUIRE(after.status == 0);
    auto afterTree = parseYaml(after.out);
    auto const afterTracks = afterTree.rootref()["tracks"];

    for (auto const& member : members)
    {
      bool found = false;

      for (auto const track : afterTracks.children())
      {
        if (yaml::scalarView(track["id"]) != member.id)
        {
          continue;
        }

        found = true;
        CHECK(yaml::scalarView(track["uri"]) == "new.wav");
        REQUIRE(track.has_child("segment"));
        auto const segment = track["segment"];
        CHECK(yaml::scalarView(segment["ordinal"]) == member.ordinal);
        CHECK(yaml::scalarView(segment["startFrame"]) == member.startFrame);
        CHECK(yaml::scalarView(segment["frameCount"]) == member.frameCount);
        CHECK(yaml::scalarView(segment["endFrame"]) == member.endFrame);
      }

      REQUIRE(found);
    }

    auto const keptDuplicate = wholeTrackId(afterTracks, "duplicate.wav");
    auto const keptKeeper = wholeTrackId(afterTracks, "keeper.flac");
    CHECK(keptDuplicate == duplicateId);
    CHECK(keptKeeper == keeperId);
    checkManifest(fixture, "new.wav", members, totalFrames, "duplicate.wav", duplicateId, "keeper.flac", keeperId);
  }
} // namespace ao::cli::test
