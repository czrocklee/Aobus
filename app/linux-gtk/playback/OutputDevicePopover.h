// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/playback/output/OutputDeviceIntent.h>
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>

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
  class OutputDevicePopover final : public Gtk::Popover
  {
  public:
    OutputDevicePopover(OutputDevicePopover const&) = delete;
    OutputDevicePopover& operator=(OutputDevicePopover const&) = delete;
    OutputDevicePopover(OutputDevicePopover&&) = delete;
    OutputDevicePopover& operator=(OutputDevicePopover&&) = delete;

    OutputDevicePopover(rt::PlaybackService& playback,
                        i18n::MessageCatalog const& textCatalog,
                        uimodel::OutputDeviceIntent intent,
                        Gtk::PositionType position = Gtk::PositionType::BOTTOM);
    ~OutputDevicePopover() override;

    /// Disconnect Playback observation and make the show/selection adapters inert.
    /// The owner must still defer destruction until the current GTK dispatch returns.
    void retire();

  private:
    Gtk::Widget* createRow(Glib::RefPtr<Glib::Object> const& itemPtr);

    Gtk::ListBox _listBox;
    Glib::RefPtr<Gio::ListStore<Glib::Object>> _storePtr{};
    uimodel::OutputDeviceViewModel _outputDeviceViewModel;
    bool _retired = false;
  };
} // namespace ao::gtk
