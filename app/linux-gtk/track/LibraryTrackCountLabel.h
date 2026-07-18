// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/async/Subscription.h>
#include <ao/rt/source/TrackSourceDelta.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <gtkmm/label.h>
#include <gtkmm/widget.h>

namespace ao::gtk
{
  /**
   * LibraryTrackCountLabel displays the total number of tracks in the library.
   * It observes the AllTracksSource for changes.
   */
  class LibraryTrackCountLabel final
  {
  public:
    explicit LibraryTrackCountLabel(rt::TrackSourceLease sourceLease);
    ~LibraryTrackCountLabel();

    // Not copyable or movable
    LibraryTrackCountLabel(LibraryTrackCountLabel const&) = delete;
    LibraryTrackCountLabel& operator=(LibraryTrackCountLabel const&) = delete;
    LibraryTrackCountLabel(LibraryTrackCountLabel&&) = delete;
    LibraryTrackCountLabel& operator=(LibraryTrackCountLabel&&) = delete;

    Gtk::Widget& widget() { return _label; }

  private:
    void handleSourceBatch(rt::TrackSourceDeltaBatch const& batch);
    void updateCount();

    rt::TrackSourceLease _sourceLease;
    async::Subscription _sourceSubscription;
    Gtk::Label _label;
  };
} // namespace ao::gtk
