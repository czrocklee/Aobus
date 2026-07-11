// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/playback/ShuffleHistory.h"

#include <ao/CoreIds.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    constexpr auto kFirstTrack = TrackId{1};
    constexpr auto kSecondTrack = TrackId{2};
    constexpr auto kThirdTrack = TrackId{3};
    constexpr auto kFourthTrack = TrackId{4};

    struct ScriptedCandidateChooser final
    {
      std::vector<TrackId> selections;
      std::vector<std::vector<TrackId>> candidateSets;

      TrackId operator()(std::span<TrackId const> const candidates)
      {
        auto const selectionIndex = candidateSets.size();
        candidateSets.emplace_back(candidates.begin(), candidates.end());
        return selections.at(selectionIndex);
      }
    };

    TrackId chooseFirst(std::span<TrackId const> const candidates)
    {
      return candidates.front();
    }
  } // namespace

  TEST_CASE("ShuffleHistory - unrelated membership changes preserve an eligible sticky candidate",
            "[runtime][unit][playback-cursor]")
  {
    auto chooser = ScriptedCandidateChooser{
      .selections = {kSecondTrack, kThirdTrack},
      .candidateSets = {},
    };
    auto history = ShuffleHistory{ShuffleHistory::CandidateChooser{std::ref(chooser)}};
    constexpr auto kInitialProjection = std::array{kFirstTrack, kSecondTrack, kThirdTrack};
    constexpr auto kChangedProjection = std::array{kFourthTrack, kFirstTrack, kThirdTrack, kSecondTrack};

    auto const optInitial = history.forwardCandidate(kInitialProjection, kFirstTrack, true, false);
    auto const optAfterUnrelatedChange = history.forwardCandidate(kChangedProjection, kFirstTrack, true, false);

    CHECK(optInitial == kSecondTrack);
    CHECK(optAfterUnrelatedChange == kSecondTrack);
    REQUIRE(chooser.candidateSets.size() == 1);
    CHECK(chooser.candidateSets[0] == std::vector{kSecondTrack, kThirdTrack});
    CHECK(history.pendingForwardCandidate() == kSecondTrack);
  }

  TEST_CASE("ShuffleHistory - departed and explicitly invalidated candidates are re-rolled",
            "[runtime][unit][playback-cursor]")
  {
    auto chooser = ScriptedCandidateChooser{
      .selections = {kSecondTrack, kThirdTrack, kFourthTrack},
      .candidateSets = {},
    };
    auto history = ShuffleHistory{ShuffleHistory::CandidateChooser{std::ref(chooser)}};
    constexpr auto kInitialProjection = std::array{kFirstTrack, kSecondTrack, kThirdTrack};
    constexpr auto kWithoutSecond = std::array{kFirstTrack, kThirdTrack};
    constexpr auto kWithFourth = std::array{kFirstTrack, kThirdTrack, kFourthTrack};

    REQUIRE(history.forwardCandidate(kInitialProjection, kFirstTrack, true, false) == kSecondTrack);
    CHECK(history.forwardCandidate(kWithoutSecond, kFirstTrack, true, false) == kThirdTrack);

    history.invalidateForwardCandidate();
    CHECK(history.forwardCandidate(kWithFourth, kFirstTrack, true, false) == kFourthTrack);

    REQUIRE(chooser.candidateSets.size() == 3);
    CHECK(chooser.candidateSets[0] == std::vector{kSecondTrack, kThirdTrack});
    CHECK(chooser.candidateSets[1] == std::vector{kThirdTrack});
    CHECK(chooser.candidateSets[2] == std::vector{kThirdTrack, kFourthTrack});
  }

  TEST_CASE("ShuffleHistory - Bound and Gap eligibility follow repeat-all fallback rules",
            "[runtime][unit][playback-cursor]")
  {
    auto chooser = ScriptedCandidateChooser{
      .selections = {kFirstTrack, kFirstTrack},
      .candidateSets = {},
    };
    auto history = ShuffleHistory{ShuffleHistory::CandidateChooser{std::ref(chooser)}};
    constexpr auto kSoleCurrent = std::array{kFirstTrack};

    CHECK_FALSE(history.forwardCandidate(kSoleCurrent, kFirstTrack, true, false));
    CHECK(history.forwardCandidate(kSoleCurrent, kFirstTrack, true, true) == kFirstTrack);

    history.invalidateForwardCandidate();
    CHECK(history.forwardCandidate(kSoleCurrent, kFirstTrack, false, false) == kFirstTrack);

    REQUIRE(chooser.candidateSets.size() == 2);
    CHECK(chooser.candidateSets[0] == std::vector{kFirstTrack});
    CHECK(chooser.candidateSets[1] == std::vector{kFirstTrack});
  }

  TEST_CASE("ShuffleHistory - failed candidate discard is exact and permits a fresh choice",
            "[runtime][unit][playback-cursor]")
  {
    auto chooser = ScriptedCandidateChooser{
      .selections = {kSecondTrack, kThirdTrack},
      .candidateSets = {},
    };
    auto history = ShuffleHistory{ShuffleHistory::CandidateChooser{std::ref(chooser)}};
    constexpr auto kProjection = std::array{kFirstTrack, kSecondTrack, kThirdTrack};

    REQUIRE(history.forwardCandidate(kProjection, kFirstTrack, true, false) == kSecondTrack);
    CHECK(history.discardForwardCandidate(kSecondTrack));
    REQUIRE(history.forwardCandidate(kProjection, kFirstTrack, true, false) == kThirdTrack);

    CHECK_FALSE(history.discardForwardCandidate(kSecondTrack));
    CHECK(history.pendingForwardCandidate() == kThirdTrack);
  }

  TEST_CASE("ShuffleHistory - failure walk excludes every attempted forward candidate",
            "[runtime][unit][playback-cursor]")
  {
    auto chooser = ScriptedCandidateChooser{
      .selections = {kSecondTrack, kFourthTrack},
      .candidateSets = {},
    };
    auto history = ShuffleHistory{ShuffleHistory::CandidateChooser{std::ref(chooser)}};
    constexpr auto kProjection = std::array{kFirstTrack, kSecondTrack, kThirdTrack, kFourthTrack};
    constexpr auto kFailed = std::array{kSecondTrack, kThirdTrack};

    REQUIRE(history.forwardCandidate(kProjection, kFirstTrack, true, false) == kSecondTrack);
    history.invalidateForwardCandidate();
    CHECK(history.forwardCandidate(kProjection, kFirstTrack, true, false, kFailed) == kFourthTrack);

    REQUIRE(chooser.candidateSets.size() == 2);
    CHECK(chooser.candidateSets[1] == std::vector{kFourthTrack});
  }

  TEST_CASE("ShuffleHistory - forward and sequential previous transitions record the actual path",
            "[runtime][unit][playback-cursor]")
  {
    auto history = ShuffleHistory{chooseFirst};
    constexpr auto kProjection = std::array{kFirstTrack, kSecondTrack, kThirdTrack};

    history.recordTransition(kFirstTrack, kThirdTrack, ShuffleHistory::TransitionOrigin::Forward);
    history.recordTransition(kThirdTrack, kSecondTrack, ShuffleHistory::TransitionOrigin::SequentialPrevious);

    CHECK(history.historySize() == 2);
    CHECK(history.popPrevious(kProjection, kSecondTrack) == kThirdTrack);
    CHECK(history.popPrevious(kProjection, kSecondTrack) == kFirstTrack);
    CHECK_FALSE(history.popPrevious(kProjection, kSecondTrack));
  }

  TEST_CASE("ShuffleHistory - successful history previous walks backward without bouncing",
            "[runtime][unit][playback-cursor]")
  {
    auto history = ShuffleHistory{chooseFirst};
    constexpr auto kProjection = std::array{kFirstTrack, kSecondTrack, kThirdTrack};
    history.recordTransition(kFirstTrack, kSecondTrack, ShuffleHistory::TransitionOrigin::Forward);
    history.recordTransition(kSecondTrack, kThirdTrack, ShuffleHistory::TransitionOrigin::Forward);

    auto const optFirstPrevious = history.popPrevious(kProjection, kThirdTrack);
    REQUIRE(optFirstPrevious == kSecondTrack);
    history.recordTransition(kThirdTrack, *optFirstPrevious, ShuffleHistory::TransitionOrigin::HistoryPrevious);

    CHECK(history.historySize() == 1);
    CHECK(history.popPrevious(kProjection, kSecondTrack) == kFirstTrack);
    CHECK_FALSE(history.popPrevious(kProjection, kFirstTrack));
  }

  TEST_CASE("ShuffleHistory - hasPrevious scans membership without mutating stale or current entries",
            "[runtime][unit][playback-cursor]")
  {
    auto history = ShuffleHistory{chooseFirst};
    history.recordTransition(kFirstTrack, kSecondTrack, ShuffleHistory::TransitionOrigin::Forward);
    history.recordTransition(kSecondTrack, kThirdTrack, ShuffleHistory::TransitionOrigin::Forward);
    history.recordTransition(kThirdTrack, kFirstTrack, ShuffleHistory::TransitionOrigin::Forward);
    constexpr auto kWithOlderCandidate = std::array{kFirstTrack, kSecondTrack};
    constexpr auto kOnlyCurrent = std::array{kFirstTrack};

    CHECK(history.hasPrevious(kWithOlderCandidate, kFirstTrack));
    CHECK_FALSE(history.hasPrevious(kOnlyCurrent, kFirstTrack));
    CHECK(history.historySize() == 3);

    CHECK(history.popPrevious(kWithOlderCandidate, kFirstTrack) == kSecondTrack);
    CHECK(history.historySize() == 1);
    CHECK_FALSE(history.popPrevious(kOnlyCurrent, kFirstTrack));
    CHECK(history.historySize() == 0);
  }

  TEST_CASE("ShuffleHistory - failed history candidate remains popped so the walk can continue",
            "[runtime][unit][playback-cursor]")
  {
    auto history = ShuffleHistory{chooseFirst};
    constexpr auto kProjection = std::array{kFirstTrack, kSecondTrack, kThirdTrack};
    history.recordTransition(kFirstTrack, kSecondTrack, ShuffleHistory::TransitionOrigin::Forward);
    history.recordTransition(kSecondTrack, kThirdTrack, ShuffleHistory::TransitionOrigin::Forward);

    auto const optFailedCandidate = history.popPrevious(kProjection, kThirdTrack);
    REQUIRE(optFailedCandidate == kSecondTrack);

    CHECK(history.popPrevious(kProjection, kThirdTrack) == kFirstTrack);
    CHECK(history.historySize() == 0);
  }

  TEST_CASE("ShuffleHistory - same-track replay and restart do not push history", "[runtime][unit][playback-cursor]")
  {
    auto history = ShuffleHistory{chooseFirst};

    history.recordTransition(kFirstTrack, kFirstTrack, ShuffleHistory::TransitionOrigin::Forward);
    history.recordTransition(kFirstTrack, kFirstTrack, ShuffleHistory::TransitionOrigin::Restart);
    history.recordTransition(kFirstTrack, kSecondTrack, ShuffleHistory::TransitionOrigin::Forward);
    history.recordTransition(kSecondTrack, kSecondTrack, ShuffleHistory::TransitionOrigin::Forward);
    history.recordTransition(kSecondTrack, kSecondTrack, ShuffleHistory::TransitionOrigin::Restart);

    CHECK(history.historySize() == 1);
  }

  TEST_CASE("ShuffleHistory - source invalidation clears candidate and navigation history",
            "[runtime][unit][playback-cursor]")
  {
    auto history = ShuffleHistory{chooseFirst};
    constexpr auto kProjection = std::array{kFirstTrack, kSecondTrack};
    history.recordTransition(kFirstTrack, kSecondTrack, ShuffleHistory::TransitionOrigin::Forward);
    REQUIRE(history.historySize() == 1);
    REQUIRE(history.forwardCandidate(kProjection, kSecondTrack, true, false) == kFirstTrack);
    REQUIRE(history.pendingForwardCandidate() == kFirstTrack);

    history.clear();

    CHECK_FALSE(history.pendingForwardCandidate());
    CHECK(history.historySize() == 0);
    CHECK_FALSE(history.hasPrevious(kProjection, kSecondTrack));
  }

  TEST_CASE("ShuffleHistory - navigation history retains only the newest 64 entries",
            "[runtime][unit][playback-cursor]")
  {
    auto history = ShuffleHistory{chooseFirst};
    auto projection = std::vector<TrackId>{};

    for (std::uint32_t value = 1; value <= 66; ++value)
    {
      projection.emplace_back(value);
    }

    for (std::uint32_t value = 1; value <= 65; ++value)
    {
      history.recordTransition(TrackId{value}, TrackId{value + 1}, ShuffleHistory::TransitionOrigin::Forward);
    }

    REQUIRE(history.historySize() == ShuffleHistory::kHistoryCapacity);
    auto popped = std::vector<TrackId>{};

    while (auto const optTrackId = history.popPrevious(projection, TrackId{66}))
    {
      popped.push_back(*optTrackId);
    }

    REQUIRE(popped.size() == ShuffleHistory::kHistoryCapacity);
    CHECK(popped.front() == TrackId{65});
    CHECK(popped.back() == TrackId{2});
    CHECK_FALSE(std::ranges::contains(popped, TrackId{1}));
  }
} // namespace ao::rt::test
