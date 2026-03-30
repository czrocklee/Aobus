// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/Type.h>

#include <functional>
#include <optional>
#include <vector>

namespace app::gtkmm4::model
{

using TrackId = rs::core::TrackId;

/**
 * TrackIdListObserver - Observer interface for TrackIdList changes.
 * All notifications are emitted AFTER internal state is updated.
 */
class TrackIdListObserver
{
public:
  virtual ~TrackIdListObserver() = default;

  virtual void onReset() = 0;
  virtual void onInserted(TrackId id, std::size_t index) = 0;
  virtual void onUpdated(TrackId id, std::size_t index) = 0;
  virtual void onRemoved(TrackId id, std::size_t index) = 0;
};

/**
 * TrackIdList - Abstract base class for TrackId membership lists.
 * Provides single-phase observer notification (no begin/end).
 */
class TrackIdList
{
public:
  virtual ~TrackIdList() = default;

  [[nodiscard]] virtual std::size_t size() const = 0;
  [[nodiscard]] virtual TrackId trackIdAt(std::size_t index) const = 0;
  [[nodiscard]] virtual std::optional<std::size_t> indexOf(TrackId id) const = 0;

  void attach(TrackIdListObserver* observer);
  void detach(TrackIdListObserver* observer);

protected:
  TrackIdList() = default;

  void notifyReset();
  void notifyInserted(TrackId id, std::size_t index);
  void notifyUpdated(TrackId id, std::size_t index);
  void notifyRemoved(TrackId id, std::size_t index);

private:
  std::vector<TrackIdListObserver*> _observers;
};

} // namespace app::gtkmm4::model