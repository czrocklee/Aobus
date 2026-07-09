// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/source/TrackSource.h>

#include <gtkmm/label.h>
#include <gtkmm/widget.h>

#include <cstddef>

namespace ao::gtk
{
  /**
   * LibraryTrackCountLabel displays the total number of tracks in the library.
   * It observes the AllTracksSource for changes.
   */
  class LibraryTrackCountLabel final : public rt::TrackSourceObserver
  {
  public:
    explicit LibraryTrackCountLabel(rt::TrackSource& source);
    ~LibraryTrackCountLabel() override;

    // Not copyable or movable
    LibraryTrackCountLabel(LibraryTrackCountLabel const&) = delete;
    LibraryTrackCountLabel& operator=(LibraryTrackCountLabel const&) = delete;
    LibraryTrackCountLabel(LibraryTrackCountLabel&&) = delete;
    LibraryTrackCountLabel& operator=(LibraryTrackCountLabel&&) = delete;

    // TrackSourceObserver interface
    void handleReset() override;
    void handleInserted(TrackId id, std::size_t index) override;
    void handleUpdated(TrackId id, std::size_t index) override;
    void handleRemoved(TrackId id, std::size_t index) override;
    void handleSourceDestroyed() override;

    Gtk::Widget& widget() { return _label; }

  private:
    void updateCount();

    rt::TrackSource* _source;
    Gtk::Label _label;
  };
} // namespace ao::gtk
