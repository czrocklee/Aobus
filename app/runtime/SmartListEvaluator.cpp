// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "SmartListEvaluator.h"

#include "SmartListSource.h"
#include "TrackSource.h"

#include <ao/lmdb/Transaction.h>
#include <ao/query/ExecutionPlan.h>
#include <ao/query/Parser.h>
#include <ao/utility/Log.h>
#include <ao/utility/ScopedTimer.h>

#include <algorithm>
#include <flat_set>
#include <stdexcept>
#include <utility>

namespace ao::rt
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

    if (auto const it = _evaluator._buckets.find(&_source); it != _evaluator._buckets.end())
    {
      _evaluator.handleSourceReset(*it->second);
    }
  }

  void SourceObserver::onInserted(TrackId const id, std::size_t const index)
  {
    if (!_valid)
    {
      return;
    }

    if (auto const it = _evaluator._buckets.find(&_source); it != _evaluator._buckets.end())
    {
      _evaluator.handleSourceInserted(*it->second, id, index);
    }
  }

  void SourceObserver::onUpdated(TrackId const id, std::size_t const index)
  {
    if (!_valid)
    {
      return;
    }

    if (auto const it = _evaluator._buckets.find(&_source); it != _evaluator._buckets.end())
    {
      _evaluator.handleSourceUpdated(*it->second, id, index);
    }
  }

  void SourceObserver::onRemoved(TrackId const id, std::size_t /*index*/)
  {
    if (!_valid)
    {
      return;
    }

    if (auto const it = _evaluator._buckets.find(&_source); it != _evaluator._buckets.end())
    {
      _evaluator.handleSourceRemoved(*it->second, id);
    }
  }

  void SourceObserver::onInserted(std::span<TrackId const> const ids)
  {
    if (!_valid)
    {
      return;
    }

    if (auto const it = _evaluator._buckets.find(&_source); it != _evaluator._buckets.end())
    {
      _evaluator.handleSourceInserted(*it->second, ids);
    }
  }

  void SourceObserver::onUpdated(std::span<TrackId const> const ids)
  {
    if (!_valid)
    {
      return;
    }

    if (auto const it = _evaluator._buckets.find(&_source); it != _evaluator._buckets.end())
    {
      _evaluator.handleSourceUpdated(*it->second, ids);
    }
  }

  void SourceObserver::onRemoved(std::span<TrackId const> const ids)
  {
    if (!_valid)
    {
      return;
    }

    if (auto const it = _evaluator._buckets.find(&_source); it != _evaluator._buckets.end())
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

    if (auto const it = _evaluator._buckets.find(&_source); it != _evaluator._buckets.end())
    {
      _evaluator.handleSourceDestroyed(*it->second);
    }
  }

  // SmartListEvaluator implementation

  SmartListEvaluator::SmartListEvaluator(library::MusicLibrary& ml)
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
        utility::unsafeDowncast<SourceObserver>(bucket->observer.get())->invalidate();

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
    if (auto const it = _buckets.find(&source); it != _buckets.end())
    {
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
  }

  void SmartListEvaluator::rebuild(SmartListSource& list)
  {
    if (auto const it = _buckets.find(&list._source); it != _buckets.end())
    {
      if (!list._dirty)
      {
        list.stageExpression(list._current.expression);
      }

      evaluateDirtyLists(*it->second);
    }
  }

  void SmartListEvaluator::notifyUpdated(TrackSource& source, TrackId const trackId)
  {
    if (auto const it = _buckets.find(&source); it != _buckets.end())
    {
      // Re-evaluate membership for all lists in this bucket

      if (auto const optSourceIndex = source.indexOf(trackId))
      {
        handleSourceUpdated(*it->second, trackId, *optSourceIndex);
      }
    }
  }

  void SmartListEvaluator::evaluateAllLists(SourceBucket& bucket)
  {
    if (bucket.lists.empty())
    {
      return;
    }

    evaluateLists(bucket.lists);
  }

  void SmartListEvaluator::evaluateDirtyLists(SourceBucket& bucket)
  {
    auto dirtyLists = std::vector<SmartListSource*>{};

    for (auto* const list : bucket.lists)
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

    evaluateLists(dirtyLists);
  }

  void SmartListEvaluator::evaluateLists(std::span<SmartListSource*> const lists)
  {
    if (lists.empty())
    {
      return;
    }

    auto evaluatableLists = std::vector<SmartListSource*>{};

    for (auto* const list : lists)
    {
      if (list->_current.hasError || !list->_current.plan)
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
      evaluateMembers(lists.front()->_source, evaluatableLists, mode);
    }

    // Notify Reset for only the provided lists
    for (auto* const list : lists)
    {
      list->TrackSource::notifyReset();
    }
  }

  void SmartListEvaluator::evaluateMembers(TrackSource& source,
                                           std::span<SmartListSource*> const lists,
                                           library::TrackStore::Reader::LoadMode const mode)
  {
    auto const timer = utility::ScopedTimer{"SmartListEvaluator::evaluateMembers"};

    if (lists.empty())
    {
      return;
    }

    auto const txn = _ml.readTransaction();
    auto const reader = _ml.tracks().reader(txn);

    auto nextMembers = std::vector<std::vector<TrackId>>(lists.size());

    for (auto const idx : std::views::iota(0UZ, source.size()))
    {
      auto const id = source.trackIdAt(idx);
      auto const optView = reader.get(id, mode);

      if (!optView)
      {
        continue;
      }

      for (std::size_t i = 0; i < lists.size(); ++i)
      {
        if (lists[i]->_planEvaluator.matches(*lists[i]->_current.plan, *optView))
        {
          nextMembers[i].push_back(id);
        }
      }
    }

    for (std::size_t listIdx = 0; listIdx < lists.size(); ++listIdx)
    {
      std::ranges::sort(nextMembers[listIdx]);
      lists[listIdx]->_members = std::flat_set<TrackId>(std::sorted_unique, std::move(nextMembers[listIdx]));
    }
  }

  void SmartListEvaluator::handleSourceReset(SourceBucket& bucket)
  {
    evaluateAllLists(bucket);
  }

  void SmartListEvaluator::handleSourceInserted(SourceBucket& bucket, TrackId const id, std::size_t /*sourceIndex*/)
  {
    auto evaluatableLists = std::vector<SmartListSource*>{};
    evaluatableLists.reserve(bucket.lists.size());

    for (auto* const list : bucket.lists)
    {
      if (list->_current.hasError || !list->_current.plan || list->_dirty)
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
      if (list->_planEvaluator.matches(*list->_current.plan, *optView))
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

  void SmartListEvaluator::handleSourceUpdated(SourceBucket& bucket, TrackId const id, std::size_t /*sourceIndex*/)
  {
    auto evaluatableLists = std::vector<SmartListSource*>{};

    for (auto* const list : bucket.lists)
    {
      if (list->_current.hasError || !list->_current.plan || list->_dirty)
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
      bool const nowMatches = optView && list->_planEvaluator.matches(*list->_current.plan, *optView);
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

  void SmartListEvaluator::handleSourceRemoved(SourceBucket& bucket, TrackId const id)
  {
    for (auto* const list : bucket.lists)
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

  void SmartListEvaluator::handleSourceInserted(SourceBucket& bucket, std::span<TrackId const> const ids)
  {
    if (ids.empty())
    {
      return;
    }

    auto evaluatableLists = std::vector<SmartListSource*>{};

    for (auto* const list : bucket.lists)
    {
      if (list->_current.hasError || !list->_current.plan || list->_dirty)
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

    auto matchedIds = std::vector<std::vector<TrackId>>(evaluatableLists.size());

    for (auto const id : ids)
    {
      auto const optView = reader.get(id, mode);

      if (!optView)
      {
        continue;
      }

      for (std::size_t i = 0; i < evaluatableLists.size(); ++i)
      {
        if (evaluatableLists[i]->_planEvaluator.matches(*evaluatableLists[i]->_current.plan, *optView))
        {
          matchedIds[i].push_back(id);
        }
      }
    }

    for (std::size_t idx = 0; idx < evaluatableLists.size(); ++idx)
    {
      if (!matchedIds[idx].empty())
      {
        auto& list = *evaluatableLists[idx];
        std::ranges::sort(matchedIds[idx]);
        auto const [first, last] = std::ranges::unique(matchedIds[idx]);
        matchedIds[idx].erase(first, last);

        list._members.insert(matchedIds[idx].begin(), matchedIds[idx].end());
        list.TrackSource::notifyInserted(matchedIds[idx]);
      }
    }
  }

  void SmartListEvaluator::handleSourceUpdated(SourceBucket& bucket, std::span<TrackId const> const ids)
  {
    if (ids.empty())
    {
      return;
    }

    auto evaluatableLists = std::vector<SmartListSource*>{};

    for (auto* const list : bucket.lists)
    {
      if (list->_current.hasError || !list->_current.plan || list->_dirty)
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
    struct Transitions final
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
        bool const nowMatches = optView && list._planEvaluator.matches(*list._current.plan, *optView);
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

    for (std::size_t idx = 0; idx < evaluatableLists.size(); ++idx)
    {
      auto& list = *evaluatableLists[idx];
      auto& trans = transitions[idx];

      if (!trans.removed.empty())
      {
        // Batch removal for flat_set
        std::ranges::sort(trans.removed);
        auto const [first, last] = std::ranges::unique(trans.removed);
        trans.removed.erase(first, last);

        auto newMembers = std::vector<TrackId>{};
        newMembers.reserve(list._members.size());
        std::ranges::set_difference(list._members, trans.removed, std::back_inserter(newMembers));
        list._members = std::flat_set<TrackId>(std::sorted_unique, std::move(newMembers));

        list.TrackSource::notifyRemoved(trans.removed);
      }

      if (!trans.inserted.empty())
      {
        std::ranges::sort(trans.inserted);
        auto const [first, last] = std::ranges::unique(trans.inserted);
        trans.inserted.erase(first, last);

        list._members.insert(trans.inserted.begin(), trans.inserted.end());
        list.TrackSource::notifyInserted(trans.inserted);
      }

      if (!trans.updated.empty())
      {
        list.TrackSource::notifyUpdated(trans.updated);
      }
    }
  }

  void SmartListEvaluator::handleSourceRemoved(SourceBucket& bucket, std::span<TrackId const> const ids)
  {
    for (auto* const list : bucket.lists)
    {
      if (list->_dirty)
      {
        continue;
      }

      auto removed = std::vector<TrackId>{};

      for (auto const id : ids)
      {
        if (list->_members.contains(id))
        {
          removed.push_back(id);
        }
      }

      if (!removed.empty())
      {
        std::ranges::sort(removed);
        auto const [first, last] = std::ranges::unique(removed);
        removed.erase(first, last);

        auto newMembers = std::vector<TrackId>{};
        newMembers.reserve(list->_members.size());
        std::ranges::set_difference(list->_members, removed, std::back_inserter(newMembers));
        list->_members = std::flat_set<TrackId>(std::sorted_unique, std::move(newMembers));

        list->notifyRemoved(removed);
      }
    }
  }

  void SmartListEvaluator::handleSourceDestroyed(SourceBucket& bucket)
  {
    bucket.sourceAlive = false;

    for (auto* const list : bucket.lists)
    {
      list->_members.clear();
      list->TrackSource::notifyReset();
    }
  }

  library::TrackStore::Reader::LoadMode SmartListEvaluator::getUnionMode(std::span<SmartListSource*> const lists)
  {
    bool needsHot = false;
    bool needsCold = false;

    for (auto* const list : lists)
    {
      if (!list->_current.plan)
      {
        continue;
      }

      switch (list->_current.plan->accessProfile)
      {
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
      return library::TrackStore::Reader::LoadMode::Both;
    }

    if (needsCold)
    {
      return library::TrackStore::Reader::LoadMode::Cold;
    }

    return library::TrackStore::Reader::LoadMode::Hot;
  }
}
