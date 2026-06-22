// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TestUtils.h"
#include "test/unit/TestUtils.h"
#include <ao/Type.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    struct CompletionTrackSpec final
    {
      TrackSpec track;
      std::vector<std::string> tags{};
      std::vector<std::pair<std::string, std::string>> custom{};
    };

    TrackId addCompletionTrack(TestMusicLibrary& testLib, CompletionTrackSpec const& spec)
    {
      auto txn = testLib.library().writeTransaction();
      auto writer = testLib.library().tracks().writer(txn);
      auto builder = library::TrackBuilder::createNew();

      builder.metadata()
        .title(spec.track.title)
        .artist(spec.track.artist)
        .album(spec.track.album)
        .albumArtist(spec.track.albumArtist)
        .genre(spec.track.genre)
        .composer(spec.track.composer)
        .work(spec.track.work)
        .movement(spec.track.movement)
        .year(spec.track.year)
        .discNumber(spec.track.discNumber)
        .trackNumber(spec.track.trackNumber)
        .movementNumber(spec.track.movementNumber)
        .movementTotal(spec.track.movementTotal);
      builder.property()
        .uri("/tmp/completion.flac")
        .duration(spec.track.duration)
        .bitrate(Bitrate{320000})
        .sampleRate(SampleRate{44100})
        .channels(Channels{2})
        .bitDepth(BitDepth{16});

      for (auto const& tag : spec.tags)
      {
        builder.tags().add(tag);
      }

      for (auto const& [key, value] : spec.custom)
      {
        builder.customMetadata().add(key, value);
      }

      auto hotData = builder.serializeHot(txn, testLib.library().dictionary());
      REQUIRE(hotData);
      auto coldData = builder.serializeCold(txn, testLib.library().dictionary(), testLib.library().resources());
      REQUIRE(coldData);
      auto [id, _] = ao::test::requireValue(writer.createHotCold(*hotData, *coldData));
      txn.commit();
      return id;
    }

    std::vector<std::pair<std::string, std::uint32_t>> pairs(std::span<VocabularyEntry const> entries)
    {
      auto result = std::vector<std::pair<std::string, std::uint32_t>>{};

      for (auto const& entry : entries)
      {
        result.emplace_back(entry.value, entry.frequency);
      }

      return result;
    }
  } // namespace

  TEST_CASE("CompletionService - Builds Tag And Custom Key Vocabularies", "[runtime][unit][completion]")
  {
    auto testLib = TestMusicLibrary{};
    addCompletionTrack(testLib,
                       CompletionTrackSpec{
                         .track = TrackSpec{.title = "One"},
                         .tags = {"Rock", "Favorite"},
                         .custom = {{"Mood", "Bright"}, {"ReplayGain", "-6"}},
                       });
    addCompletionTrack(testLib,
                       CompletionTrackSpec{
                         .track = TrackSpec{.title = "Two"},
                         .tags = {"Rock", "Live"},
                         .custom = {{"Mood", "Dark"}},
                       });

    auto changes = LibraryChanges{};
    auto service = CompletionService{testLib.library(), changes};

    CHECK(pairs(service.tags()) == std::vector<std::pair<std::string, std::uint32_t>>{
                                     {"Rock", 2},
                                     {"Favorite", 1},
                                     {"Live", 1},
                                   });
    CHECK(pairs(service.customKeys()) == std::vector<std::pair<std::string, std::uint32_t>>{
                                           {"Mood", 2},
                                           {"ReplayGain", 1},
                                         });
  }

  TEST_CASE("CompletionService - Builds Metadata Value Vocabularies For Whitelisted Fields",
            "[runtime][unit][completion]")
  {
    auto testLib = TestMusicLibrary{};
    addCompletionTrack(testLib,
                       CompletionTrackSpec{.track = TrackSpec{.title = "One",
                                                              .artist = "Bach",
                                                              .album = "Goldberg",
                                                              .albumArtist = "Glenn Gould",
                                                              .genre = "Classical",
                                                              .composer = "Bach",
                                                              .work = "Variations"}});
    addCompletionTrack(testLib,
                       CompletionTrackSpec{.track = TrackSpec{.title = "Two",
                                                              .artist = "Bach",
                                                              .album = "Cello Suites",
                                                              .albumArtist = "Yo-Yo Ma",
                                                              .genre = "Classical",
                                                              .composer = "Bach",
                                                              .work = "Suites"}});
    addCompletionTrack(testLib,
                       CompletionTrackSpec{.track = TrackSpec{.title = "Three",
                                                              .artist = "Glass",
                                                              .album = "Glassworks",
                                                              .albumArtist = "Philip Glass",
                                                              .genre = "Minimal",
                                                              .composer = "Glass",
                                                              .work = "Glassworks"}});

    auto changes = LibraryChanges{};
    auto service = CompletionService{testLib.library(), changes};

    CHECK(pairs(service.valuesFor(TrackField::Artist)) == std::vector<std::pair<std::string, std::uint32_t>>{
                                                            {"Bach", 2},
                                                            {"Glass", 1},
                                                          });
    CHECK(pairs(service.valuesFor(TrackField::Album)) == std::vector<std::pair<std::string, std::uint32_t>>{
                                                           {"Cello Suites", 1},
                                                           {"Glassworks", 1},
                                                           {"Goldberg", 1},
                                                         });
    CHECK(pairs(service.valuesFor(TrackField::Work)) == std::vector<std::pair<std::string, std::uint32_t>>{
                                                          {"Glassworks", 1},
                                                          {"Suites", 1},
                                                          {"Variations", 1},
                                                        });

    CHECK(trackFieldSupportsValueCompletion(TrackField::Composer));
    CHECK_FALSE(trackFieldSupportsValueCompletion(TrackField::Title));
    CHECK_FALSE(trackFieldSupportsValueCompletion(TrackField::Movement));
    CHECK_FALSE(trackFieldSupportsValueCompletion(TrackField::Year));
    CHECK(service.valuesFor(TrackField::Title).empty());
  }

  TEST_CASE("CompletionService - Invalidates Snapshots On Track Mutation", "[runtime][unit][completion]")
  {
    auto testLib = TestMusicLibrary{};
    addCompletionTrack(testLib, CompletionTrackSpec{.track = TrackSpec{.title = "One"}, .tags = {"Rock"}});

    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{testLib.library(), changes};
    auto service = CompletionService{testLib.library(), changes};

    CHECK(pairs(service.tags()) == std::vector<std::pair<std::string, std::uint32_t>>{{"Rock", 1}});

    auto const trackId =
      addCompletionTrack(testLib, CompletionTrackSpec{.track = TrackSpec{.title = "Two"}, .tags = {"Jazz"}});
    // addCompletionTrack writes directly; drive a writer mutation so the change
    // notification fires and invalidates the completion cache.
    REQUIRE_FALSE(writer.updateMetadata(std::array{trackId}, MetadataPatch{.optTitle = "Two"}).mutatedIds.empty());

    CHECK(pairs(service.tags()) == std::vector<std::pair<std::string, std::uint32_t>>{
                                     {"Jazz", 1},
                                     {"Rock", 1},
                                   });
  }

  TEST_CASE("CompletionService - Invalidates Metadata Value Vocabularies On Track Mutation",
            "[runtime][unit][completion]")
  {
    auto testLib = TestMusicLibrary{};
    addCompletionTrack(
      testLib,
      CompletionTrackSpec{.track =
                            TrackSpec{.title = "One", .artist = "Bach", .album = "Goldberg", .work = "Variations"}});

    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{testLib.library(), changes};
    auto service = CompletionService{testLib.library(), changes};

    CHECK(pairs(service.valuesFor(TrackField::Artist)) == std::vector<std::pair<std::string, std::uint32_t>>{
                                                            {"Bach", 1},
                                                          });
    CHECK(pairs(service.valuesFor(TrackField::Album)) == std::vector<std::pair<std::string, std::uint32_t>>{
                                                           {"Goldberg", 1},
                                                         });
    CHECK(pairs(service.valuesFor(TrackField::Work)) == std::vector<std::pair<std::string, std::uint32_t>>{
                                                          {"Variations", 1},
                                                        });

    auto const trackId = addCompletionTrack(
      testLib,
      CompletionTrackSpec{.track =
                            TrackSpec{.title = "Two", .artist = "Glass", .album = "Glassworks", .work = "Etudes"}});
    // addCompletionTrack writes directly; drive a writer mutation so the change
    // notification fires and invalidates the completion cache.
    REQUIRE_FALSE(writer.updateMetadata(std::array{trackId}, MetadataPatch{.optTitle = "Two"}).mutatedIds.empty());

    CHECK(pairs(service.valuesFor(TrackField::Artist)) == std::vector<std::pair<std::string, std::uint32_t>>{
                                                            {"Bach", 1},
                                                            {"Glass", 1},
                                                          });
    CHECK(pairs(service.valuesFor(TrackField::Album)) == std::vector<std::pair<std::string, std::uint32_t>>{
                                                           {"Glassworks", 1},
                                                           {"Goldberg", 1},
                                                         });
    CHECK(pairs(service.valuesFor(TrackField::Work)) == std::vector<std::pair<std::string, std::uint32_t>>{
                                                          {"Etudes", 1},
                                                          {"Variations", 1},
                                                        });
  }

  TEST_CASE("CompletionService - Lazily Rebuilds Dirty Value Vocabularies", "[runtime][unit][completion]")
  {
    auto testLib = TestMusicLibrary{};
    addCompletionTrack(
      testLib,
      CompletionTrackSpec{
        .track = TrackSpec{
          .title = "One", .artist = "Bach", .album = "Goldberg", .genre = "Classical", .work = "Variations"}});

    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{testLib.library(), changes};
    auto service = CompletionService{testLib.library(), changes};

    CHECK(pairs(service.valuesFor(TrackField::Artist)) == std::vector<std::pair<std::string, std::uint32_t>>{
                                                            {"Bach", 1},
                                                          });
    CHECK(pairs(service.valuesFor(TrackField::Album)) == std::vector<std::pair<std::string, std::uint32_t>>{
                                                           {"Goldberg", 1},
                                                         });
    CHECK(pairs(service.valuesFor(TrackField::Genre)) == std::vector<std::pair<std::string, std::uint32_t>>{
                                                           {"Classical", 1},
                                                         });
    CHECK(pairs(service.valuesFor(TrackField::Work)) == std::vector<std::pair<std::string, std::uint32_t>>{
                                                          {"Variations", 1},
                                                        });

    auto const trackId = addCompletionTrack(
      testLib,
      CompletionTrackSpec{.track =
                            TrackSpec{.title = "Two", .artist = "Glass", .album = "Glassworks", .work = "Etudes"}});
    // addCompletionTrack writes directly; drive a writer mutation so the change
    // notification fires and invalidates the completion cache.
    REQUIRE_FALSE(writer.updateMetadata(std::array{trackId}, MetadataPatch{.optTitle = "Two"}).mutatedIds.empty());

    CHECK(pairs(service.valuesFor(TrackField::Work)) == std::vector<std::pair<std::string, std::uint32_t>>{
                                                          {"Etudes", 1},
                                                          {"Variations", 1},
                                                        });

    CHECK(pairs(service.valuesFor(TrackField::Composer)).empty());
    CHECK(pairs(service.valuesFor(TrackField::Title)).empty());
  }

  TEST_CASE("CompletionService - CoreRuntime Owns Completion Service", "[runtime][unit][completion]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);

    CHECK(runtime.completion().tags().empty());
  }
} // namespace ao::rt::test
