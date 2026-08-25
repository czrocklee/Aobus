// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/completion/CompletionService.h>

#include "runtime/library/LibraryMutationService.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include "test/unit/runtime/library/LibraryMutationTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/i18n/IcuTextOrdering.h>
#include <ao/library/LibraryWrite.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/completion/CompletionAliasPolicy.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
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
    class RecordingCompletionAliasPolicy final : public CompletionAliasPolicy
    {
    public:
      Result<> makeAliasesInto(std::vector<std::string>& output, std::string_view const text) const override
      {
        _inputs.emplace_back(text);
        output.clear();

        if (text == "周杰倫")
        {
          output.emplace_back("zhoujielun");
        }
        else if (text == "王菲")
        {
          output.emplace_back("wangfei");
        }
        else if (text == "宇多田ヒカル")
        {
          output.emplace_back("hikaru");
        }
        else if (text == "音乐")
        {
          output.emplace_back("yinle");
        }

        return {};
      }

      std::size_t callCount(std::string_view const text) const
      {
        return static_cast<std::size_t>(std::ranges::count(_inputs, text));
      }

    private:
      mutable std::vector<std::string> _inputs;
    };

    std::vector<std::pair<std::string, std::uint32_t>> pairs(std::span<VocabularyEntry const> entries)
    {
      auto result = std::vector<std::pair<std::string, std::uint32_t>>{};

      for (auto const& entry : entries)
      {
        result.emplace_back(entry.value, entry.frequency);
      }

      return result;
    }

    std::vector<std::string> aliasValues(std::span<std::string const> aliases)
    {
      return {aliases.begin(), aliases.end()};
    }

    std::vector<std::pair<std::string, std::uint32_t>> sortedPairs(std::span<VocabularyEntry const> entries)
    {
      auto result = pairs(entries);
      std::ranges::sort(result);
      return result;
    }
  } // namespace

  TEST_CASE("CompletionService - locale policy orders equal-frequency vocabulary ties",
            "[runtime][unit][completion][collation]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    library::test::addTrackWithUniqueFixtureUri(
      libraryFixture.library(), library::test::TrackSpec{.title = "One", .artist = "ä"});
    library::test::addTrackWithUniqueFixtureUri(
      libraryFixture.library(), library::test::TrackSpec{.title = "Two", .artist = "z"});
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto germanPolicyRes = i18n::createIcuTextOrderingPolicy("de-DE");
    auto swedishPolicyRes = i18n::createIcuTextOrderingPolicy("sv-SE");
    REQUIRE(germanPolicyRes);
    REQUIRE(swedishPolicyRes);

    auto german = CompletionService{libraryFixture.library(), changes, germanPolicyRes->get()};
    auto swedish = CompletionService{libraryFixture.library(), changes, swedishPolicyRes->get()};

    CHECK(pairs(german.valuesFor(TrackField::Artist)) ==
          std::vector<std::pair<std::string, std::uint32_t>>{{"ä", 1}, {"z", 1}});
    CHECK(pairs(swedish.valuesFor(TrackField::Artist)) ==
          std::vector<std::pair<std::string, std::uint32_t>>{{"z", 1}, {"ä", 1}});
  }

  TEST_CASE("CompletionService - equal locale keys fall back to raw NFC bytes",
            "[runtime][unit][completion][collation]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    library::test::addTrackWithUniqueFixtureUri(
      libraryFixture.library(), library::test::TrackSpec{.title = "One", .tags = {"ＡＢＣ"}});
    library::test::addTrackWithUniqueFixtureUri(
      libraryFixture.library(), library::test::TrackSpec{.title = "Two", .tags = {"ABC"}});
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto policyRes = i18n::createIcuTextOrderingPolicy("en-US");
    REQUIRE(policyRes);
    auto service = CompletionService{libraryFixture.library(), changes, policyRes->get()};

    CHECK(pairs(service.tags()) == std::vector<std::pair<std::string, std::uint32_t>>{{"ABC", 1}, {"ＡＢＣ", 1}});
  }

  TEST_CASE("CompletionService - builds tag and custom-key vocabularies", "[runtime][unit][completion][vocabulary]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    library::test::addTrackWithUniqueFixtureUri(
      libraryFixture.library(),
      library::test::TrackSpec{
        .title = "One", .tags = {"Rock", "Favorite"}, .customMetadata = {{"Mood", "Bright"}, {"ReplayGain", "-6"}}});
    library::test::addTrackWithUniqueFixtureUri(
      libraryFixture.library(),
      library::test::TrackSpec{.title = "Two", .tags = {"Rock", "Live"}, .customMetadata = {{"Mood", "Dark"}}});

    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
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
    library::test::addTrackWithUniqueFixtureUri(libraryFixture.library(),
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
    library::test::addTrackWithUniqueFixtureUri(libraryFixture.library(),
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
    library::test::addTrackWithUniqueFixtureUri(libraryFixture.library(),
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

    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
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
    library::test::addTrackWithUniqueFixtureUri(libraryFixture.library(),
                                                library::test::TrackSpec{.title = "Shared",
                                                                         .artist = "Shared",
                                                                         .album = "Excluded Album",
                                                                         .conductor = "Excluded Conductor",
                                                                         .work = "Selected Work",
                                                                         .tags = {"Shared", "Tag Only"}});
    library::test::addTrackWithUniqueFixtureUri(libraryFixture.library(),
                                                library::test::TrackSpec{.title = "Other",
                                                                         .artist = "Shared",
                                                                         .album = "Another Excluded Album",
                                                                         .conductor = "Another Excluded Conductor",
                                                                         .work = "Selected Work",
                                                                         .tags = {"Tag Only"}});

    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
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

  TEST_CASE("CompletionService - one snapshot alias record serves every materialized vocabulary",
            "[runtime][unit][completion-alias][cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    library::test::addTrackWithUniqueFixtureUri(
      libraryFixture.library(),
      library::test::TrackSpec{.title = "周杰倫", .artist = "周杰倫", .albumArtist = "周杰倫", .tags = {"周杰倫"}});
    library::test::addTrackWithUniqueFixtureUri(
      libraryFixture.library(),
      library::test::TrackSpec{.title = "Other", .artist = "王菲", .albumArtist = "宇多田ヒカル", .tags = {"音乐"}});
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto policy = RecordingCompletionAliasPolicy{};
    auto service = CompletionService{libraryFixture.library(), changes, nullptr, &policy};

    auto const artists = service.valuesFor(TrackField::Artist);
    auto const shared = std::ranges::find(artists, std::string_view{"周杰倫"}, &VocabularyEntry::value);
    REQUIRE(shared != artists.end());
    REQUIRE(aliasValues(shared->aliases) == std::vector<std::string>{"zhoujielun"});
    auto const* const borrowedAliasData = shared->aliases.data();

    REQUIRE_FALSE(service.valuesFor(TrackField::AlbumArtist).empty());
    REQUIRE_FALSE(service.tags().empty());
    CHECK(shared->aliases.data() == borrowedAliasData);
    CHECK(aliasValues(shared->aliases) == std::vector<std::string>{"zhoujielun"});

    constexpr auto kFirstAggregate = std::to_array({TrackField::Title, TrackField::Artist});
    auto aggregate = service.aggregateValues({.fields = kFirstAggregate, .includeTags = true});
    auto aggregateShared = std::ranges::find(aggregate, std::string_view{"周杰倫"}, &VocabularyEntry::value);
    REQUIRE(aggregateShared != aggregate.end());
    CHECK(aggregateShared->frequency == 3);
    CHECK(aliasValues(aggregateShared->aliases) == std::vector<std::string>{"zhoujielun"});

    constexpr auto kSecondAggregate = std::to_array({TrackField::AlbumArtist, TrackField::Artist});
    aggregate = service.aggregateValues({.fields = kSecondAggregate, .includeTags = true});
    aggregateShared = std::ranges::find(aggregate, std::string_view{"周杰倫"}, &VocabularyEntry::value);
    REQUIRE(aggregateShared != aggregate.end());
    CHECK(aggregateShared->frequency == 3);
    CHECK(policy.callCount("周杰倫") == 1);
  }

  TEST_CASE("CompletionService - snapshot invalidation retires stale aliases",
            "[runtime][unit][completion-alias][cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = library::test::addTrackWithUniqueFixtureUri(
      libraryFixture.library(), library::test::TrackSpec{.title = "One", .artist = "周杰倫"});
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto policy = RecordingCompletionAliasPolicy{};
    auto service = CompletionService{libraryFixture.library(), changes, nullptr, &policy};

    auto artists = service.valuesFor(TrackField::Artist);
    REQUIRE(artists.size() == 1);
    REQUIRE(aliasValues(artists.front().aliases) == std::vector<std::string>{"zhoujielun"});

    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    REQUIRE(writerFixture.updateMetadata(std::array{trackId}, MetadataPatch{.optArtist = "王菲"}));

    artists = service.valuesFor(TrackField::Artist);
    REQUIRE(artists.size() == 1);
    CHECK(artists.front().value == "王菲");
    CHECK(aliasValues(artists.front().aliases) == std::vector<std::string>{"wangfei"});
    CHECK(policy.callCount("周杰倫") == 1);
    CHECK(policy.callCount("王菲") == 1);
  }

  TEST_CASE("CompletionService - one library snapshot serves every live vocabulary",
            "[runtime][unit][completion-vocabulary][cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    library::test::addTrackWithUniqueFixtureUri(libraryFixture.library(),
                                                library::test::TrackSpec{.title = "First Title",
                                                                         .artist = "First Artist",
                                                                         .work = "First Work",
                                                                         .tags = {"First Tag"},
                                                                         .customMetadata = {{"First Key", "Value"}}});
    auto const baselineRevision = [&]
    {
      auto const transaction = libraryFixture.library().readTransaction();
      return libraryFixture.library().libraryRevision(transaction);
    }();
    auto changesExecutor = ManualExecutor{};
    auto changes = LibraryChanges{changesExecutor, baselineRevision, "test-library"};
    auto service = CompletionService{libraryFixture.library(), changes};
    constexpr auto kAggregateFields = std::to_array({TrackField::Title, TrackField::Artist, TrackField::Work});

    REQUIRE(pairs(service.tags()) == std::vector<std::pair<std::string, std::uint32_t>>{{"First Tag", 1}});

    auto asyncRuntime = async::Runtime{changesExecutor};
    auto mutationService = LibraryMutationService{
      asyncRuntime.callbackExecutor(), library::test::requireWritableLibrary(libraryFixture.library()), changes};
    auto task = executeInteractiveMutation(
      mutationService.captureSubmission(),
      [&libraryFixture](library::LibraryWrite& write) -> Result<OperationOutcome<TrackId>>
      {
        auto const trackId = library::test::addTrackWithUniqueFixtureUri(libraryFixture.library(),
                                                                         write,
                                                                         library::test::TrackSpec{
                                                                           .title = "Second Title",
                                                                           .artist = "Second Artist",
                                                                           .work = "Second Work",
                                                                           .tags = {"Second Tag"},
                                                                           .customMetadata = {{"Second Key", "Value"}},
                                                                         });
        return Changed<TrackId>{.value = trackId, .changeSet = LibraryChangeSet{.tracksInserted = {trackId}}};
      });
    auto future = asyncRuntime.spawn(std::move(task));
    changesExecutor.checkQueued();

    // Storage is committed, but publication is still queued. Every vocabulary
    // must keep using the same already-built snapshot until phase two arrives.

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

    changesExecutor.runUntilIdle();
    auto executionRes = future.get();
    REQUIRE(executionRes);
    REQUIRE(executionRes->value != kInvalidTrackId);

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
    auto const originalId = library::test::addTrackWithUniqueFixtureUri(
      libraryFixture.library(), library::test::TrackSpec{.title = "Original", .artist = "Original Artist"});
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto service = CompletionService{libraryFixture.library(), changes};
    constexpr auto kFields = std::to_array({TrackField::Title, TrackField::Artist});
    auto vocabulary = [&] { return sortedPairs(service.aggregateValues({.fields = kFields})); };

    REQUIRE(vocabulary() == std::vector<std::pair<std::string, std::uint32_t>>{
                              {"Original", 1},
                              {"Original Artist", 1},
                            });

    SECTION("Insertion")
    {
      addTrackAndPublish(
        libraryFixture.library(), changes, library::test::TrackSpec{.title = "Inserted", .artist = "Inserted Artist"});

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
      REQUIRE(writerFixture.runTask(writerFixture.writer().deleteTrack(originalId)));
      CHECK(vocabulary().empty());
    }

    SECTION("Library reset")
    {
      addTrackAndPublishReset(
        libraryFixture.library(), changes, library::test::TrackSpec{.title = "Reset", .artist = "Reset Artist"});

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
    auto const trackId = library::test::addTrackWithUniqueFixtureUri(
      libraryFixture.library(), library::test::TrackSpec{.title = "One", .tags = {"Rock"}});

    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto service = CompletionService{libraryFixture.library(), changes};

    CHECK(pairs(service.tags()) == std::vector<std::pair<std::string, std::uint32_t>>{{"Rock", 1}});

    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto const tagsToAdd = std::array{std::string{"Jazz"}};
    auto const editRes = writerFixture.editTags(std::array{trackId}, tagsToAdd, {});
    REQUIRE(editRes);
    CHECK_FALSE(editRes->changes.empty());

    CHECK(pairs(service.tags()) == std::vector<std::pair<std::string, std::uint32_t>>{
                                     {"Jazz", 1},
                                     {"Rock", 1},
                                   });
  }

  TEST_CASE("CompletionService - invalidates metadata value vocabularies on track mutation",
            "[runtime][unit][completion-vocabulary][cache]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = library::test::addTrackWithUniqueFixtureUri(
      libraryFixture.library(),
      library::test::TrackSpec{
        .title = "One", .artist = "Bach", .album = "Goldberg", .conductor = "Carlos Kleiber", .work = "Variations"});

    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
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

    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto const updateRes = writerFixture.updateMetadata(std::array{trackId},
                                                        MetadataPatch{
                                                          .optArtist = "Glass",
                                                          .optAlbum = "Glassworks",
                                                          .optConductor = "Michael Riesman",
                                                          .optWork = "Etudes",
                                                        });
    REQUIRE(updateRes);
    CHECK_FALSE(updateRes->changes.empty());

    CHECK(pairs(service.valuesFor(TrackField::Artist)) == std::vector<std::pair<std::string, std::uint32_t>>{
                                                            {"Glass", 1},
                                                          });
    CHECK(pairs(service.valuesFor(TrackField::Album)) == std::vector<std::pair<std::string, std::uint32_t>>{
                                                           {"Glassworks", 1},
                                                         });
    CHECK(pairs(service.valuesFor(TrackField::Work)) == std::vector<std::pair<std::string, std::uint32_t>>{
                                                          {"Etudes", 1},
                                                        });
    CHECK(pairs(service.valuesFor(TrackField::Conductor)) == std::vector<std::pair<std::string, std::uint32_t>>{
                                                               {"Michael Riesman", 1},
                                                             });
  }

  TEST_CASE("CompletionService - deleting the last contributor removes every cached vocabulary value",
            "[runtime][regression][completion-vocabulary]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId =
      library::test::addTrackWithUniqueFixtureUri(libraryFixture.library(),
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

    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
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

    REQUIRE(writerFixture.runTask(writerFixture.writer().deleteTrack(trackId)));

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
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto service = CompletionService{libraryFixture.library(), changes};

    REQUIRE(service.tags().empty());
    REQUIRE(service.customKeys().empty());
    REQUIRE(service.valuesFor(TrackField::Artist).empty());
    REQUIRE(service.valuesFor(TrackField::Work).empty());

    bool libraryReset = false;

    SECTION("Track insertion")
    {
      libraryReset = false;
    }

    SECTION("Library reset")
    {
      libraryReset = true;
    }

    auto const spec = library::test::TrackSpec{.title = "Added",
                                               .artist = "Added Artist",
                                               .work = "Added Work",
                                               .tags = {"Added Tag"},
                                               .customMetadata = {{"Added Key", "Value"}}};

    if (libraryReset)
    {
      addTrackAndPublishReset(libraryFixture.library(), changes, spec);
    }
    else
    {
      addTrackAndPublish(libraryFixture.library(), changes, spec);
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
    auto const trackId = library::test::addTrackWithUniqueFixtureUri(
      libraryFixture.library(),
      library::test::TrackSpec{
        .title = "One", .artist = "Bach", .album = "Goldberg", .genre = "Classical", .work = "Variations"});

    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
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

    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto const updateRes = writerFixture.updateMetadata(std::array{trackId},
                                                        MetadataPatch{
                                                          .optArtist = "Glass",
                                                          .optAlbum = "Glassworks",
                                                          .optWork = "Etudes",
                                                        });
    REQUIRE(updateRes);
    CHECK_FALSE(updateRes->changes.empty());

    CHECK(pairs(service.valuesFor(TrackField::Work)) == std::vector<std::pair<std::string, std::uint32_t>>{
                                                          {"Etudes", 1},
                                                        });

    CHECK(pairs(service.valuesFor(TrackField::Composer)).empty());
    CHECK(pairs(service.valuesFor(TrackField::Title)).empty());
  }

  TEST_CASE("CompletionService - starts empty when owned by CoreRuntime", "[runtime][unit][completion][core-runtime]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtimePtr = makeStateOnlyRuntime(tempDir);

    CHECK(runtimePtr->completion().tags().empty());
  }
} // namespace ao::rt::test
