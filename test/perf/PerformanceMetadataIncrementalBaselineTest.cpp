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
#include <ao/rt/ViewIds.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/TrackSourceDelta.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

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
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    constexpr auto kLibrarySizes = std::to_array<std::size_t>({10000, 100000});
    constexpr auto kListLengths = std::to_array<std::size_t>({0, 1, 8, 32});
    constexpr std::size_t kTemplateNameCount = 32;

    struct IncrementalFixture final
    {
      ao::test::TempDir tempDir;
      library::MusicLibrary library{
        ao::test::requireValue(library::MusicLibrary::open(tempDir.path(),
                                                           tempDir.path(),
                                                           {.pinnedMapBytes = std::uint64_t{2} * 1024 * 1024 * 1024}))};
      std::vector<TrackId> ids;
    };

    // The identical credit template every track of one homogeneous control
    // library carries. Former list entries are Performer credits; keep the legacy
    // text/report labels and the original operation oracles. Roles repeat, and no
    // template text contains the inequality literal.
    std::vector<library::Credit> templateMusicians(std::size_t const listLength)
    {
      auto musicians = std::vector<library::Credit>{};
      musicians.reserve(listLength);

      for (std::size_t entry = 0; entry < listLength; ++entry)
      {
        musicians.push_back(library::Credit{.name = std::format("Musician {:03}", entry % kTemplateNameCount),
                                            .role = std::format("Instrument {:02}", entry % 8)});
      }

      return musicians;
    }

    void requireReplacementLiteralAbsentFromTemplate()
    {
      for (std::size_t entry = 0; entry < kTemplateNameCount; ++entry)
      {
        CHECK_FALSE(std::format("Musician {:03}", entry).contains("Replacement Member"));
      }

      for (std::size_t entry = 0; entry < 8; ++entry)
      {
        CHECK_FALSE(std::format("Instrument {:02}", entry).contains("Replacement Member"));
      }
    }

    void seedLibrary(IncrementalFixture& fixture, std::size_t const count, std::size_t const listLength)
    {
      auto const musicians = templateMusicians(listLength);
      fixture.ids.reserve(count);
      auto transaction = library::test::writeTransaction(fixture.library);
      auto seedRes = transaction.apply(
        [&](library::LibraryWrite& write) -> Result<>
        {
          auto writer = write.tracks();

          for (std::size_t index = 0; index < count; ++index)
          {
            auto const spec = library::test::TrackSpec{
              .title = std::format("Title {:06}", index),
              .credits = musicians,
              .uri = std::format("incremental-baseline/{:06}.flac", index),
            };
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

    // Durable state A -> B edit, outside every clock: the first track's
    // leading credit now carries the inequality literal, so it leaves the
    // `!=` membership; length-0 tracks gain that single credit at slot 0.
    void setReplacementCredit(library::MusicLibrary& library, TrackId const trackId, std::size_t const listLength)
    {
      library::test::updateTrackSpec(
        library,
        trackId,
        [listLength](library::test::TrackSpec& spec)
        {
          if (listLength == 0)
          {
            spec.credits.push_back(library::Credit{.name = "Replacement Member", .role = "Instrument 00"});
            return;
          }

          spec.credits.front().name = "Replacement Member";
        });
    }

    // Durable state B -> A edit, outside every clock: the first track's list
    // returns to the exact seeded template.
    void restoreTemplateCredit(library::MusicLibrary& library, TrackId const trackId, std::size_t const listLength)
    {
      library::test::updateTrackSpec(library,
                                     trackId,
                                     [listLength](library::test::TrackSpec& spec)
                                     { spec.credits = templateMusicians(listLength); });
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

    void pushDeliveryMeasurement(std::vector<Measurement>& measurements,
                                 std::string_view const scenario,
                                 std::string const& dataset,
                                 std::size_t const inputCount,
                                 std::vector<std::int64_t> elapsed)
    {
      auto measurement = Measurement{
        .capability = "performance-incremental",
        .scenario = std::string{scenario},
        .dataset = dataset,
        .inputCount = inputCount,
      };
      setPercentiles(measurement, elapsed);
      measurements.push_back(std::move(measurement));
    }

    // One homogeneous control library per (size, length): every track carries
    // the fixed-length template. The `$credit != 'Replacement Member'`
    // filter initially selects the complete source, including length-0 rows.
    // Each sample times exactly one single-track UpdateRange delivery into
    // the filtered source and its Title projection: an erasure (replacement
    // credit) or a reinsertion (restored template). The counterpart delivery
    // is untimed and restores the measured state between samples. Durable
    // edits, validation, and full ID-sequence checks stay outside the clocks;
    // the counters pin that no full projection, upstream-index, or membership
    // rebuild replaces the incremental path, so unaffected rows are reused.
    void measureIncrementalDeliveries(std::size_t const count,
                                      std::size_t const listLength,
                                      std::vector<Measurement>& measurements,
                                      std::size_t const warmups,
                                      std::size_t const samples)
    {
      auto fixture = IncrementalFixture{};
      seedLibrary(fixture, count, listLength);

      auto const expectedAll = fixture.ids;
      auto const expectedErased = std::vector<TrackId>{expectedAll.begin() + 1, expectedAll.end()};
      // Reverse source order independently of the unique Title sequence so an
      // identity projection cannot satisfy the sorting oracle accidentally.
      auto const expectedSourceAll = std::vector<TrackId>{expectedAll.rbegin(), expectedAll.rend()};
      auto const expectedSourceErased = std::vector<TrackId>{expectedSourceAll.begin(), expectedSourceAll.end() - 1};
      auto const dataset = std::format("performance-incremental-v1/library={}/musicians-per-track={}/source=full"
                                       "/page-cache=seeded-warm",
                                       count,
                                       listLength);

      auto sourcePtr = makeMutableTrackSource(expectedSourceAll);
      auto evaluator = SmartListEvaluator{fixture.library};
      auto filteredPtr = std::make_shared<SmartListSource>(TrackSourceLease{sourcePtr}, evaluator);
      filteredPtr->setExpression("$credit != 'Replacement Member'");
      filteredPtr->reload();
      auto projection = TrackListProjection{kInvalidViewId,
                                            TrackSourceLease{filteredPtr},
                                            fixture.library,
                                            TrackOrderSpec{.sortBy = {{TrackSortField::Title}}}};

      CAPTURE(count, listLength);
      REQUIRE_FALSE(filteredPtr->hasError());
      REQUIRE(sourceTrackIds(*filteredPtr) == expectedSourceAll);
      requireProjectionOrder(projection, expectedAll);
      REQUIRE(projection.size() == count);

      auto const projectionCounts = detail::RuntimeOperationProbe::counts(projection);
      auto const evaluatorCounts = detail::RuntimeOperationProbe::counts(evaluator);

      auto deliverFirstTrack = [&sourcePtr, sourcePosition = count - 1](TrackId const trackId)
      {
        sourcePtr->publishBatch(TrackSourceDelta{delta::RegularTrackEditScript{
          .edits = {delta::UpdateRange{.start = sourcePosition, .trackIds = {trackId}}}}});
      };

      auto erasureElapsed = std::vector<std::int64_t>{};
      erasureElapsed.reserve(samples);
      auto reinsertionElapsed = std::vector<std::int64_t>{};
      reinsertionElapsed.reserve(samples);

      for (std::size_t run = 0; run < warmups + samples; ++run)
      {
        // Erasure: the durable replacement is outside the clock; the timed
        // delivery removes exactly the first row from membership and order.
        setReplacementCredit(fixture.library, fixture.ids.front(), listLength);
        auto const start = std::chrono::steady_clock::now();
        deliverFirstTrack(fixture.ids.front());
        auto const erasureDuration =
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();

        REQUIRE_FALSE(filteredPtr->hasError());
        CHECK(sourceTrackIds(*filteredPtr) == expectedSourceErased);
        requireProjectionOrder(projection, expectedErased);
        CHECK(projection.size() == count - 1);

        if (run >= warmups)
        {
          erasureElapsed.push_back(erasureDuration);
        }

        // Counterpart insertion delivery, untimed between samples, restores
        // the complete membership and order for the next sample.
        restoreTemplateCredit(fixture.library, fixture.ids.front(), listLength);
        deliverFirstTrack(fixture.ids.front());
        REQUIRE_FALSE(filteredPtr->hasError());
        CHECK(sourceTrackIds(*filteredPtr) == expectedSourceAll);
        requireProjectionOrder(projection, expectedAll);
        CHECK(projection.size() == count);
      }

      for (std::size_t run = 0; run < warmups + samples; ++run)
      {
        // The erasure delivery is the untimed counterpart here; the timed
        // delivery reinserts exactly the first row.
        setReplacementCredit(fixture.library, fixture.ids.front(), listLength);
        deliverFirstTrack(fixture.ids.front());
        REQUIRE_FALSE(filteredPtr->hasError());
        CHECK(sourceTrackIds(*filteredPtr) == expectedSourceErased);
        requireProjectionOrder(projection, expectedErased);
        CHECK(projection.size() == count - 1);

        restoreTemplateCredit(fixture.library, fixture.ids.front(), listLength);
        auto const start = std::chrono::steady_clock::now();
        deliverFirstTrack(fixture.ids.front());
        auto const reinsertionDuration =
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();

        REQUIRE_FALSE(filteredPtr->hasError());
        CHECK(sourceTrackIds(*filteredPtr) == expectedSourceAll);
        requireProjectionOrder(projection, expectedAll);
        CHECK(projection.size() == count);

        if (run >= warmups)
        {
          reinsertionElapsed.push_back(reinsertionDuration);
        }
      }

      auto const afterProjectionCounts = detail::RuntimeOperationProbe::counts(projection);
      auto const afterEvaluatorCounts = detail::RuntimeOperationProbe::counts(evaluator);
      CHECK(afterProjectionCounts.fullProjectionRebuilds == projectionCounts.fullProjectionRebuilds);
      // Each direction loop delivers one timed and one untimed event per run,
      // four incremental projection updates per run across both loops.
      CHECK(afterProjectionCounts.incrementalProjectionUpdates - projectionCounts.incrementalProjectionUpdates ==
            4 * (warmups + samples));
      CHECK(afterEvaluatorCounts.upstreamIndexRebuilds == evaluatorCounts.upstreamIndexRebuilds);
      CHECK(afterEvaluatorCounts.membershipIndexRebuilds == evaluatorCounts.membershipIndexRebuilds);

      pushDeliveryMeasurement(measurements, "musician-inequality-erasure", dataset, count, std::move(erasureElapsed));
      pushDeliveryMeasurement(
        measurements, "musician-inequality-reinsertion", dataset, count, std::move(reinsertionElapsed));
    }
  } // namespace

  TEST_CASE("PerformanceMetadataIncrementalBaseline - single-row musician inequality erasures and reinsertions",
            "[perf][integration][performance-incremental]")
  {
    auto const samples = configuredCount("AOBUS_PERF_SAMPLES", 20, 1);
    auto const warmups = configuredCount("AOBUS_PERF_WARMUPS", 1, 0);
    auto measurements = std::vector<Measurement>{};
    requireReplacementLiteralAbsentFromTemplate();

    for (auto const count : kLibrarySizes)
    {
      for (auto const listLength : kListLengths)
      {
        measureIncrementalDeliveries(count, listLength, measurements, warmups, samples);
      }
    }

    REQUIRE(measurements.size() == 16);
    writeReport(measurements, warmups, samples);
  }
} // namespace ao::rt::test
