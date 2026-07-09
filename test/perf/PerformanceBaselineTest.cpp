// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors
//
// Phase 0 baseline measurement — synthetic data, no fixed pass/fail thresholds.

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/TrackStore.h>
#include <ao/query/Field.h>
#include <ao/query/PlanEvaluator.h>
#include <ao/query/detail/Bytecode.h>
#include <ao/rt/Log.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/projection/LiveTrackListProjection.h>
#include <ao/rt/source/SmartListEvaluator.h>
#include <ao/rt/source/SmartListSource.h>
#include <ao/rt/source/TrackSource.h>

#include <boost/unordered/unordered_flat_map.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <functional>
#include <ios>
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
      std::string_view conductorKey{};
      std::string_view ensembleKey{};
      std::string_view workKey{};
      std::string_view soloistKey{};
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
      std::uint32_t conductorRank = 0;
      std::uint32_t ensembleRank = 0;
      std::uint32_t workRank = 0;
      std::uint32_t soloistRank = 0;
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

    struct BaselineMetric final
    {
      std::string name{};
      std::int64_t value = 0;
      std::string unit{};
    };

    struct BaselineRecord final
    {
      std::string benchmark{};
      std::vector<BaselineMetric> metrics{};
    };

    std::string jsonEscape(std::string_view value)
    {
      auto out = std::string{};
      out.reserve(value.size() + 8);

      for (auto const ch : value)
      {
        switch (ch)
        {
          case '"': out += "\\\""; break;
          case '\\': out += "\\\\"; break;
          case '\b': out += "\\b"; break;
          case '\f': out += "\\f"; break;
          case '\n': out += "\\n"; break;
          case '\r': out += "\\r"; break;
          case '\t': out += "\\t"; break;
          default:
            if (static_cast<unsigned char>(ch) < 0x20)
            {
              constexpr auto kHex = std::string_view{"0123456789abcdef"};
              out += "\\u00";
              out += kHex[(static_cast<unsigned char>(ch) >> 4) & 0x0f];
              out += kHex[static_cast<unsigned char>(ch) & 0x0f];
            }
            else
            {
              out += ch;
            }

            break;
        }
      }

      return out;
    }

    class BaselineRecorder final
    {
    public:
      BaselineRecorder() = default;
      BaselineRecorder(BaselineRecorder const&) = delete;
      BaselineRecorder& operator=(BaselineRecorder const&) = delete;
      BaselineRecorder(BaselineRecorder&&) = delete;
      BaselineRecorder& operator=(BaselineRecorder&&) = delete;
      ~BaselineRecorder()
      {
        auto const* const path = std::getenv("AOBUS_PERF_BASELINE_JSON");

        if (path == nullptr || path[0] == '\0')
        {
          return;
        }

        auto out = std::ofstream{path, std::ios::trunc};

        if (!out)
        {
          // NOLINTNEXTLINE(modernize-use-std-print) — destructor must not throw
          std::fprintf(stderr, "Aobus performance baseline: failed to open %s\n", path);
          return;
        }

        out << "{\n";
        out << R"(  "schema": "aobus-performance-baseline/v1",)" << "\n";
        out << "  \"records\": [\n";

        for (std::size_t recordIndex = 0; recordIndex < _records.size(); ++recordIndex)
        {
          auto const& record = _records[recordIndex];

          out << "    {\n";
          out << R"(      "benchmark": ")" << jsonEscape(record.benchmark) << R"(",)" << "\n";
          out << "      \"metrics\": [\n";

          for (std::size_t metricIndex = 0; metricIndex < record.metrics.size(); ++metricIndex)
          {
            auto const& metric = record.metrics[metricIndex];

            out << R"(        {"name": ")" << jsonEscape(metric.name) << R"(", "value": )" << metric.value
                << R"(, "unit": ")" << jsonEscape(metric.unit) << R"("})";

            if (metricIndex + 1 < record.metrics.size())
            {
              out << ",";
            }

            out << "\n";
          }

          out << "      ]\n";
          out << "    }";

          if (recordIndex + 1 < _records.size())
          {
            out << ",";
          }

          out << "\n";
        }

        out << "  ]\n";
        out << "}\n";

        if (!out)
        {
          // NOLINTNEXTLINE(modernize-use-std-print) — destructor must not throw
          std::fprintf(stderr, "Aobus performance baseline: failed to write %s\n", path);
        }
      }

      void record(BaselineRecord record) { _records.push_back(std::move(record)); }

    private:
      std::vector<BaselineRecord> _records;
    };

    BaselineRecorder& baselineRecorder()
    {
      static auto recorder = BaselineRecorder{};
      return recorder;
    }

    BaselineMetric metric(std::string name, std::int64_t value, std::string unit)
    {
      return BaselineMetric{.name = std::move(name), .value = value, .unit = std::move(unit)};
    }

    void recordBaseline(std::string benchmark, std::vector<BaselineMetric> metrics)
    {
      baselineRecorder().record(BaselineRecord{.benchmark = std::move(benchmark), .metrics = std::move(metrics)});
    }

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

      for (std::size_t index = 0; index < kPoolSize; ++index)
      {
        pool.push_back(std::format("sort-key-{:04d}", index));
      }

      return pool;
    }

    std::vector<SortCacheOrderEntry> makeSortCacheEntries(std::vector<std::string> const& pool, std::size_t count)
    {
      auto entries = std::vector<SortCacheOrderEntry>{};
      entries.reserve(count);

      for (std::size_t index = 0; index < count; ++index)
      {
        auto const scrambled = (index * 48271 + 9973) % count;
        auto const poolIndex = [poolSize = pool.size()](std::size_t value, std::size_t salt) -> std::size_t
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
              .titleKey = pool[poolIndex(scrambled, 1)],
              .artistKey = pool[poolIndex(scrambled / 50, 2)],
              .albumKey = pool[poolIndex(scrambled / 200, 3)],
              .albumArtistKey = pool[poolIndex(scrambled / 75, 4)],
              .genreKey = pool[poolIndex(scrambled % 20, 5)],
              .composerKey = pool[poolIndex(scrambled / 400, 6)],
              .conductorKey = pool[poolIndex(scrambled / 450, 7)],
              .ensembleKey = pool[poolIndex(scrambled / 300, 8)],
              .workKey = pool[poolIndex(scrambled / 500, 9)],
              .soloistKey = pool[poolIndex(scrambled / 350, 10)],
            },
          .groupKey = pool[poolIndex(scrambled / 50, 11)],
          .primaryText = pool[poolIndex(scrambled, 12)],
          .secondaryText = pool[poolIndex(scrambled / 200, 13)],
          .tertiaryText = pool[poolIndex(scrambled / 400, 14)],
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
        case TrackSortField::Conductor: return lhs.conductorKey.compare(rhs.conductorKey);
        case TrackSortField::Ensemble: return lhs.ensembleKey.compare(rhs.ensembleKey);
        case TrackSortField::Work: return lhs.workKey.compare(rhs.workKey);
        case TrackSortField::Soloist: return lhs.soloistKey.compare(rhs.soloistKey);
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

    using StringRanks = boost::unordered_flat_map<std::string_view, std::uint32_t>;

    StringRanks makeStringRanks(std::vector<std::string> const& pool)
    {
      auto sortedViews = std::vector<std::string_view>{};
      sortedViews.reserve(pool.size());

      for (auto const& value : pool)
      {
        sortedViews.push_back(value);
      }

      std::ranges::sort(sortedViews);
      sortedViews.erase(std::ranges::unique(sortedViews).begin(), sortedViews.end());

      auto ranks = StringRanks{};
      ranks.reserve(sortedViews.size());

      for (auto const& [index, value] : sortedViews | std::views::enumerate)
      {
        ranks.emplace(value, static_cast<std::uint32_t>(index + 1));
      }

      return ranks;
    }

    std::uint32_t rankOf(StringRanks const& ranks, std::string_view value)
    {
      if (auto const it = ranks.find(value); it != ranks.end())
      {
        return it->second;
      }

      return 0;
    }

    std::vector<RankedSortCacheOrderEntry> makeRankedSortCacheEntries(std::vector<SortCacheOrderEntry> const& entries,
                                                                      StringRanks const& ranks)
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
              .conductorRank = rankOf(ranks, entry.keys.conductorKey),
              .ensembleRank = rankOf(ranks, entry.keys.ensembleKey),
              .workRank = rankOf(ranks, entry.keys.workKey),
              .soloistRank = rankOf(ranks, entry.keys.soloistKey),
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
      auto const ranks = makeStringRanks(pool);
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

      for (auto const& [index, entry] : entries | std::views::enumerate)
      {
        positionIndex[entry.trackId] = static_cast<std::size_t>(index);
      }

      auto const positionEnd = std::chrono::steady_clock::now();

      auto const groupStart = std::chrono::steady_clock::now();
      std::size_t sectionCount = 0;

      if (!entries.empty())
      {
        ++sectionCount;

        for (std::size_t index = 1; index < entries.size(); ++index)
        {
          if (entries[index].groupKey != entries[index - 1].groupKey)
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

      for (std::size_t index = 0; index < listSize; ++index)
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
          .constValue = static_cast<std::int64_t>(1990 + index),
          .size = 0,
          .data = nullptr,
        });

        plan.instructions.push_back(query::Instruction{
          .op = query::OpCode::Eq,
          .field = static_cast<std::uint8_t>(query::Field::Year),
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

      return plan;
    }

    query::ExecutionPlan makeSetYearInPlan(std::size_t listSize)
    {
      auto plan = query::ExecutionPlan{};
      auto set = query::InSet{};

      for (std::size_t index = 0; index < listSize; ++index)
      {
        set.numericValues.insert(static_cast<std::int64_t>(1990 + index));
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
        .field = static_cast<std::uint8_t>(query::Field::Year),
        .operand = 0,
        .constValue = 0,
        .size = 0,
        .data = nullptr,
      });

      return plan;
    }

    struct ScaleBench final
    {
      MusicLibraryFixture libraryFixture;
      std::vector<TrackId> ids;
    };

    void buildLibrary(ScaleBench& bench, std::int32_t trackCount)
    {
      bench.ids.reserve(trackCount);

      for (std::int32_t index = 0; index < trackCount; ++index)
      {
        auto const spec = library::test::TrackSpec{
          .title = std::format("Track {:06d}", index),
          .artist = std::format("Artist {:04d}", index % (trackCount / 50 + 1)),
          .album = std::format("Album {:04d}", index % (trackCount / 200 + 1)),
          .genre = std::format("Genre {:02d}", index % 20),
          .year = static_cast<std::uint16_t>(1990 + (index % 35)),
          .discNumber = static_cast<std::uint16_t>(1 + (index % 3)),
          .trackNumber = static_cast<std::uint16_t>(1 + (index % 20)),
          .duration = std::chrono::minutes{3} + std::chrono::milliseconds{static_cast<std::uint32_t>(
                                                  (static_cast<std::int64_t>(index) * 137) %
                                                  std::chrono::milliseconds{std::chrono::minutes{7}}.count())},
        };
        bench.ids.push_back(bench.libraryFixture.addTrack(spec));
      }
    }

    class BenchmarkTrackSource final : public TrackSource
    {
    public:
      explicit BenchmarkTrackSource(std::vector<TrackId> ids)
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
      auto& lib = bench.libraryFixture.library();

      // 1. Projection construction + setPresentation
      auto source = BenchmarkTrackSource{bench.ids};

      auto const t0 = std::chrono::steady_clock::now();
      auto proj = LiveTrackListProjection{ViewId{1}, source, lib};
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

    void recordScaleBaseline(std::size_t trackCount, std::chrono::milliseconds buildDuration, Timings const& timing)
    {
      recordBaseline("scale-baseline",
                     {
                       metric("track_count", static_cast<std::int64_t>(trackCount), "count"),
                       metric("library_build", buildDuration.count(), "ms"),
                       metric("projection_construct", timing.createProjectionDuration.count(), "ms"),
                       metric("set_presentation_sort", timing.setTitleSortDuration.count(), "ms"),
                       metric("evaluate_members", timing.evaluateMembersDuration.count(), "ms"),
                       metric("filter_eval_heavy_predicate", timing.filterEvalDuration.count(), "ms"),
                       metric("filter_eval_large_in_list", timing.largeInEvalDuration.count(), "ms"),
                       metric("index_of_10000", timing.indexOfLookupDuration.count(), "us"),
                     });
    }

    InThresholdTiming measureYearInThreshold(ScaleBench& bench, std::size_t listSize)
    {
      auto expandedPlan = makeExpandedYearInPlan(listSize);
      auto setPlan = makeSetYearInPlan(listSize);
      auto evaluator = query::PlanEvaluator{};
      auto& lib = bench.libraryFixture.library();
      auto transaction = lib.readTransaction();
      auto reader = lib.tracks().reader(transaction);

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
      auto source = BenchmarkTrackSource{bench.ids};
      auto proj = LiveTrackListProjection{ViewId{1}, source, bench.libraryFixture.library()};

      auto const start = std::chrono::steady_clock::now();
      proj.setPresentation(TrackPresentationSpec{
        .groupBy = TrackGroupKey::None, .sortBy = {TrackSortTerm{.field = field, .ascending = true}}});
      auto const end = std::chrono::steady_clock::now();

      return std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    }

    std::chrono::milliseconds measureProjectionPresentationDuration(ScaleBench& bench,
                                                                    TrackPresentationSpec const& spec)
    {
      auto source = BenchmarkTrackSource{bench.ids};
      auto proj = LiveTrackListProjection{ViewId{1}, source, bench.libraryFixture.library()};

      auto const start = std::chrono::steady_clock::now();
      proj.setPresentation(spec);
      auto const end = std::chrono::steady_clock::now();

      return std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    }
  } // namespace

  TEST_CASE("PerformanceBaseline - phase 0 10k baseline", "[perf][unit][baseline]")
  {
    Log::initialize(LogLevel::Info);
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
    recordScaleBaseline(kN, buildDuration, t);

    // Regression thresholds — deliberately generous to avoid flakes
    CHECK(t.createProjectionDuration < std::chrono::seconds{5});
    CHECK(t.setTitleSortDuration < std::chrono::seconds{5});
    CHECK(t.evaluateMembersDuration < std::chrono::seconds{10});
    CHECK(t.filterEvalDuration < std::chrono::seconds{10});
    CHECK(t.largeInEvalDuration < std::chrono::seconds{10});
    CHECK(t.indexOfLookupDuration < std::chrono::microseconds{500000});
  }

  TEST_CASE("PerformanceBaseline - phase 0 query IN threshold sweep", "[perf][unit][baseline][query]")
  {
    Log::initialize(LogLevel::Info);
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
      recordBaseline("query-in-threshold",
                     {
                       metric("track_count", kN, "count"),
                       metric("list_size", static_cast<std::int64_t>(timing.listSize), "count"),
                       metric("expanded", timing.expandedDuration.count(), "us"),
                       metric("set", timing.setDuration.count(), "us"),
                       metric("expanded_matches", static_cast<std::int64_t>(timing.expandedMatches), "count"),
                       metric("set_matches", static_cast<std::int64_t>(timing.setMatches), "count"),
                     });

      CHECK(timing.expandedMatches == timing.setMatches);
    }
  }

  TEST_CASE("PerformanceBaseline - phase 0 OrderEntry direct entry sort cache pressure",
            "[perf][unit][baseline][projection]")
  {
    Log::initialize(LogLevel::Info);
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
      recordBaseline("order-entry-direct-sort-cache-pressure",
                     {
                       metric("entry_count", static_cast<std::int64_t>(timing.count), "count"),
                       metric("entry_payload", static_cast<std::int64_t>(entryBytes / 1024), "KiB"),
                       metric("duration", timing.duration.count(), "ms"),
                     });
      CHECK(timing.checksum != 0);
    }
  }

  TEST_CASE("PerformanceBaseline - phase 0 OrderEntry comparator dispatch cost", "[perf][unit][baseline][projection]")
  {
    Log::initialize(LogLevel::Info);
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
      recordBaseline("order-entry-comparator-dispatch-cost",
                     {
                       metric("entry_count", static_cast<std::int64_t>(count), "count"),
                       metric("generic_loop_switch_comparator", generic.duration.count(), "ms"),
                       metric("specialized_comparator", specialized.duration.count(), "ms"),
                     });

      CHECK(generic.checksum == specialized.checksum);
    }
  }

  TEST_CASE("PerformanceBaseline - phase 0 OrderEntry ranked integer key sort", "[perf][unit][baseline][projection]")
  {
    Log::initialize(LogLevel::Info);
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
      recordBaseline("order-entry-ranked-integer-key-sort",
                     {
                       metric("entry_count", static_cast<std::int64_t>(timing.count), "count"),
                       metric("build_rank_map", timing.buildRankMapDuration.count(), "ms"),
                       metric("build_ranked_entries", timing.buildEntriesDuration.count(), "ms"),
                       metric("ranked_integer_sort", timing.sortDuration.count(), "ms"),
                       metric("total_rank_sort_path", timing.totalDuration.count(), "ms"),
                     });

      CHECK(timing.checksum != 0);
    }
  }

  TEST_CASE("PerformanceBaseline - phase 0 OrderEntry compact active key sort", "[perf][unit][baseline][projection]")
  {
    Log::initialize(LogLevel::Info);
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
      recordBaseline("order-entry-compact-active-key-sort",
                     {
                       metric("entry_count", static_cast<std::int64_t>(timing.count), "count"),
                       metric("build_compact_entries", timing.buildEntriesDuration.count(), "ms"),
                       metric("compact_string_sort", timing.sortDuration.count(), "ms"),
                       metric("total_compact_path", timing.totalDuration.count(), "ms"),
                     });

      CHECK(timing.checksum != 0);
    }
  }

  TEST_CASE("PerformanceBaseline - phase 0 OrderEntry index sort cache pressure", "[perf][unit][baseline][projection]")
  {
    Log::initialize(LogLevel::Info);
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
      recordBaseline("order-entry-index-sort-cache-pressure",
                     {
                       metric("entry_count", static_cast<std::int64_t>(timing.count), "count"),
                       metric("entry_payload", static_cast<std::int64_t>(entryBytes / 1024), "KiB"),
                       metric("index_payload", static_cast<std::int64_t>(indexBytes / 1024), "KiB"),
                       metric("duration", timing.duration.count(), "ms"),
                     });
      CHECK(timing.checksum != 0);
    }
  }

  TEST_CASE("PerformanceBaseline - phase 0 OrderEntry index materialize cache pressure",
            "[perf][unit][baseline][projection]")
  {
    Log::initialize(LogLevel::Info);
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
      recordBaseline("order-entry-index-materialize-cache-pressure",
                     {
                       metric("entry_count", static_cast<std::int64_t>(timing.count), "count"),
                       metric("entry_payload", static_cast<std::int64_t>(entryBytes / 1024), "KiB"),
                       metric("index_payload", static_cast<std::int64_t>(indexBytes / 1024), "KiB"),
                       metric("duration", timing.duration.count(), "ms"),
                     });
      CHECK(timing.checksum != 0);
    }
  }

  TEST_CASE("PerformanceBaseline - phase 0 projection rebuild stage timing", "[perf][unit][baseline][projection]")
  {
    Log::initialize(LogLevel::Info);
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
      recordBaseline("projection-rebuild-stage-timing",
                     {
                       metric("entry_count", static_cast<std::int64_t>(timing.count), "count"),
                       metric("section_count", static_cast<std::int64_t>(timing.sectionCount), "count"),
                       metric("build_entries_keys", timing.buildEntriesDuration.count(), "ms"),
                       metric("sort_entries", timing.sortDuration.count(), "ms"),
                       metric("rebuild_position_index", timing.rebuildPositionIndexDuration.count(), "ms"),
                       metric("build_group_sections", timing.buildGroupSectionsDuration.count(), "ms"),
                       metric("total_synthetic_rebuild", timing.totalDuration.count(), "ms"),
                     });
      CHECK(timing.checksum != 0);
    }
  }

  TEST_CASE("PerformanceBaseline - phase 0 projection sort field timing", "[perf][unit][baseline][projection]")
  {
    Log::initialize(LogLevel::Info);
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
    recordBaseline("projection-sort-field-timing",
                   {
                     metric("track_count", kN, "count"),
                     metric("title_sort", titleDuration.count(), "ms"),
                     metric("artist_sort", artistDuration.count(), "ms"),
                     metric("album_sort", albumDuration.count(), "ms"),
                     metric("genre_sort", genreDuration.count(), "ms"),
                   });

    CHECK(titleDuration < std::chrono::minutes{5});
    CHECK(artistDuration < std::chrono::minutes{5});
    CHECK(albumDuration < std::chrono::minutes{5});
    CHECK(genreDuration < std::chrono::minutes{5});
  }

  TEST_CASE("PerformanceBaseline - phase 0 projection preset timing", "[perf][unit][baseline][projection]")
  {
    Log::initialize(LogLevel::Info);
    constexpr int kN = 1000000;

    APP_LOG_INFO("=== Phase 0 Projection preset timing: {} tracks ===", kN);

    auto bench = ScaleBench{};
    buildLibrary(bench, kN);

    for (auto const& preset : builtinTrackPresentationPresets())
    {
      auto const duration = measureProjectionPresentationDuration(bench, preset.spec);
      APP_LOG_INFO("  {:<20} {} ms", preset.spec.id, duration.count());
      recordBaseline(std::format("projection-preset-timing/{}", preset.spec.id),
                     {
                       metric("track_count", kN, "count"),
                       metric("duration", duration.count(), "ms"),
                     });
      CHECK(duration < std::chrono::minutes{5});
    }
  }

  TEST_CASE("PerformanceBaseline - phase 0 100k baseline", "[perf][unit][baseline]")
  {
    Log::initialize(LogLevel::Info);
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
    recordScaleBaseline(kN, buildDuration, t);

    // Regression thresholds — deliberately generous to avoid flakes
    CHECK(t.createProjectionDuration < std::chrono::seconds{30});
    CHECK(t.setTitleSortDuration < std::chrono::seconds{30});
    CHECK(t.evaluateMembersDuration < std::chrono::minutes{1});
    CHECK(t.filterEvalDuration < std::chrono::minutes{1});
    CHECK(t.largeInEvalDuration < std::chrono::minutes{1});
    CHECK(t.indexOfLookupDuration < std::chrono::microseconds{500000});
  }

  TEST_CASE("PerformanceBaseline - phase 0 1M baseline", "[perf][unit][baseline]")
  {
    Log::initialize(LogLevel::Info);
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
    recordScaleBaseline(kN, buildDuration, t);

    // Regression thresholds — deliberately generous to avoid flakes
    CHECK(t.createProjectionDuration < std::chrono::minutes{5});
    CHECK(t.setTitleSortDuration < std::chrono::minutes{5});
    CHECK(t.evaluateMembersDuration < std::chrono::minutes{10});
    CHECK(t.filterEvalDuration < std::chrono::minutes{10});
    CHECK(t.largeInEvalDuration < std::chrono::minutes{10});
    CHECK(t.indexOfLookupDuration < std::chrono::microseconds{500000});
  }
} // namespace ao::rt::test
