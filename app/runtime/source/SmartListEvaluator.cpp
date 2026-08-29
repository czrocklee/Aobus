// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/source/SmartListEvaluator.h>

#include "runtime/RuntimeOperationProbe.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/query/ExecutionPlan.h>
#include <ao/query/Field.h>
#include <ao/query/PlanEvaluator.h>
#include <ao/query/detail/Bytecode.h>
#include <ao/rt/ScopedTimer.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/source/SmartListSource.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceDelta.h>

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt
{
  namespace
  {
    library::TrackStore::Reader::LoadMode loadModeForAccessProfile(query::AccessProfile const profile)
    {
      using LoadMode = library::TrackStore::Reader::LoadMode;

      switch (profile)
      {
        case query::AccessProfile::NoTrackData:
        case query::AccessProfile::HotOnly: return LoadMode::Hot;
        case query::AccessProfile::ColdOnly: return LoadMode::Cold;
        case query::AccessProfile::HotAndCold: return LoadMode::Both;
      }

      AO_FATAL("Unknown query access profile");
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

    struct RegularBatchTrackChanges final
    {
      std::vector<TrackId> updatedTrackIds;
      std::vector<TrackId> touchedTrackIds;
      std::vector<TrackId> preferredMovedIds;
    };

    RegularBatchTrackChanges summarizeTrackChanges(delta::RegularTrackEditScript const& script)
    {
      auto result = RegularBatchTrackChanges{};
      auto removedTrackIds = boost::unordered_flat_set<TrackId, std::hash<TrackId>>{};
      auto insertedTrackIds = std::vector<TrackId>{};

      for (auto const& edit : script.edits)
      {
        if (auto const* insertion = std::get_if<delta::InsertRange>(&edit); insertion != nullptr)
        {
          insertedTrackIds.append_range(insertion->trackIds);
          result.touchedTrackIds.append_range(insertion->trackIds);
        }
        else if (auto const* removal = std::get_if<delta::RemoveRange>(&edit); removal != nullptr)
        {
          removedTrackIds.insert(removal->trackIds.begin(), removal->trackIds.end());
        }
        else if (auto const* update = std::get_if<delta::UpdateRange>(&edit); update != nullptr)
        {
          result.updatedTrackIds.append_range(update->trackIds);
          result.touchedTrackIds.append_range(update->trackIds);
        }
      }

      std::ranges::sort(result.touchedTrackIds);
      result.touchedTrackIds.erase(std::ranges::unique(result.touchedTrackIds).begin(), result.touchedTrackIds.end());

      for (auto const trackId : insertedTrackIds)
      {
        if (removedTrackIds.contains(trackId))
        {
          result.preferredMovedIds.push_back(trackId);
        }
      }

      return result;
    }

    bool isUpdateOnlyBatch(delta::RegularTrackEditScript const& script)
    {
      return std::ranges::all_of(script.edits,
                                 [](delta::RegularTrackEdit const& edit)
                                 { return std::holds_alternative<delta::UpdateRange>(edit); });
    }
  } // namespace

  SmartListEvaluator::SmartListEvaluator(library::MusicLibrary const& ml)
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
        it->second->upstreamTracks.assign(snapshotSource(source));
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
      bucket.subscription = source.subscribe([this, source = &source](TrackSourceDelta const& batch)
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

    if (!list._optPending)
    {
      list.setExpression(list._current.expression);
    }

    evaluatePendingLists(*it->second);
  }

  void SmartListEvaluator::handleSourceBatch(TrackSource& source, TrackSourceDelta const& batch)
  {
    auto const it = _buckets.find(&source);

    if (it == _buckets.end() || it->second->invalidated)
    {
      return;
    }

    if (std::holds_alternative<SourceInvalidated>(batch))
    {
      handleSourceInvalidated(*it->second);
      return;
    }

    if (std::holds_alternative<SourceReset>(batch))
    {
      handleSourceReset(*it->second);
      return;
    }

    handleRegularBatch(*it->second, std::get<delta::RegularTrackEditScript>(batch));
  }

  void SmartListEvaluator::handleSourceReset(SourceBucket& bucket)
  {
    bucket.upstreamTracks.assign(snapshotSource(*bucket.source));
    ++_upstreamIndexRebuildCount;
    rebuildLists(bucket, bucket.lists);
  }

  bool SmartListEvaluator::isEvaluatable(SmartListSource const& list)
  {
    return list.state() == TrackSourceState::Live && !list._optPending && !list._current.optError &&
           list._current.planPtr != nullptr;
  }

  std::vector<SmartListEvaluator::DerivedWork> SmartListEvaluator::buildDerivedWorks(SourceBucket const& bucket) const
  {
    auto works = std::vector<DerivedWork>{};
    works.reserve(bucket.lists.size());

    for (auto* const list : bucket.lists)
    {
      auto work = DerivedWork{
        .list = list,
        .oldMembers = list->_members.vector(),
        .members = list->_members.vector(),
        .active = isEvaluatable(*list),
      };

      works.push_back(std::move(work));
    }

    return works;
  }

  SmartListEvaluator::TrackMatches SmartListEvaluator::evaluateTouchedTracks(
    std::span<SmartListSource* const> const lists,
    std::span<TrackId const> const touchedTrackIds) const
  {
    auto evaluatableLists = std::vector<SmartListSource*>{};
    evaluatableLists.reserve(lists.size());

    for (auto* const list : lists)
    {
      if (isEvaluatable(*list))
      {
        evaluatableLists.push_back(list);
      }
    }

    auto matchesByTrackId = TrackMatches{};
    matchesByTrackId.reserve(touchedTrackIds.size());

    if (touchedTrackIds.empty() || evaluatableLists.empty())
    {
      for (auto const trackId : touchedTrackIds)
      {
        matchesByTrackId.emplace(trackId, std::vector<bool>(lists.size(), false));
      }

      return matchesByTrackId;
    }

    auto const transaction = _ml.readTransaction();
    auto const reader = _ml.tracks().reader(transaction);
    auto const storeMode = loadModeForAccessProfile(unionAccessProfile(evaluatableLists));
    auto dictionaryCache = library::DictionaryReadCache{_ml.dictionary()};
    auto dictionaryContext = library::DictionaryReadContext{dictionaryCache};
    auto bindings = std::vector<std::optional<query::PlanBinding>>(lists.size());

    for (std::size_t index = 0; index < lists.size(); ++index)
    {
      if (auto* const list = lists[index]; isEvaluatable(*list))
      {
        bindings[index].emplace(*list->_current.planPtr, dictionaryContext);
      }
    }

    for (auto const trackId : touchedTrackIds)
    {
      auto matches = std::vector<bool>(lists.size(), false);

      if (auto const optView = reader.get(trackId, storeMode); optView)
      {
        for (std::size_t index = 0; index < lists.size(); ++index)
        {
          if (auto* const list = lists[index]; isEvaluatable(*list))
          {
            auto const& binding = bindings[index];
            AO_INVARIANT(binding);

            if (query::hasRequiredTrackData(list->_current.planPtr->accessProfile, *optView))
            {
              matches[index] = list->_planEvaluator.matches(*binding, *optView);
            }
          }
        }
      }

      matchesByTrackId.emplace(trackId, std::move(matches));
    }

    return matchesByTrackId;
  }

  delta::RegularTrackEditScript SmartListEvaluator::buildUpdateScript(SmartListSource const& list,
                                                                      std::size_t const listIndex,
                                                                      std::span<TrackId const> const updatedTrackIds,
                                                                      TrackMatches const& matchesByTrackId,
                                                                      IndexedTrackSequence const& upstreamTracks) const
  {
    struct IndexedTrack final
    {
      std::size_t index = 0;
      TrackId trackId = kInvalidTrackId;
    };

    auto removals = std::vector<IndexedTrack>{};
    auto insertions = std::vector<IndexedTrack>{};
    auto retainedUpdates = std::vector<IndexedTrack>{};
    removals.reserve(updatedTrackIds.size());
    insertions.reserve(updatedTrackIds.size());
    retainedUpdates.reserve(updatedTrackIds.size());

    for (auto const trackId : updatedTrackIds)
    {
      auto const matchesIt = matchesByTrackId.find(trackId);
      AO_INVARIANT(matchesIt != matchesByTrackId.end() && listIndex < matchesIt->second.size());
      auto const matches = matchesIt->second[listIndex];

      if (auto const optMemberIndex = list._members.indexOf(trackId); optMemberIndex && !matches)
      {
        removals.push_back(IndexedTrack{.index = *optMemberIndex, .trackId = trackId});
      }
      else if (!optMemberIndex && matches)
      {
        auto const optUpstreamIndex = upstreamTracks.indexOf(trackId);
        AO_INVARIANT(optUpstreamIndex);
        insertions.push_back(IndexedTrack{.index = *optUpstreamIndex, .trackId = trackId});
      }
      else if (optMemberIndex && matches)
      {
        retainedUpdates.push_back(IndexedTrack{.index = *optMemberIndex, .trackId = trackId});
      }
    }

    auto removalIndices = std::vector<std::size_t>{};
    removalIndices.reserve(removals.size());

    for (auto const& removal : removals)
    {
      removalIndices.push_back(removal.index);
    }

    std::ranges::sort(removalIndices);
    std::ranges::sort(removals, std::greater{}, &IndexedTrack::index);
    std::ranges::sort(insertions, {}, &IndexedTrack::index);

    auto const countBefore = [](auto const& sorted, std::size_t const value)
    { return static_cast<std::size_t>(std::ranges::lower_bound(sorted, value) - sorted.begin()); };
    auto const upstreamIndexOf = [&upstreamTracks](TrackId const trackId)
    {
      auto const optIndex = upstreamTracks.indexOf(trackId);
      AO_INVARIANT(optIndex);
      return *optIndex;
    };

    auto coalescer = delta::Coalescer{};

    for (auto const& removal : removals)
    {
      coalescer.appendRemove(removal.index, std::span{&removal.trackId, std::size_t{1}});
    }

    auto const oldMembers = list._members.ids();

    for (std::size_t insertionIndex = 0; insertionIndex < insertions.size(); ++insertionIndex)
    {
      auto const& insertion = insertions[insertionIndex];
      auto const oldPosition = static_cast<std::size_t>(
        std::ranges::lower_bound(oldMembers, insertion.index, {}, upstreamIndexOf) - oldMembers.begin());
      auto const position = oldPosition - countBefore(removalIndices, oldPosition) + insertionIndex;
      coalescer.appendInsert(position, std::span{&insertion.trackId, std::size_t{1}});
    }

    for (auto& update : retainedUpdates)
    {
      auto const upstreamIndex = upstreamIndexOf(update.trackId);
      auto const insertedBefore = static_cast<std::size_t>(
        std::ranges::lower_bound(insertions, upstreamIndex, {}, &IndexedTrack::index) - insertions.begin());
      update.index = update.index - countBefore(removalIndices, update.index) + insertedBefore;
    }

    std::ranges::sort(retainedUpdates, {}, &IndexedTrack::index);

    for (auto const& update : retainedUpdates)
    {
      coalescer.appendUpdate(update.index, std::span{&update.trackId, std::size_t{1}});
    }

    return coalescer.take();
  }

  void SmartListEvaluator::handleUpdateBatch(SourceBucket& bucket,
                                             delta::RegularTrackEditScript const& script,
                                             bool const verifyFinalSnapshot)
  {
    AO_INVARIANT(delta::validate(script, bucket.upstreamTracks.size()));
    AO_INVARIANT(!verifyFinalSnapshot || bucket.source->size() == bucket.upstreamTracks.size());

    for (auto const& edit : script.edits)
    {
      auto const& update = std::get<delta::UpdateRange>(edit);
      auto const mirrored = bucket.upstreamTracks.ids().subspan(update.start, update.trackIds.size());
      AO_INVARIANT(std::ranges::equal(mirrored, update.trackIds));

      if (verifyFinalSnapshot)
      {
        for (std::size_t offset = 0; offset < update.trackIds.size(); ++offset)
        {
          AO_INVARIANT(bucket.source->trackIdAt(update.start + offset) == update.trackIds[offset]);
        }
      }
    }

    bucket.upstreamTracks.applyScript(script);
    auto const changes = summarizeTrackChanges(script);
    auto const matchesByTrackId = evaluateTouchedTracks(bucket.lists, changes.updatedTrackIds);

    struct IncrementalWork final
    {
      SmartListSource* list = nullptr;
      delta::RegularTrackEditScript script{};
      std::size_t previousSize = 0;
    };

    auto works = std::vector<IncrementalWork>{};
    works.reserve(bucket.lists.size());

    for (std::size_t listIndex = 0; listIndex < bucket.lists.size(); ++listIndex)
    {
      auto* const list = bucket.lists[listIndex];

      if (!isEvaluatable(*list))
      {
        continue;
      }

      auto derivedScript =
        buildUpdateScript(*list, listIndex, changes.updatedTrackIds, matchesByTrackId, bucket.upstreamTracks);

      if (!derivedScript.edits.empty())
      {
        works.push_back(
          IncrementalWork{.list = list, .script = std::move(derivedScript), .previousSize = list->_members.size()});
      }
    }

    for (auto& work : works)
    {
      work.list->_members.applyScript(work.script);
    }

    for (auto& work : works)
    {
      std::ignore = work.list->publishDelta(std::move(work.script), work.previousSize);
    }
  }

  void SmartListEvaluator::updateDerivedWorks(std::span<DerivedWork> const works,
                                              IndexedTrackSequence const& upstreamTracks,
                                              TrackMatches const& matchesByTrackId,
                                              std::span<TrackId const> const updatedTrackIds,
                                              std::span<TrackId const> const preferredMovedIds)
  {
    for (std::size_t workIndex = 0; workIndex < works.size(); ++workIndex)
    {
      auto& work = works[workIndex];

      if (!work.active)
      {
        continue;
      }

      work.members.clear();
      work.members.reserve(upstreamTracks.size());

      for (auto const trackId : upstreamTracks.ids())
      {
        auto const touched = matchesByTrackId.find(trackId);
        auto const include =
          touched == matchesByTrackId.end() ? work.list->_members.contains(trackId) : touched->second[workIndex];

        if (include)
        {
          work.members.push_back(trackId);
        }
      }

      auto const script = delta::diff(work.oldMembers, work.members, updatedTrackIds, preferredMovedIds);
      AO_INVARIANT((
        [&work, &script]
        {
          auto const applied = delta::apply(work.oldMembers, script);
          return applied && *applied == work.members;
        }()));
      work.script = std::move(script);
    }
  }

  void SmartListEvaluator::commitDerivedWorks(SourceBucket& bucket,
                                              IndexedTrackSequence upstreamTracks,
                                              std::vector<DerivedWork>& works)
  {
    bucket.upstreamTracks = std::move(upstreamTracks);

    for (auto& work : works)
    {
      if (work.active)
      {
        work.list->replaceMembers(std::move(work.members));
        ++_membershipIndexRebuildCount;
      }
    }

    for (auto& work : works)
    {
      if (!work.active || work.script.edits.empty())
      {
        continue;
      }

      std::ignore = work.list->publishDelta(std::move(work.script), work.oldMembers.size());
    }
  }

  void SmartListEvaluator::handleRegularBatch(SourceBucket& bucket,
                                              delta::RegularTrackEditScript const& script,
                                              bool const verifyFinalSnapshot)
  {
    auto const timer = rt::ScopedTimer{"SmartListEvaluator::handleRegularBatch"};

    if (isUpdateOnlyBatch(script))
    {
      handleUpdateBatch(bucket, script, verifyFinalSnapshot);
      return;
    }

    auto upstreamTracks = bucket.upstreamTracks;
    upstreamTracks.applyScript(script);
    ++_upstreamIndexRebuildCount;
    AO_INVARIANT(!verifyFinalSnapshot || upstreamTracks.vector() == snapshotSource(*bucket.source));

    auto works = buildDerivedWorks(bucket);
    auto changes = summarizeTrackChanges(script);
    auto const matchesByTrackId = evaluateTouchedTracks(bucket.lists, changes.touchedTrackIds);
    updateDerivedWorks(works, upstreamTracks, matchesByTrackId, changes.updatedTrackIds, changes.preferredMovedIds);
    commitDerivedWorks(bucket, std::move(upstreamTracks), works);
  }

  void SmartListEvaluator::handleSourceInvalidated(SourceBucket& bucket)
  {
    bucket.invalidated = true;
    bucket.subscription.reset();
    bucket.upstreamTracks.clear();

    for (auto* const list : bucket.lists)
    {
      if (list->state() == TrackSourceState::Invalidated)
      {
        continue;
      }

      list->invalidate();
    }
  }

  void SmartListEvaluator::evaluatePendingLists(SourceBucket& bucket)
  {
    auto pendingLists = std::vector<SmartListSource*>{};

    for (auto* const list : bucket.lists)
    {
      if (list->_optPending && list->state() == TrackSourceState::Live)
      {
        list->applyPendingState();
        pendingLists.push_back(list);
      }
    }

    rebuildLists(bucket, pendingLists);
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
      auto const storeMode =
        loadModeForAccessProfile(unionAccessProfile(std::span<SmartListSource* const>{evaluatableLists}));
      auto dictionaryCache = library::DictionaryReadCache{_ml.dictionary()};
      auto dictionaryContext = library::DictionaryReadContext{dictionaryCache};
      auto bindings = std::vector<std::optional<query::PlanBinding>>(lists.size());

      for (std::size_t index = 0; index < lists.size(); ++index)
      {
        if (auto* const list = lists[index];
            list->state() == TrackSourceState::Live && !list->_current.optError && list->_current.planPtr != nullptr)
        {
          bindings[index].emplace(*list->_current.planPtr, dictionaryContext);
        }
      }

      reader.visitTracks(bucket.upstreamTracks.ids(),
                         storeMode,
                         [&](TrackId trackId, library::TrackView const& view)
                         {
                           for (std::size_t index = 0; index < lists.size(); ++index)
                           {
                             if (auto* const list = lists[index];
                                 list->state() == TrackSourceState::Live && !list->_current.optError &&
                                 list->_current.planPtr != nullptr &&
                                 query::hasRequiredTrackData(list->_current.planPtr->accessProfile, view) &&
                                 list->_planEvaluator.matches(*bindings[index], view))
                             {
                               nextMembers[index].push_back(trackId);
                             }
                           }
                         });
    }

    auto previousSizes = std::vector<std::size_t>{};
    previousSizes.reserve(lists.size());

    for (std::size_t index = 0; index < lists.size(); ++index)
    {
      previousSizes.push_back(lists[index]->_members.size());
      lists[index]->replaceMembers(std::move(nextMembers[index]));
      ++_membershipIndexRebuildCount;
    }

    for (std::size_t index = 0; index < lists.size(); ++index)
    {
      auto* const list = lists[index];

      if (list->state() != TrackSourceState::Live)
      {
        continue;
      }

      std::ignore = list->publishDelta(SourceReset{}, previousSizes[index]);
    }
  }

  query::AccessProfile SmartListEvaluator::unionAccessProfile(std::span<SmartListSource* const> const lists)
  {
    bool needsHot = false;
    bool needsCold = false;

    for (auto* const list : lists)
    {
      if (!list->_current.planPtr)
      {
        continue;
      }

      auto const profile = list->_current.planPtr->accessProfile;
      needsHot = needsHot || query::isHotDataRequired(profile);
      needsCold = needsCold || query::isColdDataRequired(profile);
    }

    if (needsHot && needsCold)
    {
      return query::AccessProfile::HotAndCold;
    }

    if (needsCold)
    {
      return query::AccessProfile::ColdOnly;
    }

    if (needsHot)
    {
      return query::AccessProfile::HotOnly;
    }

    return query::AccessProfile::NoTrackData;
  }

  namespace detail
  {
    SmartListEvaluatorOperationCounts RuntimeOperationProbe::counts(SmartListEvaluator const& evaluator) noexcept
    {
      return {.upstreamIndexRebuilds = evaluator._upstreamIndexRebuildCount,
              .membershipIndexRebuilds = evaluator._membershipIndexRebuildCount};
    }
  } // namespace detail
} // namespace ao::rt
