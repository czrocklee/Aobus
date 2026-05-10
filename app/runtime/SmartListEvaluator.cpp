// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "SmartListEvaluator.h"

#include "SmartListSource.h"
#include "TrackSource.h"

#include <ao/lmdb/Transaction.h>
#include <ao/query/ExecutionPlan.h>
#include <ao/query/Parser.h>
#include <ao/utility/Log.h>

#include <algorithm>
#include <flat_set>
#include <stdexcept>
#include <utility>

namespace ao::app
{
  // SourceObserver implementation

  SourceObserver::SourceObserver(SmartListEvaluator& evaluator, TrackSource& source)
    : _evaluator{evaluator}, _source{source}
  {
  }

  void SourceObserver::onReset()
  {
    if (!_valid)
    {
      return;
    }

    if (auto it = _evaluator._buckets.find(&_source); it != _evaluator._buckets.end())
    {
      _evaluator.handleSourceReset(*it->second);
    }
  }

  void SourceObserver::onInserted(TrackId id, std::size_t index)
  {
    if (!_valid)
    {
      return;
    }

    if (auto it = _evaluator._buckets.find(&_source); it != _evaluator._buckets.end())
    {
      _evaluator.handleSourceInserted(*it->second, id, index);
    }
  }

  void SourceObserver::onUpdated(TrackId id, std::size_t index)
  {
    if (!_valid)
    {
      return;
    }

    if (auto it = _evaluator._buckets.find(&_source); it != _evaluator._buckets.end())
    {
      _evaluator.handleSourceUpdated(*it->second, id, index);
    }
  }

  void SourceObserver::onRemoved(TrackId id, std::size_t /*index*/)
  {
    if (!_valid)
    {
      return;
    }

    if (auto it = _evaluator._buckets.find(&_source); it != _evaluator._buckets.end())
    {
      _evaluator.handleSourceRemoved(*it->second, id);
    }
  }

  void SourceObserver::onInserted(std::span<TrackId const> ids)
  {
    if (!_valid)
    {
      return;
    }

    if (auto it = _evaluator._buckets.find(&_source); it != _evaluator._buckets.end())
    {
      _evaluator.handleSourceInserted(*it->second, ids);
    }
  }

  void SourceObserver::onUpdated(std::span<TrackId const> ids)
  {
    if (!_valid)
    {
      return;
    }

    if (auto it = _evaluator._buckets.find(&_source); it != _evaluator._buckets.end())
    {
      _evaluator.handleSourceUpdated(*it->second, ids);
    }
  }

  void SourceObserver::onRemoved(std::span<TrackId const> ids)
  {
    if (!_valid)
    {
      return;
    }

    if (auto it = _evaluator._buckets.find(&_source); it != _evaluator._buckets.end())
    {
      _evaluator.handleSourceRemoved(*it->second, ids);
    }
  }

  void SourceObserver::onSourceDestroyed()
  {
    if (!_valid)
    {
      return;
    }

    if (auto it = _evaluator._buckets.find(&_source); it != _evaluator._buckets.end())
    {
      _evaluator.handleSourceDestroyed(*it->second);
    }
  }

  // SmartListEvaluator implementation

  SmartListEvaluator::SmartListEvaluator(ao::library::MusicLibrary& ml)
    : _ml{ml}
  {
  }

  SmartListEvaluator::~SmartListEvaluator()
  {
    _alive = false;

    for (auto& [source, bucket] : _buckets)
    {
      if (bucket->observer)
      {
        ao::utility::unsafeDowncast<SourceObserver>(bucket->observer.get())->invalidate();

        if (bucket->sourceAlive)
        {
          source->detach(bucket->observer.get());
        }
      }
    }
  }

  void SmartListEvaluator::registerList(TrackSource& source, SmartListSource& list)
  {
    auto& bucket = _buckets[&source];

    if (!bucket)
    {
      bucket = std::make_unique<SourceBucket>();
      bucket->source = &source;
      bucket->observer = std::make_unique<SourceObserver>(*this, source);
      source.attach(bucket->observer.get());
    }

    bucket->lists.push_back(&list);
  }

