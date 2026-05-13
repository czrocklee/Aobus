// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <runtime/TrackSource.h>

#include <gtkmm/label.h>

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

    // TrackSourceObserver interface
    void onReset() override;
    void onInserted(TrackId id, std::size_t index) override;
    void onUpdated(TrackId id, std::size_t index) override;
    void onRemoved(TrackId id, std::size_t index) override;
    void onSourceDestroyed() override;

    Gtk::Widget& widget() { return _label; }

  private:
    void updateCount();

    rt::TrackSource* _source;
    Gtk::Label _label;
  };
} // namespace ao::gtk
