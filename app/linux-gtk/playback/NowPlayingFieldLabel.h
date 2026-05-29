// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/TrackField.h>
#include <ao/uimodel/playback/NowPlayingViewModel.h>

#include <gtkmm/label.h>
#include <gtkmm/widget.h>

#include <memory>

namespace ao::rt
{
  class AppRuntime;
}

namespace ao::gtk
{
  class NowPlayingFieldLabel final
  {
  public:
    using Action = uimodel::playback::NowPlayingFieldAction;

    NowPlayingFieldLabel(rt::AppRuntime& runtime, rt::TrackField field, Action action = Action::None);

    Gtk::Widget& widget() { return _label; }

  private:
    void applyState(uimodel::playback::NowPlayingViewState const& view);
    void onLabelClicked();

    rt::AppRuntime& _runtime;
    rt::TrackField _field;
    Action _action;
    Gtk::Label _label;

    std::unique_ptr<uimodel::playback::NowPlayingViewModel> _controllerPtr;
  };
} // namespace ao::gtk
