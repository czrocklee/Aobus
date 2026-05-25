// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/CorePrimitives.h>
#include <ao/rt/TrackField.h>

#include <gtkmm/label.h>
#include <gtkmm/widget.h>

#include <cstdint>

namespace ao::rt
{
  class AppRuntime;
}

namespace ao::gtk
{
  class NowPlayingFieldLabel final
  {
  public:
    enum class Action : std::uint8_t
    {
      None,
      Reveal,
      PlayPause,
      FilterByField
    };

    NowPlayingFieldLabel(rt::AppRuntime& runtime, rt::TrackField field, Action action = Action::None);

    Gtk::Widget& widget() { return _label; }

  private:
    void refresh();
    void onLabelClicked();

    rt::AppRuntime& _runtime;
    rt::TrackField _field;
    Action _action;
    Gtk::Label _label;

    rt::Subscription _nowPlayingSub;
    rt::Subscription _idleSub;
  };
} // namespace ao::gtk
