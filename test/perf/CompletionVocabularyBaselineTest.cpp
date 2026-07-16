// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "runtime/library/LibraryMutationService.h"
#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include <ao/library/DictionaryStore.h>
#include <ao/rt/Log.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/uimodel/library/track/TrackFilterCompleter.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    constexpr std::size_t kTrackCount = 50000;
    constexpr std::size_t kMeasuredRuns = 5;
    constexpr std::size_t kLookupIterations = 100;
    constexpr std::size_t kLookupLimit = 8;
    constexpr auto kRepresentativeQuickFilterFields = std::to_array({
      TrackField::Title,
      TrackField::Artist,
      TrackField::Album,
      TrackField::AlbumArtist,
      TrackField::Genre,
      TrackField::Composer,
      TrackField::Work,
    });

    struct Timing final
    {
      std::int64_t median = 0;
      std::int64_t percentile95 = 0;
      std::size_t resultSize = 0;
      std::uint64_t checksum = 0;
    };

    struct VocabularyTiming final
    {
      Timing snapshotAndAggregate;
      Timing tags;
      Timing customKeys;
      Timing artist;
      Timing work;
    };

    Timing summarize(std::vector<std::int64_t> samples, std::size_t resultSize, std::uint64_t checksum = 0)
    {
      std::ranges::sort(samples);
      return Timing{
        .median = samples[samples.size() / 2],
        .percentile95 = samples.back(),
        .resultSize = resultSize,
        .checksum = checksum,
      };
    }

    VocabularyTiming measureVocabularyRebuilds(CompletionService& service, LibraryMutationService& mutationService)
    {
      auto aggregateSamples = std::vector<std::int64_t>{};
      auto tagSamples = std::vector<std::int64_t>{};
      auto customKeySamples = std::vector<std::int64_t>{};
      auto artistSamples = std::vector<std::int64_t>{};
      auto workSamples = std::vector<std::int64_t>{};
      aggregateSamples.reserve(kMeasuredRuns);
      tagSamples.reserve(kMeasuredRuns);
      customKeySamples.reserve(kMeasuredRuns);
      artistSamples.reserve(kMeasuredRuns);
      workSamples.reserve(kMeasuredRuns);
      std::size_t aggregateSize = 0;
      std::size_t tagSize = 0;
      std::size_t customKeySize = 0;
      std::size_t artistSize = 0;
      std::size_t workSize = 0;

      auto timedSize = [](auto&& read)
      {
        auto const start = std::chrono::steady_clock::now();
        auto const size = read().size();
        auto const elapsed =
          std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
        return std::pair{size, elapsed};
      };

      for (std::size_t run = 0; run <= kMeasuredRuns; ++run)
      {
        auto mutation = ao::test::requireValue(mutationService.beginInteractiveMutation());
        REQUIRE(mutation.commit(LibraryChangeSet{.libraryReset = true}));
        auto const [currentAggregateSize, aggregateElapsed] = timedSize(
          [&]
          {
            return service.aggregateValues(TrackValueVocabularySpec{
              .fields = kRepresentativeQuickFilterFields,
              .includeTags = true,
            });
          });
        auto const [currentTagSize, tagElapsed] = timedSize([&] { return service.tags(); });
        auto const [currentCustomKeySize, customKeyElapsed] = timedSize([&] { return service.customKeys(); });
        auto const [currentArtistSize, artistElapsed] =
          timedSize([&] { return service.valuesFor(TrackField::Artist); });
        auto const [currentWorkSize, workElapsed] = timedSize([&] { return service.valuesFor(TrackField::Work); });
        aggregateSize = currentAggregateSize;
        tagSize = currentTagSize;
        customKeySize = currentCustomKeySize;
        artistSize = currentArtistSize;
        workSize = currentWorkSize;

        if (run != 0)
        {
          aggregateSamples.push_back(aggregateElapsed);
          tagSamples.push_back(tagElapsed);
          customKeySamples.push_back(customKeyElapsed);
          artistSamples.push_back(artistElapsed);
          workSamples.push_back(workElapsed);
        }
      }

      return VocabularyTiming{
        .snapshotAndAggregate = summarize(std::move(aggregateSamples), aggregateSize),
        .tags = summarize(std::move(tagSamples), tagSize),
        .customKeys = summarize(std::move(customKeySamples), customKeySize),
        .artist = summarize(std::move(artistSamples), artistSize),
        .work = summarize(std::move(workSamples), workSize),
      };
    }

    std::uint64_t completionChecksum(std::optional<CompletionResult> const& optResult)
    {
      if (!optResult)
      {
        return 1;
      }

      auto checksum = static_cast<std::uint64_t>(optResult->items.size() + 1);

      for (auto const& item : optResult->items)
      {
        checksum += item.displayText.size();
        checksum += item.detail.size();
        checksum += item.rank;
      }

      return checksum;
    }

    Timing measureLookups(uimodel::TrackFilterCompleter& completer, std::string_view prefix)
    {
      auto samples = std::vector<std::int64_t>{};
      samples.reserve(kMeasuredRuns);
      std::uint64_t checksum = 0;
      std::size_t resultSize = 0;

      for (std::size_t run = 0; run <= kMeasuredRuns; ++run)
      {
        auto const start = std::chrono::steady_clock::now();
        std::uint64_t runChecksum = 0;

        for (std::size_t iteration = 0; iteration < kLookupIterations; ++iteration)
        {
          auto const optResult = completer.complete(prefix, prefix.size(), kLookupLimit);
          resultSize = optResult ? optResult->items.size() : 0;
          runChecksum += completionChecksum(optResult);
        }

        auto const elapsed =
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();

        if (run != 0)
        {
          samples.push_back(elapsed / static_cast<std::int64_t>(kLookupIterations));
          checksum += runChecksum;
        }
      }

      return summarize(std::move(samples), resultSize, checksum);
    }
  } // namespace

  // Log-only production baseline: elapsed-time thresholds would be machine dependent.
  // The representative cardinalities populate every Quick-filter field, tags, and
  // additional non-search fields so the dictionary is larger than the live aggregate.
  TEST_CASE("CompletionVocabularyBaseline - shared snapshot rebuild and Quick-filter lookup at 50k tracks",
            "[perf][unit][completion-vocabulary][baseline]")
  {
    Log::initialize(LogLevel::Info);
    auto libraryFixture = MusicLibraryFixture{};

    for (std::size_t index = 0; index < kTrackCount; ++index)
    {
      libraryFixture.addTrack(library::test::TrackSpec{
        .title = std::format("Track {:05}", index),
        .artist = std::format("Artist {:05}", index % (kTrackCount / 10)),
        .album = std::format("Album {:05}", index % (kTrackCount / 5)),
        .albumArtist = std::format("Album Artist {:04}", index % (kTrackCount / 50)),
        .genre = std::format("Genre {:02}", index % 50),
        .composer = std::format("Composer {:04}", index % (kTrackCount / 20)),
        .conductor = std::format("Conductor {:03}", index % (kTrackCount / 100)),
        .ensemble = std::format("Ensemble {:04}", index % (kTrackCount / 50)),
        .work = std::format("Work {:05}", index % (kTrackCount / 2)),
        .movement = std::format("Movement {:05}", index % (kTrackCount / 5)),
        .soloist = std::format("Soloist {:05}", index % (kTrackCount / 10)),
        .tags =
          {
            std::format("Tag {:02}", index % 100),
            std::format("Mood {:02}", index % 20),
          },
        .customMetadata = {{std::format("Custom Key {:02}", index % 20), "Value"}},
      });
    }

    auto changes = LibraryChanges{};
    auto executor = InlineExecutor{};
    auto mutationService =
      LibraryMutationService{executor, library::test::requireWritableLibrary(libraryFixture.library()), changes};
    auto service = CompletionService{libraryFixture.library(), changes};
    auto const vocabulary = measureVocabularyRebuilds(service, mutationService);

    APP_LOG_INFO("=== Shared completion vocabulary snapshot: {} tracks ===", kTrackCount);
    APP_LOG_INFO("  rebuild + aggregate: median/p95 {} / {} us, {} values from {} dictionary entries",
                 vocabulary.snapshotAndAggregate.median,
                 vocabulary.snapshotAndAggregate.percentile95,
                 vocabulary.snapshotAndAggregate.resultSize,
                 libraryFixture.library().dictionary().size());
    APP_LOG_INFO("  in-memory tags:       median/p95 {} / {} us, {} values",
                 vocabulary.tags.median,
                 vocabulary.tags.percentile95,
                 vocabulary.tags.resultSize);
    APP_LOG_INFO("  in-memory custom keys: median/p95 {} / {} us, {} values",
                 vocabulary.customKeys.median,
                 vocabulary.customKeys.percentile95,
                 vocabulary.customKeys.resultSize);
    APP_LOG_INFO("  in-memory artist:     median/p95 {} / {} us, {} values",
                 vocabulary.artist.median,
                 vocabulary.artist.percentile95,
                 vocabulary.artist.resultSize);
    APP_LOG_INFO("  in-memory work:       median/p95 {} / {} us, {} values",
                 vocabulary.work.median,
                 vocabulary.work.percentile95,
                 vocabulary.work.resultSize);

    CHECK(vocabulary.tags.resultSize == 120);
    CHECK(vocabulary.customKeys.resultSize == 20);
    CHECK(vocabulary.artist.resultSize == kTrackCount / 10);
    CHECK(vocabulary.work.resultSize == kTrackCount / 2);
    REQUIRE(vocabulary.snapshotAndAggregate.resultSize == 93670);

    auto completer = uimodel::TrackFilterCompleter{service};
    constexpr auto kLookupPrefixes = std::to_array<std::string_view>({
      "Track",
      "\"artist 049",
      "\"Work 2499",
      "missing",
    });

    APP_LOG_INFO("=== Cached Quick-filter lookup: {} iterations per sample ===", kLookupIterations);

    for (auto const prefix : kLookupPrefixes)
    {
      auto const timing = measureLookups(completer, prefix);
      APP_LOG_INFO(
        "  '{}': {} candidates; median/p95 {} / {} ns", prefix, timing.resultSize, timing.median, timing.percentile95);
      CHECK(timing.checksum != 0);

      if (prefix == "missing")
      {
        CHECK(timing.resultSize == 0);
      }
      else
      {
        CHECK(timing.resultSize == kLookupLimit);
      }
    }
  }
} // namespace ao::rt::test
