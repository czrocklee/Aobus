// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/FormBuilder.h"

#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
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
    : _labelSizeGroup{Gtk::SizeGroup::create(Gtk::SizeGroup::Mode::HORIZONTAL)}
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
    label->set_halign(Gtk::Align::START);
    _labelSizeGroup->add_widget(*label);

    box->append(*label);

    widget.set_hexpand(true);
    widget.set_halign(Gtk::Align::FILL);
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

namespace ao::gtk::FormBuilder
{
  namespace
  {
    constexpr int kRowSpacingLegacy = 12;
    constexpr int kLegacyLabelWidth = 120;
  }

  Gtk::ListBox* createBoxedList()
  {
    auto* const list = Gtk::make_managed<Gtk::ListBox>();
    list->add_css_class("ao-boxed-list");
    list->set_selection_mode(Gtk::SelectionMode::NONE);
    return list;
  }

  Gtk::ListBoxRow* createFormRow(std::string const& labelText, Gtk::Widget& widget)
  {
    auto* const row = Gtk::make_managed<Gtk::ListBoxRow>();
    auto* const box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, kRowSpacingLegacy);

    auto* const label = Gtk::make_managed<Gtk::Label>(labelText);
    label->set_halign(Gtk::Align::START);
    label->set_size_request(kLegacyLabelWidth, -1);

    box->append(*label);

    widget.set_hexpand(true);
    widget.set_halign(Gtk::Align::FILL);
    box->append(widget);

    row->set_child(*box);
    row->set_activatable(false);
    return row;
  }

  Gtk::ListBoxRow* createEntryRow(std::string const& labelText, Gtk::Widget& entry)
  {
    entry.add_css_class("flat-entry");
    return createFormRow(labelText, entry);
  }
} // namespace ao::gtk::FormBuilder