  void SmartListEvaluator::unregisterList(TrackSource& source, SmartListSource& list)
  {
    auto it = _buckets.find(&source);

    if (it == _buckets.end())
    {
      return;
    }

    auto& bucket = *it->second;
    std::erase(bucket.lists, &list);

    if (bucket.lists.empty())
    {
      if (bucket.sourceAlive)
      {
        source.detach(bucket.observer.get());
      }

      _buckets.erase(it);
    }
  }

  void SmartListEvaluator::rebuild(SmartListSource& list)
  {
    auto const it = _buckets.find(&list._source);

    if (it == _buckets.end())
    {
      return;
    }

    if (!list._dirty)
    {
      list.stageExpression(list._expression);
    }

    rebuildDirtyLists(*it->second);
  }

  void SmartListEvaluator::notifyUpdated(TrackSource& source, TrackId trackId)
  {
    auto const it = _buckets.find(&source);

    if (it == _buckets.end())
    {
      return;
    }

    // Re-evaluate membership for all lists in this bucket

    if (auto const optSourceIndex = source.indexOf(trackId))
    {
      handleSourceUpdated(*it->second, trackId, *optSourceIndex);
    }
  }

  void SmartListEvaluator::rebuildActiveLists(SourceBucket& bucket)
  {
    if (bucket.lists.empty())
    {
      return;
    }

    rebuildLists(bucket.lists);
  }

  void SmartListEvaluator::rebuildDirtyLists(SourceBucket& bucket)
  {
    auto dirtyLists = std::vector<SmartListSource*>{};
    for (auto* list : bucket.lists)
    {
      if (list->_dirty)
      {
        list->applyStagedState();
        dirtyLists.push_back(list);
      }
    }

    if (dirtyLists.empty())
    {
      return;
    }

    rebuildLists(dirtyLists);
  }

  void SmartListEvaluator::rebuildLists(std::span<SmartListSource*> lists)
  {
    if (lists.empty())
    {
      return;
    }

    auto evaluatableLists = std::vector<SmartListSource*>{};
    for (auto* list : lists)
    {
      if (list->_hasError || !list->_plan)
      {
        list->_members.clear();
      }
      else
      {
        evaluatableLists.push_back(list);
      }
    }

    if (!evaluatableLists.empty())
    {
      auto const mode = getUnionMode(evaluatableLists);
      rebuildGroup(lists.front()->_source, evaluatableLists, mode);
    }

    // Notify Reset for only the provided lists
    for (auto* list : lists)
    {
      list->TrackSource::notifyReset();
    }
  }

  void SmartListEvaluator::rebuildGroup(TrackSource& source,
                                        std::span<SmartListSource*> lists,
                                        ao::library::TrackStore::Reader::LoadMode mode)
  {
    if (lists.empty())
    {
      return;
    }

    auto const txn = _ml.readTransaction();
    auto const reader = _ml.tracks().reader(txn);

    std::vector<std::vector<TrackId>> nextMembers(lists.size());

    for (auto const i : std::views::iota(0uz, source.size()))
    {
      auto const id = source.trackIdAt(i);
      auto const optView = reader.get(id, mode);

      if (!optView)
      {
        continue;
      }

      for (auto const& [listIndex, list] : std::views::enumerate(lists))
      {
        if (list->_planEvaluator.matches(*list->_plan, *optView))
        {
          nextMembers[listIndex].push_back(id);
        }
      }
    }

    for (std::size_t i = 0; i < lists.size(); ++i)
    {
      std::ranges::sort(nextMembers[i]);
      lists[i]->_members = std::flat_set<TrackId>(std::sorted_unique, std::move(nextMembers[i]));
    }
  }

  void SmartListEvaluator::handleSourceReset(SourceBucket& bucket)
  {
    rebuildActiveLists(bucket);
  }

