// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors
//
// Phase 0 baseline measurement — synthetic data, no fixed pass/fail thresholds.

#include "TestUtils.h"
#include <ao/Type.h>
#include <ao/library/TrackStore.h>
#include <ao/query/ExecutionPlan.h>
#include <ao/query/Field.h>
#include <ao/query/PlanEvaluator.h>
#include <ao/rt/SmartListEvaluator.h>
#include <ao/rt/SmartListSource.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackListProjection.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/TrackSource.h>
#include <ao/utility/Log.h>

#include <boost/unordered/unordered_flat_map.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <numeric>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    struct Timings final
    {
      std::chrono::milliseconds createProjectionDuration{};
      std::chrono::milliseconds setTitleSortDuration{};
      std::chrono::milliseconds evaluateMembersDuration{};
      std::chrono::milliseconds filterEvalDuration{};
      std::chrono::milliseconds largeInEvalDuration{};
      std::chrono::microseconds indexOfLookupDuration{};
    };

    struct InThresholdTiming final
    {
      std::size_t listSize = 0;
      std::chrono::microseconds expandedDuration{};
      std::chrono::microseconds setDuration{};
      std::size_t expandedMatches = 0;
      std::size_t setMatches = 0;
    };

    struct SortCacheKeys final
    {
      std::uint16_t year = 0;
      std::uint16_t discNumber = 0;
      std::uint16_t trackNumber = 0;
      std::uint16_t movementNumber = 0;
      std::chrono::milliseconds duration{0};
      std::string_view titleKey{};
      std::string_view artistKey{};
      std::string_view albumKey{};
      std::string_view albumArtistKey{};
      std::string_view genreKey{};
      std::string_view composerKey{};
      std::string_view workKey{};
    };

    struct SortCacheOrderEntry final
    {
      TrackId trackId{};
      SortCacheKeys keys{};
      std::string_view groupKey{};
      std::string_view primaryText{};
      std::string_view secondaryText{};
      std::string_view tertiaryText{};
      ResourceId imageId{kInvalidResourceId};
    };

    struct RankedSortCacheKeys final
    {
      std::uint16_t year = 0;
      std::uint16_t discNumber = 0;
      std::uint16_t trackNumber = 0;
      std::uint16_t movementNumber = 0;
      std::chrono::milliseconds duration{0};
      std::uint32_t titleRank = 0;
      std::uint32_t artistRank = 0;
      std::uint32_t albumRank = 0;
      std::uint32_t albumArtistRank = 0;
      std::uint32_t genreRank = 0;
      std::uint32_t composerRank = 0;
      std::uint32_t workRank = 0;
    };

    struct RankedSortCacheOrderEntry final
    {
      TrackId trackId{};
      RankedSortCacheKeys keys{};
      std::uint32_t groupRank = 0;
      std::uint32_t primaryRank = 0;
      std::uint32_t secondaryRank = 0;
      std::uint32_t tertiaryRank = 0;
      ResourceId imageId{kInvalidResourceId};
    };

    struct CompactSortCacheEntry final
    {
      TrackId trackId{};
      std::string_view artistKey{};
      std::string_view albumKey{};
      std::string_view titleKey{};
      std::uint16_t discNumber = 0;
      std::uint16_t trackNumber = 0;
    };

    struct SortCacheStrategyTiming final
    {
      std::size_t count = 0;
      std::chrono::milliseconds duration{};
      std::uint64_t checksum = 0;
    };

    struct RebuildStageTiming final
    {
      std::size_t count = 0;
      std::size_t sectionCount = 0;
      std::chrono::milliseconds buildEntriesDuration{};
      std::chrono::milliseconds sortDuration{};
      std::chrono::milliseconds rebuildPositionIndexDuration{};
      std::chrono::milliseconds buildGroupSectionsDuration{};
      std::chrono::milliseconds totalDuration{};
      std::uint64_t checksum = 0;
    };

    struct RankedSortTiming final
    {
      std::size_t count = 0;
      std::chrono::milliseconds buildRankMapDuration{};
      std::chrono::milliseconds buildEntriesDuration{};
      std::chrono::milliseconds sortDuration{};
      std::chrono::milliseconds totalDuration{};
      std::uint64_t checksum = 0;
    };

    struct CompactSortTiming final
    {
      std::size_t count = 0;
      std::chrono::milliseconds buildEntriesDuration{};
      std::chrono::milliseconds sortDuration{};
      std::chrono::milliseconds totalDuration{};
      std::uint64_t checksum = 0;
    };

    // A comparison-heavy predicate that every synthetic track satisfies, so the
    // PlanEvaluator runs each instruction to completion for every track. Unlike
    // the empty (match-all) filter, this actually exercises the instruction loop
    // and its field-operand resolution — the path the field-load index optimizes.
    constexpr auto kHeavyFilter = "$year >= 1990 && $year <= 2024 && $trackNumber >= 1 && $trackNumber <= 20 && "
                                  "$discNumber >= 1 && @duration >= 1m && $title ~ 'Track' && $artist ~ 'Artist' && "
                                  "$genre ~ 'Genre' && $album ~ 'Album'";

    // A large IN list exercises the compiled membership path used by broad
    // imported filters and tag-generated smart-list predicates.
    std::string makeLargeInFilter()
    {
      auto expr = std::string{"$year in ["};

      for (std::int32_t value = 1; value <= 256; ++value)
      {
        if (value > 1)
        {
          expr += ", ";
        }

        expr += std::to_string(1900 + value);
      }

      expr += "]";
      return expr;
    }

    std::int32_t compareSortCacheEntry(SortCacheOrderEntry const& lhs, SortCacheOrderEntry const& rhs)
    {
      if (auto const cmp = lhs.keys.artistKey.compare(rhs.keys.artistKey); cmp != 0)
      {
        return cmp;
      }

      if (auto const cmp = lhs.keys.albumKey.compare(rhs.keys.albumKey); cmp != 0)
      {
        return cmp;
      }

      if (lhs.keys.discNumber != rhs.keys.discNumber)
      {
        return lhs.keys.discNumber < rhs.keys.discNumber ? -1 : 1;
      }

      if (lhs.keys.trackNumber != rhs.keys.trackNumber)
      {
        return lhs.keys.trackNumber < rhs.keys.trackNumber ? -1 : 1;
      }

      if (auto const cmp = lhs.keys.titleKey.compare(rhs.keys.titleKey); cmp != 0)
      {
        return cmp;
      }

      if (lhs.trackId != rhs.trackId)
      {
        return lhs.trackId < rhs.trackId ? -1 : 1;
      }

      return 0;
    }

    bool lessSortCacheEntry(SortCacheOrderEntry const& lhs, SortCacheOrderEntry const& rhs)
    {
      return compareSortCacheEntry(lhs, rhs) < 0;
    }

    std::vector<std::string> makeSortCacheStringPool()
    {
      constexpr std::size_t kPoolSize = 4096;
      auto pool = std::vector<std::string>{};
      pool.reserve(kPoolSize);

      for (std::size_t idx = 0; idx < kPoolSize; ++idx)
      {
        pool.push_back(std::format("sort-key-{:04d}", idx));
      }

      return pool;
    }

    std::vector<SortCacheOrderEntry> makeSortCacheEntries(std::vector<std::string> const& pool, std::size_t count)
    {
      auto entries = std::vector<SortCacheOrderEntry>{};
      entries.reserve(count);

      for (std::size_t idx = 0; idx < count; ++idx)
      {
        auto const scrambled = (idx * 48271 + 9973) % count;
        auto const poolIdx = [poolSize = pool.size()](std::size_t value, std::size_t salt) -> std::size_t
        { return (value * 1103515245 + salt * 12345) % poolSize; };

        entries.push_back(SortCacheOrderEntry{
          .trackId = TrackId{static_cast<std::uint32_t>(scrambled + 1)},
          .keys =
            {
              .year = static_cast<std::uint16_t>(1990 + (scrambled % 35)),
              .discNumber = static_cast<std::uint16_t>(1 + (scrambled % 3)),
              .trackNumber = static_cast<std::uint16_t>(1 + (scrambled % 20)),
              .movementNumber = static_cast<std::uint16_t>(scrambled % 12),
              .duration = std::chrono::milliseconds{180000 + static_cast<std::int64_t>(scrambled % 420000)},
              .titleKey = pool[poolIdx(scrambled, 1)],
              .artistKey = pool[poolIdx(scrambled / 50, 2)],
              .albumKey = pool[poolIdx(scrambled / 200, 3)],
              .albumArtistKey = pool[poolIdx(scrambled / 75, 4)],
              .genreKey = pool[poolIdx(scrambled % 20, 5)],
              .composerKey = pool[poolIdx(scrambled / 400, 6)],
              .workKey = pool[poolIdx(scrambled / 500, 7)],
            },
          .groupKey = pool[poolIdx(scrambled / 50, 8)],
          .primaryText = pool[poolIdx(scrambled, 9)],
          .secondaryText = pool[poolIdx(scrambled / 200, 10)],
          .tertiaryText = pool[poolIdx(scrambled / 400, 11)],
          .imageId = ResourceId{static_cast<std::uint32_t>(scrambled % 1024)},
        });
      }

      return entries;
    }

    template<typename T>
    std::int32_t compareSortValue(T const& lhsVal, T const& rhsVal)
    {
      if (lhsVal < rhsVal)
      {
        return -1;
      }

      if (rhsVal < lhsVal)
      {
        return 1;
      }

      return 0;
    }

    std::int32_t compareGenericSortCacheField(TrackSortField field, SortCacheKeys const& lhs, SortCacheKeys const& rhs)
    {
      switch (field)
      {
        case TrackSortField::Year: return compareSortValue(lhs.year, rhs.year);
        case TrackSortField::DiscNumber: return compareSortValue(lhs.discNumber, rhs.discNumber);
        case TrackSortField::TrackNumber: return compareSortValue(lhs.trackNumber, rhs.trackNumber);
        case TrackSortField::Movement: return compareSortValue(lhs.movementNumber, rhs.movementNumber);
        case TrackSortField::Duration: return compareSortValue(lhs.duration, rhs.duration);
        case TrackSortField::Title: return lhs.titleKey.compare(rhs.titleKey);
        case TrackSortField::Artist: return lhs.artistKey.compare(rhs.artistKey);
        case TrackSortField::Album: return lhs.albumKey.compare(rhs.albumKey);
        case TrackSortField::AlbumArtist: return lhs.albumArtistKey.compare(rhs.albumArtistKey);
        case TrackSortField::Genre: return lhs.genreKey.compare(rhs.genreKey);
        case TrackSortField::Composer: return lhs.composerKey.compare(rhs.composerKey);
        case TrackSortField::Work: return lhs.workKey.compare(rhs.workKey);
      }

      return 0;
    }

    bool lessGenericSortCacheEntry(SortCacheOrderEntry const& lhs,
                                   SortCacheOrderEntry const& rhs,
                                   std::vector<TrackSortTerm> const& sortBy)
    {
      for (auto const& term : sortBy)
      {
        if (auto const cmp = compareGenericSortCacheField(term.field, lhs.keys, rhs.keys); cmp != 0)
        {
          return term.ascending ? (cmp < 0) : (cmp > 0);
        }
      }

      return lhs.trackId < rhs.trackId;
    }

    std::uint64_t checksumSortCacheEntries(std::vector<SortCacheOrderEntry> const& entries)
    {
      std::uint64_t checksum = 1469598103934665603ULL;

      for (auto const& entry : entries)
      {
        checksum ^= entry.trackId.raw();
        checksum *= 1099511628211ULL;
      }

      return checksum;
    }

    std::uint64_t checksumRankedSortCacheEntries(std::vector<RankedSortCacheOrderEntry> const& entries)
    {
      std::uint64_t checksum = 1469598103934665603ULL;

      for (auto const& entry : entries)
      {
        checksum ^= entry.trackId.raw();
        checksum *= 1099511628211ULL;
      }

      return checksum;
    }

    std::uint64_t checksumCompactSortCacheEntries(std::vector<CompactSortCacheEntry> const& entries)
    {
      std::uint64_t checksum = 1469598103934665603ULL;

      for (auto const& entry : entries)
      {
        checksum ^= entry.trackId.raw();
        checksum *= 1099511628211ULL;
      }

      return checksum;
    }

    std::uint64_t checksumSortCacheIndices(std::vector<SortCacheOrderEntry> const& entries,
                                           std::vector<std::uint32_t> const& indices)
    {
      std::uint64_t checksum = 1469598103934665603ULL;

      for (auto const index : indices)
      {
        checksum ^= entries[index].trackId.raw();
        checksum *= 1099511628211ULL;
      }

      return checksum;
    }

    using StringRankMap = boost::unordered_flat_map<std::string_view, std::uint32_t>;

    StringRankMap makeStringRankMap(std::vector<std::string> const& pool)
    {
      auto sortedViews = std::vector<std::string_view>{};
      sortedViews.reserve(pool.size());

      for (auto const& value : pool)
      {
        sortedViews.push_back(value);
      }

      std::ranges::sort(sortedViews);
      sortedViews.erase(std::ranges::unique(sortedViews).begin(), sortedViews.end());

      auto ranks = StringRankMap{};
      ranks.reserve(sortedViews.size());

      for (auto const& [idx, value] : sortedViews | std::views::enumerate)
      {
        ranks.emplace(value, static_cast<std::uint32_t>(idx + 1));
      }

      return ranks;
    }

    std::uint32_t rankOf(StringRankMap const& ranks, std::string_view value)
    {
      if (auto const it = ranks.find(value); it != ranks.end())
      {
        return it->second;
      }

      return 0;
    }

    std::vector<RankedSortCacheOrderEntry> makeRankedSortCacheEntries(std::vector<SortCacheOrderEntry> const& entries,
                                                                      StringRankMap const& ranks)
    {
      auto rankedEntries = std::vector<RankedSortCacheOrderEntry>{};
      rankedEntries.reserve(entries.size());

      for (auto const& entry : entries)
      {
        rankedEntries.push_back(RankedSortCacheOrderEntry{
          .trackId = entry.trackId,
          .keys =
            {
              .year = entry.keys.year,
              .discNumber = entry.keys.discNumber,
              .trackNumber = entry.keys.trackNumber,
              .movementNumber = entry.keys.movementNumber,
              .duration = entry.keys.duration,
              .titleRank = rankOf(ranks, entry.keys.titleKey),
              .artistRank = rankOf(ranks, entry.keys.artistKey),
              .albumRank = rankOf(ranks, entry.keys.albumKey),
              .albumArtistRank = rankOf(ranks, entry.keys.albumArtistKey),
              .genreRank = rankOf(ranks, entry.keys.genreKey),
              .composerRank = rankOf(ranks, entry.keys.composerKey),
              .workRank = rankOf(ranks, entry.keys.workKey),
            },
          .groupRank = rankOf(ranks, entry.groupKey),
          .primaryRank = rankOf(ranks, entry.primaryText),
          .secondaryRank = rankOf(ranks, entry.secondaryText),
          .tertiaryRank = rankOf(ranks, entry.tertiaryText),
          .imageId = entry.imageId,
        });
      }

      return rankedEntries;
    }

    std::vector<CompactSortCacheEntry> makeCompactSortCacheEntries(std::vector<SortCacheOrderEntry> const& entries)
    {
      auto compactEntries = std::vector<CompactSortCacheEntry>{};
      compactEntries.reserve(entries.size());

      for (auto const& entry : entries)
      {
        compactEntries.push_back(CompactSortCacheEntry{
          .trackId = entry.trackId,
          .artistKey = entry.keys.artistKey,
          .albumKey = entry.keys.albumKey,
          .titleKey = entry.keys.titleKey,
          .discNumber = entry.keys.discNumber,
          .trackNumber = entry.keys.trackNumber,
        });
      }

      return compactEntries;
    }

    bool isSortedEntries(std::vector<SortCacheOrderEntry> const& entries)
    {
      return std::ranges::is_sorted(entries, lessSortCacheEntry);
    }

    bool isSortedIndices(std::vector<SortCacheOrderEntry> const& entries, std::vector<std::uint32_t> const& indices)
    {
      return std::ranges::is_sorted(indices,
                                    [&entries](std::uint32_t lhs, std::uint32_t rhs)
                                    { return lessSortCacheEntry(entries[lhs], entries[rhs]); });
    }

    bool lessRankedSortCacheEntry(RankedSortCacheOrderEntry const& lhs, RankedSortCacheOrderEntry const& rhs)
    {
      if (lhs.keys.artistRank != rhs.keys.artistRank)
      {
        return lhs.keys.artistRank < rhs.keys.artistRank;
      }

      if (lhs.keys.albumRank != rhs.keys.albumRank)
      {
        return lhs.keys.albumRank < rhs.keys.albumRank;
      }

      if (lhs.keys.discNumber != rhs.keys.discNumber)
      {
        return lhs.keys.discNumber < rhs.keys.discNumber;
      }

      if (lhs.keys.trackNumber != rhs.keys.trackNumber)
      {
        return lhs.keys.trackNumber < rhs.keys.trackNumber;
      }

      if (lhs.keys.titleRank != rhs.keys.titleRank)
      {
        return lhs.keys.titleRank < rhs.keys.titleRank;
      }

      return lhs.trackId < rhs.trackId;
    }

    bool lessCompactSortCacheEntry(CompactSortCacheEntry const& lhs, CompactSortCacheEntry const& rhs)
    {
      if (auto const cmp = lhs.artistKey.compare(rhs.artistKey); cmp != 0)
      {
        return cmp < 0;
      }

      if (auto const cmp = lhs.albumKey.compare(rhs.albumKey); cmp != 0)
      {
        return cmp < 0;
      }

      if (lhs.discNumber != rhs.discNumber)
      {
        return lhs.discNumber < rhs.discNumber;
      }

      if (lhs.trackNumber != rhs.trackNumber)
      {
        return lhs.trackNumber < rhs.trackNumber;
      }

      if (auto const cmp = lhs.titleKey.compare(rhs.titleKey); cmp != 0)
      {
        return cmp < 0;
      }

      return lhs.trackId < rhs.trackId;
    }

    bool isSortedRankedEntries(std::vector<RankedSortCacheOrderEntry> const& entries)
    {
      return std::ranges::is_sorted(entries, lessRankedSortCacheEntry);
    }

    bool isSortedCompactEntries(std::vector<CompactSortCacheEntry> const& entries)
    {
      return std::ranges::is_sorted(entries, lessCompactSortCacheEntry);
    }

    SortCacheStrategyTiming measureDirectEntrySort(std::vector<SortCacheOrderEntry> const& baseline)
    {
      auto timing = SortCacheStrategyTiming{.count = baseline.size()};

      auto directEntries = baseline;
      auto const directStart = std::chrono::steady_clock::now();
      std::ranges::sort(directEntries, lessSortCacheEntry);
      auto const directEnd = std::chrono::steady_clock::now();
      timing.duration = std::chrono::duration_cast<std::chrono::milliseconds>(directEnd - directStart);
      timing.checksum = checksumSortCacheEntries(directEntries);
      CHECK(isSortedEntries(directEntries));

      return timing;
    }

    SortCacheStrategyTiming measureGenericEntrySort(std::vector<SortCacheOrderEntry> const& baseline)
    {
      auto timing = SortCacheStrategyTiming{.count = baseline.size()};
      auto sortBy = std::vector{
        TrackSortTerm{.field = TrackSortField::Artist},
        TrackSortTerm{.field = TrackSortField::Album},
        TrackSortTerm{.field = TrackSortField::DiscNumber},
        TrackSortTerm{.field = TrackSortField::TrackNumber},
        TrackSortTerm{.field = TrackSortField::Title},
      };

      auto entries = baseline;
      auto const start = std::chrono::steady_clock::now();
      std::ranges::sort(entries,
                        [&sortBy](SortCacheOrderEntry const& lhs, SortCacheOrderEntry const& rhs)
                        { return lessGenericSortCacheEntry(lhs, rhs, sortBy); });
      auto const end = std::chrono::steady_clock::now();
      timing.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
      timing.checksum = checksumSortCacheEntries(entries);
      CHECK(std::ranges::is_sorted(entries,
                                   [&sortBy](SortCacheOrderEntry const& lhs, SortCacheOrderEntry const& rhs)
                                   { return lessGenericSortCacheEntry(lhs, rhs, sortBy); }));

      return timing;
    }

    SortCacheStrategyTiming measureIndexSort(std::vector<SortCacheOrderEntry> const& baseline)
    {
      auto timing = SortCacheStrategyTiming{.count = baseline.size()};

      auto indices = std::vector<std::uint32_t>(baseline.size());
      auto const indexStart = std::chrono::steady_clock::now();
      std::ranges::iota(indices, 0);
      std::ranges::sort(indices,
                        [&baseline](std::uint32_t lhs, std::uint32_t rhs)
                        { return lessSortCacheEntry(baseline[lhs], baseline[rhs]); });
      auto const indexEnd = std::chrono::steady_clock::now();
      timing.duration = std::chrono::duration_cast<std::chrono::milliseconds>(indexEnd - indexStart);
      timing.checksum = checksumSortCacheIndices(baseline, indices);
      CHECK(isSortedIndices(baseline, indices));

      return timing;
    }

    RankedSortTiming measureRankedEntrySort(std::vector<std::string> const& pool,
                                            std::vector<SortCacheOrderEntry> const& baseline)
    {
      auto timing = RankedSortTiming{.count = baseline.size()};
      auto const totalStart = std::chrono::steady_clock::now();

      auto const rankStart = std::chrono::steady_clock::now();
      auto const ranks = makeStringRankMap(pool);
      auto const rankEnd = std::chrono::steady_clock::now();

      auto const buildStart = std::chrono::steady_clock::now();
      auto rankedEntries = makeRankedSortCacheEntries(baseline, ranks);
      auto const buildEnd = std::chrono::steady_clock::now();

      auto const sortStart = std::chrono::steady_clock::now();
      std::ranges::sort(rankedEntries, lessRankedSortCacheEntry);
      auto const sortEnd = std::chrono::steady_clock::now();
      auto const totalEnd = std::chrono::steady_clock::now();

      timing.buildRankMapDuration = std::chrono::duration_cast<std::chrono::milliseconds>(rankEnd - rankStart);
      timing.buildEntriesDuration = std::chrono::duration_cast<std::chrono::milliseconds>(buildEnd - buildStart);
      timing.sortDuration = std::chrono::duration_cast<std::chrono::milliseconds>(sortEnd - sortStart);
      timing.totalDuration = std::chrono::duration_cast<std::chrono::milliseconds>(totalEnd - totalStart);
      timing.checksum = checksumRankedSortCacheEntries(rankedEntries);
      CHECK(isSortedRankedEntries(rankedEntries));

      return timing;
    }

    CompactSortTiming measureCompactEntrySort(std::vector<SortCacheOrderEntry> const& baseline)
    {
      auto timing = CompactSortTiming{.count = baseline.size()};
      auto const totalStart = std::chrono::steady_clock::now();

      auto const buildStart = std::chrono::steady_clock::now();
      auto compactEntries = makeCompactSortCacheEntries(baseline);
      auto const buildEnd = std::chrono::steady_clock::now();

      auto const sortStart = std::chrono::steady_clock::now();
      std::ranges::sort(compactEntries, lessCompactSortCacheEntry);
      auto const sortEnd = std::chrono::steady_clock::now();
      auto const totalEnd = std::chrono::steady_clock::now();

      timing.buildEntriesDuration = std::chrono::duration_cast<std::chrono::milliseconds>(buildEnd - buildStart);
      timing.sortDuration = std::chrono::duration_cast<std::chrono::milliseconds>(sortEnd - sortStart);
      timing.totalDuration = std::chrono::duration_cast<std::chrono::milliseconds>(totalEnd - totalStart);
      timing.checksum = checksumCompactSortCacheEntries(compactEntries);
      CHECK(isSortedCompactEntries(compactEntries));

      return timing;
    }

    SortCacheStrategyTiming measureIndexSortAndMaterialize(std::vector<SortCacheOrderEntry> const& baseline)
    {
      auto timing = SortCacheStrategyTiming{.count = baseline.size()};

      auto materializedIndices = std::vector<std::uint32_t>(baseline.size());
      auto materializedEntries = std::vector<SortCacheOrderEntry>{};
      materializedEntries.reserve(baseline.size());

      auto const materializedStart = std::chrono::steady_clock::now();
      std::ranges::iota(materializedIndices, 0);
      std::ranges::sort(materializedIndices,
                        [&baseline](std::uint32_t lhs, std::uint32_t rhs)
                        { return lessSortCacheEntry(baseline[lhs], baseline[rhs]); });

      for (auto const index : materializedIndices)
      {
        materializedEntries.push_back(baseline[index]);
      }

      auto const materializedEnd = std::chrono::steady_clock::now();
      timing.duration = std::chrono::duration_cast<std::chrono::milliseconds>(materializedEnd - materializedStart);
      timing.checksum = checksumSortCacheEntries(materializedEntries);
      CHECK(isSortedEntries(materializedEntries));

      return timing;
    }

    RebuildStageTiming measureProjectionRebuildStages(std::vector<std::string> const& pool, std::size_t count)
    {
      auto timing = RebuildStageTiming{.count = count};
      auto const totalStart = std::chrono::steady_clock::now();

      auto const buildStart = std::chrono::steady_clock::now();
      auto entries = makeSortCacheEntries(pool, count);
      auto const buildEnd = std::chrono::steady_clock::now();

      auto const sortStart = std::chrono::steady_clock::now();
      std::ranges::sort(entries, lessSortCacheEntry);
      auto const sortEnd = std::chrono::steady_clock::now();

      auto const positionStart = std::chrono::steady_clock::now();
      auto positionIndex = boost::unordered_flat_map<TrackId, std::size_t, std::hash<TrackId>>{};
      positionIndex.reserve(entries.size());

      for (auto const& [idx, entry] : entries | std::views::enumerate)
      {
        positionIndex[entry.trackId] = static_cast<std::size_t>(idx);
      }

      auto const positionEnd = std::chrono::steady_clock::now();

      auto const groupStart = std::chrono::steady_clock::now();
      std::size_t sectionCount = 0;

      if (!entries.empty())
      {
        ++sectionCount;

        for (std::size_t idx = 1; idx < entries.size(); ++idx)
        {
          if (entries[idx].groupKey != entries[idx - 1].groupKey)
          {
            ++sectionCount;
          }
        }
      }

      auto const groupEnd = std::chrono::steady_clock::now();
      auto const totalEnd = std::chrono::steady_clock::now();

      timing.sectionCount = sectionCount;
      timing.buildEntriesDuration = std::chrono::duration_cast<std::chrono::milliseconds>(buildEnd - buildStart);
      timing.sortDuration = std::chrono::duration_cast<std::chrono::milliseconds>(sortEnd - sortStart);
      timing.rebuildPositionIndexDuration =
        std::chrono::duration_cast<std::chrono::milliseconds>(positionEnd - positionStart);
      timing.buildGroupSectionsDuration = std::chrono::duration_cast<std::chrono::milliseconds>(groupEnd - groupStart);
      timing.totalDuration = std::chrono::duration_cast<std::chrono::milliseconds>(totalEnd - totalStart);
      timing.checksum = checksumSortCacheEntries(entries);

      CHECK(positionIndex.size() == entries.size());
      CHECK(isSortedEntries(entries));

      return timing;
    }

    query::ExecutionPlan makeExpandedYearInPlan(std::size_t listSize)
    {
      auto plan = query::ExecutionPlan{};
      std::int32_t nextReg = 0;
      bool first = true;

      for (std::size_t idx = 0; idx < listSize; ++idx)
      {
        plan.instructions.push_back(query::Instruction{
          .op = query::OpCode::LoadField,
          .field = static_cast<std::uint8_t>(query::Field::Year),
          .operand = nextReg++,
          .constValue = 0,
          .size = 0,
          .data = nullptr,
        });
        plan.instructions.push_back(query::Instruction{
          .op = query::OpCode::LoadConstant,
          .field = 0,
          .operand = nextReg++,
          .constValue = static_cast<std::int64_t>(1990 + idx),
          .size = 0,
          .data = nullptr,
        });

        plan.instructions.push_back(query::Instruction{
          .op = query::OpCode::Eq,
          .field = 0,
          .operand = nextReg - 1,
          .constValue = 0,
          .size = 0,
          .data = nullptr,
        });
        --nextReg;

        if (first)
        {
          first = false;
          continue;
        }

        plan.instructions.push_back(query::Instruction{
          .op = query::OpCode::Or,
          .field = 0,
          .operand = nextReg - 1,
          .constValue = 0,
          .size = 0,
          .data = nullptr,
        });
        --nextReg;
      }

      plan.indexFieldLoads();
      return plan;
    }

    query::ExecutionPlan makeSetYearInPlan(std::size_t listSize)
    {
      auto plan = query::ExecutionPlan{};
      auto set = query::InSet{};

      for (std::size_t idx = 0; idx < listSize; ++idx)
      {
        set.numericValues.insert(static_cast<std::int64_t>(1990 + idx));
      }

      plan.inSets.push_back(std::move(set));
      plan.instructions.push_back(query::Instruction{
        .op = query::OpCode::LoadField,
        .field = static_cast<std::uint8_t>(query::Field::Year),
        .operand = 0,
        .constValue = 0,
        .size = 0,
        .data = nullptr,
      });
      plan.instructions.push_back(query::Instruction{
        .op = query::OpCode::InSet,
        .field = 0,
        .operand = 0,
        .constValue = 0,
        .size = 0,
        .data = nullptr,
      });

      plan.indexFieldLoads();
      return plan;
    }

    struct ScaleBench final
    {
      TestMusicLibrary lib;
      std::vector<TrackId> ids;
    };

    void buildLibrary(ScaleBench& bench, std::int32_t trackCount)
    {
      bench.ids.reserve(trackCount);

      for (std::int32_t idx = 0; idx < trackCount; ++idx)
      {
        auto const spec = TrackSpec{
          .title = std::format("Track {:06d}", idx),
          .artist = std::format("Artist {:04d}", idx % (trackCount / 50 + 1)),
          .album = std::format("Album {:04d}", idx % (trackCount / 200 + 1)),
          .genre = std::format("Genre {:02d}", idx % 20),
          .year = static_cast<std::uint16_t>(1990 + (idx % 35)),
          .discNumber = static_cast<std::uint16_t>(1 + (idx % 3)),
          .trackNumber = static_cast<std::uint16_t>(1 + (idx % 20)),
          .duration = std::chrono::minutes{3} + std::chrono::milliseconds{static_cast<std::uint32_t>(
                                                  (static_cast<std::int64_t>(idx) * 137) %
                                                  std::chrono::milliseconds{std::chrono::minutes{7}}.count())},
        };
        bench.ids.push_back(bench.lib.addTrack(spec));
      }
    }

    class CountingSource final : public TrackSource
    {
    public:
      explicit CountingSource(std::vector<TrackId> ids)
        : _ids{std::move(ids)}
      {
      }

      std::size_t size() const override { return _ids.size(); }
      TrackId trackIdAt(std::size_t index) const override { return _ids.at(index); }
      std::optional<std::size_t> indexOf(TrackId id) const override
      {
        for (std::size_t i = 0; i < _ids.size(); ++i)
        {
          if (_ids[i] == id)
          {
            return i;
          }
        }

        return std::nullopt;
      }

    private:
      std::vector<TrackId> _ids;
    };

    Timings measureScale(ScaleBench& bench, std::int32_t trackCount)
    {
      auto t = Timings{};
      auto& lib = bench.lib.library();

      // 1. Projection construction + setPresentation
      auto source = CountingSource{bench.ids};

      auto const t0 = std::chrono::steady_clock::now();
      auto proj = TrackListProjection{ViewId{1}, source, lib};
      auto const t1 = std::chrono::steady_clock::now();
      proj.setPresentation(TrackPresentationSpec{
        .groupBy = TrackGroupKey::None, .sortBy = {TrackSortTerm{.field = TrackSortField::Title}}});
      auto const t2 = std::chrono::steady_clock::now();

      t.createProjectionDuration = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);
      t.setTitleSortDuration = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1);

      // 2. SmartListEvaluator evaluateMembers (simulates expression filter via SmartListSource)
      auto evaluator = SmartListEvaluator{lib};
      auto filtered = SmartListSource{source, lib, evaluator};

      auto const t3 = std::chrono::steady_clock::now();

      // Set expression via stage/apply pattern (mirrors ViewService::setFilter path)
      // No filter expression → match everything
      filtered.reload();
      auto const t4 = std::chrono::steady_clock::now();

      t.evaluateMembersDuration = std::chrono::duration_cast<std::chrono::milliseconds>(t4 - t3);

      // 2b. Filter evaluation with a comparison-heavy predicate. A dedicated
      // evaluator keeps this measurement independent of the match-all list above.
      auto filterEvaluator = SmartListEvaluator{lib};
      auto filteredExpr = SmartListSource{source, lib, filterEvaluator};
      filteredExpr.setExpression(kHeavyFilter);

      auto const filterStart = std::chrono::steady_clock::now();
      filteredExpr.reload();
      auto const filterEnd = std::chrono::steady_clock::now();

      t.filterEvalDuration = std::chrono::duration_cast<std::chrono::milliseconds>(filterEnd - filterStart);

      // 2c. Large IN-list filter.
      auto inEvaluator = SmartListEvaluator{lib};
      auto filteredIn = SmartListSource{source, lib, inEvaluator};
      filteredIn.setExpression(makeLargeInFilter());

      auto const inStart = std::chrono::steady_clock::now();
      filteredIn.reload();
      auto const inEnd = std::chrono::steady_clock::now();

      t.largeInEvalDuration = std::chrono::duration_cast<std::chrono::milliseconds>(inEnd - inStart);

      // 3. indexOf — 10k iterations at a fixed position
      auto const midId = proj.trackIdAt(static_cast<std::size_t>(trackCount / 2));
      [[maybe_unused]] auto const optWarm = proj.indexOf(midId); // warm

      constexpr int kLookupIters = 10000;
      auto const t5 = std::chrono::steady_clock::now();

      for (std::int32_t i = 0; i < kLookupIters; ++i)
      {
        [[maybe_unused]] auto const optResult = proj.indexOf(midId);
      }

      auto const t6 = std::chrono::steady_clock::now();

      t.indexOfLookupDuration = std::chrono::duration_cast<std::chrono::microseconds>(t6 - t5);

      return t;
    }

    InThresholdTiming measureYearInThreshold(ScaleBench& bench, std::size_t listSize)
    {
      auto expandedPlan = makeExpandedYearInPlan(listSize);
      auto setPlan = makeSetYearInPlan(listSize);
      auto evaluator = query::PlanEvaluator{};
      auto& lib = bench.lib.library();
      auto txn = lib.readTransaction();
      auto reader = lib.tracks().reader(txn);

      auto timing = InThresholdTiming{.listSize = listSize};

      auto const expandedStart = std::chrono::steady_clock::now();

      for (auto const id : bench.ids)
      {
        auto optView = reader.get(id, library::TrackStore::Reader::LoadMode::Hot);

        if (optView && evaluator.evaluateFull(expandedPlan, *optView))
        {
          ++timing.expandedMatches;
        }
      }

      auto const expandedEnd = std::chrono::steady_clock::now();
      auto const setStart = std::chrono::steady_clock::now();

      for (auto const id : bench.ids)
      {
        auto optView = reader.get(id, library::TrackStore::Reader::LoadMode::Hot);

        if (optView && evaluator.evaluateFull(setPlan, *optView))
        {
          ++timing.setMatches;
        }
      }

      auto const setEnd = std::chrono::steady_clock::now();

      timing.expandedDuration = std::chrono::duration_cast<std::chrono::microseconds>(expandedEnd - expandedStart);
      timing.setDuration = std::chrono::duration_cast<std::chrono::microseconds>(setEnd - setStart);

      return timing;
    }

    std::chrono::milliseconds measureProjectionSortFieldDuration(ScaleBench& bench, TrackSortField field)
    {
      auto source = CountingSource{bench.ids};
      auto proj = TrackListProjection{ViewId{1}, source, bench.lib.library()};

      auto const start = std::chrono::steady_clock::now();
      proj.setPresentation(TrackPresentationSpec{
        .groupBy = TrackGroupKey::None, .sortBy = {TrackSortTerm{.field = field, .ascending = true}}});
      auto const end = std::chrono::steady_clock::now();

      return std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    }

    std::chrono::milliseconds measureProjectionPresentationDuration(ScaleBench& bench,
                                                                    TrackPresentationSpec const& spec)
    {
      auto source = CountingSource{bench.ids};
      auto proj = TrackListProjection{ViewId{1}, source, bench.lib.library()};

      auto const start = std::chrono::steady_clock::now();
      proj.setPresentation(spec);
      auto const end = std::chrono::steady_clock::now();

      return std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    }
  } // namespace

  TEST_CASE("Phase 0 — 10k Baseline", "[baseline][unit]")
  {
    log::Log::init(log::LogLevel::Info);
    constexpr int kN = 10000;
    APP_LOG_INFO("=== Phase 0 Baseline: {} tracks ===", kN);

    auto bench = ScaleBench{};
    auto const t0 = std::chrono::steady_clock::now();
    buildLibrary(bench, kN);
    auto const t1 = std::chrono::steady_clock::now();

    auto const buildDuration = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);
    APP_LOG_INFO("  Library build: {} ms", buildDuration.count());

    auto const t = measureScale(bench, kN);

    APP_LOG_INFO("  Projection construct: {} ms", t.createProjectionDuration.count());
    APP_LOG_INFO("  setPresentation (sort): {} ms", t.setTitleSortDuration.count());
    APP_LOG_INFO("  SmartListEvaluator::evaluateMembers: {} ms", t.evaluateMembersDuration.count());
    APP_LOG_INFO("  Filter eval (heavy predicate): {} ms", t.filterEvalDuration.count());
    APP_LOG_INFO("  Filter eval (large IN list): {} ms", t.largeInEvalDuration.count());
    APP_LOG_INFO("  indexOf x10000: {} us", t.indexOfLookupDuration.count());

    // Regression thresholds — deliberately generous to avoid flakes
    CHECK(t.createProjectionDuration < std::chrono::seconds{5});
    CHECK(t.setTitleSortDuration < std::chrono::seconds{5});
    CHECK(t.evaluateMembersDuration < std::chrono::seconds{10});
    CHECK(t.filterEvalDuration < std::chrono::seconds{10});
    CHECK(t.largeInEvalDuration < std::chrono::seconds{10});
    CHECK(t.indexOfLookupDuration < std::chrono::microseconds{500000});
  }

  TEST_CASE("Phase 0 — Query IN threshold sweep", "[baseline][unit][query]")
  {
    log::Log::init(log::LogLevel::Info);
    constexpr int kN = 10000;
    constexpr auto kListSizes = std::array<std::size_t, 8>{1, 2, 3, 4, 6, 8, 12, 16};
    APP_LOG_INFO("=== Phase 0 Query IN threshold sweep: {} tracks ===", kN);

    auto bench = ScaleBench{};
    buildLibrary(bench, kN);

    for (auto const listSize : kListSizes)
    {
      auto const timing = measureYearInThreshold(bench, listSize);
      APP_LOG_INFO("  IN size {:>2}: expanded {:>7} us, set {:>7} us, matches {}",
                   timing.listSize,
                   timing.expandedDuration.count(),
                   timing.setDuration.count(),
                   timing.expandedMatches);

      CHECK(timing.expandedMatches == timing.setMatches);
    }
  }

  TEST_CASE("Phase 0 — OrderEntry direct entry sort cache pressure", "[baseline][unit][projection]")
  {
    log::Log::init(log::LogLevel::Info);
    constexpr auto kCounts = std::array<std::size_t, 3>{10000, 100000, 1000000};

    APP_LOG_INFO("=== Phase 0 OrderEntry direct entry sort cache pressure ===");
    APP_LOG_INFO("  SortKeys-like size: {} bytes", sizeof(SortCacheKeys));
    APP_LOG_INFO("  OrderEntry-like size: {} bytes", sizeof(SortCacheOrderEntry));
    APP_LOG_INFO("  32-bit index size: {} bytes", sizeof(std::uint32_t));

    auto const pool = makeSortCacheStringPool();

    for (auto const count : kCounts)
    {
      auto const baseline = makeSortCacheEntries(pool, count);
      auto const timing = measureDirectEntrySort(baseline);
      auto const entryBytes = count * sizeof(SortCacheOrderEntry);

      APP_LOG_INFO("  {:>7} entries: entry payload {} KiB", timing.count, entryBytes / 1024);
      APP_LOG_INFO("    direct entry sort:              {:>6} ms", timing.duration.count());
      CHECK(timing.checksum != 0);
    }
  }

  TEST_CASE("Phase 0 — OrderEntry comparator dispatch cost", "[baseline][unit][projection]")
  {
    log::Log::init(log::LogLevel::Info);
    constexpr auto kCounts = std::array<std::size_t, 3>{10000, 100000, 1000000};

    APP_LOG_INFO("=== Phase 0 OrderEntry comparator dispatch cost ===");
    APP_LOG_INFO("  OrderEntry-like size: {} bytes", sizeof(SortCacheOrderEntry));

    auto const pool = makeSortCacheStringPool();

    for (auto const count : kCounts)
    {
      auto const baseline = makeSortCacheEntries(pool, count);
      auto const generic = measureGenericEntrySort(baseline);
      auto const specialized = measureDirectEntrySort(baseline);

      APP_LOG_INFO("  {:>7} entries", count);
      APP_LOG_INFO("    generic loop/switch comparator: {:>6} ms", generic.duration.count());
      APP_LOG_INFO("    specialized comparator:         {:>6} ms", specialized.duration.count());

      CHECK(generic.checksum == specialized.checksum);
    }
  }

  TEST_CASE("Phase 0 — OrderEntry ranked integer key sort", "[baseline][unit][projection]")
  {
    log::Log::init(log::LogLevel::Info);
    constexpr auto kCounts = std::array<std::size_t, 3>{10000, 100000, 1000000};

    APP_LOG_INFO("=== Phase 0 OrderEntry ranked integer key sort ===");
    APP_LOG_INFO("  String OrderEntry-like size: {} bytes", sizeof(SortCacheOrderEntry));
    APP_LOG_INFO("  Ranked OrderEntry-like size: {} bytes", sizeof(RankedSortCacheOrderEntry));

    auto const pool = makeSortCacheStringPool();

    for (auto const count : kCounts)
    {
      auto const baseline = makeSortCacheEntries(pool, count);
      auto const timing = measureRankedEntrySort(pool, baseline);

      APP_LOG_INFO("  {:>7} entries", timing.count);
      APP_LOG_INFO("    build rank map:                 {:>6} ms", timing.buildRankMapDuration.count());
      APP_LOG_INFO("    build ranked entries:           {:>6} ms", timing.buildEntriesDuration.count());
      APP_LOG_INFO("    ranked integer sort:            {:>6} ms", timing.sortDuration.count());
      APP_LOG_INFO("    total rank+sort path:           {:>6} ms", timing.totalDuration.count());

      CHECK(timing.checksum != 0);
    }
  }

  TEST_CASE("Phase 0 — OrderEntry compact active key sort", "[baseline][unit][projection]")
  {
    log::Log::init(log::LogLevel::Info);
    constexpr auto kCounts = std::array<std::size_t, 3>{10000, 100000, 1000000};

    APP_LOG_INFO("=== Phase 0 OrderEntry compact active key sort ===");
    APP_LOG_INFO("  Full OrderEntry-like size: {} bytes", sizeof(SortCacheOrderEntry));
    APP_LOG_INFO("  Compact active-key size: {} bytes", sizeof(CompactSortCacheEntry));

    auto const pool = makeSortCacheStringPool();

    for (auto const count : kCounts)
    {
      auto const baseline = makeSortCacheEntries(pool, count);
      auto const timing = measureCompactEntrySort(baseline);

      APP_LOG_INFO("  {:>7} entries", timing.count);
      APP_LOG_INFO("    build compact entries:          {:>6} ms", timing.buildEntriesDuration.count());
      APP_LOG_INFO("    compact string sort:            {:>6} ms", timing.sortDuration.count());
      APP_LOG_INFO("    total compact path:             {:>6} ms", timing.totalDuration.count());

      CHECK(timing.checksum != 0);
    }
  }

  TEST_CASE("Phase 0 — OrderEntry index sort cache pressure", "[baseline][unit][projection]")
  {
    log::Log::init(log::LogLevel::Info);
    constexpr auto kCounts = std::array<std::size_t, 3>{10000, 100000, 1000000};

    APP_LOG_INFO("=== Phase 0 OrderEntry index sort cache pressure ===");
    APP_LOG_INFO("  SortKeys-like size: {} bytes", sizeof(SortCacheKeys));
    APP_LOG_INFO("  OrderEntry-like size: {} bytes", sizeof(SortCacheOrderEntry));
    APP_LOG_INFO("  32-bit index size: {} bytes", sizeof(std::uint32_t));

    auto const pool = makeSortCacheStringPool();

    for (auto const count : kCounts)
    {
      auto const baseline = makeSortCacheEntries(pool, count);
      auto const timing = measureIndexSort(baseline);
      auto const entryBytes = count * sizeof(SortCacheOrderEntry);
      auto const indexBytes = count * sizeof(std::uint32_t);

      APP_LOG_INFO("  {:>7} entries: entry payload {} KiB, index payload {} KiB",
                   timing.count,
                   entryBytes / 1024,
                   indexBytes / 1024);
      APP_LOG_INFO("    index sort:                     {:>6} ms", timing.duration.count());
      CHECK(timing.checksum != 0);
    }
  }

  TEST_CASE("Phase 0 — OrderEntry index materialize cache pressure", "[baseline][unit][projection]")
  {
    log::Log::init(log::LogLevel::Info);
    constexpr auto kCounts = std::array<std::size_t, 3>{10000, 100000, 1000000};

    APP_LOG_INFO("=== Phase 0 OrderEntry index materialize cache pressure ===");
    APP_LOG_INFO("  SortKeys-like size: {} bytes", sizeof(SortCacheKeys));
    APP_LOG_INFO("  OrderEntry-like size: {} bytes", sizeof(SortCacheOrderEntry));
    APP_LOG_INFO("  32-bit index size: {} bytes", sizeof(std::uint32_t));

    auto const pool = makeSortCacheStringPool();

    for (auto const count : kCounts)
    {
      auto const baseline = makeSortCacheEntries(pool, count);
      auto const timing = measureIndexSortAndMaterialize(baseline);
      auto const entryBytes = count * sizeof(SortCacheOrderEntry);
      auto const indexBytes = count * sizeof(std::uint32_t);

      APP_LOG_INFO("  {:>7} entries: entry payload {} KiB, index payload {} KiB",
                   timing.count,
                   entryBytes / 1024,
                   indexBytes / 1024);
      APP_LOG_INFO("    index sort + materialize:       {:>6} ms", timing.duration.count());
      CHECK(timing.checksum != 0);
    }
  }

  TEST_CASE("Phase 0 — Projection rebuild stage timing", "[baseline][unit][projection]")
  {
    log::Log::init(log::LogLevel::Info);
    constexpr auto kCounts = std::array<std::size_t, 3>{10000, 100000, 1000000};

    APP_LOG_INFO("=== Phase 0 Projection rebuild stage timing ===");
    APP_LOG_INFO("  SortKeys-like size: {} bytes", sizeof(SortCacheKeys));
    APP_LOG_INFO("  OrderEntry-like size: {} bytes", sizeof(SortCacheOrderEntry));

    auto const pool = makeSortCacheStringPool();

    for (auto const count : kCounts)
    {
      auto const timing = measureProjectionRebuildStages(pool, count);

      APP_LOG_INFO("  {:>7} entries, {:>5} sections", timing.count, timing.sectionCount);
      APP_LOG_INFO("    build entries / keys:           {:>6} ms", timing.buildEntriesDuration.count());
      APP_LOG_INFO("    sort entries:                   {:>6} ms", timing.sortDuration.count());
      APP_LOG_INFO("    rebuild position index:         {:>6} ms", timing.rebuildPositionIndexDuration.count());
      APP_LOG_INFO("    build group sections:           {:>6} ms", timing.buildGroupSectionsDuration.count());
      APP_LOG_INFO("    total synthetic rebuild:        {:>6} ms", timing.totalDuration.count());
      CHECK(timing.checksum != 0);
    }
  }

  TEST_CASE("Phase 0 — Projection sort field timing", "[baseline][unit][projection]")
  {
    log::Log::init(log::LogLevel::Info);
    constexpr int kN = 1000000;

    APP_LOG_INFO("=== Phase 0 Projection sort field timing: {} tracks ===", kN);

    auto bench = ScaleBench{};
    buildLibrary(bench, kN);

    auto const titleDuration = measureProjectionSortFieldDuration(bench, TrackSortField::Title);
    auto const artistDuration = measureProjectionSortFieldDuration(bench, TrackSortField::Artist);
    auto const albumDuration = measureProjectionSortFieldDuration(bench, TrackSortField::Album);
    auto const genreDuration = measureProjectionSortFieldDuration(bench, TrackSortField::Genre);

    APP_LOG_INFO("  title sort:  {} ms", titleDuration.count());
    APP_LOG_INFO("  artist sort: {} ms", artistDuration.count());
    APP_LOG_INFO("  album sort:  {} ms", albumDuration.count());
    APP_LOG_INFO("  genre sort:  {} ms", genreDuration.count());

    CHECK(titleDuration < std::chrono::minutes{5});
    CHECK(artistDuration < std::chrono::minutes{5});
    CHECK(albumDuration < std::chrono::minutes{5});
    CHECK(genreDuration < std::chrono::minutes{5});
  }

  TEST_CASE("Phase 0 — Projection preset timing", "[baseline][unit][projection]")
  {
    log::Log::init(log::LogLevel::Info);
    constexpr int kN = 1000000;

    APP_LOG_INFO("=== Phase 0 Projection preset timing: {} tracks ===", kN);

    auto bench = ScaleBench{};
    buildLibrary(bench, kN);

    for (auto const& preset : builtinTrackPresentationPresets())
    {
      auto const duration = measureProjectionPresentationDuration(bench, preset.spec);
      APP_LOG_INFO("  {:<20} {} ms", preset.spec.id, duration.count());
      CHECK(duration < std::chrono::minutes{5});
    }
  }

  TEST_CASE("Phase 0 — 100k Baseline", "[baseline][unit]")
  {
    log::Log::init(log::LogLevel::Info);
    constexpr int kN = 100000;
    APP_LOG_INFO("=== Phase 0 Baseline: {} tracks ===", kN);

    auto bench = ScaleBench{};
    auto const t0 = std::chrono::steady_clock::now();
    buildLibrary(bench, kN);
    auto const t1 = std::chrono::steady_clock::now();

    auto const buildDuration = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);
    APP_LOG_INFO("  Library build: {} ms", buildDuration.count());

    auto const t = measureScale(bench, kN);

    APP_LOG_INFO("  Projection construct: {} ms", t.createProjectionDuration.count());
    APP_LOG_INFO("  setPresentation (sort): {} ms", t.setTitleSortDuration.count());
    APP_LOG_INFO("  SmartListEvaluator::evaluateMembers: {} ms", t.evaluateMembersDuration.count());
    APP_LOG_INFO("  Filter eval (heavy predicate): {} ms", t.filterEvalDuration.count());
    APP_LOG_INFO("  Filter eval (large IN list): {} ms", t.largeInEvalDuration.count());
    APP_LOG_INFO("  indexOf x10000: {} us", t.indexOfLookupDuration.count());

    // Regression thresholds — deliberately generous to avoid flakes
    CHECK(t.createProjectionDuration < std::chrono::seconds{30});
    CHECK(t.setTitleSortDuration < std::chrono::seconds{30});
    CHECK(t.evaluateMembersDuration < std::chrono::minutes{1});
    CHECK(t.filterEvalDuration < std::chrono::minutes{1});
    CHECK(t.largeInEvalDuration < std::chrono::minutes{1});
    CHECK(t.indexOfLookupDuration < std::chrono::microseconds{500000});
  }

  TEST_CASE("Phase 0 — 1M Baseline", "[baseline][unit]")
  {
    log::Log::init(log::LogLevel::Info);
    constexpr int kN = 1000000;
    APP_LOG_INFO("=== Phase 0 Baseline: {} tracks ===", kN);

    auto bench = ScaleBench{};
    auto const t0 = std::chrono::steady_clock::now();
    buildLibrary(bench, kN);
    auto const t1 = std::chrono::steady_clock::now();

    auto const buildDuration = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);
    APP_LOG_INFO("  Library build: {} ms", buildDuration.count());

    auto const t = measureScale(bench, kN);

    APP_LOG_INFO("  Projection construct: {} ms", t.createProjectionDuration.count());
    APP_LOG_INFO("  setPresentation (sort): {} ms", t.setTitleSortDuration.count());
    APP_LOG_INFO("  SmartListEvaluator::evaluateMembers: {} ms", t.evaluateMembersDuration.count());
    APP_LOG_INFO("  Filter eval (heavy predicate): {} ms", t.filterEvalDuration.count());
    APP_LOG_INFO("  Filter eval (large IN list): {} ms", t.largeInEvalDuration.count());
    APP_LOG_INFO("  indexOf x10000: {} us", t.indexOfLookupDuration.count());

    // Regression thresholds — deliberately generous to avoid flakes
    CHECK(t.createProjectionDuration < std::chrono::minutes{5});
    CHECK(t.setTitleSortDuration < std::chrono::minutes{5});
    CHECK(t.evaluateMembersDuration < std::chrono::minutes{10});
    CHECK(t.filterEvalDuration < std::chrono::minutes{10});
    CHECK(t.largeInEvalDuration < std::chrono::minutes{10});
    CHECK(t.indexOfLookupDuration < std::chrono::microseconds{500000});
  }
}
