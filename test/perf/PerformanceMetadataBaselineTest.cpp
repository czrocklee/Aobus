// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "PerformanceReport.h"
#include "runtime/source/SmartListEvaluator.h"
#include "runtime/source/SmartListSource.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/Credits.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/RecordingDate.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackWriter.h>
#include <ao/rt/PlaybackLaunchSpec.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    constexpr auto kLibrarySizes = std::to_array<std::size_t>({10000, 100000});
    constexpr std::size_t kMovementsPerPerformance = 10;

    // Keep legacy fixture text and report scenario labels for comparison. All
    // former list members are Performer credits; operations and oracles stay fixed.
    // One quarter has neither date nor credits, so its block is omitted.
    constexpr std::size_t kDateKindPeriod = 4;
    constexpr std::size_t kNameCount = 256;
    constexpr std::size_t kYearPeriod = 40;
    constexpr std::uint16_t kFirstFixtureYear = 1950;
    constexpr std::uint16_t kEqualityYear = 1955;
    constexpr std::uint16_t kRangeStartYear = 1950;
    constexpr std::uint16_t kRangeEndYear = 1959;
    constexpr std::size_t kPerformanceBlockBytes = 12;
    constexpr std::size_t kMusicianEntryBytes = 8;
    constexpr auto kListLengths = std::to_array<std::size_t>({0, 1, 8, 32});

    struct PerformanceMetadataFixture final
    {
      ao::test::TempDir tempDir;
      library::MusicLibrary library{
        ao::test::requireValue(library::MusicLibrary::open(tempDir.path(),
                                                           tempDir.path(),
                                                           {.pinnedMapBytes = std::uint64_t{2} * 1024 * 1024 * 1024}))};
      std::vector<TrackId> ids;
    };

    // The fixture derives dates through this local enum and its component
    // arithmetic; query literals and oracles use the same arithmetic and never
    // the production parse, format, compare, or readback paths.
    enum class FixtureDateKind : std::uint8_t
    {
      Absent,
      YearOnly,
      YearMonth,
      Full,
    };

    struct FixtureDate final
    {
      FixtureDateKind kind = FixtureDateKind::Absent;
      std::uint16_t year = 0;
      std::uint8_t month = 0;
      std::uint8_t day = 0;
    };

    FixtureDate fixtureDate(std::size_t const performanceIndex)
    {
      auto const kind = static_cast<FixtureDateKind>(performanceIndex % kDateKindPeriod);

      if (kind == FixtureDateKind::Absent)
      {
        return {};
      }

      return FixtureDate{
        .kind = kind,
        .year = static_cast<std::uint16_t>(kFirstFixtureYear + ((performanceIndex / 4) % kYearPeriod)),
        .month =
          kind == FixtureDateKind::YearOnly ? std::uint8_t{0} : static_cast<std::uint8_t>((performanceIndex % 12) + 1),
        .day = kind == FixtureDateKind::Full ? static_cast<std::uint8_t>((performanceIndex % 28) + 1) : std::uint8_t{0},
      };
    }

    std::size_t fixtureListLength(std::size_t const performanceIndex)
    {
      return kListLengths[performanceIndex % kListLengths.size()];
    }

    library::test::TrackSpec performanceTrackSpec(std::size_t const index)
    {
      auto const performanceIndex = index / kMovementsPerPerformance;
      auto const date = fixtureDate(performanceIndex);
      auto const listLength = fixtureListLength(performanceIndex);
      auto musicians = std::vector<library::Credit>{};
      musicians.reserve(listLength);

      for (std::size_t entry = 0; entry < listLength; ++entry)
      {
        musicians.push_back(
          library::Credit{.name = std::format("Musician {:03}", (performanceIndex + entry) % kNameCount),
                          .role = std::format("Instrument {:02}", entry % 8)});
      }

      return library::test::TrackSpec{
        .title = std::format("Title {:06}", index),
        .recordingDate = library::RecordingDate{.year = date.year, .month = date.month, .day = date.day},
        .credits = std::move(musicians),
        .uri = std::format("performance-baseline/{:06}.flac", index),
      };
    }

    void seedLibrary(PerformanceMetadataFixture& fixture, std::size_t const count)
    {
      fixture.ids.reserve(count);
      auto transaction = library::test::writeTransaction(fixture.library);
      auto seedRes = transaction.apply(
        [&](library::LibraryWrite& write) -> Result<>
        {
          auto writer = write.tracks();

          for (std::size_t index = 0; index < count; ++index)
          {
            auto const spec = performanceTrackSpec(index);
            auto builder = library::TrackBuilder::makeEmpty();
            library::test::applyTrackSpec(builder, spec);
            auto createRes = writer.create(builder, library::FileManifestBuilder::makeEmpty());

            if (!createRes)
            {
              INFO(std::format("Track {}: {}", index, createRes.error().message));
              REQUIRE(createRes);
              return std::unexpected{createRes.error()};
            }

            fixture.ids.push_back(*createRes);
          }

          return {};
        });
      REQUIRE(seedRes);
      REQUIRE(transaction.commit());
    }

    std::string datasetLabel(PerformanceMetadataFixture const& fixture, std::size_t const candidates)
    {
      return std::format("performance-v1/library={}/candidates={}/list-lengths=0-1-8-32/page-cache=seeded-warm",
                         fixture.ids.size(),
                         candidates);
    }

    // Structural payload arithmetic over the fixture, outside every clock. The
    // byte counts are expected encoded shapes (12-byte prefix per engaged
    // performance block plus eight bytes per entry), not live payload or heap.
    std::size_t expectedMusicianEntries(std::size_t const trackCount)
    {
      auto const performanceCount = trackCount / kMovementsPerPerformance;
      std::size_t entries = 0;

      for (std::size_t performanceIndex = 0; performanceIndex < performanceCount; ++performanceIndex)
      {
        entries += fixtureListLength(performanceIndex) * kMovementsPerPerformance;
      }

      return entries;
    }

    std::size_t expectedPerformanceBlockTracks(std::size_t const trackCount)
    {
      auto const performanceCount = trackCount / kMovementsPerPerformance;
      std::size_t tracks = 0;

      for (std::size_t performanceIndex = 0; performanceIndex < performanceCount; ++performanceIndex)
      {
        if (fixtureDate(performanceIndex).kind != FixtureDateKind::Absent)
        {
          tracks += kMovementsPerPerformance;
        }
      }

      return tracks;
    }

    enum class DateMatch : std::uint8_t
    {
      YearEquality,
      FullEquality,
      YearRange,
      Existence,
    };

    std::vector<TrackId> expectedDateMembers(std::span<TrackId const> candidates,
                                             DateMatch const match,
                                             FixtureDate const& target)
    {
      auto expected = std::vector<TrackId>{};

      for (std::size_t index = 0; index < candidates.size(); ++index)
      {
        auto const date = fixtureDate(index / kMovementsPerPerformance);
        bool matches = false;

        switch (match)
        {
          case DateMatch::YearEquality:
            matches = date.kind != FixtureDateKind::Absent && date.year == target.year;
            break;
          case DateMatch::FullEquality:
            matches = date.kind == FixtureDateKind::Full && date.year == target.year && date.month == target.month &&
                      date.day == target.day;
            break;
          case DateMatch::YearRange:
            matches =
              date.kind != FixtureDateKind::Absent && kRangeStartYear <= date.year && date.year <= kRangeEndYear;
            break;
          case DateMatch::Existence: matches = date.kind != FixtureDateKind::Absent; break;
        }

        if (matches)
        {
          expected.push_back(candidates[index]);
        }
      }

      return expected;
    }

    std::optional<FixtureDate> firstFullDate(std::span<TrackId const> candidates)
    {
      for (std::size_t index = 0; index < candidates.size(); ++index)
      {
        auto const date = fixtureDate(index / kMovementsPerPerformance);

        if (date.kind == FixtureDateKind::Full)
        {
          return date;
        }
      }

      return std::nullopt;
    }

    // Membership oracle for name-index targets: a track matches when any list
    // entry resolves to a name index inside [targetNameIndex, +targetNameCount).
    std::vector<TrackId> expectedMusicianMembers(std::span<TrackId const> candidates,
                                                 std::size_t const targetNameIndex,
                                                 std::size_t const targetNameCount)
    {
      auto expected = std::vector<TrackId>{};

      for (std::size_t index = 0; index < candidates.size(); ++index)
      {
        auto const performanceIndex = index / kMovementsPerPerformance;
        auto const listLength = fixtureListLength(performanceIndex);

        for (std::size_t entry = 0; entry < listLength; ++entry)
        {
          auto const nameIndex = (performanceIndex + entry) % kNameCount;

          if (targetNameIndex <= nameIndex && nameIndex < targetNameIndex + targetNameCount)
          {
            expected.push_back(candidates[index]);
            break;
          }
        }
      }

      return expected;
    }

    // Quarter source for one exact list length: enumerates global seed indices
    // from the seed formula, so candidate positions are never mistaken for
    // compressed quarter positions; every member carries exactly @p listLength
    // entries. Fixed-width titles keep source order equal to title order.
    std::vector<TrackId> quarterSourceIds(PerformanceMetadataFixture const& fixture, std::size_t const listLength)
    {
      auto ids = std::vector<TrackId>{};
      ids.reserve(fixture.ids.size() / kListLengths.size());

      for (std::size_t index = 0; index < fixture.ids.size(); ++index)
      {
        if (fixtureListLength(index / kMovementsPerPerformance) == listLength)
        {
          ids.push_back(fixture.ids[index]);
        }
      }

      REQUIRE(ids.size() == fixture.ids.size() / kListLengths.size());
      return ids;
    }

    // The substring-miss literal is checked against the complete fixture text
    // pool derived from the same seed formulas, so an empty match set is an
    // independently verified miss rather than an assumed one.
    void requireMissLiteralAbsentFromFixtureText(std::string_view const literal)
    {
      for (std::size_t nameIndex = 0; nameIndex < kNameCount; ++nameIndex)
      {
        CHECK_FALSE(std::format("Musician {:03}", nameIndex).contains(literal));
      }

      // Entry roles cycle over the eight distinct texts the seed formula emits.
      for (std::size_t entry = 0; entry < 8; ++entry)
      {
        CHECK_FALSE(std::format("Instrument {:02}", entry).contains(literal));
      }
    }

    // Full ID-order oracle: sorts fixture-derived tuples, never production sort
    // keys or comparators. Absence stays last; present dates order by the
    // recorded components; fixed-width titles break ties by source index.
    std::vector<TrackId> expectedRecordingOrder(std::span<TrackId const> candidates, bool const fullTuple)
    {
      auto entries = std::vector<std::tuple<bool, std::uint16_t, std::uint8_t, std::uint8_t, std::size_t>>{};
      entries.reserve(candidates.size());

      for (std::size_t index = 0; index < candidates.size(); ++index)
      {
        auto const date = fixtureDate(index / kMovementsPerPerformance);
        entries.emplace_back(date.kind == FixtureDateKind::Absent,
                             date.year,
                             fullTuple ? date.month : std::uint8_t{0},
                             fullTuple ? date.day : std::uint8_t{0},
                             index);
      }

      std::ranges::sort(entries);

      auto expected = std::vector<TrackId>{};
      expected.reserve(candidates.size());

      for (auto const& entry : entries)
      {
        expected.push_back(candidates[std::get<4>(entry)]);
      }

      return expected;
    }

    void requireProjectionOrder(TrackListProjection const& projection, std::span<TrackId const> expected)
    {
      REQUIRE(projection.size() == expected.size());

      for (std::size_t rowIndex = 0; rowIndex < expected.size(); ++rowIndex)
      {
        if (auto const actualId = projection.trackIdAt(rowIndex); actualId != expected[rowIndex])
        {
          CAPTURE(rowIndex, actualId, expected[rowIndex]);
          FAIL("Projection ID sequence differs from the fixture oracle");
        }
      }
    }

    template<typename Operation, typename Validate>
    void measure(std::vector<Measurement>& measurements,
                 Measurement measurement,
                 std::size_t warmups,
                 std::size_t samples,
                 Operation operation,
                 Validate validate)
    {
      auto elapsed = std::vector<std::int64_t>{};
      elapsed.reserve(samples);

      for (std::size_t run = 0; run < warmups + samples; ++run)
      {
        auto const start = std::chrono::steady_clock::now();
        operation();
        auto const duration =
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
        validate();

        if (run >= warmups)
        {
          elapsed.push_back(duration);
        }
      }

      setPercentiles(measurement, elapsed);
      measurements.push_back(std::move(measurement));
    }

    void measureDateQueries(PerformanceMetadataFixture& fixture,
                            std::span<TrackId const> candidates,
                            std::vector<Measurement>& measurements,
                            std::size_t warmups,
                            std::size_t samples)
    {
      auto const optFullTarget = firstFullDate(candidates);
      INFO("Fixture candidate source must include a full-precision recording date");
      REQUIRE(optFullTarget);
      auto const fullTarget = *optFullTarget;
      auto const fullLiteral = std::format("{:04}-{:02}-{:02}", fullTarget.year, fullTarget.month, fullTarget.day);

      struct DateWorkload final
      {
        std::string_view scenario;
        std::string expression;
        DateMatch match;
        FixtureDate target{};
      };

      auto const workloads = std::vector<DateWorkload>{
        {"date-year-equality",
         "$recordingDate = 1955",
         DateMatch::YearEquality,
         FixtureDate{.kind = FixtureDateKind::Full, .year = kEqualityYear}},
        {"date-full-equality", std::format("$recordingDate = '{}'", fullLiteral), DateMatch::FullEquality, fullTarget},
        {"date-year-range", "$recordingDate in 1950..1959", DateMatch::YearRange, {}},
        {"date-existence", "$recordingDate?", DateMatch::Existence, {}},
      };

      for (auto const& workload : workloads)
      {
        CAPTURE(workload.scenario, candidates.size());
        auto sourcePtr = makeMutableTrackSource(candidates);
        auto evaluator = SmartListEvaluator{fixture.library};
        auto filtered = SmartListSource{TrackSourceLease{sourcePtr}, evaluator};
        auto const expected = expectedDateMembers(candidates, workload.match, workload.target);

        REQUIRE_FALSE(expected.empty());

        if (workload.match == DateMatch::Existence)
        {
          REQUIRE(candidates.size() % (kMovementsPerPerformance * kDateKindPeriod) == 0);
          CHECK(expected.size() == candidates.size() / 4 * 3);
        }

        measure(
          measurements,
          {.capability = "performance-query",
           .scenario = std::string{workload.scenario},
           .dataset = datasetLabel(fixture, candidates.size()),
           .inputCount = candidates.size()},
          warmups,
          samples,
          [&]
          {
            filtered.setExpression(workload.expression);
            filtered.reload();
          },
          [&]
          {
            REQUIRE_FALSE(filtered.hasError());
            CHECK(sourceTrackIds(filtered) == expected);
          });
      }
    }

    void measureMusicianQueries(PerformanceMetadataFixture& fixture,
                                std::span<TrackId const> candidates,
                                std::vector<Measurement>& measurements,
                                std::size_t warmups,
                                std::size_t samples)
    {
      struct MusicianWorkload final
      {
        std::string_view scenario;
        std::string_view expression;
        std::size_t targetNameIndex;
        std::size_t targetNameCount;
      };

      constexpr auto kMusicianWorkloads = std::to_array<MusicianWorkload>({
        {"musician-equality", "$credit = 'Musician 003'", 3, 1},
        {"musician-substring", "$credit ~ 'sician 00'", 0, 10},
      });

      for (auto const& workload : kMusicianWorkloads)
      {
        CAPTURE(workload.scenario, candidates.size());
        auto sourcePtr = makeMutableTrackSource(candidates);
        auto evaluator = SmartListEvaluator{fixture.library};
        auto filtered = SmartListSource{TrackSourceLease{sourcePtr}, evaluator};
        auto const expected = expectedMusicianMembers(candidates, workload.targetNameIndex, workload.targetNameCount);

        REQUIRE_FALSE(expected.empty());

        if (workload.targetNameCount == 1)
        {
          // Hand-derived pin over the name-index congruences: 'Musician 003'
          // is carried exactly by performances congruent to one of ten
          // residues modulo 256 ({2, 3, 231, 235, 239, 243, 247, 251, 254,
          // 255}), counting wraparound credits of the 8- and 32-entry lists.
          // That yields 2, 33, or 392 performances for 100, 1000, or 10000
          // performances, hence 20, 330, or 3920 tracks.
          constexpr auto kExpectedEqualityCounts = std::to_array<std::pair<std::size_t, std::size_t>>({
            {1000, 20},
            {10000, 330},
            {100000, 3920},
          });
          bool hasKnownCandidateCount = false;

          for (auto const& [candidateCount, expectedCount] : kExpectedEqualityCounts)
          {
            if (candidates.size() == candidateCount)
            {
              CHECK(expected.size() == expectedCount);
              hasKnownCandidateCount = true;
            }
          }

          REQUIRE(hasKnownCandidateCount);
        }

        measure(
          measurements,
          {.capability = "performance-query",
           .scenario = std::string{workload.scenario},
           .dataset = datasetLabel(fixture, candidates.size()),
           .inputCount = candidates.size()},
          warmups,
          samples,
          [&]
          {
            filtered.setExpression(std::string{workload.expression});
            filtered.reload();
          },
          [&]
          {
            REQUIRE_FALSE(filtered.hasError());
            CHECK(sourceTrackIds(filtered) == expected);
          });
      }
    }

    void measureProjectionRebuilds(PerformanceMetadataFixture& fixture,
                                   std::span<TrackId const> candidates,
                                   std::vector<Measurement>& measurements,
                                   std::size_t warmups,
                                   std::size_t samples)
    {
      auto sourcePtr = makeMutableTrackSource(candidates);

      constexpr auto kProjectionWorkloads = std::to_array<std::tuple<TrackSortField, std::string_view, bool>>({
        {TrackSortField::RecordingDate, "recording-date-sort-rebuild", true},
        {TrackSortField::RecordingYear, "recording-year-sort-rebuild", false},
        {TrackSortField::Title, "title-sort-rebuild-hot-control", false},
      });

      for (auto const& [field, scenario, fullTuple] : kProjectionWorkloads)
      {
        CAPTURE(scenario, candidates.size());
        auto const expected = field == TrackSortField::Title
                                ? std::vector<TrackId>{candidates.begin(), candidates.end()}
                                : expectedRecordingOrder(candidates, fullTuple);

        if (field != TrackSortField::Title && candidates.size() == 10000)
        {
          // Pin an interior example: the first present recording is the second
          // performance's first movement, ahead of the same year's more
          // precise dates by the zero-component order.
          CHECK(expected.front() == candidates[10]);
        }

        auto const sortBy = field == TrackSortField::Title
                              ? std::vector<TrackSortTerm>{{TrackSortField::Title}}
                              : std::vector<TrackSortTerm>{{field}, {TrackSortField::Title}};
        auto projectionPtr = std::unique_ptr<TrackListProjection>{};

        // Each sample builds a fresh projection with empty derived caches;
        // retirement happens in validation, outside the clock. The hot Title
        // control sorts a source whose tracks carry up to 32 musician entries
        // each; this row stays hot-shaped only while the projection does not
        // materialize cold lists per row, whose allocation evidence is
        // collected separately by the profiler pass.
        measure(
          measurements,
          {.capability = "performance-projection",
           .scenario = std::string{scenario},
           .dataset = datasetLabel(fixture, candidates.size()),
           .inputCount = candidates.size()},
          warmups,
          samples,
          [&]
          {
            projectionPtr = std::make_unique<TrackListProjection>(
              kInvalidViewId, TrackSourceLease{sourcePtr}, fixture.library, TrackOrderSpec{.sortBy = sortBy});
          },
          [&]
          {
            requireProjectionOrder(*projectionPtr, expected);
            CHECK(projectionPtr->indexOf(candidates.back()).has_value());
            projectionPtr.reset();
          });
      }
    }
    // List-length isolation: the mixed 0/1/8/32 source alone cannot compare
    // per-length cost, so each length gets its own quarter source. The miss
    // substring forces a complete member traversal of every list (an empty
    // result is the verified expectation), and the Title control on the same
    // quarter source stays hot-shaped while the lists stay populated. Every
    // sample uses fresh derived caches; retirement stays outside the clock.
    void measureListLengthIsolation(PerformanceMetadataFixture& fixture,
                                    std::vector<Measurement>& measurements,
                                    std::size_t warmups,
                                    std::size_t samples)
    {
      constexpr auto kMissLiteral = std::string_view{"not-present-fragment"};
      requireMissLiteralAbsentFromFixtureText(kMissLiteral);

      for (auto const listLength : kListLengths)
      {
        auto const candidates = quarterSourceIds(fixture, listLength);
        auto const dataset = std::format("performance-v1/library={}/musicians-per-track={}/source=quarter"
                                         "/page-cache=seeded-warm",
                                         fixture.ids.size(),
                                         listLength);
        CAPTURE(listLength, candidates.size());

        {
          auto sourcePtr = makeMutableTrackSource(candidates);
          auto evaluator = SmartListEvaluator{fixture.library};
          auto filtered = SmartListSource{TrackSourceLease{sourcePtr}, evaluator};

          measure(
            measurements,
            {.capability = "performance-query",
             .scenario = "musician-substring-miss",
             .dataset = dataset,
             .inputCount = candidates.size()},
            warmups,
            samples,
            [&]
            {
              filtered.setExpression(std::format("$credit ~ '{}'", kMissLiteral));
              filtered.reload();
            },
            [&]
            {
              REQUIRE_FALSE(filtered.hasError());
              CHECK(sourceTrackIds(filtered).empty());
            });
        }

        {
          auto sourcePtr = makeMutableTrackSource(candidates);
          auto projectionPtr = std::unique_ptr<TrackListProjection>{};

          measure(
            measurements,
            {.capability = "performance-projection",
             .scenario = "title-sort-rebuild-hot-control",
             .dataset = dataset,
             .inputCount = candidates.size()},
            warmups,
            samples,
            [&]
            {
              projectionPtr =
                std::make_unique<TrackListProjection>(kInvalidViewId,
                                                      TrackSourceLease{sourcePtr},
                                                      fixture.library,
                                                      TrackOrderSpec{.sortBy = {{TrackSortField::Title}}});
            },
            [&]
            {
              requireProjectionOrder(*projectionPtr, candidates);
              CHECK(projectionPtr->indexOf(candidates.back()).has_value());
              projectionPtr.reset();
            });
        }
      }
    }
  } // namespace

  TEST_CASE("PerformanceMetadataBaseline - date and musician-list workloads over full and subset sources",
            "[perf][integration][performance-metadata]")
  {
    auto const samples = configuredCount("AOBUS_PERF_SAMPLES", 20, 1);
    auto const warmups = configuredCount("AOBUS_PERF_WARMUPS", 1, 0);
    auto measurements = std::vector<Measurement>{};

    for (auto const count : kLibrarySizes)
    {
      auto fixture = PerformanceMetadataFixture{};
      seedLibrary(fixture, count);

      auto const capacity = fixture.library.storageCapacity();
      measurements.push_back(Measurement{
        .capability = "performance-storage",
        .scenario = "seeded-library-high-water",
        .dataset = datasetLabel(fixture, count),
        .inputCount = count,
        .optByteMetric = Measurement::ByteMetric{.kind = "database-high-water",
                                                 .count = static_cast<std::size_t>(capacity.highWaterBytes)},
      });

      auto const entryCount = expectedMusicianEntries(count);
      auto const blockTrackCount = expectedPerformanceBlockTracks(count);
      measurements.push_back(Measurement{
        .capability = "performance-storage",
        .scenario = "seeded-performance-block-structural",
        .dataset = datasetLabel(fixture, count),
        .inputCount = entryCount,
        .optByteMetric = Measurement::ByteMetric{.kind = "performance-block-structural-bytes",
                                                 .count = (kPerformanceBlockBytes * blockTrackCount) +
                                                          (kMusicianEntryBytes * entryCount)},
      });

      measureListLengthIsolation(fixture, measurements, warmups, samples);

      for (auto const candidateCount : {count, count / 10})
      {
        auto const candidates = std::span<TrackId const>{fixture.ids}.first(candidateCount);
        measureDateQueries(fixture, candidates, measurements, warmups, samples);
        measureMusicianQueries(fixture, candidates, measurements, warmups, samples);
        measureProjectionRebuilds(fixture, candidates, measurements, warmups, samples);
      }
    }

    REQUIRE(measurements.size() == 56);
    writeReport(measurements, warmups, samples);
  }
} // namespace ao::rt::test
