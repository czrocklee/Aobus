// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/async/Subscription.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceDelta.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("TrackSource - live subscription publishes exact deltas without a synthetic batch",
            "[runtime][unit][source]")
  {
    auto source = MutableTrackSource{};
    source.addInitial(TrackId{10});
    auto batches = std::vector<TrackSourceDelta>{};
    auto subscription = source.subscribe([&](TrackSourceDelta const& batch) noexcept { batches.push_back(batch); });

    CHECK(batches.empty());

    source.insert(TrackId{20}, 1);
    source.update(TrackId{10});
    source.remove(TrackId{20});

    REQUIRE(batches.size() == 3);

    auto const& inserted = std::get<delta::InsertRange>(sourceEditScript(batches[0]).edits[0]);
    CHECK(inserted.start == 1);
    CHECK(inserted.trackIds == std::vector{TrackId{20}});

    auto const& updated = std::get<delta::UpdateRange>(sourceEditScript(batches[1]).edits[0]);
    CHECK(updated.start == 0);
    CHECK(updated.trackIds == std::vector{TrackId{10}});

    auto const& removed = std::get<delta::RemoveRange>(sourceEditScript(batches[2]).edits[0]);
    CHECK(removed.start == 1);
    CHECK(removed.trackIds == std::vector{TrackId{20}});
  }

  TEST_CASE("TrackSource - one mixed operation publishes after the complete source state is visible",
            "[runtime][unit][source]")
  {
    auto source = MutableTrackSource{};
    source.setInitial(std::to_array<TrackId>({TrackId{10}, TrackId{20}, TrackId{30}}));
    auto observedIds = std::vector<TrackId>{};
    auto observedBatches = std::vector<TrackSourceDelta>{};
    auto subscription = source.subscribe(
      [&](TrackSourceDelta const& batch) noexcept
      {
        for (std::size_t index = 0; index < source.size(); ++index)
        {
          observedIds.push_back(source.trackIdAt(index));
        }

        observedBatches.push_back(batch);
      });

    auto const finalIds = std::to_array<TrackId>({TrackId{10}, TrackId{30}, TrackId{40}, TrackId{50}});
    source.replaceWithBatch(finalIds,
                            delta::RegularTrackEditScript{
                              .edits = {delta::RemoveRange{.start = 1, .trackIds = {TrackId{20}}},
                                        delta::InsertRange{
                                          .start = 2,
                                          .trackIds = {TrackId{40}, TrackId{50}},
                                        }},
                            });

    REQUIRE(observedBatches.size() == 1);
    REQUIRE(sourceEditScript(observedBatches[0]).edits.size() == 2);
    CHECK(observedIds == std::vector<TrackId>(finalIds.begin(), finalIds.end()));
  }

  TEST_CASE("TrackSource - invalidation is terminal and idempotent", "[runtime][unit][source]")
  {
    auto source = MutableTrackSource{};
    auto batches = std::vector<TrackSourceDelta>{};
    auto subscription = source.subscribe([&](TrackSourceDelta const& batch) noexcept { batches.push_back(batch); });

    source.invalidate();
    source.invalidate();
    source.emitReset();

    REQUIRE(batches.size() == 1);
    CHECK(std::holds_alternative<SourceInvalidated>(batches[0]));
    CHECK(source.state() == TrackSourceState::Invalidated);

    auto lateBatches = std::vector<TrackSourceDelta>{};
    auto lateSubscription =
      source.subscribe([&](TrackSourceDelta const& batch) noexcept { lateBatches.push_back(batch); });

    REQUIRE(lateBatches.size() == 1);
    CHECK(std::holds_alternative<SourceInvalidated>(lateBatches[0]));
  }

  TEST_CASE("TrackSource - destruction disconnects subscribers without semantic invalidation",
            "[runtime][unit][source]")
  {
    auto batches = std::vector<TrackSourceDelta>{};
    auto subscription = async::Subscription{};

    {
      auto sourcePtr = std::make_unique<MutableTrackSource>();
      subscription = sourcePtr->subscribe([&](TrackSourceDelta const& batch) noexcept { batches.push_back(batch); });
    }

    CHECK(batches.empty());
    subscription.reset();
  }

  TEST_CASE("TrackSource - releasing a subscription stops later delivery", "[runtime][unit][source]")
  {
    auto source = MutableTrackSource{};
    std::uint32_t batchCount = 0;
    std::uint32_t retainedBatchCount = 0;
    auto subscription = source.subscribe([&](TrackSourceDelta const&) noexcept { ++batchCount; });
    auto retainedSubscription = source.subscribe([&](TrackSourceDelta const&) noexcept { ++retainedBatchCount; });

    subscription.reset();
    source.emitReset();

    CHECK(batchCount == 0);
    CHECK(retainedBatchCount == 1);
  }

  TEST_CASE("TrackSourceLease - shared ownership pins source identity", "[runtime][unit][source]")
  {
    auto sourcePtr = std::make_shared<MutableTrackSource>();
    auto weakSourcePtr = std::weak_ptr<TrackSource>{sourcePtr};

    {
      auto lease = TrackSourceLease{sourcePtr};
      auto secondLease = TrackSourceLease{lease};
      sourcePtr = nullptr;

      REQUIRE_FALSE(weakSourcePtr.expired());
      CHECK(&lease.source() == weakSourcePtr.lock().get());
      CHECK(&secondLease.source() == weakSourcePtr.lock().get());
    }

    CHECK(weakSourcePtr.expired());
  }
} // namespace ao::rt::test