  void SmartListEvaluator::handleSourceInserted(SourceBucket& bucket, TrackId id, std::size_t /*sourceIndex*/)
  {
    std::vector<SmartListSource*> evaluatableLists;
    evaluatableLists.reserve(bucket.lists.size());

    for (auto* list : bucket.lists)
    {
      if (list->_hasError || !list->_plan || list->_dirty)
      {
        continue;
      }

      evaluatableLists.push_back(list);
    }

    if (evaluatableLists.empty())
    {
      return;
    }

    auto const txn = _ml.readTransaction();
    auto const reader = _ml.tracks().reader(txn);
    auto const mode = getUnionMode(evaluatableLists);
    auto const optView = reader.get(id, mode);

    if (!optView)
    {
      return;
    }

    for (auto* const list : evaluatableLists)
    {
      if (list->_planEvaluator.matches(*list->_plan, *optView))
      {
        auto [it, inserted] = list->_members.insert(id);

        if (inserted)
        {
          auto const index = static_cast<std::size_t>(std::ranges::distance(list->_members.begin(), it));
          list->TrackSource::notifyInserted(id, index);
        }
      }
    }
  }

  void SmartListEvaluator::handleSourceUpdated(SourceBucket& bucket, TrackId id, std::size_t /*sourceIndex*/)
  {
    std::vector<SmartListSource*> evaluatableLists;

    for (auto* list : bucket.lists)
    {
      if (list->_hasError || !list->_plan || list->_dirty)
      {
        continue;
      }

      evaluatableLists.push_back(list);
    }

    if (evaluatableLists.empty())
    {
      return;
    }

    auto const txn = _ml.readTransaction();
    auto const reader = _ml.tracks().reader(txn);
    auto const mode = getUnionMode(evaluatableLists);
    auto const optView = reader.get(id, mode);

    for (auto* const list : evaluatableLists)
    {
      bool const nowMatches = optView && list->_planEvaluator.matches(*list->_plan, *optView);
      auto const it = list->_members.find(id);
      bool const wasPresent = it != list->_members.end();

      if (nowMatches && !wasPresent)
      {
        if (auto [it2, inserted] = list->_members.insert(id); inserted)
        {
          auto const index = static_cast<std::size_t>(std::distance(list->_members.begin(), it2));
          list->TrackSource::notifyInserted(id, index);
        }
      }
      else if (!nowMatches && wasPresent)
      {
        auto const index = static_cast<std::size_t>(std::distance(list->_members.begin(), it));
        list->_members.erase(it);
        list->TrackSource::notifyRemoved(id, index);
      }
      else if (nowMatches && wasPresent)
      {
        auto const index = static_cast<std::size_t>(std::distance(list->_members.begin(), it));
        list->TrackSource::notifyUpdated(id, index);
      }
    }
  }

  void SmartListEvaluator::handleSourceRemoved(SourceBucket& bucket, TrackId id)
  {
    for (auto* list : bucket.lists)
    {
      if (list->_dirty)
      {
        continue;
      }

      if (auto const it = list->_members.find(id); it != list->_members.end())
      {
        auto const index = static_cast<std::size_t>(std::distance(list->_members.begin(), it));
        list->_members.erase(it);
        list->TrackSource::notifyRemoved(id, index);
      }
    }
  }

  void SmartListEvaluator::handleSourceInserted(SourceBucket& bucket, std::span<TrackId const> ids)
  {
    if (ids.empty())
    {
      return;
    }

    std::vector<SmartListSource*> evaluatableLists;

    for (auto* list : bucket.lists)
    {
      if (list->_hasError || !list->_plan || list->_dirty)
      {
        continue;
      }

      evaluatableLists.push_back(list);
    }

    if (evaluatableLists.empty())
    {
      return;
    }

    auto const txn = _ml.readTransaction();
    auto const reader = _ml.tracks().reader(txn);
    auto const mode = getUnionMode(evaluatableLists);

    std::vector<std::vector<TrackId>> matchedIds(evaluatableLists.size());

    for (auto const id : ids)
    {
      auto const optView = reader.get(id, mode);

      if (!optView)
      {
        continue;
      }

      for (std::size_t i = 0; i < evaluatableLists.size(); ++i)
      {
        if (evaluatableLists[i]->_planEvaluator.matches(*evaluatableLists[i]->_plan, *optView))
        {
          matchedIds[i].push_back(id);
        }
      }
    }

    for (std::size_t i = 0; i < evaluatableLists.size(); ++i)
    {
      if (!matchedIds[i].empty())
      {
        auto& list = *evaluatableLists[i];
        list._members.insert(matchedIds[i].begin(), matchedIds[i].end());
        list.TrackSource::notifyInserted(matchedIds[i]);
      }
    }
  }

