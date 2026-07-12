// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/playback/PlaybackCursor.h"
#include "runtime/playback/ProjectionAnchor.h"
#include "runtime/playback/ShuffleHistory.h"
#include <ao/CoreIds.h>
#include <ao/Exception.h>
#include <ao/rt/PlaybackLaunchSpec.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/projection/TrackListProjection.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    constexpr auto kFirstTrack = TrackId{1};
    constexpr auto kSecondTrack = TrackId{2};
    constexpr auto kThirdTrack = TrackId{3};
    constexpr auto kFourthTrack = TrackId{4};

    class CursorPolicyDouble final : public PlaybackCursorPolicy
    {
    public:
      explicit CursorPolicyDouble(std::vector<TrackId> trackIds)
        : _trackIds{std::move(trackIds)}
        , _shuffleHistory{ShuffleHistory::CandidateChooser{
            [this](std::span<TrackId const> const candidates)
            {
              if (_optShuffleSelection && std::ranges::contains(candidates, *_optShuffleSelection))
              {
                return *_optShuffleSelection;
              }

              return candidates.back();
            }}}
      {
      }

      std::size_t projectionSize() const override
      {
        beforeQuery();
        return _trackIds.size();
      }

      TrackId trackIdAt(std::size_t const index) const override
      {
        beforeQuery();
        return _trackIds.at(index);
      }

      std::optional<std::size_t> indexOf(TrackId const trackId) const override
      {
        beforeQuery();

        if (_onIndexOf)
        {
          _onIndexOf();
        }

        auto const it = std::ranges::find(_trackIds, trackId);

        if (it == _trackIds.end())
        {
          return std::nullopt;
        }

        return static_cast<std::size_t>(std::distance(_trackIds.begin(), it));
      }

      std::optional<TrackId> shuffleForwardCandidate(TrackId const currentTrackId,
                                                     bool const currentIsBound,
                                                     bool const repeatAll) override
      {
        beforeQuery();
        ++_shuffleForwardQueries;
        return _shuffleHistory.forwardCandidate(_trackIds, currentTrackId, currentIsBound, repeatAll);
      }

      bool hasShufflePrevious(TrackId const currentTrackId) const override
      {
        beforeQuery();
        ++_shufflePreviousQueries;
        return _shuffleHistory.hasPrevious(_trackIds, currentTrackId);
      }

      std::optional<TrackId> popShufflePrevious(TrackId const currentTrackId) override
      {
        beforeQuery();
        ++_shufflePreviousQueries;
        return _shuffleHistory.popPrevious(_trackIds, currentTrackId);
      }

      void invalidateShuffleForwardCandidate() noexcept override
      {
        ++_shuffleCandidateInvalidations;
        _shuffleHistory.invalidateForwardCandidate();
      }

      void clearShuffleState() noexcept override
      {
        ++_shuffleClears;
        _shuffleHistory.clear();
      }

      void setTracks(std::vector<TrackId> trackIds) { _trackIds = std::move(trackIds); }
      std::vector<TrackId> const& tracks() const noexcept { return _trackIds; }
      void selectShuffleCandidate(TrackId trackId) { _optShuffleSelection = trackId; }
      void forbidQueries() noexcept { _queriesForbidden = true; }

      void recordHistory(TrackId leavingTrackId, TrackId arrivingTrackId)
      {
        _shuffleHistory.recordTransition(leavingTrackId, arrivingTrackId, ShuffleHistory::TransitionOrigin::Forward);
      }

      std::size_t historySize() const noexcept { return _shuffleHistory.historySize(); }
      std::size_t queryCount() const noexcept { return _queryCount; }
      std::size_t shuffleForwardQueryCount() const noexcept { return _shuffleForwardQueries; }
      std::size_t shuffleClearCount() const noexcept { return _shuffleClears; }
      void setIndexOfObserver(std::function<void()> observer) { _onIndexOf = std::move(observer); }

    private:
      void beforeQuery() const
      {
        if (_queriesForbidden)
        {
          throwException<Exception>("terminal cursor queried stale sequence state");
        }

        ++_queryCount;
      }

      std::vector<TrackId> _trackIds;
      std::optional<TrackId> _optShuffleSelection;
      ShuffleHistory _shuffleHistory;
      bool _queriesForbidden = false;
      mutable std::size_t _queryCount = 0;
      mutable std::size_t _shuffleForwardQueries = 0;
      mutable std::size_t _shufflePreviousQueries = 0;
      std::size_t _shuffleCandidateInvalidations = 0;
      std::size_t _shuffleClears = 0;
      std::function<void()> _onIndexOf{};
    };

    PlaybackLaunchSpec launchSpec()
    {
      return PlaybackLaunchSpec{
        .sourceListId = ListId{17},
        .quickFilterExpression = "$artist = \"Aobus\"",
      };
    }

    PlaybackCursor boundCursor(CursorPolicyDouble& policy,
                               TrackId const currentTrackId,
                               std::size_t const currentIndex,
                               RepeatMode const repeatMode = RepeatMode::Off,
                               ShuffleMode const shuffleMode = ShuffleMode::Off)
    {
      return PlaybackCursor{launchSpec(),
                            ProjectionAnchor::bound(currentTrackId, currentIndex, policy.tracks().size()),
                            repeatMode,
                            shuffleMode,
                            policy};
    }

    TrackListProjectionDeltaBatch insertBatch(std::size_t const start, std::size_t const count = 1)
    {
      return TrackListProjectionDeltaBatch{
        .deltas = {ProjectionInsertRange{TrackRowRange{.start = start, .count = count}}},
      };
    }

    TrackListProjectionDeltaBatch removeBatch(std::size_t const start, std::size_t const count = 1)
    {
      return TrackListProjectionDeltaBatch{
        .deltas = {ProjectionRemoveRange{TrackRowRange{.start = start, .count = count}}},
      };
    }

    TrackListProjectionDeltaBatch resetBatch()
    {
      return TrackListProjectionDeltaBatch{.deltas = {ProjectionReset{}}};
    }

    PlaybackCursor::CommandResolution startTrack(TrackId const trackId)
    {
      return PlaybackCursor::CommandResolution{
        .action = PlaybackCursor::CommandAction::StartTrack,
        .trackId = trackId,
      };
    }

    PlaybackCursor::CommandResolution restartCurrent(TrackId const trackId)
    {
      return PlaybackCursor::CommandResolution{
        .action = PlaybackCursor::CommandAction::RestartCurrent,
        .trackId = trackId,
      };
    }

    PlaybackCursor::CommandResolution stopPlayback()
    {
      return PlaybackCursor::CommandResolution{.action = PlaybackCursor::CommandAction::Stop};
    }

    PlaybackCursor::CommandResolution noCommand()
    {
      return PlaybackCursor::CommandResolution{.action = PlaybackCursor::CommandAction::NoOp};
    }

    struct ReferenceAnchor final
    {
      bool bound = false;
      std::size_t index = 0;
    };

    void reconcileReferenceAnchor(ReferenceAnchor& anchor,
                                  TrackId const currentTrackId,
                                  std::vector<TrackId> const& finalTracks)
    {
      auto const current = std::ranges::find(finalTracks, currentTrackId);

      if (current != finalTracks.end())
      {
        anchor.bound = true;
        anchor.index = static_cast<std::size_t>(std::distance(finalTracks.begin(), current));
        return;
      }

      anchor.bound = false;
      anchor.index = std::min(anchor.index, finalTracks.size());
    }

    void applyReferenceBatch(ReferenceAnchor& anchor,
                             TrackListProjectionDeltaBatch const& batch,
                             TrackId const currentTrackId,
                             std::vector<TrackId> const& finalTracks)
    {
      if (std::holds_alternative<ProjectionSourceInvalidated>(batch.deltas.front()))
      {
        throwException<Exception>("reference anchor cannot consume a source-invalidated projection");
      }

      if (std::holds_alternative<ProjectionReset>(batch.deltas.front()))
      {
        if (batch.deltas.size() != 1)
        {
          throwException<Exception>("reference projection reset must be a singleton");
        }

        reconcileReferenceAnchor(anchor, currentTrackId, finalTracks);
        return;
      }

      for (auto const& delta : batch.deltas)
      {
        if (auto const* insert = std::get_if<ProjectionInsertRange>(&delta); insert != nullptr)
        {
          if (anchor.bound ? insert->range.start <= anchor.index : insert->range.start < anchor.index)
          {
            anchor.index += insert->range.count;
          }
        }
        else if (auto const* remove = std::get_if<ProjectionRemoveRange>(&delta); remove != nullptr)
        {
          if (auto const end = remove->range.start + remove->range.count; end <= anchor.index)
          {
            anchor.index -= remove->range.count;
          }
          else if (remove->range.start <= anchor.index && anchor.index < end)
          {
            anchor.index = remove->range.start;
            anchor.bound = false;
          }
        }
        else if (std::holds_alternative<ProjectionUpdateRange>(delta))
        {
          continue;
        }
        else if (std::holds_alternative<ProjectionSourceInvalidated>(delta))
        {
          throwException<Exception>("reference anchor cannot consume a source-invalidated projection");
        }
        else
        {
          throwException<Exception>("reference projection reset must be a singleton");
        }
      }

      reconcileReferenceAnchor(anchor, currentTrackId, finalTracks);
    }

    std::optional<TrackId> referenceSuccessor(std::vector<TrackId> const& tracks,
                                              ReferenceAnchor const& anchor,
                                              TrackId const currentTrackId,
                                              RepeatMode const repeatMode)
    {
      if (repeatMode == RepeatMode::One)
      {
        return currentTrackId;
      }

      auto const candidateIndex = anchor.bound ? anchor.index + 1 : anchor.index;

      if (candidateIndex < tracks.size())
      {
        return tracks[candidateIndex];
      }

      if (repeatMode == RepeatMode::All && !tracks.empty())
      {
        return tracks.front();
      }

      return std::nullopt;
    }

    std::optional<TrackId> referencePrevious(std::vector<TrackId> const& tracks,
                                             ReferenceAnchor const& anchor,
                                             RepeatMode const repeatMode)
    {
      if (anchor.index > 0)
      {
        return tracks[anchor.index - 1];
      }

      if (repeatMode == RepeatMode::All && !tracks.empty())
      {
        return tracks.back();
      }

      return std::nullopt;
    }

    PlaybackCursor::SemanticTuple referenceTuple(std::vector<TrackId> const& tracks,
                                                 ReferenceAnchor const& anchor,
                                                 TrackId const currentTrackId,
                                                 RepeatMode const repeatMode)
    {
      auto const optSuccessor = referenceSuccessor(tracks, anchor, currentTrackId, repeatMode);
      return PlaybackCursor::SemanticTuple{
        .sourceState = PlaybackCursor::SourceState::Live,
        .currentTrackId = currentTrackId,
        .hasNext = optSuccessor.has_value(),
        .hasPrevious = referencePrevious(tracks, anchor, repeatMode).has_value(),
        .optResolvedSuccessor = optSuccessor,
      };
    }
  } // namespace

  TEST_CASE("PlaybackCursor - construction owns fixed launch spec and exact initial semantic tuple",
            "[runtime][unit][playback-cursor]")
  {
    auto policy = CursorPolicyDouble{{kFirstTrack, kSecondTrack, kThirdTrack}};
    auto spec = launchSpec();
    auto const originalSpec = spec;
    auto cursor = PlaybackCursor{spec,
                                 ProjectionAnchor::bound(kSecondTrack, 1, policy.tracks().size()),
                                 RepeatMode::Off,
                                 ShuffleMode::Off,
                                 policy};
    spec.sourceListId = ListId{99};
    spec.quickFilterExpression = "changed";
    auto const expectedTuple = PlaybackCursor::SemanticTuple{
      .sourceState = PlaybackCursor::SourceState::Live,
      .currentTrackId = kSecondTrack,
      .hasNext = true,
      .hasPrevious = true,
      .optResolvedSuccessor = kThirdTrack,
    };

    CHECK(cursor.launchSpec() == originalSpec);
    CHECK(cursor.currentTrackId() == kSecondTrack);
    CHECK(cursor.anchor().state() == ProjectionAnchor::State::Bound);
    CHECK(cursor.anchor().anchorIndex() == 1);
    CHECK(cursor.semanticTuple() == expectedTuple);
    CHECK(cursor.semanticRevision() == 0);
  }

  TEST_CASE("PlaybackCursor - source invalidation is terminal without stale sequence queries",
            "[runtime][unit][playback-cursor]")
  {
    auto policy = CursorPolicyDouble{{kFirstTrack, kSecondTrack, kThirdTrack}};
    policy.recordHistory(kFirstTrack, kSecondTrack);
    auto cursor = boundCursor(policy, kSecondTrack, 1, RepeatMode::One, ShuffleMode::On);
    auto const queriesBeforeInvalidation = policy.queryCount();

    auto effect = cursor.invalidateSource(policy);

    CHECK(effect == (PlaybackCursor::MutationEffect{.semanticChanged = true, .persistenceIntentChanged = false}));
    CHECK(cursor.sourceState() == PlaybackCursor::SourceState::Invalidated);
    CHECK(cursor.semanticTuple().sourceState == PlaybackCursor::SourceState::Invalidated);
    CHECK_FALSE(cursor.semanticTuple().hasNext);
    CHECK_FALSE(cursor.semanticTuple().hasPrevious);
    CHECK_FALSE(cursor.semanticTuple().optResolvedSuccessor);
    CHECK(policy.queryCount() == queriesBeforeInvalidation);
    CHECK(policy.shuffleClearCount() == 1);

    policy.forbidQueries();
    CHECK(cursor.resolveNext() == stopPlayback());
    CHECK(cursor.resolveNaturalAdvance() == stopPlayback());
    CHECK(cursor.resolvePrevious(policy) == noCommand());

    effect = cursor.setRepeatMode(RepeatMode::All, policy);
    CHECK(effect == (PlaybackCursor::MutationEffect{.semanticChanged = false, .persistenceIntentChanged = true}));
    effect = cursor.setShuffleMode(ShuffleMode::Off, policy);
    CHECK(effect == (PlaybackCursor::MutationEffect{.semanticChanged = false, .persistenceIntentChanged = true}));

    effect = cursor.setPreviousRestartAvailable(true, policy);
    CHECK(effect == (PlaybackCursor::MutationEffect{.semanticChanged = true, .persistenceIntentChanged = false}));
    CHECK(cursor.resolvePrevious(policy) == restartCurrent(kSecondTrack));
    CHECK(cursor.resolveNext() == stopPlayback());

    auto const frozenAnchor = cursor.anchor();
    effect = cursor.adoptInvalidatedCurrent(kFourthTrack);
    CHECK(effect == (PlaybackCursor::MutationEffect{.semanticChanged = true, .persistenceIntentChanged = true}));
    CHECK(cursor.currentTrackId() == kFourthTrack);
    CHECK(cursor.anchor() == frozenAnchor);
    CHECK(cursor.resolvePrevious(policy) == restartCurrent(kFourthTrack));
    REQUIRE_THROWS_AS(cursor.applyProjectionBatch(resetBatch(), policy), Exception);
  }

  TEST_CASE("PlaybackCursor - repeat one overrides shuffle forward but not previous policy",
            "[runtime][unit][playback-cursor]")
  {
    auto policy = CursorPolicyDouble{{kFirstTrack, kSecondTrack, kThirdTrack, kFourthTrack}};
    policy.selectShuffleCandidate(kFourthTrack);
    policy.recordHistory(kThirdTrack, kSecondTrack);
    auto cursor = boundCursor(policy, kSecondTrack, 1, RepeatMode::One, ShuffleMode::On);

    CHECK(cursor.semanticTuple().optResolvedSuccessor == kSecondTrack);
    CHECK(cursor.resolveNext() == startTrack(kSecondTrack));
    CHECK(policy.shuffleForwardQueryCount() == 0);
    CHECK(cursor.resolvePrevious(policy) == startTrack(kThirdTrack));
  }

  TEST_CASE("PlaybackCursor - elapsed restart then shuffle then sequential order command precedence",
            "[runtime][unit][playback-cursor]")
  {
    auto shufflePolicy = CursorPolicyDouble{{kFirstTrack, kSecondTrack, kThirdTrack, kFourthTrack}};
    shufflePolicy.selectShuffleCandidate(kFourthTrack);
    shufflePolicy.recordHistory(kThirdTrack, kSecondTrack);
    auto shuffleCursor = boundCursor(shufflePolicy, kSecondTrack, 1, RepeatMode::Off, ShuffleMode::On);

    CHECK(shuffleCursor.resolveNext() == startTrack(kFourthTrack));
    CHECK(shuffleCursor.resolvePrevious(shufflePolicy) == startTrack(kThirdTrack));

    auto restartPolicy = CursorPolicyDouble{{kFirstTrack, kSecondTrack, kThirdTrack, kFourthTrack}};
    restartPolicy.selectShuffleCandidate(kFourthTrack);
    restartPolicy.recordHistory(kThirdTrack, kSecondTrack);
    auto restartCursor = boundCursor(restartPolicy, kSecondTrack, 1, RepeatMode::Off, ShuffleMode::On);
    auto const restartEffect = restartCursor.setPreviousRestartAvailable(true, restartPolicy);

    CHECK(restartEffect.persistenceIntentChanged == false);
    CHECK(restartCursor.resolvePrevious(restartPolicy) == restartCurrent(kSecondTrack));
    CHECK(restartPolicy.historySize() == 1);

    auto sequentialPolicy = CursorPolicyDouble{{kFirstTrack, kSecondTrack, kThirdTrack, kFourthTrack}};
    auto sequentialCursor = boundCursor(sequentialPolicy, kSecondTrack, 1);
    CHECK(sequentialCursor.resolveNext() == startTrack(kThirdTrack));
    CHECK(sequentialCursor.resolvePrevious(sequentialPolicy) == startTrack(kFirstTrack));
  }

  TEST_CASE("PlaybackCursor - Gap and empty projections follow sequential repeat rules",
            "[runtime][unit][playback-cursor]")
  {
    auto policy = CursorPolicyDouble{{kFirstTrack, kSecondTrack, kThirdTrack}};
    auto cursor = boundCursor(policy, kSecondTrack, 1);

    auto effect = cursor.adoptLiveCurrent(ProjectionAnchor::gap(TrackId{9}, 1, policy.tracks().size()), policy);
    CHECK(effect.persistenceIntentChanged);
    CHECK(cursor.resolveNext() == startTrack(kSecondTrack));
    CHECK(cursor.resolvePrevious(policy) == startTrack(kFirstTrack));

    effect = cursor.adoptLiveCurrent(ProjectionAnchor::gap(TrackId{9}, 0, policy.tracks().size()), policy);
    REQUIRE(effect.persistenceIntentChanged);
    effect = cursor.setRepeatMode(RepeatMode::All, policy);
    REQUIRE(effect.persistenceIntentChanged);
    CHECK(cursor.resolveNext() == startTrack(kFirstTrack));
    CHECK(cursor.resolvePrevious(policy) == startTrack(kThirdTrack));

    policy.setTracks({});
    effect = cursor.applyProjectionBatch(resetBatch(), policy);
    CHECK(cursor.anchor().state() == ProjectionAnchor::State::Gap);
    CHECK(cursor.anchor().anchorIndex() == 0);
    CHECK(cursor.resolveNext() == stopPlayback());
    CHECK(cursor.resolvePrevious(policy) == noCommand());

    effect = cursor.setRepeatMode(RepeatMode::One, policy);
    CHECK(effect.persistenceIntentChanged);
    CHECK(cursor.resolveNext() == startTrack(TrackId{9}));
    CHECK(cursor.resolvePrevious(policy) == noCommand());
  }

  TEST_CASE("PlaybackCursor - semantic revision and persistence intent classify distinct mutations",
            "[runtime][unit][playback-cursor]")
  {
    auto policy = CursorPolicyDouble{{kFirstTrack, kSecondTrack, kThirdTrack, kFourthTrack}};
    auto cursor = boundCursor(policy, kSecondTrack, 1);
    REQUIRE(cursor.semanticTuple().optResolvedSuccessor == kThirdTrack);
    REQUIRE(cursor.semanticRevision() == 0);

    policy.setTracks({kFirstTrack, kSecondTrack, kThirdTrack, kFourthTrack, TrackId{5}});
    auto effect = cursor.applyProjectionBatch(insertBatch(4), policy);
    CHECK(effect == PlaybackCursor::MutationEffect{});
    CHECK(cursor.semanticRevision() == 0);

    policy.setTracks({TrackId{6}, kFirstTrack, kSecondTrack, kThirdTrack, kFourthTrack, TrackId{5}});
    effect = cursor.applyProjectionBatch(insertBatch(0), policy);
    CHECK(effect == (PlaybackCursor::MutationEffect{.semanticChanged = false, .persistenceIntentChanged = true}));
    CHECK(cursor.anchor().anchorIndex() == 2);
    CHECK(cursor.semanticRevision() == 0);

    policy.setTracks({TrackId{6}, kFirstTrack, kThirdTrack, kFourthTrack, TrackId{5}});
    effect = cursor.applyProjectionBatch(removeBatch(2), policy);
    CHECK(effect == PlaybackCursor::MutationEffect{});
    CHECK(cursor.anchor().state() == ProjectionAnchor::State::Gap);
    CHECK(cursor.anchor().anchorIndex() == 2);
    CHECK(cursor.semanticTuple().optResolvedSuccessor == kThirdTrack);
    CHECK(cursor.semanticRevision() == 0);

    policy.setTracks({TrackId{6}, kFirstTrack, kFourthTrack, TrackId{5}});
    effect = cursor.applyProjectionBatch(removeBatch(2), policy);
    CHECK(effect == (PlaybackCursor::MutationEffect{.semanticChanged = true, .persistenceIntentChanged = false}));
    CHECK(cursor.semanticTuple().optResolvedSuccessor == kFourthTrack);
    CHECK(cursor.semanticRevision() == 1);

    effect = cursor.setRepeatMode(RepeatMode::All, policy);
    CHECK(effect == (PlaybackCursor::MutationEffect{.semanticChanged = false, .persistenceIntentChanged = true}));
    CHECK(cursor.semanticRevision() == 1);

    effect = cursor.setPreviousRestartAvailable(true, policy);
    CHECK(effect == PlaybackCursor::MutationEffect{});
    CHECK(cursor.semanticRevision() == 1);

    effect = cursor.invalidateSource(policy);
    CHECK(effect == (PlaybackCursor::MutationEffect{.semanticChanged = true, .persistenceIntentChanged = false}));
    CHECK(cursor.semanticRevision() == 2);
  }

  TEST_CASE("PlaybackCursor - projection move reconciliation is observable only after the complete batch",
            "[runtime][unit][playback-cursor]")
  {
    auto policy = CursorPolicyDouble{{kFirstTrack, kSecondTrack, kThirdTrack, kFourthTrack}};
    auto cursor = boundCursor(policy, kSecondTrack, 1);
    auto optObservedAnchor = std::optional<ProjectionAnchor>{};
    auto optObservedRevision = std::optional<std::uint64_t>{};
    policy.setIndexOfObserver(
      [&]
      {
        optObservedAnchor = cursor.anchor();
        optObservedRevision = cursor.semanticRevision();
      });
    policy.setTracks({kFirstTrack, kThirdTrack, kFourthTrack, kSecondTrack});
    auto const moveBatch = TrackListProjectionDeltaBatch{
      .deltas =
        {
          ProjectionRemoveRange{TrackRowRange{.start = 1, .count = 1}},
          ProjectionInsertRange{TrackRowRange{.start = 3, .count = 1}},
        },
    };

    auto const effect = cursor.applyProjectionBatch(moveBatch, policy);

    REQUIRE(optObservedAnchor);
    CHECK(optObservedAnchor->state() == ProjectionAnchor::State::Bound);
    CHECK(optObservedAnchor->anchorIndex() == 1);
    CHECK(optObservedRevision == 0);
    CHECK(effect == (PlaybackCursor::MutationEffect{.semanticChanged = true, .persistenceIntentChanged = true}));
    CHECK(cursor.anchor().state() == ProjectionAnchor::State::Bound);
    CHECK(cursor.anchor().anchorIndex() == 3);
    CHECK(cursor.semanticRevision() == 1);
    CHECK_FALSE(cursor.semanticTuple().optResolvedSuccessor);
  }

  TEST_CASE("PlaybackCursor - deterministic reference sequence preserves anchor and semantic invariants",
            "[runtime][unit][playback-cursor][model]")
  {
    constexpr std::uint32_t kSeed = 0x00C0FFEEU;
    constexpr std::size_t kStepCount = 80;
    auto randomState = kSeed;
    auto nextRandom = [&randomState]
    {
      randomState = (randomState * 1'664'525U) + 1'013'904'223U;
      return randomState;
    };
    auto tracks = std::vector{kFirstTrack, kSecondTrack, kThirdTrack, kFourthTrack};
    auto policy = CursorPolicyDouble{tracks};
    auto cursor = boundCursor(policy, kSecondTrack, 1);
    auto referenceAnchor = ReferenceAnchor{.bound = true, .index = 1};
    auto repeatMode = RepeatMode::Off;
    auto expectedTuple = referenceTuple(tracks, referenceAnchor, kSecondTrack, repeatMode);
    std::uint64_t expectedRevision = 0;
    std::uint32_t nextTrackValue = 100;
    auto trace = std::string{};

    for (std::size_t step = 0; step < kStepCount; ++step)
    {
      INFO("seed=" << kSeed << " step=" << step << "\n" << trace);

      if (step > 0 && step % 13 == 0)
      {
        auto const previousTuple = expectedTuple;
        repeatMode = repeatMode == RepeatMode::Off ? RepeatMode::All : RepeatMode::Off;
        auto const modeEffect = cursor.setRepeatMode(repeatMode, policy);
        expectedTuple = referenceTuple(tracks, referenceAnchor, kSecondTrack, repeatMode);
        auto const semanticChanged = expectedTuple != previousTuple;

        if (semanticChanged)
        {
          ++expectedRevision;
        }

        trace += std::format("{}: repeat={}\n", step, repeatMode == RepeatMode::All ? "all" : "off");
        CHECK(modeEffect.semanticChanged == semanticChanged);
        CHECK(modeEffect.persistenceIntentChanged);
        CHECK(cursor.semanticTuple() == expectedTuple);
        CHECK(cursor.semanticRevision() == expectedRevision);
      }

      auto batch = TrackListProjectionDeltaBatch{};
      auto operation = std::string{};
      auto const operationKind = nextRandom() % 5U;
      auto const currentPresent = std::ranges::contains(tracks, kSecondTrack);

      if (operationKind == 0U || (tracks.empty() && operationKind != 4U))
      {
        auto const insertionIndex = static_cast<std::size_t>(nextRandom()) % (tracks.size() + 1);
        auto const insertCurrent = !currentPresent && nextRandom() % 3U == 0U;
        auto const insertedTrackId = insertCurrent ? kSecondTrack : TrackId{nextTrackValue++};
        tracks.insert(tracks.begin() + static_cast<std::ptrdiff_t>(insertionIndex), insertedTrackId);
        batch = insertBatch(insertionIndex);
        operation = std::format("insert {} at {}", insertedTrackId.raw(), insertionIndex);
      }
      else if (operationKind == 1U)
      {
        auto const start = static_cast<std::size_t>(nextRandom()) % tracks.size();
        auto const maximumCount = std::min<std::size_t>(2, tracks.size() - start);
        auto const count = 1 + (static_cast<std::size_t>(nextRandom()) % maximumCount);
        tracks.erase(tracks.begin() + static_cast<std::ptrdiff_t>(start),
                     tracks.begin() + static_cast<std::ptrdiff_t>(start + count));
        batch = removeBatch(start, count);
        operation = std::format("remove [{}, {})", start, start + count);
      }
      else if (operationKind == 2U && tracks.size() > 1)
      {
        auto const from = static_cast<std::size_t>(nextRandom()) % tracks.size();
        auto const movedTrackId = tracks[from];
        tracks.erase(tracks.begin() + static_cast<std::ptrdiff_t>(from));
        auto target = static_cast<std::size_t>(nextRandom()) % (tracks.size() + 1);

        if (target == from)
        {
          target = (target + 1) % (tracks.size() + 1);
        }

        tracks.insert(tracks.begin() + static_cast<std::ptrdiff_t>(target), movedTrackId);
        batch = TrackListProjectionDeltaBatch{
          .deltas =
            {
              ProjectionRemoveRange{TrackRowRange{.start = from, .count = 1}},
              ProjectionInsertRange{TrackRowRange{.start = target, .count = 1}},
            },
        };
        operation = std::format("move {} from {} to {}", movedTrackId.raw(), from, target);
      }
      else if (operationKind == 3U || (operationKind == 2U && tracks.size() == 1))
      {
        auto const index = static_cast<std::size_t>(nextRandom()) % tracks.size();
        batch = TrackListProjectionDeltaBatch{
          .deltas = {ProjectionUpdateRange{TrackRowRange{.start = index, .count = 1}}},
        };
        operation = std::format("update {}", index);
      }
      else
      {
        auto const count = static_cast<std::size_t>(nextRandom() % 6U);
        tracks.clear();
        tracks.reserve(count);

        for (std::size_t index = 0; index < count; ++index)
        {
          tracks.emplace_back(nextTrackValue++);
        }

        if (!tracks.empty() && step % 2 == 0)
        {
          tracks[static_cast<std::size_t>(nextRandom()) % tracks.size()] = kSecondTrack;
        }

        batch = resetBatch();
        operation = std::format("reset {} rows", count);
      }

      trace += std::format("{}: {}\n", step, operation);
      INFO("seed=" << kSeed << " step=" << step << "\n" << trace);
      auto const previousTuple = expectedTuple;
      auto const previousAnchorIndex = referenceAnchor.index;
      policy.setTracks(tracks);
      applyReferenceBatch(referenceAnchor, batch, kSecondTrack, tracks);
      expectedTuple = referenceTuple(tracks, referenceAnchor, kSecondTrack, repeatMode);
      auto const semanticChanged = expectedTuple != previousTuple;
      auto const persistenceIntentChanged = referenceAnchor.index != previousAnchorIndex;

      if (semanticChanged)
      {
        ++expectedRevision;
      }

      auto const effect = cursor.applyProjectionBatch(batch, policy);
      auto const current = std::ranges::find(tracks, kSecondTrack);
      auto const expectedNext =
        expectedTuple.optResolvedSuccessor ? startTrack(*expectedTuple.optResolvedSuccessor) : stopPlayback();
      auto const optPrevious = referencePrevious(tracks, referenceAnchor, repeatMode);
      auto const expectedPrevious = optPrevious ? startTrack(*optPrevious) : noCommand();

      CHECK(cursor.anchor().anchorIndex() <= tracks.size());
      CHECK((cursor.anchor().state() == ProjectionAnchor::State::Bound) == (current != tracks.end()));
      CHECK(cursor.anchor().anchorIndex() == referenceAnchor.index);
      CHECK(effect.semanticChanged == semanticChanged);
      CHECK(effect.persistenceIntentChanged == persistenceIntentChanged);
      CHECK(cursor.semanticTuple() == expectedTuple);
      CHECK(cursor.semanticRevision() == expectedRevision);
      CHECK(cursor.resolveNext() == expectedNext);
      CHECK(cursor.resolvePrevious(policy) == expectedPrevious);
    }
  }
} // namespace ao::rt::test
