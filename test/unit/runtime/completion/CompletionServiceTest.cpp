// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "runtime/library/LibraryMutationService.h"
#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

    std::vector<std::pair<std::string, std::uint32_t>> sortedPairs(std::span<VocabularyEntry const> entries)
    {
      auto result = pairs(entries);
      std::ranges::sort(result);
      return result;
    }

    void publishCommittedChange(library::MusicLibrary& library, LibraryChanges& changes, LibraryChangeSet changeSet)
    {
      auto executor = InlineExecutor{};
      auto mutationService = LibraryMutationService{executor, library::test::requireWritableLibrary(library), changes};
      auto mutation = ao::test::requireValue(mutationService.beginInteractiveMutation());
      auto const commitResult = mutation.commit(std::move(changeSet));
      REQUIRE(commitResult);
    }
  } // namespace

  TEST_CASE("CompletionService - builds tag and custom-key vocabularies", "[runtime][unit][completion][vocabulary]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    library::test::addTrack(
      libraryFixture.library(),
      library::test::TrackSpec{
        .title = "One", .tags = {"Rock", "Favorite"}, .customMetadata = {{"Mood", "Bright"}, {"ReplayGain", "-6"}}});
    library::test::addTrack(
      libraryFixture.library(),
      library::test::TrackSpec{.title = "Two", .tags = {"Rock", "Live"}, .customMetadata = {{"Mood", "Dark"}}});

    auto changes = LibraryChanges{};
    auto service = CompletionService{libraryFixture.library(), changes};

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

  TEST_CASE("CompletionService - builds metadata value vocabularies for supported fields",
            "[runtime][unit][completion-vocabulary][value]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    library::test::addTrack(libraryFixture.library(),
                            library::test::TrackSpec{.title = "One",
                                                     .artist = "Bach",
                                                     .album = "Goldberg",
                                                     .albumArtist = "Glenn Gould",
                                                     .genre = "Classical",
                                                     .composer = "Bach",
                                                     .conductor = "Carlos Kleiber",
                                                     .ensemble = "Vienna Philharmonic",
                                                     .work = "Variations",
                                                     .movement = "Aria",
                                                     .soloist = "Glenn Gould"});
    library::test::addTrack(libraryFixture.library(),
                            library::test::TrackSpec{.title = "Two",
                                                     .artist = "Bach",
                                                     .album = "Cello Suites",
                                                     .albumArtist = "Yo-Yo Ma",
                                                     .genre = "Classical",
                                                     .composer = "Bach",
                                                     .conductor = "Carlos Kleiber",
                                                     .ensemble = "Staatskapelle Dresden",
                                                     .work = "Suites",
                                                     .movement = "Prelude",
                                                     .soloist = "Yo-Yo Ma"});
    library::test::addTrack(libraryFixture.library(),
                            library::test::TrackSpec{.title = "Three",
                                                     .artist = "Glass",
                                                     .album = "Glassworks",
                                                     .albumArtist = "Philip Glass",
                                                     .genre = "Minimal",
                                                     .composer = "Glass",
                                                     .conductor = "Michael Riesman",
                                                     .ensemble = "Philip Glass Ensemble",
                                                     .work = "Glassworks",
                                                     .movement = "Opening",
                                                     .soloist = "Philip Glass"});

    auto changes = LibraryChanges{};
    auto service = CompletionService{libraryFixture.library(), changes};

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
    CHECK(pairs(service.valuesFor(TrackField::Conductor)) == std::vector<std::pair<std::string, std::uint32_t>>{
                                                               {"Carlos Kleiber", 2},
                                                               {"Michael Riesman", 1},
                                                             });
    CHECK(pairs(service.valuesFor(TrackField::Ensemble)) == std::vector<std::pair<std::string, std::uint32_t>>{
                                                              {"Philip Glass Ensemble", 1},
                                                              {"Staatskapelle Dresden", 1},
                                                              {"Vienna Philharmonic", 1},
                                                            });
    CHECK(pairs(service.valuesFor(TrackField::Movement)) == std::vector<std::pair<std::string, std::uint32_t>>{
                                                              {"Aria", 1},
                                                              {"Opening", 1},
                                                              {"Prelude", 1},
                                                            });
    CHECK(pairs(service.valuesFor(TrackField::Soloist)) == std::vector<std::pair<std::string, std::uint32_t>>{
                                                             {"Glenn Gould", 1},
                                                             {"Philip Glass", 1},
                                                             {"Yo-Yo Ma", 1},
                                                           });

    CHECK(supportsTrackFieldValueCompletion(TrackField::Composer));
    CHECK(supportsTrackFieldValueCompletion(TrackField::Conductor));
    CHECK(supportsTrackFieldValueCompletion(TrackField::Ensemble));
    CHECK(supportsTrackFieldValueCompletion(TrackField::Movement));
    CHECK(supportsTrackFieldValueCompletion(TrackField::Soloist));
    CHECK_FALSE(supportsTrackFieldValueCompletion(TrackField::Title));
    CHECK_FALSE(supportsTrackFieldValueCompletion(TrackField::Year));
    CHECK(service.valuesFor(TrackField::Title).empty());
  }

  TEST_CASE("CompletionService - aggregates selected live track values and tags",
            "[runtime][unit][completion-vocabulary][aggregate]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    library::test::addTrack(libraryFixture.library(),
                            library::test::TrackSpec{.title = "Shared",
                                                     .artist = "Shared",
                                                     .album = "Excluded Album",
                                                     .conductor = "Excluded Conductor",
                                                     .work = "Selected Work",
                                                     .tags = {"Shared", "Tag Only"}});
    library::test::addTrack(libraryFixture.library(),
                            library::test::TrackSpec{.title = "Other",
                                                     .artist = "Shared",
                                                     .album = "Another Excluded Album",
                                                     .conductor = "Another Excluded Conductor",
                                                     .work = "Selected Work",
                                                     .tags = {"Tag Only"}});

    auto changes = LibraryChanges{};
    auto service = CompletionService{libraryFixture.library(), changes};
    constexpr auto kFields = std::to_array({TrackField::Work, TrackField::Artist, TrackField::Title});

    CHECK(sortedPairs(service.aggregateValues({.fields = kFields, .includeTags = true})) ==
          std::vector<std::pair<std::string, std::uint32_t>>{
            {"Other", 1},
            {"Selected Work", 2},
            {"Shared", 4},
            {"Tag Only", 2},
          });

    constexpr auto kTitlesOnly = std::to_array({TrackField::Title});
    CHECK(sortedPairs(service.aggregateValues({.fields = kTitlesOnly})) ==
          std::vector<std::pair<std::string, std::uint32_t>>{
            {"Other", 1},
            {"Shared", 1},
          });
  }

  TEST_CASE("CompletionService - one library snapshot serves every live vocabulary",
            "[runtime][unit][completion-vocabulary][cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    library::test::addTrack(libraryFixture.library(),
                            library::test::TrackSpec{.title = "First Title",
                                                     .artist = "First Artist",
                                                     .work = "First Work",
                                                     .tags = {"First Tag"},
                                                     .customMetadata = {{"First Key", "Value"}}});
    auto changes = LibraryChanges{};
    auto service = CompletionService{libraryFixture.library(), changes};
    constexpr auto kAggregateFields = std::to_array({TrackField::Title, TrackField::Artist, TrackField::Work});

    REQUIRE(pairs(service.tags()) == std::vector<std::pair<std::string, std::uint32_t>>{{"First Tag", 1}});

    // The fixture write intentionally bypasses LibraryChanges. Every other vocabulary must keep
    // using the already-built snapshot until the committed change is published below.
    auto const secondId =
      library::test::addTrack(libraryFixture.library(),
                              library::test::TrackSpec{.title = "Second Title",
                                                       .artist = "Second Artist",
                                                       .work = "Second Work",
                                                       .tags = {"Second Tag"},
                                                       .customMetadata = {{"Second Key", "Value"}}});

    CHECK(pairs(service.customKeys()) == std::vector<std::pair<std::string, std::uint32_t>>{{"First Key", 1}});
    CHECK(pairs(service.valuesFor(TrackField::Artist)) ==
          std::vector<std::pair<std::string, std::uint32_t>>{{"First Artist", 1}});
    CHECK(pairs(service.valuesFor(TrackField::Work)) ==
          std::vector<std::pair<std::string, std::uint32_t>>{{"First Work", 1}});
    CHECK(sortedPairs(service.aggregateValues({.fields = kAggregateFields, .includeTags = true})) ==
          std::vector<std::pair<std::string, std::uint32_t>>{
            {"First Artist", 1},
            {"First Tag", 1},
            {"First Title", 1},
            {"First Work", 1},
          });

    publishCommittedChange(libraryFixture.library(), changes, LibraryChangeSet{.tracksInserted = {secondId}});

    CHECK(pairs(service.tags()) == std::vector<std::pair<std::string, std::uint32_t>>{
                                     {"First Tag", 1},
                                     {"Second Tag", 1},
                                   });
    CHECK(pairs(service.customKeys()) == std::vector<std::pair<std::string, std::uint32_t>>{
                                           {"First Key", 1},
                                           {"Second Key", 1},
                                         });
    CHECK(pairs(service.valuesFor(TrackField::Artist)) == std::vector<std::pair<std::string, std::uint32_t>>{
                                                            {"First Artist", 1},
                                                            {"Second Artist", 1},
                                                          });
    CHECK(pairs(service.valuesFor(TrackField::Work)) == std::vector<std::pair<std::string, std::uint32_t>>{
                                                          {"First Work", 1},
                                                          {"Second Work", 1},
                                                        });
    CHECK(sortedPairs(service.aggregateValues({.fields = kAggregateFields, .includeTags = true})) ==
          std::vector<std::pair<std::string, std::uint32_t>>{
            {"First Artist", 1},
            {"First Tag", 1},
            {"First Title", 1},
            {"First Work", 1},
            {"Second Artist", 1},
            {"Second Tag", 1},
            {"Second Title", 1},
            {"Second Work", 1},
          });
  }

  TEST_CASE("CompletionService - invalidates aggregate values for every track change kind",
            "[runtime][unit][completion-vocabulary][cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const originalId = library::test::addTrack(
      libraryFixture.library(), library::test::TrackSpec{.title = "Original", .artist = "Original Artist"});
    auto changes = LibraryChanges{};
    auto service = CompletionService{libraryFixture.library(), changes};
    constexpr auto kFields = std::to_array({TrackField::Title, TrackField::Artist});
    auto vocabulary = [&] { return sortedPairs(service.aggregateValues({.fields = kFields})); };

    REQUIRE(vocabulary() == std::vector<std::pair<std::string, std::uint32_t>>{
                              {"Original", 1},
                              {"Original Artist", 1},
                            });

    SECTION("Insertion")
    {
      auto const insertedId = library::test::addTrack(
        libraryFixture.library(), library::test::TrackSpec{.title = "Inserted", .artist = "Inserted Artist"});
      publishCommittedChange(libraryFixture.library(), changes, LibraryChangeSet{.tracksInserted = {insertedId}});

      CHECK(vocabulary() == std::vector<std::pair<std::string, std::uint32_t>>{
                              {"Inserted", 1},
                              {"Inserted Artist", 1},
                              {"Original", 1},
                              {"Original Artist", 1},
                            });
    }

    SECTION("Mutation")
    {
      auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
      REQUIRE(writerFixture.updateMetadata(std::array{originalId}, MetadataPatch{.optTitle = "Changed"}));

      CHECK(vocabulary() == std::vector<std::pair<std::string, std::uint32_t>>{
                              {"Changed", 1},
                              {"Original Artist", 1},
                            });
    }

    SECTION("Deletion")
    {
      auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
      REQUIRE(writerFixture.writer().deleteTrack(originalId));
      CHECK(vocabulary().empty());
    }

    SECTION("Library reset")
    {
      auto const insertedId = library::test::addTrack(
        libraryFixture.library(), library::test::TrackSpec{.title = "Reset", .artist = "Reset Artist"});
      publishCommittedChange(
        libraryFixture.library(), changes, LibraryChangeSet{.libraryReset = true, .tracksInserted = {insertedId}});

      CHECK(vocabulary() == std::vector<std::pair<std::string, std::uint32_t>>{
                              {"Original", 1},
                              {"Original Artist", 1},
                              {"Reset", 1},
                              {"Reset Artist", 1},
                            });
    }
  }

  TEST_CASE("CompletionService - invalidates tag snapshots on track mutation",
            "[runtime][unit][completion-vocabulary][cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    library::test::addTrack(libraryFixture.library(), library::test::TrackSpec{.title = "One", .tags = {"Rock"}});

    auto changes = LibraryChanges{};
    auto service = CompletionService{libraryFixture.library(), changes};

    CHECK(pairs(service.tags()) == std::vector<std::pair<std::string, std::uint32_t>>{{"Rock", 1}});

    auto const trackId =
      library::test::addTrack(libraryFixture.library(), library::test::TrackSpec{.title = "Two", .tags = {"Jazz"}});
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    // addTrack writes directly; drive a writer mutation so the change
    // notification fires and invalidates the completion cache.
    auto const updateResult =
      writerFixture.updateMetadata(std::array{trackId}, MetadataPatch{.optTitle = "Two Updated"});
    REQUIRE(updateResult);
    CHECK_FALSE(updateResult->changes.empty());

    CHECK(pairs(service.tags()) == std::vector<std::pair<std::string, std::uint32_t>>{
                                     {"Jazz", 1},
                                     {"Rock", 1},
                                   });
  }

  TEST_CASE("CompletionService - invalidates metadata value vocabularies on track mutation",
            "[runtime][unit][completion-vocabulary][cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    library::test::addTrack(
      libraryFixture.library(),
      library::test::TrackSpec{
        .title = "One", .artist = "Bach", .album = "Goldberg", .conductor = "Carlos Kleiber", .work = "Variations"});

    auto changes = LibraryChanges{};
    auto service = CompletionService{libraryFixture.library(), changes};

    CHECK(pairs(service.valuesFor(TrackField::Artist)) == std::vector<std::pair<std::string, std::uint32_t>>{
                                                            {"Bach", 1},
                                                          });
    CHECK(pairs(service.valuesFor(TrackField::Album)) == std::vector<std::pair<std::string, std::uint32_t>>{
                                                           {"Goldberg", 1},
                                                         });
    CHECK(pairs(service.valuesFor(TrackField::Work)) == std::vector<std::pair<std::string, std::uint32_t>>{
                                                          {"Variations", 1},
                                                        });
    CHECK(pairs(service.valuesFor(TrackField::Conductor)) == std::vector<std::pair<std::string, std::uint32_t>>{
                                                               {"Carlos Kleiber", 1},
                                                             });

    auto const trackId = library::test::addTrack(
      libraryFixture.library(),
      library::test::TrackSpec{
        .title = "Two", .artist = "Glass", .album = "Glassworks", .conductor = "Michael Riesman", .work = "Etudes"});
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    // addTrack writes directly; drive a writer mutation so the change
    // notification fires and invalidates the completion cache.
    auto const updateResult =
      writerFixture.updateMetadata(std::array{trackId}, MetadataPatch{.optTitle = "Two Updated"});
    REQUIRE(updateResult);
    CHECK_FALSE(updateResult->changes.empty());

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
    CHECK(pairs(service.valuesFor(TrackField::Conductor)) == std::vector<std::pair<std::string, std::uint32_t>>{
                                                               {"Carlos Kleiber", 1},
                                                               {"Michael Riesman", 1},
                                                             });
  }

  TEST_CASE("CompletionService - deleting the last contributor removes every cached vocabulary value",
            "[runtime][regression][completion-vocabulary]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = library::test::addTrack(libraryFixture.library(),
                                                 library::test::TrackSpec{.title = "Only Track",
                                                                          .artist = "Only Artist",
                                                                          .album = "Only Album",
                                                                          .albumArtist = "Only Album Artist",
                                                                          .genre = "Only Genre",
                                                                          .composer = "Only Composer",
                                                                          .conductor = "Only Conductor",
                                                                          .ensemble = "Only Ensemble",
                                                                          .work = "Only Work",
                                                                          .movement = "Only Movement",
                                                                          .soloist = "Only Soloist",
                                                                          .tags = {"Only Tag"},
                                                                          .customMetadata = {{"Only Key", "Value"}}});

    auto changes = LibraryChanges{};
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto service = CompletionService{libraryFixture.library(), changes};
    constexpr auto kValueFields = std::to_array({TrackField::Artist,
                                                 TrackField::Album,
                                                 TrackField::AlbumArtist,
                                                 TrackField::Genre,
                                                 TrackField::Composer,
                                                 TrackField::Conductor,
                                                 TrackField::Ensemble,
                                                 TrackField::Work,
                                                 TrackField::Movement,
                                                 TrackField::Soloist});

    REQUIRE_FALSE(service.tags().empty());
    REQUIRE_FALSE(service.customKeys().empty());

    for (auto const field : kValueFields)
    {
      REQUIRE_FALSE(service.valuesFor(field).empty());
    }

    REQUIRE(writerFixture.writer().deleteTrack(trackId));

    CHECK(service.tags().empty());
    CHECK(service.customKeys().empty());

    for (auto const field : kValueFields)
    {
      CHECK(service.valuesFor(field).empty());
    }
  }

  TEST_CASE("CompletionService - insertion and reset changes invalidate every vocabulary kind",
            "[runtime][unit][completion-vocabulary][cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = LibraryChanges{};
    auto service = CompletionService{libraryFixture.library(), changes};

    REQUIRE(service.tags().empty());
    REQUIRE(service.customKeys().empty());
    REQUIRE(service.valuesFor(TrackField::Artist).empty());
    REQUIRE(service.valuesFor(TrackField::Work).empty());

    auto const trackId = library::test::addTrack(libraryFixture.library(),
                                                 library::test::TrackSpec{.title = "Added",
                                                                          .artist = "Added Artist",
                                                                          .work = "Added Work",
                                                                          .tags = {"Added Tag"},
                                                                          .customMetadata = {{"Added Key", "Value"}}});

    SECTION("Track insertion")
    {
      publishCommittedChange(libraryFixture.library(), changes, LibraryChangeSet{.tracksInserted = {trackId}});
    }

    SECTION("Library reset")
    {
      publishCommittedChange(libraryFixture.library(), changes, LibraryChangeSet{.libraryReset = true});
    }

    CHECK(pairs(service.tags()) == std::vector<std::pair<std::string, std::uint32_t>>{{"Added Tag", 1}});
    CHECK(pairs(service.customKeys()) == std::vector<std::pair<std::string, std::uint32_t>>{{"Added Key", 1}});
    CHECK(pairs(service.valuesFor(TrackField::Artist)) ==
          std::vector<std::pair<std::string, std::uint32_t>>{{"Added Artist", 1}});
    CHECK(pairs(service.valuesFor(TrackField::Work)) ==
          std::vector<std::pair<std::string, std::uint32_t>>{{"Added Work", 1}});
  }

  TEST_CASE("CompletionService - lazily rebuilds one dirty snapshot before deriving field values",
            "[runtime][unit][completion-vocabulary][cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    library::test::addTrack(
      libraryFixture.library(),
      library::test::TrackSpec{
        .title = "One", .artist = "Bach", .album = "Goldberg", .genre = "Classical", .work = "Variations"});

    auto changes = LibraryChanges{};
    auto service = CompletionService{libraryFixture.library(), changes};

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
      libraryFixture.library(),
      library::test::TrackSpec{.title = "Two", .artist = "Glass", .album = "Glassworks", .work = "Etudes"});
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    // addTrack writes directly; drive a writer mutation so the change
    // notification fires and invalidates the completion cache.
    auto const updateResult =
      writerFixture.updateMetadata(std::array{trackId}, MetadataPatch{.optTitle = "Two Updated"});
    REQUIRE(updateResult);
    CHECK_FALSE(updateResult->changes.empty());

    CHECK(pairs(service.valuesFor(TrackField::Work)) == std::vector<std::pair<std::string, std::uint32_t>>{
                                                          {"Etudes", 1},
                                                          {"Variations", 1},
                                                        });

    CHECK(pairs(service.valuesFor(TrackField::Composer)).empty());
    CHECK(pairs(service.valuesFor(TrackField::Title)).empty());
  }

  TEST_CASE("CompletionService - starts empty when owned by CoreRuntime", "[runtime][unit][completion][core-runtime]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);

    CHECK(runtime.completion().tags().empty());
  }
} // namespace ao::rt::test
