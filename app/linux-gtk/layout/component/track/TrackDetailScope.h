// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/ProjectionTypes.h>

#include <sigc++/signal.h>

namespace ao::gtk::layout
{
  /**
   * @brief Interface for track detail data scope providers.
   */
  class ITrackDetailScope
  {
  public:
    ITrackDetailScope() = default;
    virtual ~ITrackDetailScope() = default;
    ITrackDetailScope(ITrackDetailScope const&) = delete;
    ITrackDetailScope& operator=(ITrackDetailScope const&) = delete;
    ITrackDetailScope(ITrackDetailScope&&) = delete;
    ITrackDetailScope& operator=(ITrackDetailScope&&) = delete;

    virtual rt::TrackDetailSnapshot const& snapshot() const = 0;
    virtual bool isEditLocked() const = 0;
    virtual void setEditLocked(bool locked) = 0;

    virtual sigc::signal<void(rt::TrackDetailSnapshot const&)>& signalSnapshotChanged() = 0;
    virtual sigc::signal<void(bool)>& signalEditLockChanged() = 0;
  };
} // namespace ao::gtk::layout
