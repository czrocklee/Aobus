// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/PlaybackState.h>
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>

#include <glibmm/object.h>
#include <glibmm/refptr.h>
#include <gtkmm/enums.h>
#include <gtkmm/listbox.h>
#include <gtkmm/popover.h>
#include <gtkmm/widget.h>

#include <functional>

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
  class OutputDeviceSelector final : public Gtk::Popover
  {
  public:
    OutputDeviceSelector(OutputDeviceSelector const&) = delete;
    OutputDeviceSelector& operator=(OutputDeviceSelector const&) = delete;
    OutputDeviceSelector(OutputDeviceSelector&&) = delete;
    OutputDeviceSelector& operator=(OutputDeviceSelector&&) = delete;

    explicit OutputDeviceSelector(rt::PlaybackService& playback,
                                  Gtk::PositionType position = Gtk::PositionType::BOTTOM,
                                  std::function<void(rt::OutputDeviceSelection const&)> onSelected = {});
    ~OutputDeviceSelector() override;

  private:
    Gtk::Widget* createRow(Glib::RefPtr<Glib::Object> const& item);

    rt::PlaybackService& _playback;
    std::function<void(rt::OutputDeviceSelection const&)> _onSelected;

    Gtk::ListBox _listBox;
    Glib::RefPtr<Gio::ListStore<Glib::Object>> _storePtr{};
    uimodel::OutputDeviceViewModel _outputDeviceViewModel;
  };
} // namespace ao::gtk
