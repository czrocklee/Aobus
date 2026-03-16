// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/fbs/List_generated.h>

#include <gtkmm.h>

#include <string>

class NewListDialog : public Gtk::Dialog
{
public:
  NewListDialog(Gtk::Window& parent);
  virtual ~NewListDialog() = default;

  rs::fbs::ListT list() const;

private:
  void setupUi();

  Gtk::Entry _nameEntry;
  Gtk::Entry _descEntry;
  Gtk::Entry _exprEntry;
  Gtk::Button _okButton;
  Gtk::Button _cancelButton;
};
