// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/rt/TrackEditScript.h>

#include "runtime/projection/TrackProjectionEditScript.h"
#include <ao/CoreIds.h>
#include <ao/rt/projection/TrackListProjection.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <ranges>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt::delta::test
{
  namespace
  {
    std::pair<std::vector<TrackId>, std::vector<TrackId>> sequencesFor(std::uint32_t seed)
    {
      auto random = std::mt19937{seed};
      auto from = std::vector<TrackId>{};

      for (std::uint32_t value = 1; value <= 24; ++value)
      {
        from.emplace_back(value);
      }

      std::ranges::shuffle(from, random);
      from.resize(8U + (seed % 17U));

      auto to = from;

      for (std::size_t index = to.size(); index != 0; --index)
      {
        if ((random() % 5U) == 0U)
        {
          to.erase(to.begin() + static_cast<std::ptrdiff_t>(index - 1U));
        }
      }

      for (std::uint32_t value = 0; value < seed % 7U; ++value)
      {
        auto const position = to.empty() ? 0U : random() % (to.size() + 1U);
        to.insert(to.begin() + static_cast<std::ptrdiff_t>(position), TrackId{100U + (seed * 8U) + value});
      }

      std::ranges::shuffle(to, random);
      return {std::move(from), std::move(to)};
    }

    std::vector<TrackId> applyNaively(std::vector<TrackId> trackIds, RegularTrackEditScript const& script)
    {
      for (auto const& edit : script.edits)
      {
        std::visit(
          [&trackIds](auto const& range)
          {
            using Range = std::remove_cvref_t<decltype(range)>;
            REQUIRE(range.start <= trackIds.size());

            if constexpr (std::same_as<Range, InsertRange>)
            {
              trackIds.insert(trackIds.begin() + static_cast<std::ptrdiff_t>(range.start),
                              range.trackIds.begin(),
                              range.trackIds.end());
            }
            else
            {
              REQUIRE(range.trackIds.size() <= trackIds.size() - range.start);
              REQUIRE(
                std::ranges::equal(range.trackIds, std::span{trackIds}.subspan(range.start, range.trackIds.size())));

              if constexpr (std::same_as<Range, RemoveRange>)
              {
                trackIds.erase(trackIds.begin() + static_cast<std::ptrdiff_t>(range.start),
                               trackIds.begin() + static_cast<std::ptrdiff_t>(range.start + range.trackIds.size()));
              }
            }
          },
          edit);
      }

      return trackIds;
    }
  } // namespace

  TEST_CASE("TrackEditScript - random unique sequences round-trip through diff", "[runtime][unit][delta][property]")
  {
    auto const seed = GENERATE(Catch::Generators::range(0U, 128U));
    auto const [from, to] = sequencesFor(seed);
    auto const script = diff(from, to);

    REQUIRE(validate(script, from.size()));
    auto const result = apply(from, script);
    REQUIRE(result);
    CHECK(*result == to);
    CHECK(applyNaively(from, script) == to);
    CHECK(validateTrackListProjectionDeltaBatch(eraseTrackIds(script), from.size()));

    bool seenInsertion = false;
    auto previousRemovalStart = from.size();
    std::size_t previousInsertionStart = 0;

    for (auto const& edit : script.edits)
    {
      if (auto const* removal = std::get_if<RemoveRange>(&edit); removal != nullptr)
      {
        CHECK_FALSE(seenInsertion);
        CHECK(removal->start < previousRemovalStart);
        previousRemovalStart = removal->start;
      }
      else if (auto const* insertion = std::get_if<InsertRange>(&edit); insertion != nullptr)
      {
        seenInsertion = true;
        CHECK(insertion->start >= previousInsertionStart);
        previousInsertionStart = insertion->start;
      }
    }
  }

  TEST_CASE("TrackEditScript - retained updates are coalesced at final coordinates", "[runtime][unit][delta]")
  {
    auto const from = std::vector{TrackId{1}, TrackId{2}, TrackId{3}, TrackId{4}};
    auto const to = std::vector{TrackId{1}, TrackId{3}, TrackId{4}, TrackId{2}};
    auto const updated = std::vector{TrackId{1}, TrackId{3}, TrackId{4}, TrackId{2}};
    auto const script = diff(from, to, updated);

    REQUIRE(validate(script, from.size()));
    auto const result = apply(from, script);
    REQUIRE(result);
    CHECK(*result == to);

    std::size_t updateCount = 0;

    for (auto const& edit : script.edits)
    {
      if (auto const* update = std::get_if<UpdateRange>(&edit); update != nullptr)
      {
        updateCount += update->trackIds.size();
        CHECK_FALSE(std::ranges::contains(update->trackIds, TrackId{2}));
      }
    }

    CHECK(updateCount == 3);
  }

  TEST_CASE("TrackEditScript - apply rejects removal and update identity mismatches", "[runtime][unit][delta]")
  {
    auto const initial = std::vector{TrackId{1}, TrackId{2}, TrackId{3}};
    auto const badRemoval = RegularTrackEditScript{.edits = {RemoveRange{.start = 1, .trackIds = {TrackId{9}}}}};
    auto const badUpdate = RegularTrackEditScript{.edits = {UpdateRange{.start = 0, .trackIds = {TrackId{9}}}}};

    CHECK_FALSE(apply(initial, badRemoval));
    CHECK_FALSE(apply(initial, badUpdate));
  }

  TEST_CASE("TrackEditScript - apply rejects edits that return to an earlier canonical stage",
            "[runtime][regression][delta]")
  {
    auto const initial = std::vector{TrackId{1}, TrackId{2}, TrackId{3}};
    auto const updateThenRemove = RegularTrackEditScript{
      .edits = {UpdateRange{.start = 0, .trackIds = {TrackId{1}}}, RemoveRange{.start = 2, .trackIds = {TrackId{3}}}}};
    auto const updateThenInsert = RegularTrackEditScript{
      .edits = {UpdateRange{.start = 0, .trackIds = {TrackId{1}}}, InsertRange{.start = 3, .trackIds = {TrackId{4}}}}};
    auto const removeUpdateInsert =
      RegularTrackEditScript{.edits = {RemoveRange{.start = 2, .trackIds = {TrackId{3}}},
                                       UpdateRange{.start = 0, .trackIds = {TrackId{1}}},
                                       InsertRange{.start = 2, .trackIds = {TrackId{4}}}}};

    CHECK_FALSE(validate(updateThenRemove, initial.size()));
    CHECK_FALSE(validate(updateThenInsert, initial.size()));
    CHECK_FALSE(validate(removeUpdateInsert, initial.size()));
    CHECK_FALSE(apply(initial, updateThenRemove));
    CHECK_FALSE(apply(initial, updateThenInsert));
    CHECK_FALSE(apply(initial, removeUpdateInsert));
  }

  TEST_CASE("TrackEditScript - coalescer output validates and preserves range identities", "[runtime][unit][delta]")
  {
    auto coalescer = Coalescer{};
    coalescer.appendRemove(4, std::array{TrackId{5}, TrackId{6}});
    coalescer.appendRemove(2, std::array{TrackId{3}, TrackId{4}});
    coalescer.appendInsert(1, std::array{TrackId{7}});
    coalescer.appendInsert(2, std::array{TrackId{8}});
    coalescer.appendUpdate(0, std::array{TrackId{1}, TrackId{7}});
    auto const script = coalescer.take();

    REQUIRE(validate(script, 6));
    auto const result =
      apply(std::vector{TrackId{1}, TrackId{2}, TrackId{3}, TrackId{4}, TrackId{5}, TrackId{6}}, script);
    REQUIRE(result);
    CHECK(*result == std::vector{TrackId{1}, TrackId{7}, TrackId{8}, TrackId{2}});
  }

  TEST_CASE("RangeEditValidator - enforces stage order and running bounds without payloads", "[runtime][unit][delta]")
  {
    using Kind = RangeEditKind;

    SECTION("descending removes, then ascending inserts, then ascending updates")
    {
      auto validator = RangeEditValidator{6};
      CHECK(validator.accept(Kind::Remove, 4, 2));
      CHECK(validator.accept(Kind::Remove, 2, 2));
      CHECK(validator.accept(Kind::Insert, 1, 1));
      CHECK(validator.accept(Kind::Insert, 2, 1));
      CHECK(validator.accept(Kind::Update, 0, 2));
    }

    SECTION("removals must strictly descend")
    {
      auto validator = RangeEditValidator{6};
      REQUIRE(validator.accept(Kind::Remove, 2, 2));
      CHECK_FALSE(validator.accept(Kind::Remove, 2, 1));
    }

    SECTION("a removal after an insertion is rejected")
    {
      auto validator = RangeEditValidator{6};
      REQUIRE(validator.accept(Kind::Insert, 0, 1));
      CHECK_FALSE(validator.accept(Kind::Remove, 0, 1));
    }

    SECTION("a removal after an update is rejected")
    {
      auto validator = RangeEditValidator{6};
      REQUIRE(validator.accept(Kind::Update, 0, 1));
      CHECK_FALSE(validator.accept(Kind::Remove, 5, 1));
    }

    SECTION("an insertion after an update is rejected")
    {
      auto validator = RangeEditValidator{6};
      REQUIRE(validator.accept(Kind::Update, 0, 1));
      CHECK_FALSE(validator.accept(Kind::Insert, 6, 1));
    }

    SECTION("insertions must not overlap and address the post-removal size")
    {
      auto validator = RangeEditValidator{2};
      REQUIRE(validator.accept(Kind::Insert, 0, 2));
      CHECK_FALSE(validator.accept(Kind::Insert, 1, 1));
      CHECK(validator.accept(Kind::Insert, 4, 1));
      CHECK_FALSE(validator.accept(Kind::Insert, 9, 1));
    }

    SECTION("updates address the final sequence and must ascend")
    {
      auto validator = RangeEditValidator{3};
      REQUIRE(validator.accept(Kind::Update, 0, 2));
      CHECK_FALSE(validator.accept(Kind::Update, 1, 1));
      CHECK(validator.accept(Kind::Update, 2, 1));
      CHECK_FALSE(validator.accept(Kind::Update, 3, 1));
    }

    SECTION("an empty range is never a valid edit")
    {
      auto validator = RangeEditValidator{4};
      CHECK_FALSE(validator.accept(Kind::Remove, 0, 0));
      CHECK_FALSE(validator.accept(Kind::Insert, 0, 0));
      CHECK_FALSE(validator.accept(Kind::Update, 0, 0));
    }

    SECTION("an insertion that would overflow the size counter is rejected")
    {
      auto validator = RangeEditValidator{1};
      CHECK_FALSE(validator.accept(Kind::Insert, 0, std::numeric_limits<std::size_t>::max()));
    }
  }
} // namespace ao::rt::delta::test
