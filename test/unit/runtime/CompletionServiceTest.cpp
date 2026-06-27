// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/TestUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/rt/TrackField.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
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
    library::test::addTrack(
      testLib.library(),
      library::test::TrackSpec{
        .title = "One", .tags = {"Rock", "Favorite"}, .customMetadata = {{"Mood", "Bright"}, {"ReplayGain", "-6"}}});
    library::test::addTrack(
      testLib.library(),
      library::test::TrackSpec{.title = "Two", .tags = {"Rock", "Live"}, .customMetadata = {{"Mood", "Dark"}}});

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
    library::test::addTrack(testLib.library(),
                            library::test::TrackSpec{.title = "One",
                                                     .artist = "Bach",
                                                     .album = "Goldberg",
                                                     .albumArtist = "Glenn Gould",
                                                     .genre = "Classical",
                                                     .composer = "Bach",
                                                     .work = "Variations"});
    library::test::addTrack(testLib.library(),
                            library::test::TrackSpec{.title = "Two",
                                                     .artist = "Bach",
                                                     .album = "Cello Suites",
                                                     .albumArtist = "Yo-Yo Ma",
                                                     .genre = "Classical",
                                                     .composer = "Bach",
                                                     .work = "Suites"});
    library::test::addTrack(testLib.library(),
                            library::test::TrackSpec{.title = "Three",
                                                     .artist = "Glass",
                                                     .album = "Glassworks",
                                                     .albumArtist = "Philip Glass",
                                                     .genre = "Minimal",
                                                     .composer = "Glass",
                                                     .work = "Glassworks"});

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
    library::test::addTrack(testLib.library(), library::test::TrackSpec{.title = "One", .tags = {"Rock"}});

    auto changes = LibraryChanges{};
    auto writer = LibraryWriter{testLib.library(), changes};
    auto service = CompletionService{testLib.library(), changes};

    CHECK(pairs(service.tags()) == std::vector<std::pair<std::string, std::uint32_t>>{{"Rock", 1}});

    auto const trackId =
      library::test::addTrack(testLib.library(), library::test::TrackSpec{.title = "Two", .tags = {"Jazz"}});
    // addTrack writes directly; drive a writer mutation so the change
    // notification fires and invalidates the completion cache.
    CHECK_FALSE(writer.updateMetadata(std::array{trackId}, MetadataPatch{.optTitle = "Two"}).mutatedIds.empty());

    CHECK(pairs(service.tags()) == std::vector<std::pair<std::string, std::uint32_t>>{
                                     {"Jazz", 1},
                                     {"Rock", 1},
                                   });
  }

  TEST_CASE("CompletionService - Invalidates Metadata Value Vocabularies On Track Mutation",
            "[runtime][unit][completion]")
  {
    auto testLib = TestMusicLibrary{};
    library::test::addTrack(
      testLib.library(),
      library::test::TrackSpec{.title = "One", .artist = "Bach", .album = "Goldberg", .work = "Variations"});

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

    auto const trackId = library::test::addTrack(
      testLib.library(),
      library::test::TrackSpec{.title = "Two", .artist = "Glass", .album = "Glassworks", .work = "Etudes"});
    // addTrack writes directly; drive a writer mutation so the change
    // notification fires and invalidates the completion cache.
    CHECK_FALSE(writer.updateMetadata(std::array{trackId}, MetadataPatch{.optTitle = "Two"}).mutatedIds.empty());

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
    library::test::addTrack(
      testLib.library(),
      library::test::TrackSpec{
        .title = "One", .artist = "Bach", .album = "Goldberg", .genre = "Classical", .work = "Variations"});

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

    auto const trackId = library::test::addTrack(
      testLib.library(),
      library::test::TrackSpec{.title = "Two", .artist = "Glass", .album = "Glassworks", .work = "Etudes"});
    // addTrack writes directly; drive a writer mutation so the change
    // notification fires and invalidates the completion cache.
    CHECK_FALSE(writer.updateMetadata(std::array{trackId}, MetadataPatch{.optTitle = "Two"}).mutatedIds.empty());

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
