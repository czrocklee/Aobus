// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/AobusSoul.h"
#include "playback/AobusSoulWindow.h"
#include <ao/uimodel/playback/AobusSoulViewModel.h>

#include <gtkmm/button.h>
#include <gtkmm/widget.h>

#include <functional>
#include <utility>

namespace ao::rt
{
  class PlaybackService;
}

namespace ao::gtk
{
  class OutputSelector final
  {
  public:
    struct Actions final
    {
      std::function<void()> onPrimaryClick;
      std::function<void()> onPrimaryLongPress;
      std::function<void()> onSecondaryClick;
      std::function<void()> onSecondaryLongPress;
    };

    explicit OutputSelector(rt::PlaybackService& playback);
    ~OutputSelector();

    OutputSelector(OutputSelector const&) = delete;
    OutputSelector& operator=(OutputSelector const&) = delete;
    OutputSelector(OutputSelector&&) = delete;
    OutputSelector& operator=(OutputSelector&&) = delete;

    void setActions(Actions actions) { _actions = std::move(actions); }

    Gtk::Widget& widget() { return _button; }

  private:
    friend class OutputSelectorTestPeer;

    rt::PlaybackService& _playback;
    Actions _actions;
    Gtk::Button _button;
    AobusSoul _soul;

    bool _longPressHandled = false;
    uimodel::playback::AobusSoulViewModel _soulController;
  };
} // namespace ao::gtk
