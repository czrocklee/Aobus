// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <runtime/CorePrimitives.h>

#include <gtkmm/label.h>

#include <string>

namespace ao::rt
{
  class AppSession;
}

namespace ao::gtk
{
  /**
   * NowPlayingStatusLabel displays the currently playing track's Artist - Title.
   * Clicking the label reveals the track in the UI.
   */
  class NowPlayingStatusLabel final
  {
  public:
    explicit NowPlayingStatusLabel(ao::rt::AppSession& session);
    ~NowPlayingStatusLabel();

    Gtk::Widget& widget() { return _label; }

  private:
    void updateState();

    ao::rt::AppSession& _session;
    Gtk::Label _label;

    ao::rt::Subscription _startedSub;
    ao::rt::Subscription _pausedSub;
    ao::rt::Subscription _idleSub;
    ao::rt::Subscription _stoppedSub;
  };
} // namespace ao::gtk
