// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <runtime/CorePrimitives.h>

#include <gtkmm/label.h>

namespace ao::rt
{
  class AppSession;
}

namespace ao::gtk
{
  /**
   * LibraryTrackCountLabel displays the total number of tracks in the library.
   * it self-subscribes to LibraryMutationService import completion.
   */
  class LibraryTrackCountLabel final
  {
  public:
    explicit LibraryTrackCountLabel(ao::rt::AppSession& session);
    ~LibraryTrackCountLabel();

    Gtk::Widget& widget() { return _label; }

  private:
    void updateCount();

    ao::rt::AppSession& _session;
    Gtk::Label _label;
    ao::rt::Subscription _importCompletedSub;
  };
} // namespace ao::gtk
