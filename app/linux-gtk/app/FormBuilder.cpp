// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/FormBuilder.h"

#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/listboxrow.h>
#include <gtkmm/object.h>
#include <gtkmm/sizegroup.h>
#include <gtkmm/widget.h>

#include <string>

namespace ao::gtk
{
  namespace
  {
    constexpr int kRowSpacing = 12;
  }

  FormBoxedList::FormBoxedList()
    : _labelSizeGroupPtr{Gtk::SizeGroup::create(Gtk::SizeGroup::Mode::HORIZONTAL)}
  {
    add_css_class("ao-boxed-list");
    set_selection_mode(Gtk::SelectionMode::NONE);
  }

  FormBoxedList::~FormBoxedList() = default;

  void FormBoxedList::addRow(std::string const& labelText, Gtk::Widget& widget)
  {
    auto* const row = Gtk::make_managed<Gtk::ListBoxRow>();
    auto* const box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, kRowSpacing);

    auto* const label = Gtk::make_managed<Gtk::Label>(labelText);
    label->set_halign(Gtk::Align::FILL);
    label->set_xalign(0.0F);
    label->set_hexpand(true);
    _labelSizeGroupPtr->add_widget(*label);

    box->append(*label);

    widget.set_halign(Gtk::Align::END);
    widget.set_valign(Gtk::Align::CENTER);
    box->append(widget);

    row->set_child(*box);
    row->set_activatable(false);
    append(*row);
  }

  void FormBoxedList::addEntryRow(std::string const& labelText, Gtk::Widget& entry)
  {
    entry.add_css_class("flat-entry");
    addRow(labelText, entry);
  }
} // namespace ao::gtk
