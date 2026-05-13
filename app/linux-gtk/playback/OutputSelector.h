// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "playback/AobusSoul.h"
#include "playback/AobusSoulWindow.h"
#include "runtime/PlaybackService.h"
#include <giomm/liststore.h>
#include <gtkmm/button.h>
#include <gtkmm/listbox.h>
#include <gtkmm/popover.h>
#include <memory>

namespace ao::rt
{
  class AppSession;
}

namespace ao::gtk::playback
{
  /**
   * @brief A composite widget for selecting audio output devices and backends.
   */
  class OutputSelector final
  {
  public:
    explicit OutputSelector(ao::rt::AppSession& session);
    ~OutputSelector();

    OutputSelector(OutputSelector const&) = delete;
    OutputSelector& operator=(OutputSelector const&) = delete;

    Gtk::Widget& widget() { return _button; }

  private:
    Gtk::Widget* createRow(Glib::RefPtr<Glib::Object> const& item);
    void rebuildModel();

    ao::rt::AppSession& _session;
    Gtk::Button _button;
    AobusSoul _soul;
    std::unique_ptr<AobusSoulWindow> _soulWindow;
    Gtk::Popover _popover;
    Gtk::ListBox _listBox;
    Glib::RefPtr<Gio::ListStore<Glib::Object>> _store;
  };
} // namespace ao::gtk::playback
