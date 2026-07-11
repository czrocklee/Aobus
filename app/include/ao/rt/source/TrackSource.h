// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "../Signal.h"
#include "../Subscription.h"
#include "TrackSourceDelta.h"
#include <ao/CoreIds.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>

namespace ao::rt
{
  // Forward declaration
  class SmartListEvaluator;

  enum class TrackSourceState : std::uint8_t
  {
    Live,
    Invalidated,
  };

  /**
   * TrackSource - Abstract base class for TrackId membership lists.
   */
  class TrackSource
  {
  public:
    virtual ~TrackSource();

    TrackSource(TrackSource const&) = delete;
    TrackSource& operator=(TrackSource const&) = delete;
    TrackSource(TrackSource&&) = delete;
    TrackSource& operator=(TrackSource&&) = delete;

    virtual std::size_t size() const = 0;
    virtual TrackId trackIdAt(std::size_t index) const = 0;
    virtual std::optional<std::size_t> indexOf(TrackId id) const = 0;

    std::uint64_t revision() const noexcept { return _revision; }
    TrackSourceState state() const noexcept { return _state; }

    Subscription subscribe(std::move_only_function<void(TrackSourceDeltaBatch const&)> handler);
    void invalidate();

    // Public notification API
    virtual void notifyUpdated(TrackId id);
    virtual void notifyInserted(std::span<TrackId const> ids);
    virtual void notifyUpdated(std::span<TrackId const> ids);

  protected:
    TrackSource() = default;

    void notifyReset();
    void notifyInserted(TrackId id, std::size_t index);
    void notifyUpdated(TrackId id, std::size_t index);
    void notifyRemoved(TrackId id, std::size_t index);

    bool publishDeltaBatch(TrackSourceDeltaBatch batch, std::size_t previousSize);

  private:
    std::uint64_t _revision = 0;
    TrackSourceState _state = TrackSourceState::Live;
    Signal<TrackSourceDeltaBatch const&> _changedSignal;

    friend class SmartListEvaluator;
  };
} // namespace ao::rt
