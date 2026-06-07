// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/playback/AudioOutputViewModel.h>

#include <glibmm/object.h>
#include <glibmm/refptr.h>
#include <gtkmm/enums.h>
#include <gtkmm/listbox.h>
#include <gtkmm/popover.h>
#include <gtkmm/widget.h>

namespace Gio
{
  template<typename T>
  class ListStore;
}

namespace ao::rt
{
  class PlaybackService;
}

namespace ao::gtk
{
  class AudioDeviceSelector final : public Gtk::Popover
  {
  public:
    AudioDeviceSelector(AudioDeviceSelector const&) = delete;
    AudioDeviceSelector& operator=(AudioDeviceSelector const&) = delete;
    AudioDeviceSelector(AudioDeviceSelector&&) = delete;
    AudioDeviceSelector& operator=(AudioDeviceSelector&&) = delete;

    explicit AudioDeviceSelector(rt::PlaybackService& playback, Gtk::PositionType position = Gtk::PositionType::BOTTOM);
    ~AudioDeviceSelector() override;

  private:
    Gtk::Widget* createRow(Glib::RefPtr<Glib::Object> const& item);

    rt::PlaybackService& _playback;

    Gtk::ListBox _listBox;
    Glib::RefPtr<Gio::ListStore<Glib::Object>> _storePtr{};
    uimodel::playback::AudioOutputViewModel _outputController;
  };
} // namespace ao::gtk
