// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors
//
// Phase 0 baseline measurement — synthetic data, no fixed pass/fail thresholds.

#include "TestUtils.h"
#include <ao/Type.h>
#include <ao/rt/SmartListEvaluator.h>
#include <ao/rt/SmartListSource.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackListProjection.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/TrackSource.h>
#include <ao/utility/Log.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
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
      std::chrono::microseconds indexOfLookupDuration{};
    };

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
    APP_LOG_INFO("  indexOf x10000: {} us", t.indexOfLookupDuration.count());

    // Regression thresholds — deliberately generous to avoid flakes
    CHECK(t.createProjectionDuration < std::chrono::seconds{5});
    CHECK(t.setTitleSortDuration < std::chrono::seconds{5});
    CHECK(t.evaluateMembersDuration < std::chrono::seconds{10});
    CHECK(t.indexOfLookupDuration < std::chrono::microseconds{500000});
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
    APP_LOG_INFO("  indexOf x10000: {} us", t.indexOfLookupDuration.count());

    // Regression thresholds — deliberately generous to avoid flakes
    CHECK(t.createProjectionDuration < std::chrono::seconds{30});
    CHECK(t.setTitleSortDuration < std::chrono::seconds{30});
    CHECK(t.evaluateMembersDuration < std::chrono::minutes{1});
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
    APP_LOG_INFO("  indexOf x10000: {} us", t.indexOfLookupDuration.count());

    // Regression thresholds — deliberately generous to avoid flakes
    CHECK(t.createProjectionDuration < std::chrono::minutes{5});
    CHECK(t.setTitleSortDuration < std::chrono::minutes{5});
    CHECK(t.evaluateMembersDuration < std::chrono::minutes{10});
    CHECK(t.indexOfLookupDuration < std::chrono::microseconds{500000});
  }
}
