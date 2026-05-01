// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/Type.h>

#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace rs::model
{
  using TrackId = rs::TrackId;

  // Forward declaration
  class SmartListEngine;

  /**
   * TrackIdListObserver - Observer interface for TrackIdList changes.
   * All notifications are emitted AFTER internal state is updated.
   */
  class TrackIdListObserver
  {
  public:
    virtual ~TrackIdListObserver() = default;

    virtual void onReset() = 0;

    // Single-item notifications for fine-grained UI updates
    virtual void onInserted(TrackId id, std::size_t index) = 0;
    virtual void onUpdated(TrackId id, std::size_t index) = 0;
    virtual void onRemoved(TrackId id, std::size_t index) = 0;

    // Batch notifications for performance during bulk operations.
    // Default implementation can fallback to individual notifications if needed,
    // or observers can override for better batch processing.
    virtual void onBatchInserted(std::span<TrackId const> /*ids*/) {}
    virtual void onBatchUpdated(std::span<TrackId const> /*ids*/) {}
    virtual void onBatchRemoved(std::span<TrackId const> /*ids*/) {}

    /**
     * Called when the source list is being destroyed.
     * Observers MUST NOT call any methods on source after this call returns.
     */
    virtual void onSourceDestroyed() {}
  };

  /**
   * TrackIdList - Abstract base class for TrackId membership lists.
   */
  class TrackIdList
  {
  public:
    virtual ~TrackIdList();

    virtual std::size_t size() const = 0;
    virtual TrackId trackIdAt(std::size_t index) const = 0;
    virtual std::optional<std::size_t> indexOf(TrackId id) const = 0;

    void attach(TrackIdListObserver* observer);
    void detach(TrackIdListObserver* observer);

    // Notify observers that a track's data changed (tags, etc.)
    virtual void notifyTrackDataChanged(TrackId id);

  protected:
    TrackIdList() = default;

    void notifyReset();
    void notifyInserted(TrackId id, std::size_t index);
    void notifyUpdated(TrackId id, std::size_t index);
    void notifyRemoved(TrackId id, std::size_t index);

    void notifyBatchInserted(std::span<TrackId const> ids);
    void notifyBatchUpdated(std::span<TrackId const> ids);
    void notifyBatchRemoved(std::span<TrackId const> ids);

  private:
    std::vector<TrackIdListObserver*> _observers;

    friend class SmartListEngine;
  };
} // namespace rs::model