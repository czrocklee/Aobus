// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "core/model/SmartListEngine.h"
#include "core/Log.h"

#include "core/model/TrackIdList.h"

#include <rs/expr/ExecutionPlan.h>
#include <rs/expr/Parser.h>
#include <rs/lmdb/Transaction.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace app::core::model
{

  struct SmartListEngine::SmartListState
  {
    RegistrationId id = 0;
    TrackIdList* source = nullptr;
    TrackIdList* facade = nullptr; // The FilteredTrackIdList facade

    std::string expression;
    bool hasError = false;
    std::string errorMessage;

    std::unique_ptr<rs::expr::ExecutionPlan> plan;
    rs::expr::PlanEvaluator evaluator;
    std::vector<TrackId> members;

    std::string stagedExpression;
    bool stagedHasError = false;
    std::string stagedErrorMessage;
    std::unique_ptr<rs::expr::ExecutionPlan> stagedPlan;

    // Reference to owning bucket (non-owning)
    SourceBucket* bucket = nullptr;

    // Dirty flag - needs rebuild
    bool dirty = true;
  };

  struct SmartListEngine::SourceBucket
  {
    TrackIdList* source = nullptr;
    bool sourceAlive = true;
    std::vector<RegistrationId> registrations;
    std::unique_ptr<TrackIdListObserver> observer;
  };

  // SourceObserver implementation

  SourceObserver::SourceObserver(SmartListEngine& engine, TrackIdList& source)
    : _engine{engine}, _source{source}, _valid{true}
  {
  }

  void SourceObserver::onReset()
  {
    if (!_valid)
    {
      return;
    }
    auto it = _engine._buckets.find(&_source);
    if (it != _engine._buckets.end())
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
    auto it = _engine._buckets.find(&_source);
    if (it != _engine._buckets.end())
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
    auto it = _engine._buckets.find(&_source);
    if (it != _engine._buckets.end())
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
    auto it = _engine._buckets.find(&_source);
    if (it != _engine._buckets.end())
    {
      _engine.handleSourceRemoved(*it->second, id);
    }
  }

  void SourceObserver::onSourceDestroyed()
  {
    if (!_valid)
    {
      return;
    }
    auto it = _engine._buckets.find(&_source);
    if (it != _engine._buckets.end())
    {
      _engine.handleSourceDestroyed(*it->second);
    }
  }

  // SmartListEngine implementation

  SmartListEngine::SmartListEngine(rs::core::MusicLibrary& ml)
    : _ml{&ml}
  {
  }

  SmartListEngine::~SmartListEngine()
  {
    _alive = false;
    for (auto& [source, bucket] : _buckets)
    {
      if (bucket->observer)
      {
        static_cast<SourceObserver*>(bucket->observer.get())->invalidate();
        if (bucket->sourceAlive) source->detach(bucket->observer.get());
      }
    }
  }

  SmartListEngine::SmartListState* SmartListEngine::getState(RegistrationId id)
  {
    auto it = _states.find(id);
    return (it != _states.end()) ? it->second.get() : nullptr;
  }

  SmartListEngine::SmartListState const* SmartListEngine::getState(RegistrationId id) const
  {
    auto it = _states.find(id);
    return (it != _states.end()) ? it->second.get() : nullptr;
  }

  SmartListEngine::RegistrationId SmartListEngine::registerList(TrackIdList& source, TrackIdList& facade)
  {
    auto id = _nextRegistrationId++;
    auto state = std::make_unique<SmartListState>();
    state->id = id;
    state->source = &source;
    state->facade = &facade;
    stageExpression(*state, "");

    auto& bucket = _buckets[&source];
    if (!bucket)
    {
      bucket = std::make_unique<SourceBucket>();
      bucket->source = &source;
      bucket->observer = std::make_unique<SourceObserver>(*this, source);
      source.attach(bucket->observer.get());
    }

    state->bucket = bucket.get();
    bucket->registrations.push_back(id);
    _states.emplace(id, std::move(state));
    return id;
  }

  void SmartListEngine::unregisterList(RegistrationId id)
  {
    auto* state = getState(id);
    if (!state) return;

    auto* source = state->source;
    auto* bucket = state->bucket;

    if (bucket)
    {
      std::erase(bucket->registrations, id);
      if (bucket->registrations.empty())
      {
        if (source && bucket->sourceAlive) source->detach(bucket->observer.get());
        _buckets.erase(source);
      }
    }

    _states.erase(id);
  }

  void SmartListEngine::setExpression(RegistrationId id, std::string expr)
  {
    auto* state = getState(id);
    if (!state) throw std::invalid_argument("Invalid registration id");
    stageExpression(*state, std::move(expr));
  }

  void SmartListEngine::rebuild(RegistrationId id)
  {
    auto* state = getState(id);
    if (!state || !state->bucket) return;

    if (!state->dirty) stageExpression(*state, state->expression);
    rebuildDirtyStates(*state->bucket);
  }

  std::size_t SmartListEngine::size(RegistrationId id) const
  {
    auto* state = getState(id);
    return state ? state->members.size() : 0;
  }

  TrackId SmartListEngine::trackIdAt(RegistrationId id, std::size_t index) const
  {
    auto* state = getState(id);
    if (!state) throw std::out_of_range("Invalid registration id");
    return state->members.at(index);
  }

  std::optional<std::size_t> SmartListEngine::indexOf(RegistrationId id, TrackId trackId) const
  {
    auto* state = getState(id);
    if (!state) return std::nullopt;

    auto const it = std::find(state->members.begin(), state->members.end(), trackId);
    if (it == state->members.end()) return std::nullopt;
    return static_cast<std::size_t>(std::distance(state->members.begin(), it));
  }

  bool SmartListEngine::hasError(RegistrationId id) const
  {
    auto* state = getState(id);
    return state ? (state->dirty ? state->stagedHasError : state->hasError) : true;
  }

  std::string const& SmartListEngine::errorMessage(RegistrationId id) const
  {
    static std::string const empty;
    auto* state = getState(id);
    return state ? (state->dirty ? state->stagedErrorMessage : state->errorMessage) : empty;
  }

  void SmartListEngine::stageExpression(SmartListState& state, std::string expr)
  {
    state.stagedExpression = std::move(expr);

    try
    {
      auto parsed = state.stagedExpression.empty() ? rs::expr::parse("true") : rs::expr::parse(state.stagedExpression);
      auto compiler = rs::expr::QueryCompiler{&_ml->dictionary()};
      state.stagedPlan = std::make_unique<rs::expr::ExecutionPlan>(compiler.compile(parsed));
      state.stagedHasError = false;
      state.stagedErrorMessage.clear();
    }
    catch (std::exception const& e)
    {
      APP_LOG_ERROR("Smart list expression error for '{}': {}", expr, e.what());
      state.stagedHasError = true;
      state.stagedErrorMessage = e.what();
      state.stagedPlan.reset();
    }

    state.dirty = true;
  }

  void SmartListEngine::applyStagedState(SmartListState& state)
  {
    if (!state.dirty) return;

    state.expression = state.stagedExpression;
    state.hasError = state.stagedHasError;
    state.errorMessage = state.stagedErrorMessage;
    state.plan = std::move(state.stagedPlan);
    state.dirty = false;
  }

  void SmartListEngine::notifyTrackDataChanged(RegistrationId id, TrackId trackId)
  {
    auto it = _states.find(id);
    if (it == _states.end())
    {
      return;
    }

    auto& state = *it->second;
    if (!state.bucket || !state.bucket->source)
    {
      return;
    }

    // Get the source index for proper ordering
    auto sourceIndexOpt = state.bucket->source->indexOf(trackId);
    if (!sourceIndexOpt)
    {
      return;
    }

    // Re-evaluate whether the track still matches the filter
    handleSourceUpdated(*state.bucket, trackId, *sourceIndexOpt);
  }

  void SmartListEngine::rebuildActiveStates(SourceBucket& bucket)
  {
    if (bucket.registrations.empty())
    {
      return;
    }

    std::vector<SmartListState*> allStates;
    allStates.reserve(bucket.registrations.size());

    for (auto regId : bucket.registrations)
    {
      auto it = _states.find(regId);
      if (it != _states.end())
      {
        allStates.push_back(it->second.get());
      }
    }

    if (allStates.empty())
    {
      return;
    }

    rebuildStates(allStates);
  }

  void SmartListEngine::rebuildDirtyStates(SourceBucket& bucket)
  {
    if (bucket.registrations.empty())
    {
      return;
    }

    std::vector<SmartListState*> dirtyStates;
    dirtyStates.reserve(bucket.registrations.size());

    for (auto regId : bucket.registrations)
    {
      auto it = _states.find(regId);
      if (it != _states.end() && it->second->dirty)
      {
        applyStagedState(*it->second);
        dirtyStates.push_back(it->second.get());
      }
    }

    if (dirtyStates.empty())
    {
      return;
    }

    rebuildStates(dirtyStates);
  }

  void SmartListEngine::rebuildStates(std::span<SmartListState*> states)
  {
    if (states.empty())
    {
      return;
    }

    std::vector<SmartListState*> hotOnlyStates;
    std::vector<SmartListState*> coldOrBothStates;

    for (auto* state : states)
    {
      if (state->hasError || !state->plan)
      {
        state->members.clear();
        continue;
      }

      switch (state->plan->accessProfile)
      {
        case rs::expr::AccessProfile::HotOnly: hotOnlyStates.push_back(state); break;
        case rs::expr::AccessProfile::ColdOnly:
        case rs::expr::AccessProfile::HotAndCold: coldOrBothStates.push_back(state); break;
      }
    }

    // Rebuild each group
    if (!hotOnlyStates.empty())
    {
      rebuildGroup(*states.front()->source, hotOnlyStates, rs::core::TrackStore::Reader::LoadMode::Hot);
    }

    if (!coldOrBothStates.empty())
    {
      rebuildGroup(*states.front()->source, coldOrBothStates, rs::core::TrackStore::Reader::LoadMode::Both);
    }

    // Notify facades of reset
    for (auto* state : states)
    {
      if (state->facade)
      {
        notifyFacadeReset(*state->facade);
      }
    }
  }

  void SmartListEngine::rebuildGroup(TrackIdList& source,
                                     std::span<SmartListState*> states,
                                     rs::core::TrackStore::Reader::LoadMode mode)
  {
    if (states.empty())
    {
      return;
    }

    rs::lmdb::ReadTransaction txn(_ml->readTransaction());
    auto reader = _ml->tracks().reader(txn);

    std::vector<std::vector<TrackId>> nextMembers(states.size());

    for (std::size_t i = 0; i < source.size(); ++i)
    {
      auto id = source.trackIdAt(i);
      auto view = reader.get(id, mode);

      if (!view)
      {
        continue;
      }

      for (std::size_t stateIndex = 0; stateIndex < states.size(); ++stateIndex)
      {
        auto& state = *states[stateIndex];
        if (state.evaluator.matches(*state.plan, *view))
        {
          nextMembers[stateIndex].push_back(id);
        }
      }
    }

    // Update members directly without notification - will be followed by facade reset
    for (std::size_t i = 0; i < states.size(); ++i)
    {
      auto& state = *states[i];
      state.members = std::move(nextMembers[i]);
    }
  }

  std::size_t SmartListEngine::insertionIndexForSourceOrder(SmartListState const& state, std::size_t sourceIndex)
  {
    for (std::size_t i = 0; i < state.members.size(); ++i)
    {
      auto const memberSourceIndex = state.source->indexOf(state.members[i]);
      if (!memberSourceIndex || *memberSourceIndex > sourceIndex)
      {
        return i;
      }
    }

    return state.members.size();
  }

  void SmartListEngine::handleSourceReset(SourceBucket& bucket)
  {
    rebuildActiveStates(bucket);
  }

  void SmartListEngine::handleSourceInserted(SourceBucket& bucket, TrackId id, std::size_t sourceIndex)
  {
    std::vector<SmartListState*> hotOnlyStates;
    std::vector<SmartListState*> coldOrBothStates;

    for (auto regId : bucket.registrations)
    {
      auto it = _states.find(regId);
      if (it == _states.end())
      {
        continue;
      }

      auto& state = *it->second;
      if (state.hasError || !state.plan)
      {
        continue;
      }

      switch (state.plan->accessProfile)
      {
        case rs::expr::AccessProfile::HotOnly: hotOnlyStates.push_back(&state); break;
        case rs::expr::AccessProfile::ColdOnly:
        case rs::expr::AccessProfile::HotAndCold: coldOrBothStates.push_back(&state); break;
      }
    }

    if (hotOnlyStates.empty() && coldOrBothStates.empty())
    {
      return;
    }

    rs::lmdb::ReadTransaction txn(_ml->readTransaction());
    auto reader = _ml->tracks().reader(txn);

    std::optional<rs::core::TrackView> viewHot;
    std::optional<rs::core::TrackView> viewBoth;

    if (!coldOrBothStates.empty())
    {
      viewBoth = reader.get(id, rs::core::TrackStore::Reader::LoadMode::Both);
    }
    else
    {
      viewHot = reader.get(id, rs::core::TrackStore::Reader::LoadMode::Hot);
    }

    for (auto* state : hotOnlyStates)
    {
      auto const& view = viewBoth ? viewBoth : viewHot;
      if (!view || !state->evaluator.matches(*state->plan, *view))
      {
        continue;
      }

      auto const insertIndex = insertionIndexForSourceOrder(*state, sourceIndex);
      state->members.insert(state->members.begin() + static_cast<std::ptrdiff_t>(insertIndex), id);
      if (state->facade)
      {
        notifyFacadeInserted(*state->facade, id, insertIndex);
      }
    }

    if (!viewBoth)
    {
      return;
    }

    for (auto* state : coldOrBothStates)
    {
      if (!state->evaluator.matches(*state->plan, *viewBoth))
      {
        continue;
      }

      auto const insertIndex = insertionIndexForSourceOrder(*state, sourceIndex);
      state->members.insert(state->members.begin() + static_cast<std::ptrdiff_t>(insertIndex), id);
      if (state->facade)
      {
        notifyFacadeInserted(*state->facade, id, insertIndex);
      }
    }
  }

  void SmartListEngine::handleSourceUpdated(SourceBucket& bucket, TrackId id, std::size_t sourceIndex)
  {
    std::vector<SmartListState*> hotOnlyStates;
    std::vector<SmartListState*> coldOrBothStates;

    for (auto regId : bucket.registrations)
    {
      auto it = _states.find(regId);
      if (it == _states.end())
      {
        continue;
      }

      auto& state = *it->second;
      if (state.hasError || !state.plan)
      {
        continue;
      }

      switch (state.plan->accessProfile)
      {
        case rs::expr::AccessProfile::HotOnly: hotOnlyStates.push_back(&state); break;
        case rs::expr::AccessProfile::ColdOnly:
        case rs::expr::AccessProfile::HotAndCold: coldOrBothStates.push_back(&state); break;
      }
    }

    if (hotOnlyStates.empty() && coldOrBothStates.empty())
    {
      return;
    }

    rs::lmdb::ReadTransaction txn(_ml->readTransaction());
    auto reader = _ml->tracks().reader(txn);

    std::optional<rs::core::TrackView> viewHot;
    std::optional<rs::core::TrackView> viewBoth;

    if (!coldOrBothStates.empty())
    {
      viewBoth = reader.get(id, rs::core::TrackStore::Reader::LoadMode::Both);
    }
    else
    {
      viewHot = reader.get(id, rs::core::TrackStore::Reader::LoadMode::Hot);
    }

    auto updateState = [this, id, sourceIndex](SmartListState& state, std::optional<rs::core::TrackView> const& view)
    {
      bool const nowMatches = view && state.evaluator.matches(*state.plan, *view);

      auto const it2 = std::find(state.members.begin(), state.members.end(), id);
      bool const wasPresent = it2 != state.members.end();

      if (nowMatches && !wasPresent)
      {
        auto const insertIndex = insertionIndexForSourceOrder(state, sourceIndex);
        state.members.insert(state.members.begin() + static_cast<std::ptrdiff_t>(insertIndex), id);
        if (state.facade)
        {
          notifyFacadeInserted(*state.facade, id, insertIndex);
        }
      }
      else if (!nowMatches && wasPresent)
      {
        auto removeIndex = static_cast<std::size_t>(std::distance(state.members.begin(), it2));
        state.members.erase(it2);
        if (state.facade)
        {
          notifyFacadeRemoved(*state.facade, id, removeIndex);
        }
      }
      else if (nowMatches && wasPresent)
      {
        auto updateIndex = static_cast<std::size_t>(std::distance(state.members.begin(), it2));
        if (state.facade)
        {
          notifyFacadeUpdated(*state.facade, id, updateIndex);
        }
      }
    };

    for (auto* state : hotOnlyStates)
    {
      updateState(*state, viewBoth ? viewBoth : viewHot);
    }

    for (auto* state : coldOrBothStates)
    {
      updateState(*state, viewBoth);
    }
  }

  void SmartListEngine::handleSourceRemoved(SourceBucket& bucket, TrackId id)
  {
    for (auto regId : bucket.registrations)
    {
      auto it = _states.find(regId);
      if (it == _states.end())
      {
        continue;
      }

      auto& state = *it->second;
      auto const it2 = std::find(state.members.begin(), state.members.end(), id);
      if (it2 != state.members.end())
      {
        auto removeIndex = static_cast<std::size_t>(std::distance(state.members.begin(), it2));
        state.members.erase(it2);
        if (state.facade)
        {
          notifyFacadeRemoved(*state.facade, id, removeIndex);
        }
      }
    }
  }

  void SmartListEngine::notifyFacadeReset(TrackIdList& facade)
  {
    facade.notifyReset();
  }

  void SmartListEngine::notifyFacadeInserted(TrackIdList& facade, TrackId id, std::size_t index)
  {
    facade.notifyInserted(id, index);
  }

  void SmartListEngine::notifyFacadeUpdated(TrackIdList& facade, TrackId id, std::size_t index)
  {
    facade.notifyUpdated(id, index);
  }

  void SmartListEngine::notifyFacadeRemoved(TrackIdList& facade, TrackId id, std::size_t index)
  {
    facade.notifyRemoved(id, index);
  }

  void SmartListEngine::handleSourceDestroyed(SourceBucket& bucket)
  {
    // Mark source as dead so unregisterList knows it's already detached
    bucket.sourceAlive = false;

    // Clear state members in all registrations belonging to this bucket
    for (auto regId : bucket.registrations)
    {
      auto it = _states.find(regId);
      if (it != _states.end())
      {
        it->second->members.clear();
        if (it->second->facade)
        {
          notifyFacadeReset(*it->second->facade);
        }
      }
    }

    // We don't erase the bucket or null out pointers here because unregisterList
    // might still be called for individual registrations later, and it needs
    // to find the bucket by the original source pointer (the key in _buckets).
  }

} // namespace app::core::model
