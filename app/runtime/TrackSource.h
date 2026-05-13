// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Type.h>

#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace ao::rt
{
  // Forward declaration
  class SmartListEvaluator;

  /**
   * TrackSourceObserver - Observer interface for TrackSource changes.
   * All notifications are emitted AFTER internal state is updated.
   */
  class TrackSourceObserver
  {
  public:
    virtual ~TrackSourceObserver() = default;

    virtual void onReset() = 0;

    // Single-item notifications for fine-grained UI updates
    virtual void onInserted(TrackId id, std::size_t index) = 0;
    virtual void onUpdated(TrackId id, std::size_t index) = 0;
    virtual void onRemoved(TrackId id, std::size_t index) = 0;

    // Multi-item notifications for performance during bulk operations.
    // Default implementation can fallback to individual notifications if needed.
    virtual void onInserted(std::span<TrackId const> /*ids*/) {}
    virtual void onUpdated(std::span<TrackId const> /*ids*/) {}
    virtual void onRemoved(std::span<TrackId const> /*ids*/) {}

    /**
     * Called when the source list is being destroyed.
     * Observers MUST NOT call any methods on source after this call returns.
     */
    virtual void onSourceDestroyed() {}
  };

  /**
   * TrackSource - Abstract base class for TrackId membership lists.
   */
  class TrackSource
  {
  public:
    virtual ~TrackSource();

    virtual std::size_t size() const = 0;
    virtual TrackId trackIdAt(std::size_t index) const = 0;
    virtual std::optional<std::size_t> indexOf(TrackId id) const = 0;

    void attach(TrackSourceObserver* observer);
    void detach(TrackSourceObserver* observer);

    // Public notification API
    virtual void notifyUpdated(TrackId id);
    virtual void notifyInserted(std::span<TrackId const> ids);
    virtual void notifyUpdated(std::span<TrackId const> ids);
    virtual void notifyRemoved(std::span<TrackId const> ids);

  protected:
    TrackSource() = default;

    void notifyReset();
    void notifyInserted(TrackId id, std::size_t index);
    void notifyUpdated(TrackId id, std::size_t index);
    void notifyRemoved(TrackId id, std::size_t index);

  private:
    std::vector<TrackSourceObserver*> _observers;

    friend class SmartListEvaluator;
  };
}
