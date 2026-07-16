// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/query/ExecutionPlan.h>
#include <ao/query/Field.h>
#include <ao/query/PlanEvaluator.h>
#include <ao/query/detail/Bytecode.h>
#include <ao/rt/ScopedTimer.h>
#include <ao/rt/StorageResult.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/source/SmartListEvaluator.h>
#include <ao/rt/source/SmartListSource.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceDelta.h>
#include <ao/rt/source/TrackSourceEditScript.h>

#include <boost/container/small_vector.hpp>
#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#include <gsl-lite/gsl-lite.hpp>

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

    auto const optIndex = it->second->upstreamTracks.indexOf(trackId);

    if (!optIndex)
    {
      return;
    }

    handleRegularBatch(*it->second,
                       TrackSourceDeltaBatch{
                         .deltas = {SourceUpdateRange{.start = *optIndex, .trackIds = {trackId}}},
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
    bucket.upstreamTracks.assign(snapshotSource(*bucket.source));
    ++_operationCounts.upstreamIndexRebuilds;
    rebuildLists(bucket, bucket.lists);
  }

  // One transaction-local reducer keeps every derived list aligned to the same upstream batch.
  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  void SmartListEvaluator::handleRegularBatch(SourceBucket& bucket,
                                              TrackSourceDeltaBatch const& batch,
                                              bool const verifyFinalSnapshot)
  {
    auto const timer = rt::ScopedTimer{"SmartListEvaluator::handleRegularBatch"};
    auto scriptResult = regularTrackEditScriptOf(batch);
    gsl_Assert(scriptResult);
    auto upstreamTracks = bucket.upstreamTracks;
    upstreamTracks.applyScript(*scriptResult);
    ++_operationCounts.upstreamIndexRebuilds;
    gsl_Assert(!verifyFinalSnapshot || upstreamTracks.vector() == snapshotSource(*bucket.source));

    auto works = std::vector<DerivedWork>{};
    works.reserve(bucket.lists.size());
    auto evaluatableLists = std::vector<SmartListSource*>{};
    evaluatableLists.reserve(bucket.lists.size());

    for (auto* const list : bucket.lists)
    {
      auto work = DerivedWork{
        .list = list,
        .oldMembers = list->_members.vector(),
        .members = list->_members.vector(),
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
    auto dictionaryCache = library::DictionaryReadCache{_ml.dictionary()};
    auto dictionaryContext = library::DictionaryReadContext{dictionaryCache};
    auto bindings = std::vector<std::optional<query::PlanBinding>>(works.size());

    for (std::size_t index = 0; index < works.size(); ++index)
    {
      if (auto const& work = works[index]; work.active)
      {
        bindings[index].emplace(*work.list->_current.planPtr, dictionaryContext);
      }
    }

    auto updatedTrackIds = std::vector<TrackId>{};
    auto removedTrackIds = boost::unordered_flat_set<TrackId, std::hash<TrackId>>{};
    auto insertedTrackIds = std::vector<TrackId>{};
    auto touchedTrackIds = std::vector<TrackId>{};

    for (auto const& edit : scriptResult->edits)
    {
      if (auto const* insertion = std::get_if<delta::InsertRange>(&edit); insertion != nullptr)
      {
        insertedTrackIds.append_range(insertion->trackIds);
        touchedTrackIds.append_range(insertion->trackIds);
      }
      else if (auto const* removal = std::get_if<delta::RemoveRange>(&edit); removal != nullptr)
      {
        removedTrackIds.insert(removal->trackIds.begin(), removal->trackIds.end());
      }
      else if (auto const* update = std::get_if<delta::UpdateRange>(&edit); update != nullptr)
      {
        updatedTrackIds.append_range(update->trackIds);
        touchedTrackIds.append_range(update->trackIds);
      }
    }

    std::ranges::sort(touchedTrackIds);
    touchedTrackIds.erase(std::ranges::unique(touchedTrackIds).begin(), touchedTrackIds.end());

    auto matchesByTrackId = boost::unordered_flat_map<TrackId, std::vector<bool>, std::hash<TrackId>>{};
    matchesByTrackId.reserve(touchedTrackIds.size());

    for (auto const trackId : touchedTrackIds)
    {
      auto matches = std::vector<bool>(works.size(), false);
      auto const optView =
        storageValueOrNullopt(reader.get(trackId, storeMode), "Failed to evaluate smart-list track mutation");

      if (optView)
      {
        for (std::size_t index = 0; index < works.size(); ++index)
        {
          if (auto const& work = works[index]; work.active)
          {
            auto const& binding = bindings[index];
            gsl_Expects(binding);
            matches[index] = work.list->_planEvaluator.matches(*binding, *optView);
          }
        }
      }

      matchesByTrackId.emplace(trackId, std::move(matches));
    }

    auto preferredMovedIds = std::vector<TrackId>{};

    for (auto const trackId : insertedTrackIds)
    {
      if (removedTrackIds.contains(trackId))
      {
        preferredMovedIds.push_back(trackId);
      }
    }

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
      gsl_Assert((
        [&work, &script]
        {
          auto const applied = delta::apply(work.oldMembers, script);
          return applied && *applied == work.members;
        }()));
      work.deltas = std::move(sourceBatchOf(script).deltas);
    }

    bucket.upstreamTracks = std::move(upstreamTracks);

    for (auto& work : works)
    {
      if (work.active)
      {
        work.list->replaceMembers(std::move(work.members));
        ++_operationCounts.membershipIndexRebuilds;
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

      auto visitTrack = [&](TrackId trackId, library::TrackView const& view)
      {
        for (std::size_t index = 0; index < lists.size(); ++index)
        {
          if (auto* const list = lists[index]; list->state() == TrackSourceState::Live && !list->_current.optError &&
                                               list->_current.planPtr != nullptr &&
                                               list->_planEvaluator.matches(*bindings[index], view))
          {
            nextMembers[index].push_back(trackId);
          }
        }
      };
      reader.visitTracks(bucket.upstreamTracks.ids(), storeMode, visitTrack);
    }

    auto previousSizes = std::vector<std::size_t>{};
    previousSizes.reserve(lists.size());

    for (std::size_t index = 0; index < lists.size(); ++index)
    {
      previousSizes.push_back(lists[index]->_members.size());
      lists[index]->replaceMembers(std::move(nextMembers[index]));
      ++_operationCounts.membershipIndexRebuilds;
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
