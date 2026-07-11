// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/Exception.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/query/ExecutionPlan.h>
#include <ao/query/Field.h>
#include <ao/query/detail/Bytecode.h>
#include <ao/rt/ScopedTimer.h>
#include <ao/rt/StorageResult.h>
#include <ao/rt/source/SmartListEvaluator.h>
#include <ao/rt/source/SmartListSource.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceDelta.h>

#include <boost/container/small_vector.hpp>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt
{
  namespace
  {
    template<typename Index>
    void rebuildIndex(std::vector<TrackId> const& trackIds, Index& index)
    {
      index.clear();
      index.reserve(trackIds.size());

      for (std::size_t position = 0; position < trackIds.size(); ++position)
      {
        if (!index.emplace(trackIds[position], position).second)
        {
          throwException<Exception>("Track source contains a duplicate identity");
        }
      }
    }

    std::vector<TrackId> snapshotSource(TrackSource const& source)
    {
      auto result = std::vector<TrackId>{};
      result.reserve(source.size());

      for (std::size_t index = 0; index < source.size(); ++index)
      {
        result.push_back(source.trackIdAt(index));
      }

      return result;
    }

    std::size_t filteredPosition(std::vector<TrackId> const& upstreamTrackIds,
                                 std::size_t const upstreamPosition,
                                 auto const& memberIndex)
    {
      std::size_t result = 0;

      for (std::size_t index = 0; index < upstreamPosition; ++index)
      {
        if (memberIndex.contains(upstreamTrackIds[index]))
        {
          ++result;
        }
      }

      return result;
    }

    void appendInsert(boost::container::small_vector<TrackSourceDelta, 1>& deltas,
                      std::size_t const index,
                      TrackId const trackId)
    {
      if (!deltas.empty())
      {
        if (auto* const range = std::get_if<SourceInsertRange>(&deltas.back());
            range != nullptr && range->start + range->trackIds.size() == index)
        {
          range->trackIds.push_back(trackId);
          return;
        }
      }

      deltas.push_back(SourceInsertRange{.start = index, .trackIds = {trackId}});
    }

    void appendRemove(boost::container::small_vector<TrackSourceDelta, 1>& deltas,
                      std::size_t const index,
                      TrackId const trackId)
    {
      if (!deltas.empty())
      {
        if (auto* const range = std::get_if<SourceRemoveRange>(&deltas.back());
            range != nullptr && range->start == index)
        {
          range->trackIds.push_back(trackId);
          return;
        }
      }

      deltas.push_back(SourceRemoveRange{.start = index, .trackIds = {trackId}});
    }

    void appendUpdate(boost::container::small_vector<TrackSourceDelta, 1>& deltas,
                      std::size_t const index,
                      TrackId const trackId)
    {
      if (!deltas.empty())
      {
        if (auto* const range = std::get_if<SourceUpdateRange>(&deltas.back());
            range != nullptr && range->start + range->trackIds.size() == index)
        {
          range->trackIds.push_back(trackId);
          return;
        }
      }

      deltas.push_back(SourceUpdateRange{.start = index, .trackIds = {trackId}});
    }

    std::vector<TrackId> replayDerivedBatch(std::vector<TrackId> trackIds,
                                            boost::container::small_vector<TrackSourceDelta, 1> const& deltas)
    {
      for (auto const& delta : deltas)
      {
        std::visit(
          [&trackIds](auto const& value)
          {
            using Value = std::remove_cvref_t<decltype(value)>;

            if constexpr (std::same_as<Value, SourceInsertRange>)
            {
              if (value.start > trackIds.size())
              {
                throwException<Exception>("Derived smart-list insertion is out of range");
              }

              trackIds.insert(trackIds.begin() + static_cast<std::ptrdiff_t>(value.start),
                              value.trackIds.begin(),
                              value.trackIds.end());
            }
            else if constexpr (std::same_as<Value, SourceRemoveRange>)
            {
              if (value.start > trackIds.size() || value.trackIds.size() > trackIds.size() - value.start ||
                  !std::ranges::equal(value.trackIds, std::span{trackIds}.subspan(value.start, value.trackIds.size())))
              {
                throwException<Exception>("Derived smart-list removal identity does not match working state");
              }

              trackIds.erase(trackIds.begin() + static_cast<std::ptrdiff_t>(value.start),
                             trackIds.begin() + static_cast<std::ptrdiff_t>(value.start + value.trackIds.size()));
            }
            else if constexpr (std::same_as<Value, SourceUpdateRange>)
            {
              if (value.start > trackIds.size() || value.trackIds.size() > trackIds.size() - value.start ||
                  !std::ranges::equal(value.trackIds, std::span{trackIds}.subspan(value.start, value.trackIds.size())))
              {
                throwException<Exception>("Derived smart-list update identity does not match working state");
              }
            }
            else
            {
              throwException<Exception>("Unexpected terminal delta in a regular smart-list batch");
            }
          },
          delta);
      }

      return trackIds;
    }

    void retainOnlyFinalUpdates(boost::container::small_vector<TrackSourceDelta, 1>& deltas,
                                std::vector<TrackId> const& finalMembers,
                                std::vector<TrackId> const& updatedTrackIds)
    {
      deltas.clear();

      for (std::size_t index = 0; index < finalMembers.size(); ++index)
      {
        if (std::ranges::contains(updatedTrackIds, finalMembers[index]))
        {
          appendUpdate(deltas, index, finalMembers[index]);
        }
      }
    }
  } // namespace

  SmartListEvaluator::SmartListEvaluator(library::MusicLibrary& ml)
    : _ml{ml}
  {
  }

  SmartListEvaluator::~SmartListEvaluator()
  {
    _alive = false;

    for (auto& [source, bucketPtr] : _buckets)
    {
      std::ignore = source;
      bucketPtr->subscription.reset();

      for (auto* const list : bucketPtr->lists)
      {
        list->_evaluator = nullptr;
      }
    }
  }

  void SmartListEvaluator::registerList(SmartListSource& list)
  {
    auto& source = list.source();
    auto [it, inserted] = _buckets.try_emplace(&source);

    if (inserted)
    {
      it->second = std::make_unique<SourceBucket>();
      it->second->source = &source;

      if (source.state() == TrackSourceState::Live)
      {
        it->second->upstreamTrackIds = snapshotSource(source);
        rebuildIndex(it->second->upstreamTrackIds, it->second->upstreamIndex);
      }
      else
      {
        it->second->invalidated = true;
      }
    }

    auto& bucket = *it->second;
    bucket.lists.push_back(&list);

    if (inserted && !bucket.invalidated)
    {
      bucket.subscription = source.subscribe([this, source = &source](TrackSourceDeltaBatch const& batch)
                                             { handleSourceBatch(*source, batch); });
    }

    if (bucket.invalidated)
    {
      list.invalidate();
    }
  }

  void SmartListEvaluator::unregisterList(SmartListSource& list)
  {
    auto* const source = &list.source();
    auto const it = _buckets.find(source);

    if (it == _buckets.end())
    {
      return;
    }

    std::erase(it->second->lists, &list);

    if (it->second->lists.empty())
    {
      it->second->subscription.reset();
      _buckets.erase(it);
    }
  }

  void SmartListEvaluator::rebuild(SmartListSource& list)
  {
    auto const it = _buckets.find(&list.source());

    if (it == _buckets.end() || it->second->invalidated)
    {
      return;
    }

    if (!list._dirty)
    {
      list.stageExpression(list._current.expression);
    }

    evaluateDirtyLists(*it->second);
  }

  void SmartListEvaluator::notifyUpdated(SmartListSource& list, TrackId const trackId)
  {
    auto const it = _buckets.find(&list.source());

    if (it == _buckets.end() || it->second->invalidated)
    {
      return;
    }

    auto const indexIt = it->second->upstreamIndex.find(trackId);

    if (indexIt == it->second->upstreamIndex.end())
    {
      return;
    }

    handleRegularBatch(*it->second,
                       TrackSourceDeltaBatch{
                         .deltas = {SourceUpdateRange{.start = indexIt->second, .trackIds = {trackId}}},
                       });
  }

  void SmartListEvaluator::handleSourceBatch(TrackSource& source, TrackSourceDeltaBatch const& batch)
  {
    auto const it = _buckets.find(&source);

    if (it == _buckets.end() || it->second->invalidated)
    {
      return;
    }

    if (batch.deltas.size() == 1 && std::holds_alternative<SourceInvalidated>(batch.deltas.front()))
    {
      handleSourceInvalidated(*it->second);
      return;
    }

    if (batch.deltas.size() == 1 && std::holds_alternative<SourceReset>(batch.deltas.front()))
    {
      handleSourceReset(*it->second);
      return;
    }

    handleRegularBatch(*it->second, batch);
  }

  void SmartListEvaluator::handleSourceReset(SourceBucket& bucket)
  {
    bucket.upstreamTrackIds = snapshotSource(*bucket.source);
    rebuildIndex(bucket.upstreamTrackIds, bucket.upstreamIndex);
    rebuildLists(bucket, bucket.lists);
  }

  // One transaction-local reducer keeps every derived list aligned to the same upstream batch.
  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  void SmartListEvaluator::handleRegularBatch(SourceBucket& bucket,
                                              TrackSourceDeltaBatch const& batch,
                                              bool const verifyFinalSnapshot)
  {
    auto const timer = rt::ScopedTimer{"SmartListEvaluator::handleRegularBatch"};
    auto upstreamTrackIds = bucket.upstreamTrackIds;
    auto upstreamIndex = bucket.upstreamIndex;
    auto works = std::vector<DerivedWork>{};
    works.reserve(bucket.lists.size());

    auto evaluatableLists = std::vector<SmartListSource*>{};
    evaluatableLists.reserve(bucket.lists.size());

    for (auto* const list : bucket.lists)
    {
      auto work = DerivedWork{
        .list = list,
        .oldMembers = list->_members,
        .members = list->_members,
        .memberIndex = list->_memberIndex,
        .active = list->state() == TrackSourceState::Live && !list->_dirty && !list->_current.optError &&
                  list->_current.planPtr != nullptr,
      };

      if (work.active)
      {
        evaluatableLists.push_back(list);
      }

      works.push_back(std::move(work));
    }

    auto const transaction = _ml.readTransaction();
    auto const reader = _ml.tracks().reader(transaction);
    auto const mode = unionMode(std::span<SmartListSource* const>{evaluatableLists});
    auto const storeMode = static_cast<library::TrackStore::Reader::LoadMode>(mode);
    auto updatedTrackIds = std::vector<TrackId>{};

    auto evaluateMatches = [&](TrackId const trackId)
    {
      auto result = std::vector<bool>(works.size(), false);
      auto const optView =
        storageValueOrNullopt(reader.get(trackId, storeMode), "Failed to evaluate smart-list track mutation");

      if (!optView)
      {
        return result;
      }

      for (std::size_t index = 0; index < works.size(); ++index)
      {
        if (auto const& work = works[index]; work.active)
        {
          result[index] = work.list->_planEvaluator.matches(*work.list->_current.planPtr, *optView);
        }
      }

      return result;
    };

    for (auto const& delta : batch.deltas)
    {
      std::visit(
        // Keeping variant dispatch together preserves atomic cross-list replay invariants.
        // NOLINTNEXTLINE(readability-function-cognitive-complexity)
        [&](auto const& value)
        {
          using Value = std::remove_cvref_t<decltype(value)>;

          if constexpr (std::same_as<Value, SourceInsertRange>)
          {
            if (value.start > upstreamTrackIds.size())
            {
              throwException<Exception>("Smart-list upstream insertion is out of range");
            }

            for (std::size_t offset = 0; offset < value.trackIds.size(); ++offset)
            {
              auto const trackId = value.trackIds[offset];
              auto const sourceIndex = value.start + offset;

              if (upstreamIndex.contains(trackId))
              {
                throwException<Exception>("Smart-list upstream insertion duplicates a track identity");
              }

              upstreamTrackIds.insert(upstreamTrackIds.begin() + static_cast<std::ptrdiff_t>(sourceIndex), trackId);
              rebuildIndex(upstreamTrackIds, upstreamIndex);
              auto const matches = evaluateMatches(trackId);

              for (std::size_t workIndex = 0; workIndex < works.size(); ++workIndex)
              {
                auto& work = works[workIndex];

                if (!work.active || !matches[workIndex])
                {
                  continue;
                }

                auto const memberPosition = filteredPosition(upstreamTrackIds, sourceIndex, work.memberIndex);
                work.members.insert(work.members.begin() + static_cast<std::ptrdiff_t>(memberPosition), trackId);
                rebuildIndex(work.members, work.memberIndex);
                appendInsert(work.deltas, memberPosition, trackId);
              }
            }
          }
          else if constexpr (std::same_as<Value, SourceRemoveRange>)
          {
            for (auto const trackId : value.trackIds)
            {
              if (value.start >= upstreamTrackIds.size() || upstreamTrackIds[value.start] != trackId)
              {
                throwException<Exception>("Smart-list upstream removal identity does not match working state");
              }

              for (auto& work : works)
              {
                if (!work.active)
                {
                  continue;
                }

                auto const memberIt = work.memberIndex.find(trackId);

                if (memberIt == work.memberIndex.end())
                {
                  continue;
                }

                auto const memberPosition = memberIt->second;
                work.members.erase(work.members.begin() + static_cast<std::ptrdiff_t>(memberPosition));
                rebuildIndex(work.members, work.memberIndex);
                appendRemove(work.deltas, memberPosition, trackId);
              }

              upstreamTrackIds.erase(upstreamTrackIds.begin() + static_cast<std::ptrdiff_t>(value.start));
              rebuildIndex(upstreamTrackIds, upstreamIndex);
            }
          }
          else if constexpr (std::same_as<Value, SourceUpdateRange>)
          {
            if (value.start > upstreamTrackIds.size() || value.trackIds.size() > upstreamTrackIds.size() - value.start)
            {
              throwException<Exception>("Smart-list upstream update is out of range");
            }

            for (std::size_t offset = 0; offset < value.trackIds.size(); ++offset)
            {
              auto const trackId = value.trackIds[offset];
              auto const sourceIndex = value.start + offset;

              if (upstreamTrackIds[sourceIndex] != trackId)
              {
                throwException<Exception>("Smart-list upstream update identity does not match working state");
              }

              updatedTrackIds.push_back(trackId);
              auto const matches = evaluateMatches(trackId);

              for (std::size_t workIndex = 0; workIndex < works.size(); ++workIndex)
              {
                auto& work = works[workIndex];

                if (!work.active)
                {
                  continue;
                }

                auto const memberIt = work.memberIndex.find(trackId);
                auto const wasPresent = memberIt != work.memberIndex.end();

                if (auto const nowMatches = matches[workIndex]; wasPresent && nowMatches)
                {
                  appendUpdate(work.deltas, memberIt->second, trackId);
                }
                else if (wasPresent)
                {
                  auto const memberPosition = memberIt->second;
                  work.members.erase(work.members.begin() + static_cast<std::ptrdiff_t>(memberPosition));
                  rebuildIndex(work.members, work.memberIndex);
                  appendRemove(work.deltas, memberPosition, trackId);
                }
                else if (nowMatches)
                {
                  auto const memberPosition = filteredPosition(upstreamTrackIds, sourceIndex, work.memberIndex);
                  work.members.insert(work.members.begin() + static_cast<std::ptrdiff_t>(memberPosition), trackId);
                  rebuildIndex(work.members, work.memberIndex);
                  appendInsert(work.deltas, memberPosition, trackId);
                }
              }
            }
          }
          else
          {
            throwException<Exception>("Reset or invalidation must be a singleton smart-list upstream batch");
          }
        },
        delta);
    }

    if (verifyFinalSnapshot && upstreamTrackIds != snapshotSource(*bucket.source))
    {
      throwException<Exception>("Smart-list upstream working mirror diverged from the final source snapshot");
    }

    for (auto& work : works)
    {
      if (!work.active)
      {
        continue;
      }

      if (work.oldMembers == work.members)
      {
        retainOnlyFinalUpdates(work.deltas, work.members, updatedTrackIds);
      }

      if (!work.deltas.empty() && replayDerivedBatch(work.oldMembers, work.deltas) != work.members)
      {
        throwException<Exception>("Smart-list derived batch does not reproduce final membership");
      }
    }

    bucket.upstreamTrackIds = std::move(upstreamTrackIds);
    bucket.upstreamIndex = std::move(upstreamIndex);

    for (auto& work : works)
    {
      if (work.active)
      {
        work.list->replaceMembers(std::move(work.members));
      }
    }

    for (auto& work : works)
    {
      if (!work.active || work.deltas.empty())
      {
        continue;
      }

      std::ignore =
        work.list->publishDeltaBatch(TrackSourceDeltaBatch{.deltas = std::move(work.deltas)}, work.oldMembers.size());
    }
  }

  void SmartListEvaluator::handleSourceInvalidated(SourceBucket& bucket)
  {
    bucket.invalidated = true;

    for (auto* const list : bucket.lists)
    {
      if (list->state() == TrackSourceState::Invalidated)
      {
        continue;
      }

      list->invalidate();
    }
  }

  void SmartListEvaluator::evaluateDirtyLists(SourceBucket& bucket)
  {
    auto dirtyLists = std::vector<SmartListSource*>{};

    for (auto* const list : bucket.lists)
    {
      if (list->_dirty && list->state() == TrackSourceState::Live)
      {
        list->applyStagedState();
        dirtyLists.push_back(list);
      }
    }

    rebuildLists(bucket, dirtyLists);
  }

  void SmartListEvaluator::rebuildLists(SourceBucket& bucket, std::span<SmartListSource*> const lists)
  {
    if (lists.empty())
    {
      return;
    }

    auto const timer = rt::ScopedTimer{"SmartListEvaluator::rebuildLists"};
    auto evaluatableLists = std::vector<SmartListSource*>{};

    for (auto* const list : lists)
    {
      if (list->state() == TrackSourceState::Live && !list->_current.optError && list->_current.planPtr != nullptr)
      {
        evaluatableLists.push_back(list);
      }
    }

    auto nextMembers = std::vector<std::vector<TrackId>>(lists.size());

    if (!evaluatableLists.empty())
    {
      auto const transaction = _ml.readTransaction();
      auto const reader = _ml.tracks().reader(transaction);
      auto const mode = unionMode(std::span<SmartListSource* const>{evaluatableLists});
      auto const storeMode = static_cast<library::TrackStore::Reader::LoadMode>(mode);

      for (auto const trackId : bucket.upstreamTrackIds)
      {
        auto const optView =
          storageValueOrNullopt(reader.get(trackId, storeMode), "Failed to rebuild smart-list membership");

        if (!optView)
        {
          continue;
        }

        for (std::size_t index = 0; index < lists.size(); ++index)
        {
          if (auto* const list = lists[index]; list->state() == TrackSourceState::Live && !list->_current.optError &&
                                               list->_current.planPtr != nullptr &&
                                               list->_planEvaluator.matches(*list->_current.planPtr, *optView))
          {
            nextMembers[index].push_back(trackId);
          }
        }
      }
    }

    auto previousSizes = std::vector<std::size_t>{};
    previousSizes.reserve(lists.size());

    for (std::size_t index = 0; index < lists.size(); ++index)
    {
      previousSizes.push_back(lists[index]->_members.size());
      lists[index]->replaceMembers(std::move(nextMembers[index]));
    }

    for (std::size_t index = 0; index < lists.size(); ++index)
    {
      auto* const list = lists[index];

      if (list->state() != TrackSourceState::Live)
      {
        continue;
      }

      std::ignore = list->publishDeltaBatch(TrackSourceDeltaBatch{.deltas = {SourceReset{}}}, previousSizes[index]);
    }
  }

  SmartListEvaluator::TrackLoadMode SmartListEvaluator::unionMode(std::span<SmartListSource* const> const lists)
  {
    bool needsHot = false;
    bool needsCold = false;

    for (auto* const list : lists)
    {
      if (!list->_current.planPtr)
      {
        continue;
      }

      switch (list->_current.planPtr->accessProfile)
      {
        case query::AccessProfile::NoTrackData: break;
        case query::AccessProfile::HotOnly: needsHot = true; break;
        case query::AccessProfile::ColdOnly: needsCold = true; break;
        case query::AccessProfile::HotAndCold:
          needsHot = true;
          needsCold = true;
          break;
      }
    }

    if (needsHot && needsCold)
    {
      return TrackLoadMode::Both;
    }

    if (needsCold)
    {
      return TrackLoadMode::Cold;
    }

    return TrackLoadMode::Hot;
  }
} // namespace ao::rt
