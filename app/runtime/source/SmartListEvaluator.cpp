// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/query/Field.h>
#include <ao/query/detail/Bytecode.h>
#include <ao/rt/ScopedTimer.h>
#include <ao/rt/StorageResult.h>
#include <ao/rt/source/SmartListEvaluator.h>
#include <ao/rt/source/SmartListSource.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/utility/ByteView.h>

#include <algorithm>
#include <cstddef>
#include <flat_set>
#include <iterator>
#include <memory>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace ao::rt
{
  // SourceObserver implementation

  SourceObserver::SourceObserver(SmartListEvaluator& evaluator, TrackSource& source)
    : _evaluator{evaluator}, _source{source}
  {
  }

  void SourceObserver::handleReset()
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

  void SourceObserver::handleInserted(TrackId const id, std::size_t const index)
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

  void SourceObserver::handleUpdated(TrackId const id, std::size_t const index)
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

  void SourceObserver::handleRemoved(TrackId const id, std::size_t /*index*/)
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

  void SourceObserver::handleBulkInserted(std::span<TrackId const> const ids)
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

  void SourceObserver::handleBulkUpdated(std::span<TrackId const> const ids)
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

  void SourceObserver::handleBulkRemoved(std::span<TrackId const> const ids)
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

  void SourceObserver::handleSourceDestroyed()
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

    for (auto& [source, bucketPtr] : _buckets)
    {
      for (auto* list : bucketPtr->lists)
      {
        list->_evaluator = nullptr;
      }

      if (bucketPtr->observerPtr)
      {
        utility::unsafeDowncast<SourceObserver>(bucketPtr->observerPtr.get())->invalidate();

        if (bucketPtr->sourceAlive)
        {
          source->detach(bucketPtr->observerPtr.get());
        }
      }
    }
  }

  void SmartListEvaluator::registerList(TrackSource& source, SmartListSource& list)
  {
    auto& bucketPtr = _buckets[&source];

    if (!bucketPtr)
    {
      bucketPtr = std::make_unique<SourceBucket>();
      bucketPtr->source = &source;
      bucketPtr->observerPtr = std::make_unique<SourceObserver>(*this, source);
      source.attach(bucketPtr->observerPtr.get());
    }

    bucketPtr->lists.push_back(&list);
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
          source.detach(bucket.observerPtr.get());
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
      if (auto const optSourceIndex = source.indexOf(trackId); optSourceIndex)
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
      if (list->_current.optError || !list->_current.planPtr)
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
      auto const mode = unionMode(evaluatableLists);
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
                                           TrackLoadMode const mode)
  {
    auto const timer = rt::ScopedTimer{"SmartListEvaluator::evaluateMembers"};

    if (lists.empty())
    {
      return;
    }

    auto const transaction = _ml.readTransaction();
    auto const reader = _ml.tracks().reader(transaction);
    auto const storeMode = static_cast<library::TrackStore::Reader::LoadMode>(mode);

    auto nextMembers = std::vector<std::vector<TrackId>>{lists.size()};

    for (auto const index : std::views::iota(0UZ, source.size()))
    {
      auto const id = source.trackIdAt(index);
      auto const optView = storageValueOrNullopt(reader.get(id, storeMode), "Failed to evaluate smart list track");

      if (!optView)
      {
        continue;
      }

      for (std::size_t i = 0; i < lists.size(); ++i)
      {
        if (lists[i]->_planEvaluator.matches(*lists[i]->_current.planPtr, *optView))
        {
          nextMembers[i].push_back(id);
        }
      }
    }

    for (std::size_t listIndex = 0; listIndex < lists.size(); ++listIndex)
    {
      std::ranges::sort(nextMembers[listIndex]);
      // NOLINTNEXTLINE(misc-include-cleaner)
      lists[listIndex]->_members = std::flat_set{std::sorted_unique, std::move(nextMembers[listIndex])};
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
      if (list->_current.optError || !list->_current.planPtr || list->_dirty)
      {
        continue;
      }

      evaluatableLists.push_back(list);
    }

    if (evaluatableLists.empty())
    {
      return;
    }

    auto const transaction = _ml.readTransaction();
    auto const reader = _ml.tracks().reader(transaction);
    auto const mode = unionMode(evaluatableLists);
    auto const storeMode = static_cast<library::TrackStore::Reader::LoadMode>(mode);
    auto const optView =
      storageValueOrNullopt(reader.get(id, storeMode), "Failed to evaluate inserted smart list track");

    if (!optView)
    {
      return;
    }

    for (auto* const list : evaluatableLists)
    {
      if (list->_planEvaluator.matches(*list->_current.planPtr, *optView))
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
      if (list->_current.optError || !list->_current.planPtr || list->_dirty)
      {
        continue;
      }

      evaluatableLists.push_back(list);
    }

    if (evaluatableLists.empty())
    {
      return;
    }

    auto const transaction = _ml.readTransaction();
    auto const reader = _ml.tracks().reader(transaction);
    auto const mode = unionMode(evaluatableLists);
    auto const storeMode = static_cast<library::TrackStore::Reader::LoadMode>(mode);
    auto const optView =
      storageValueOrNullopt(reader.get(id, storeMode), "Failed to evaluate updated smart list track");

    for (auto* const list : evaluatableLists)
    {
      bool const nowMatches = optView && list->_planEvaluator.matches(*list->_current.planPtr, *optView);

      if (auto const it = list->_members.find(id); nowMatches && it == list->_members.end())
      {
        if (auto [it2, inserted] = list->_members.insert(id); inserted)
        {
          auto const index = static_cast<std::size_t>(std::distance(list->_members.begin(), it2));
          list->TrackSource::notifyInserted(id, index);
        }
      }
      else if (!nowMatches && it != list->_members.end())
      {
        auto const index = static_cast<std::size_t>(std::distance(list->_members.begin(), it));
        list->_members.erase(it);
        list->TrackSource::notifyRemoved(id, index);
      }
      else if (nowMatches && it != list->_members.end())
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
      if (list->_current.optError || !list->_current.planPtr || list->_dirty)
      {
        continue;
      }

      evaluatableLists.push_back(list);
    }

    if (evaluatableLists.empty())
    {
      return;
    }

    auto const transaction = _ml.readTransaction();
    auto const reader = _ml.tracks().reader(transaction);
    auto const mode = unionMode(evaluatableLists);
    auto const storeMode = static_cast<library::TrackStore::Reader::LoadMode>(mode);

    auto matchedIds = std::vector<std::vector<TrackId>>{evaluatableLists.size()};

    for (auto const id : ids)
    {
      auto const optView =
        storageValueOrNullopt(reader.get(id, storeMode), "Failed to evaluate inserted smart list tracks");

      if (!optView)
      {
        continue;
      }

      for (std::size_t i = 0; i < evaluatableLists.size(); ++i)
      {
        if (evaluatableLists[i]->_planEvaluator.matches(*evaluatableLists[i]->_current.planPtr, *optView))
        {
          matchedIds[i].push_back(id);
        }
      }
    }

    for (std::size_t index = 0; index < evaluatableLists.size(); ++index)
    {
      if (!matchedIds[index].empty())
      {
        auto& list = *evaluatableLists[index];
        std::ranges::sort(matchedIds[index]);
        auto const [first, last] = std::ranges::unique(matchedIds[index]);
        matchedIds[index].erase(first, last);

        list._members.insert(matchedIds[index].begin(), matchedIds[index].end());
        list.TrackSource::notifyInserted(matchedIds[index]);
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
      if (list->_current.optError || !list->_current.planPtr || list->_dirty)
      {
        continue;
      }

      evaluatableLists.push_back(list);
    }

    if (evaluatableLists.empty())
    {
      return;
    }

    auto const transaction = _ml.readTransaction();
    auto const reader = _ml.tracks().reader(transaction);
    auto const mode = unionMode(evaluatableLists);
    auto const storeMode = static_cast<library::TrackStore::Reader::LoadMode>(mode);

    // Track transitions for each list
    struct Transitions final
    {
      std::vector<TrackId> inserted;
      std::vector<TrackId> removed;
      std::vector<TrackId> updated;
    };

    auto transitions = std::vector<Transitions>{evaluatableLists.size()};

    for (auto const id : ids)
    {
      auto const optView =
        storageValueOrNullopt(reader.get(id, storeMode), "Failed to evaluate updated smart list tracks");

      for (std::size_t i = 0; i < evaluatableLists.size(); ++i)
      {
        auto& list = *evaluatableLists[i];
        bool const nowMatches = optView && list._planEvaluator.matches(*list._current.planPtr, *optView);

        if (bool const wasPresent = list._members.contains(id); nowMatches && !wasPresent)
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

    for (std::size_t index = 0; index < evaluatableLists.size(); ++index)
    {
      auto& list = *evaluatableLists[index];
      auto& trans = transitions[index];

      if (!trans.removed.empty())
      {
        // Batch removal for flat_set
        std::ranges::sort(trans.removed);
        auto const [first, last] = std::ranges::unique(trans.removed);
        trans.removed.erase(first, last);

        auto newMembers = std::vector<TrackId>{};
        newMembers.reserve(list._members.size());
        std::ranges::set_difference(list._members, trans.removed, std::back_inserter(newMembers));
        // NOLINTNEXTLINE(misc-include-cleaner)
        list._members = std::flat_set{std::sorted_unique, std::move(newMembers)};

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
        // NOLINTNEXTLINE(misc-include-cleaner)
        list->_members = std::flat_set{std::sorted_unique, std::move(newMembers)};

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

  SmartListEvaluator::TrackLoadMode SmartListEvaluator::unionMode(std::span<SmartListSource*> const lists)
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
