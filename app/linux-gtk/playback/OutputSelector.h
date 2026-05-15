// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/AobusSoul.h"
#include "playback/AobusSoulBinding.h"
#include "playback/AobusSoulWindow.h"
#include "runtime/PlaybackService.h"
#include <giomm/liststore.h>
#include <gtkmm/button.h>
#include <gtkmm/listbox.h>
#include <gtkmm/popover.h>
#include <memory>

namespace ao::rt
{
  class PlaybackService;
}

namespace ao::gtk
{
  class OutputSelector final
  {
  public:
    explicit OutputSelector(rt::PlaybackService& playback);
    ~OutputSelector();

    OutputSelector(OutputSelector const&) = delete;
    OutputSelector& operator=(OutputSelector const&) = delete;
    OutputSelector(OutputSelector&&) = delete;
    OutputSelector& operator=(OutputSelector&&) = delete;

    Gtk::Widget& widget() { return _button; }

  private:
    Gtk::Widget* createRow(Glib::RefPtr<Glib::Object> const& item);
    void rebuildModel();

    rt::PlaybackService& _playback;
    Gtk::Button _button;
    AobusSoul _soul;
    std::unique_ptr<AobusSoulBinding> _soulBinding;
    std::unique_ptr<AobusSoulWindow> _soulWindow;
    Gtk::Popover _popover;
    Gtk::ListBox _listBox;
    Glib::RefPtr<Gio::ListStore<Glib::Object>> _store;
  };
} // namespace ao::gtk
