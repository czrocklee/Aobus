// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <glibmm/refptr.h>
#include <gtkmm/listbox.h>
#include <gtkmm/sizegroup.h>
#include <gtkmm/widget.h>

#include <string>

namespace ao::gtk
{
  /**
   * FormBoxedList manages a boxed list and a SizeGroup to ensure all labels align.
   */
  class FormBoxedList : public Gtk::ListBox
  {
  public:
    FormBoxedList();
    ~FormBoxedList() override;

    FormBoxedList(FormBoxedList const&) = delete;
    FormBoxedList& operator=(FormBoxedList const&) = delete;
    FormBoxedList(FormBoxedList&&) = delete;
    FormBoxedList& operator=(FormBoxedList&&) = delete;

    void addRow(std::string const& labelText, Gtk::Widget& widget);
    void addEntryRow(std::string const& labelText, Gtk::Widget& entry);

  private:
    Glib::RefPtr<Gtk::SizeGroup> _labelSizeGroupPtr;
  };
} // namespace ao::gtk
