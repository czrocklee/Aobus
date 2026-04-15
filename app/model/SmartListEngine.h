// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "TrackIdList.h"

#include <rs/core/MusicLibrary.h>
#include <rs/expr/ExecutionPlan.h>
#include <rs/expr/PlanEvaluator.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rs::expr
{
  class ExecutionPlan;
  class PlanEvaluator;
}

namespace app::model
{

  // Forward declaration
  class SmartListEngine;

  // SourceObserver - handles source list events and dispatches to engine
  class SourceObserver final : public TrackIdListObserver
  {
  public:
    explicit SourceObserver(SmartListEngine& engine, TrackIdList& source);

    void onReset() override;
    void onInserted(TrackId id, std::size_t index) override;
    void onUpdated(TrackId id, std::size_t index) override;
    void onRemoved(TrackId id, std::size_t index) override;
    void onSourceDestroyed() override;

    void invalidate() { _valid = false; }

  private:
    SmartListEngine& _engine;
    TrackIdList& _source;
    bool _valid = true;  // Set to false when engine is being destroyed
  };

  /**
   * SmartListEngine - Batching expression evaluator for smart list filtering.
   *
   * SmartListEngine batches evaluation of multiple smart lists that share
   * the same source TrackIdList. When the source changes, all dependent
   * lists are evaluated together for efficiency.
   *
   * Inherited Filtering Support:
   * The engine supports inherited filtering through source chaining:
   * - Each registered list has a source TrackIdList
   * - The source may itself be a FilteredTrackIdList (from a parent)
   * - When source changes, the engine evaluates all dependent lists
   *
   * This enables chains like: AllTracks → FilteredTrackIdList(Beatles) →
   * FilteredTrackIdList(Late Beatles), where each level inherits from its parent.
   */
  class SmartListEngine final
  {
  public:
    using RegistrationId = std::uint64_t;

    explicit SmartListEngine(rs::core::MusicLibrary& ml);
    ~SmartListEngine();

    // Disable copy/move
    SmartListEngine(SmartListEngine const&) = delete;
    SmartListEngine& operator=(SmartListEngine const&) = delete;
    SmartListEngine(SmartListEngine&&) = delete;
    SmartListEngine& operator=(SmartListEngine&&) = delete;

    bool isAlive() const { return _alive; }
    RegistrationId registerList(TrackIdList& source, TrackIdList& facade);
    void unregisterList(RegistrationId id);

    void setExpression(RegistrationId id, std::string expr);
    void rebuild(RegistrationId id);

    std::size_t size(RegistrationId id) const;
    TrackId trackIdAt(RegistrationId id, std::size_t index) const;
    std::optional<std::size_t> indexOf(RegistrationId id, TrackId trackId) const;

    bool hasError(RegistrationId id) const;
    std::string const& errorMessage(RegistrationId id) const;

  private:
    struct SmartListState;
    struct SourceBucket;

    void stageExpression(SmartListState& state, std::string expr);
    void applyStagedState(SmartListState& state);
    void rebuildActiveStates(SourceBucket& bucket);
    void rebuildDirtyStates(SourceBucket& bucket);
    void rebuildStates(std::span<SmartListState*> states);
    void rebuildGroup(TrackIdList& source,
                      std::span<SmartListState*> states,
                      rs::core::TrackStore::Reader::LoadMode mode);

    void handleSourceReset(SourceBucket& bucket);
    void handleSourceInserted(SourceBucket& bucket, TrackId id, std::size_t sourceIndex);
    void handleSourceUpdated(SourceBucket& bucket, TrackId id, std::size_t sourceIndex);
    void handleSourceRemoved(SourceBucket& bucket, TrackId id);
    void handleSourceDestroyed(SourceBucket& bucket);

    static std::size_t insertionIndexForSourceOrder(SmartListState const& state,
                                                    std::size_t sourceIndex);

    void notifyFacadeReset(TrackIdList& facade);
    void notifyFacadeInserted(TrackIdList& facade, TrackId id, std::size_t index);
    void notifyFacadeUpdated(TrackIdList& facade, TrackId id, std::size_t index);
    void notifyFacadeRemoved(TrackIdList& facade, TrackId id, std::size_t index);

    rs::core::MusicLibrary* _ml;

    RegistrationId _nextRegistrationId = 1;
    std::map<RegistrationId, std::unique_ptr<SmartListState>> _states;
    std::map<TrackIdList*, std::unique_ptr<SourceBucket>> _buckets;
    bool _alive = true;

    friend class SourceObserver;
  };

} // namespace app::model
