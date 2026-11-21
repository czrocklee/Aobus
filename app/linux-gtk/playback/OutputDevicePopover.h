// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/OutputDeviceSelection.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/playback/output/OutputDeviceIntent.h>
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>

#include <glibmm/object.h>
#include <glibmm/refptr.h>
#include <gtkmm/enums.h>
#include <gtkmm/listbox.h>
#include <gtkmm/popover.h>
#include <gtkmm/widget.h>
#include <sigc++/scoped_connection.h>

#include <optional>

namespace Gtk
{
  class Switch;
}

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

    /// Disconnect Playback observation and make the native chooser events inert.
    /// The owner must still defer destruction until the current GTK dispatch returns.
    void retire();

  private:
    void applyState(uimodel::OutputDeviceViewState const& view);
    Gtk::Widget* createRow(Glib::RefPtr<Glib::Object> const& itemPtr);
    Gtk::Widget* createRoutingPolicyRow(uimodel::OutputDeviceRow const& policy,
                                        audio::OutputDeviceSelection const& expectedSelection);
    void queueRoutingPolicyChange(audio::OutputDeviceSelection const& expectedSelection);

    Gtk::ListBox _listBox;
    Glib::RefPtr<Gio::ListStore<Glib::Object>> _storePtr{};
    Gtk::Switch* _routingPolicySwitch = nullptr;
    bool _retired = false;
    uimodel::OutputDeviceViewModel _outputDeviceViewModel;
    std::optional<audio::OutputDeviceSelection> _optQueuedRoutingPolicySelection;
    sigc::scoped_connection _routingPolicyConnection;
    sigc::scoped_connection _queuedRoutingPolicyConnection;
  };
} // namespace ao::gtk
