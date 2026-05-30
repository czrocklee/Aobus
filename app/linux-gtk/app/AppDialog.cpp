// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/AppDialog.h"

#include <gtkmm/button.h>
#include <gtkmm/object.h>
#include <gtkmm/widget.h>
#include <sigc++/signal.h>

#include <cstdint>
#include <string>

namespace ao::gtk
{
  AppDialog::AppDialog()
  {
    _headerBar.set_show_title_buttons(false);
    set_titlebar(_headerBar);
    set_child(_rootBox);

    _rootBox.append(_contentWrapper);
    _contentWrapper.add_css_class("ao-dialog-content");

    // Standard dialogs are usually modal.
    set_modal(true);
  }

  AppDialog::~AppDialog() = default;

  void AppDialog::response(std::int32_t id)
  {
    _signalResponse.emit(id);
  }

  sigc::signal<void(std::int32_t)> AppDialog::signal_response()
  {
    return _signalResponse;
  }

  void AppDialog::setContentWidget(Gtk::Widget& widget)
  {
    // Clear existing content if any
    while (auto* const child = _contentWrapper.get_first_child())
    {
      _contentWrapper.remove(*child);
    }

    _contentWrapper.append(widget);
    widget.set_vexpand(true);
    widget.set_hexpand(true);
  }

  Gtk::Button* AppDialog::addPrimaryAction(std::string const& label, std::int32_t responseId)
  {
    auto* const button = Gtk::make_managed<Gtk::Button>(label);
    button->add_css_class("suggested-action");
    button->signal_clicked().connect([this, responseId] { response(responseId); });
    _headerBar.pack_end(*button);
    return button;
  }

  Gtk::Button* AppDialog::addCancelAction(std::string const& label, std::int32_t responseId)
  {
    auto* const button = Gtk::make_managed<Gtk::Button>(label);
    button->signal_clicked().connect([this, responseId] { response(responseId); });
    _headerBar.pack_start(*button);
    return button;
  }
} // namespace ao::gtk
