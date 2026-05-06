// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/model/SmartListEngine.h>

#include <ao/model/FilteredTrackIdList.h>
#include <ao/model/TrackIdList.h>
#include <ao/utility/Log.h>

#include <ao/lmdb/Transaction.h>
#include <ao/query/ExecutionPlan.h>
#include <ao/query/Parser.h>

#include <algorithm>
#include <flat_set>
#include <stdexcept>
#include <utility>

namespace ao::model
{
  // SourceObserver implementation

  SourceObserver::SourceObserver(SmartListEngine& engine, TrackIdList& source)
    : _engine{engine}, _source{source}
  {
  }

  void SourceObserver::onReset()
  {
    if (!_valid)
    {
      return;
    }

    if (auto it = _engine._buckets.find(&_source); it != _engine._buckets.end())
    {
      _engine.handleSourceReset(*it->second);
    }
  }

  void SourceObserver::onInserted(TrackId id, std::size_t index)
  {
    if (!_valid)
    {
      return;
    }

    if (auto it = _engine._buckets.find(&_source); it != _engine._buckets.end())
    {
      _engine.handleSourceInserted(*it->second, id, index);
    }
  }

  void SourceObserver::onUpdated(TrackId id, std::size_t index)
  {
    if (!_valid)
    {
      return;
    }

    if (auto it = _engine._buckets.find(&_source); it != _engine._buckets.end())
    {
      _engine.handleSourceUpdated(*it->second, id, index);
    }
  }

  void SourceObserver::onRemoved(TrackId id, std::size_t /*index*/)
  {
    if (!_valid)
    {
      return;
    }

    if (auto it = _engine._buckets.find(&_source); it != _engine._buckets.end())
    {
      _engine.handleSourceRemoved(*it->second, id);
    }
  }

  void SourceObserver::onInserted(std::span<TrackId const> ids)
  {
    if (!_valid)
    {
      return;
    }

    if (auto it = _engine._buckets.find(&_source); it != _engine._buckets.end())
    {
      _engine.handleSourceInserted(*it->second, ids);
    }
  }

  void SourceObserver::onUpdated(std::span<TrackId const> ids)
  {
    if (!_valid)
    {
      return;
    }

    if (auto it = _engine._buckets.find(&_source); it != _engine._buckets.end())
    {
      _engine.handleSourceUpdated(*it->second, ids);
    }
  }

  void SourceObserver::onRemoved(std::span<TrackId const> ids)
  {
    if (!_valid)
    {
      return;
    }

    if (auto it = _engine._buckets.find(&_source); it != _engine._buckets.end())
    {
      _engine.handleSourceRemoved(*it->second, ids);
    }
  }

  void SourceObserver::onSourceDestroyed()
  {
    if (!_valid)
    {
      return;
    }

    if (auto it = _engine._buckets.find(&_source); it != _engine._buckets.end())
    {
      _engine.handleSourceDestroyed(*it->second);
    }
  }

  // SmartListEngine implementation

  SmartListEngine::SmartListEngine(ao::library::MusicLibrary& ml)
    : _ml{ml}
  {
  }

  SmartListEngine::~SmartListEngine()
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

  void SmartListEngine::registerList(TrackIdList& source, FilteredTrackIdList& list)
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

  void SmartListEngine::unregisterList(TrackIdList& source, FilteredTrackIdList& list)
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

  void SmartListEngine::rebuild(FilteredTrackIdList& list)
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

  void SmartListEngine::notifyUpdated(TrackIdList& source, TrackId trackId)
  {
    auto const it = _buckets.find(&source);

    if (it == _buckets.end())
    {
      return;
    }

    // Re-evaluate membership for all lists in this bucket

    if (auto sourceIndex = source.indexOf(trackId))
    {
      handleSourceUpdated(*it->second, trackId, *sourceIndex);
    }
  }

  void SmartListEngine::rebuildActiveLists(SourceBucket& bucket)
  {
    if (bucket.lists.empty())
    {
      return;
    }

    rebuildLists(bucket.lists);
  }

  void SmartListEngine::rebuildDirtyLists(SourceBucket& bucket)
  {
    auto dirtyLists = std::vector<FilteredTrackIdList*>{};
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

  void SmartListEngine::rebuildLists(std::span<FilteredTrackIdList*> lists)
  {
    if (lists.empty())
    {
      return;
    }

    auto evaluatableLists = std::vector<FilteredTrackIdList*>{};
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
      list->TrackIdList::notifyReset();
    }
  }

