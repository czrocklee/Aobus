// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "PerformanceReport.h"
#include "runtime/RuntimeOperationProbe.h"
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
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackWriter.h>
#include <ao/rt/PlaybackLaunchSpec.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/TrackSourceDelta.h>
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
    constexpr std::size_t kWorkPeriod = 128;
    constexpr std::size_t kSoloistPeriod = 32;
    constexpr std::size_t kYearPeriod = 70;

    struct ClassicalMetadataFixture final
    {
      ao::test::TempDir tempDir;
      library::MusicLibrary library{
        ao::test::requireValue(library::MusicLibrary::open(tempDir.path(),
                                                           tempDir.path(),
                                                           {.pinnedMapBytes = std::uint64_t{2} * 1024 * 1024 * 1024}))};
      std::vector<TrackId> ids;
    };

    library::test::TrackSpec classicalTrackSpec(std::size_t index)
    {
      auto const performanceIndex = index / 10;
      return library::test::TrackSpec{
        .title = std::format("Movement {:06}", index),
        .artist = std::format("Artist {:02}", performanceIndex % 32),
        .album = std::format("Album {:05}", performanceIndex),
        .genre = "Classical",
        .composer = std::format("Composer {:02}", (performanceIndex % 128) / 8),
        .work = std::format("Work {:04}", performanceIndex % 128),
        .movement = std::format("Movement {}", (index % 10) + 1),
        .credits =
          {
            {.name = std::format("Conductor {:02}", performanceIndex % 16), .kind = library::CreditKind::Conductor},
            {.name = std::format("Ensemble {:02}", performanceIndex % 16), .kind = library::CreditKind::Ensemble},
            {.name = std::format("Soloist {:04}", performanceIndex % 32), .kind = library::CreditKind::Soloist},
          },
        .uri = std::format("classical-baseline/{:06}.flac", index),
        .year = static_cast<std::uint16_t>(1950 + (performanceIndex % 70)),
        .trackNumber = static_cast<std::uint16_t>((index % 10) + 1),
        .movementNumber = static_cast<std::uint16_t>((index % 10) + 1),
        .movementTotal = 10,
      };
    }

    void seedLibrary(ClassicalMetadataFixture& fixture, std::size_t count)
    {
      fixture.ids.reserve(count);
      auto transaction = library::test::writeTransaction(fixture.library);
      auto seedRes = transaction.apply(
        [&](library::LibraryWrite& write) -> Result<>
        {
          auto writer = write.tracks();

          for (std::size_t index = 0; index < count; ++index)
          {
            auto const spec = classicalTrackSpec(index);
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

    std::string datasetLabel(ClassicalMetadataFixture const& fixture, std::size_t candidates)
    {
      return std::format(
        "classical-v1/library={}/candidates={}/page-cache=seeded-warm", fixture.ids.size(), candidates);
    }

    // Expectations enumerate the fixed fixture's numeric keys, not production
    // comparators, dictionary reads, or the source/projection being checked.
    std::vector<TrackId> expectedSourceMembers(std::span<TrackId const> candidates, std::size_t period)
    {
      auto expected = std::vector<TrackId>{};

      for (std::size_t index = 0; index < candidates.size(); ++index)
      {
        if ((index / kMovementsPerPerformance) % period == 0)
        {
          expected.push_back(candidates[index]);
        }
      }

      return expected;
    }

    std::vector<TrackId> expectedPeriodTitleOrder(std::span<TrackId const> candidates,
                                                  std::size_t period,
                                                  bool onlyWorkZero = false)
    {
      REQUIRE(candidates.size() % kMovementsPerPerformance == 0);
      auto expected = std::vector<TrackId>{};
      expected.reserve(candidates.size());
      auto const performanceCount = candidates.size() / kMovementsPerPerformance;

      // Fixed-width Work/Soloist text orders by its numeric bucket; Title
      // orders each bucket's performances and their movements by source index.
      for (std::size_t bucket = 0; bucket < period; ++bucket)
      {
        for (std::size_t performanceIndex = bucket; performanceIndex < performanceCount; performanceIndex += period)
        {
          if (onlyWorkZero && performanceIndex % kWorkPeriod != 0)
          {
            continue;
          }

          expected.append_range(
            candidates.subspan(performanceIndex * kMovementsPerPerformance, kMovementsPerPerformance));
        }
      }

      return expected;
    }

    std::vector<TrackId> expectedClassicalOrder(std::span<TrackId const> candidates)
    {
      REQUIRE(candidates.size() % kMovementsPerPerformance == 0);
      auto expected = std::vector<TrackId>{};
      expected.reserve(candidates.size());
      auto const performanceCount = candidates.size() / kMovementsPerPerformance;

      // Composer = Work / 8, so ascending Work already orders Composer. Within
      // each work, Year comes first, then fixed-width Album (performance index).
      // Disc ties; Movement, TrackNumber, and Title all order the ten movements.
      for (std::size_t work = 0; work < kWorkPeriod; ++work)
      {
        for (std::size_t yearOffset = 0; yearOffset < kYearPeriod; ++yearOffset)
        {
          for (std::size_t performanceIndex = work; performanceIndex < performanceCount;
               performanceIndex += kWorkPeriod)
          {
            if (performanceIndex % kYearPeriod == yearOffset)
            {
              expected.append_range(
                candidates.subspan(performanceIndex * kMovementsPerPerformance, kMovementsPerPerformance));
            }
          }
        }
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

    struct QueryWorkload final
    {
      std::string_view scenario;
      std::string_view expression;
      std::size_t performancePeriod;
    };

    constexpr auto kQueryWorkloads = std::to_array<QueryWorkload>({
      {"work-equality", "$work = 'Work 0000'", 128},
      {"work-substring", "$work ~ 'work 0000'", 128},
      {"soloist-equality", "$soloist = 'Soloist 0000'", 32},
      {"soloist-substring", "$soloist ~ 'soloist 0000'", 32},
      {"year-equality-hot-control", "$year = 1950", 70},
    });

    void measureQueryRebuilds(ClassicalMetadataFixture& fixture,
                              std::span<TrackId const> candidates,
                              std::vector<Measurement>& measurements,
                              std::size_t warmups,
                              std::size_t samples)
    {
      for (auto const& workload : kQueryWorkloads)
      {
        CAPTURE(workload.scenario, candidates.size());
        auto sourcePtr = makeMutableTrackSource(candidates);
        auto evaluator = SmartListEvaluator{fixture.library};
        auto filtered = SmartListSource{TrackSourceLease{sourcePtr}, evaluator};
        auto const expected = expectedSourceMembers(candidates, workload.performancePeriod);

        measure(
          measurements,
          {.capability = "classical-query",
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

    TrackPresentationSpec classicalPresentation()
    {
      // Keep the pre-format chain fixed for a like-for-like after comparison.
      return TrackPresentationSpec{
        .groupBy = TrackGroupKey::Work,
        .sortBy =
          {
            {TrackSortField::Composer},
            {TrackSortField::Work},
            {TrackSortField::Year},
            {TrackSortField::Album},
            {TrackSortField::Movement},
            {TrackSortField::DiscNumber},
            {TrackSortField::TrackNumber},
            {TrackSortField::Title},
          },
      };
    }

    void measureProjectionRebuilds(ClassicalMetadataFixture& fixture,
                                   std::span<TrackId const> candidates,
                                   std::vector<Measurement>& measurements,
                                   std::size_t warmups,
                                   std::size_t samples)
    {
      auto sourcePtr = makeMutableTrackSource(candidates);

      constexpr auto kProjectionWorkloads =
        std::to_array<std::tuple<TrackSortField, std::string_view, std::size_t, std::size_t>>({
          {TrackSortField::Work, "work-sort-rebuild", kWorkPeriod, 1280},
          {TrackSortField::Soloist, "soloist-sort-rebuild", kSoloistPeriod, 320},
          {TrackSortField::Title, "title-sort-rebuild-hot-control", 1, 10},
        });

      for (auto const& [field, scenario, period, interiorSourceIndex] : kProjectionWorkloads)
      {
        CAPTURE(scenario, candidates.size());
        auto const expected = expectedPeriodTitleOrder(candidates, period);

        // Pin independent interior examples: Title-only order must not pass as
        // Work/Title or Soloist/Title order on the full 10k candidate source.
        if (candidates.size() == 10000)
        {
          CHECK(expected[10] == candidates[interiorSourceIndex]);
        }

        auto const sortBy = field == TrackSortField::Title
                              ? std::vector<TrackSortTerm>{{TrackSortField::Title}}
                              : std::vector<TrackSortTerm>{{field}, {TrackSortField::Title}};
        auto projectionPtr = std::unique_ptr<TrackListProjection>{};
        measure(
          measurements,
          {.capability = "classical-projection",
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
            // Retire outside the clock: each sample starts with empty caches.
            projectionPtr.reset();
          });
      }

      auto const expectedClassical = expectedClassicalOrder(candidates);

      if (candidates.size() == 10000)
      {
        CHECK(expectedClassical[10] == candidates[6400]);
      }

      auto projection = TrackListProjection{ViewId{1}, TrackSourceLease{sourcePtr}, fixture.library};
      projection.setPresentation(classicalPresentation());
      requireProjectionOrder(projection, expectedClassical);
      auto const beforeCounts = detail::RuntimeOperationProbe::counts(projection);
      measure(
        measurements,
        {.capability = "classical-projection",
         .scenario = "work-group-classical-sort-reset",
         .dataset = datasetLabel(fixture, candidates.size()),
         .inputCount = candidates.size()},
        warmups,
        samples,
        [&] { sourcePtr->emitReset(); },
        [&]
        {
          requireProjectionOrder(projection, expectedClassical);
          // Composer is a function of Work, so each work is one group.
          CHECK(projection.groupCount() == std::min<std::size_t>(128, candidates.size() / 10));
          CHECK(projection.indexOf(candidates.back()).has_value());
        });
      auto const afterCounts = detail::RuntimeOperationProbe::counts(projection);
      CHECK(afterCounts.fullProjectionRebuilds - beforeCounts.fullProjectionRebuilds == warmups + samples);
    }

    void measureIncrementalUpdates(ClassicalMetadataFixture& fixture,
                                   std::span<TrackId const> candidates,
                                   std::vector<Measurement>& measurements,
                                   std::size_t warmups,
                                   std::size_t samples,
                                   bool useWorkFilter)
    {
      auto sourcePtr = makeMutableTrackSource(candidates);
      auto evaluator = SmartListEvaluator{fixture.library};
      auto filteredPtr = std::shared_ptr<SmartListSource>{};
      auto projectionSourceLease = TrackSourceLease{sourcePtr};

      if (useWorkFilter)
      {
        filteredPtr = std::make_shared<SmartListSource>(TrackSourceLease{sourcePtr}, evaluator);
        filteredPtr->setExpression("$work = 'Work 0000'");
        filteredPtr->reload();
        projectionSourceLease = TrackSourceLease{filteredPtr};
      }

      auto projection =
        TrackListProjection{kInvalidViewId,
                            projectionSourceLease,
                            fixture.library,
                            TrackOrderSpec{.sortBy = {{TrackSortField::Soloist}, {TrackSortField::Title}}}};
      auto const expectedMembers = expectedSourceMembers(candidates, useWorkFilter ? kWorkPeriod : 1);
      auto const unchangedOrder = expectedPeriodTitleOrder(candidates, kSoloistPeriod, useWorkFilter);
      auto changedOrder = unchangedOrder;
      REQUIRE_FALSE(changedOrder.empty());
      REQUIRE(changedOrder.front() == candidates.front());
      // Soloist 9999 sorts after every seeded soloist; only this row moves.
      std::ignore = std::ranges::rotate(changedOrder, changedOrder.begin() + 1);

      if (useWorkFilter)
      {
        constexpr auto kExpectedCounts = std::to_array<std::pair<std::size_t, std::size_t>>({
          {1000, 10},
          {10000, 80},
          {100000, 790},
        });
        bool hasKnownCandidateCount = false;

        for (auto const& [candidateCount, expectedCount] : kExpectedCounts)
        {
          if (candidates.size() == candidateCount)
          {
            CHECK(expectedMembers.size() == expectedCount);
            hasKnownCandidateCount = true;
          }
        }

        REQUIRE(hasKnownCandidateCount);
        REQUIRE_FALSE(filteredPtr->hasError());
      }

      REQUIRE(sourceTrackIds(projectionSourceLease.source()) == expectedMembers);
      requireProjectionOrder(projection, unchangedOrder);
      auto const projectionCounts = detail::RuntimeOperationProbe::counts(projection);
      auto const evaluatorCounts = detail::RuntimeOperationProbe::counts(evaluator);
      auto elapsed = std::vector<std::int64_t>{};
      elapsed.reserve(samples);

      for (std::size_t run = 0; run < warmups + samples; ++run)
      {
        bool const changed = run % 2 == 0;
        // Repeatedly move the same row; its dictionary/arena keys stay warm.
        // Durable authoring and its commit are not part of source delivery latency.
        library::test::updateTrackSpec(fixture.library,
                                       candidates.front(),
                                       [changed](library::test::TrackSpec& spec)
                                       { spec.credits[2].name = changed ? "Soloist 9999" : "Soloist 0000"; });
        auto batch = TrackSourceDelta{
          delta::RegularTrackEditScript{.edits = {delta::UpdateRange{.start = 0, .trackIds = {candidates.front()}}}}};
        auto const start = std::chrono::steady_clock::now();
        sourcePtr->publishBatch(std::move(batch));
        auto const duration =
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();

        if (filteredPtr)
        {
          REQUIRE_FALSE(filteredPtr->hasError());
        }

        CHECK(sourceTrackIds(projectionSourceLease.source()) == expectedMembers);
        requireProjectionOrder(projection, changed ? changedOrder : unchangedOrder);
        CHECK(projection.indexOf(candidates[1]).has_value());

        if (run >= warmups)
        {
          elapsed.push_back(duration);
        }
      }

      auto const afterProjectionCounts = detail::RuntimeOperationProbe::counts(projection);
      auto const afterEvaluatorCounts = detail::RuntimeOperationProbe::counts(evaluator);
      CHECK(afterProjectionCounts.fullProjectionRebuilds == projectionCounts.fullProjectionRebuilds);
      CHECK(afterProjectionCounts.incrementalProjectionUpdates - projectionCounts.incrementalProjectionUpdates ==
            warmups + samples);

      if (useWorkFilter)
      {
        CHECK(afterEvaluatorCounts.upstreamIndexRebuilds == evaluatorCounts.upstreamIndexRebuilds);
        CHECK(afterEvaluatorCounts.membershipIndexRebuilds == evaluatorCounts.membershipIndexRebuilds);
      }

      auto measurement = Measurement{
        .capability = useWorkFilter ? "classical-pipeline" : "classical-projection",
        .scenario = useWorkFilter ? "one-row-work-filter-soloist-sort-update" : "one-row-soloist-sort-update",
        .dataset =
          std::format("{}/projection-rows={}", datasetLabel(fixture, candidates.size()), expectedMembers.size()),
        .inputCount = candidates.size(),
      };
      setPercentiles(measurement, elapsed);
      measurements.push_back(std::move(measurement));
      library::test::updateTrackSpec(fixture.library,
                                     candidates.front(),
                                     [](library::test::TrackSpec& spec) { spec.credits[2].name = "Soloist 0000"; });
    }
  } // namespace

  TEST_CASE("ClassicalMetadataBaseline - cold queries and projections over full and subset sources",
            "[perf][integration][classical-metadata]")
  {
    auto const samples = configuredCount("AOBUS_PERF_SAMPLES", 20, 1);
    auto const warmups = configuredCount("AOBUS_PERF_WARMUPS", 1, 0);
    auto measurements = std::vector<Measurement>{};

    for (auto const count : kLibrarySizes)
    {
      auto fixture = ClassicalMetadataFixture{};
      seedLibrary(fixture, count);
      auto const capacity = fixture.library.storageCapacity();
      measurements.push_back(Measurement{
        .capability = "classical-storage",
        .scenario = "seeded-library-high-water",
        .dataset = datasetLabel(fixture, count),
        .inputCount = count,
        .optByteMetric = Measurement::ByteMetric{.kind = "database-high-water",
                                                 .count = static_cast<std::size_t>(capacity.highWaterBytes)},
      });

      for (auto const candidateCount : {count, count / 10})
      {
        auto const candidates = std::span<TrackId const>{fixture.ids}.first(candidateCount);
        measureQueryRebuilds(fixture, candidates, measurements, warmups, samples);
        measureProjectionRebuilds(fixture, candidates, measurements, warmups, samples);
        measureIncrementalUpdates(fixture, candidates, measurements, warmups, samples, false);
        measureIncrementalUpdates(fixture, candidates, measurements, warmups, samples, true);
      }
    }

    REQUIRE(measurements.size() == 46);
    writeReport(measurements, warmups, samples);
  }
} // namespace ao::rt::test
