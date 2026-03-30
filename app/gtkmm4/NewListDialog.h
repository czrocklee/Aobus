// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "model/ListDraft.h"

#include <gtkmm.h>

#include <string>

class NewListDialog : public Gtk::Dialog
{
public:
  NewListDialog(Gtk::Window& parent);
  virtual ~NewListDialog() = default;

  // Returns a ListDraft populated from the dialog fields
  app::gtkmm4::model::ListDraft draft() const;

private:
  void setupUi();

  Gtk::Entry _nameEntry;
  Gtk::Entry _descEntry;
  Gtk::Entry _exprEntry;
  Gtk::Button _okButton;
  Gtk::Button _cancelButton;
};