  void SmartListEvaluator::handleSourceUpdated(SourceBucket& bucket, std::span<TrackId const> ids)
  {
    if (ids.empty())
    {
      return;
    }

    std::vector<SmartListSource*> evaluatableLists;

    for (auto* list : bucket.lists)
    {
      if (list->_hasError || !list->_plan || list->_dirty)
      {
        continue;
      }

      evaluatableLists.push_back(list);
    }

    if (evaluatableLists.empty())
    {
      return;
    }

    auto const txn = _ml.readTransaction();
    auto const reader = _ml.tracks().reader(txn);
    auto const mode = getUnionMode(evaluatableLists);

    // Track transitions for each list
    struct Transitions
    {
      std::vector<TrackId> inserted;
      std::vector<TrackId> removed;
      std::vector<TrackId> updated;
    };

    auto transitions = std::vector<Transitions>(evaluatableLists.size());

    for (auto const id : ids)
    {
      auto const optView = reader.get(id, mode);

      for (std::size_t i = 0; i < evaluatableLists.size(); ++i)
      {
        auto& list = *evaluatableLists[i];
        bool const nowMatches = optView && list._planEvaluator.matches(*list._plan, *optView);
        bool const wasPresent = list._members.contains(id);

        if (nowMatches && !wasPresent)
        {
          transitions[i].inserted.push_back(id);
        }
        else if (!nowMatches && wasPresent)
        {
          transitions[i].removed.push_back(id);
        }
        else if (nowMatches && wasPresent)
        {
          transitions[i].updated.push_back(id);
        }
      }
    }

    for (std::size_t i = 0; i < evaluatableLists.size(); ++i)
    {
      auto& list = *evaluatableLists[i];
      auto& trans = transitions[i];

      if (!trans.removed.empty())
      {
        for (auto const id : trans.removed)
        {
          list._members.erase(id);
        }

        list.TrackSource::notifyRemoved(trans.removed);
      }

      if (!trans.inserted.empty())
      {
        list._members.insert(trans.inserted.begin(), trans.inserted.end());
        list.TrackSource::notifyInserted(trans.inserted);
      }

      if (!trans.updated.empty())
      {
        list.TrackSource::notifyUpdated(trans.updated);
      }
    }
  }

  void SmartListEvaluator::handleSourceRemoved(SourceBucket& bucket, std::span<TrackId const> ids)
  {
    for (auto* list : bucket.lists)
    {
      if (list->_dirty)
      {
        continue;
      }

      std::vector<TrackId> removed;

      for (auto id : ids)
      {
        if (list->_members.erase(id) > 0)
        {
          removed.push_back(id);
        }
      }

      if (!removed.empty())
      {
        list->notifyRemoved(removed);
      }
    }
  }

  void SmartListEvaluator::handleSourceDestroyed(SourceBucket& bucket)
  {
    bucket.sourceAlive = false;

    for (auto* list : bucket.lists)
    {
      list->_members.clear();
      list->TrackSource::notifyReset();
    }
  }

  ao::library::TrackStore::Reader::LoadMode SmartListEvaluator::getUnionMode(std::span<SmartListSource*> lists)
  {
    bool needsHot = false;
    bool needsCold = false;

    for (auto* list : lists)
    {
      if (!list->_plan)
      {
        continue;
      }

      switch (list->_plan->accessProfile)
      {
        case ao::query::AccessProfile::HotOnly: needsHot = true; break;
        case ao::query::AccessProfile::ColdOnly: needsCold = true; break;
        case ao::query::AccessProfile::HotAndCold:
          needsHot = true;
          needsCold = true;
          break;
      }
    }

    if (needsHot && needsCold)
    {
      return ao::library::TrackStore::Reader::LoadMode::Both;
    }

    if (needsCold)
    {
      return ao::library::TrackStore::Reader::LoadMode::Cold;
    }

    return ao::library::TrackStore::Reader::LoadMode::Hot;
  }
}