  void SmartListEngine::rebuildGroup(TrackIdList& source,
                                     std::span<FilteredTrackIdList*> lists,
                                     ao::library::TrackStore::Reader::LoadMode mode)
  {
    if (lists.empty())
    {
      return;
    }

    ao::lmdb::ReadTransaction txn(_ml.readTransaction());
    auto reader = _ml.tracks().reader(txn);

    std::vector<std::vector<TrackId>> nextMembers(lists.size());

    for (std::size_t i = 0; i < source.size(); ++i)
    {
      auto id = source.trackIdAt(i);
      auto view = reader.get(id, mode);

      if (!view)
      {
        continue;
      }

      for (std::size_t listIndex = 0; listIndex < lists.size(); ++listIndex)
      {
        auto* list = lists[listIndex];

        if (list->_evaluator.matches(*list->_plan, *view))
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

  void SmartListEngine::handleSourceReset(SourceBucket& bucket)
  {
    rebuildActiveLists(bucket);
  }

  void SmartListEngine::handleSourceInserted(SourceBucket& bucket, TrackId id, std::size_t /*sourceIndex*/)
  {
    std::vector<FilteredTrackIdList*> evaluatableLists;
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

    ao::lmdb::ReadTransaction txn(_ml.readTransaction());
    auto reader = _ml.tracks().reader(txn);
    auto const mode = getUnionMode(evaluatableLists);
    auto const view = reader.get(id, mode);

    if (!view)
    {
      return;
    }

    for (auto* list : evaluatableLists)
    {
      if (list->_evaluator.matches(*list->_plan, *view))
      {
        auto [it, inserted] = list->_members.insert(id);

        if (inserted)
        {
          auto const index = static_cast<std::size_t>(std::ranges::distance(list->_members.begin(), it));
          list->TrackIdList::notifyInserted(id, index);
        }
      }
    }
  }

  void SmartListEngine::handleSourceUpdated(SourceBucket& bucket, TrackId id, std::size_t /*sourceIndex*/)
  {
    std::vector<FilteredTrackIdList*> evaluatableLists;

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

    ao::lmdb::ReadTransaction txn(_ml.readTransaction());
    auto reader = _ml.tracks().reader(txn);
    auto const mode = getUnionMode(evaluatableLists);
    auto const view = reader.get(id, mode);

    for (auto* list : evaluatableLists)
    {
      bool const nowMatches = view && list->_evaluator.matches(*list->_plan, *view);
      auto const it = list->_members.find(id);
      bool const wasPresent = it != list->_members.end();

      if (nowMatches && !wasPresent)
      {
        if (auto [it2, inserted] = list->_members.insert(id); inserted)
        {
          auto const index = static_cast<std::size_t>(std::distance(list->_members.begin(), it2));
          list->TrackIdList::notifyInserted(id, index);
        }
      }
      else if (!nowMatches && wasPresent)
      {
        auto const index = static_cast<std::size_t>(std::distance(list->_members.begin(), it));
        list->_members.erase(it);
        list->TrackIdList::notifyRemoved(id, index);
      }
      else if (nowMatches && wasPresent)
      {
        auto const index = static_cast<std::size_t>(std::distance(list->_members.begin(), it));
        list->TrackIdList::notifyUpdated(id, index);
      }
    }
  }

  void SmartListEngine::handleSourceRemoved(SourceBucket& bucket, TrackId id)
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
        list->TrackIdList::notifyRemoved(id, index);
      }
    }
  }

  void SmartListEngine::handleSourceInserted(SourceBucket& bucket, std::span<TrackId const> ids)
  {
    if (ids.empty())
    {
      return;
    }

    std::vector<FilteredTrackIdList*> evaluatableLists;

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

    ao::lmdb::ReadTransaction txn(_ml.readTransaction());
    auto reader = _ml.tracks().reader(txn);
    auto const mode = getUnionMode(evaluatableLists);

    std::vector<std::vector<TrackId>> matchedIds(evaluatableLists.size());

    for (auto id : ids)
    {
      auto const view = reader.get(id, mode);

      if (!view)
      {
        continue;
      }

      for (std::size_t i = 0; i < evaluatableLists.size(); ++i)
      {
        if (evaluatableLists[i]->_evaluator.matches(*evaluatableLists[i]->_plan, *view))
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
        list.TrackIdList::notifyInserted(matchedIds[i]);
      }
    }
  }

  void SmartListEngine::handleSourceUpdated(SourceBucket& bucket, std::span<TrackId const> ids)
  {
    if (ids.empty())
    {
      return;
    }

    std::vector<FilteredTrackIdList*> evaluatableLists;

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

    ao::lmdb::ReadTransaction txn(_ml.readTransaction());
    auto reader = _ml.tracks().reader(txn);
    auto const mode = getUnionMode(evaluatableLists);

    // Track transitions for each list
    struct Transitions
    {
      std::vector<TrackId> inserted;
      std::vector<TrackId> removed;
      std::vector<TrackId> updated;
    };

    std::vector<Transitions> transitions(evaluatableLists.size());

    for (auto id : ids)
    {
      auto const view = reader.get(id, mode);

      for (std::size_t i = 0; i < evaluatableLists.size(); ++i)
      {
        auto& list = *evaluatableLists[i];
        bool const nowMatches = view && list._evaluator.matches(*list._plan, *view);
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
        for (auto id : trans.removed)
        {
          list._members.erase(id);
        }

        list.TrackIdList::notifyRemoved(trans.removed);
      }

      if (!trans.inserted.empty())
      {
        list._members.insert(trans.inserted.begin(), trans.inserted.end());
        list.TrackIdList::notifyInserted(trans.inserted);
      }

      if (!trans.updated.empty())
      {
        list.TrackIdList::notifyUpdated(trans.updated);
      }
    }
  }

  void SmartListEngine::handleSourceRemoved(SourceBucket& bucket, std::span<TrackId const> ids)
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

  void SmartListEngine::handleSourceDestroyed(SourceBucket& bucket)
  {
    bucket.sourceAlive = false;

    for (auto* list : bucket.lists)
    {
      list->_members.clear();
      list->TrackIdList::notifyReset();
    }
  }

  ao::library::TrackStore::Reader::LoadMode SmartListEngine::getUnionMode(std::span<FilteredTrackIdList*> lists)
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
} // namespace ao::model
