// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <gtkmm.h>

#include <string>

class TagPromptDialog final : public Gtk::Dialog
{
public:
  TagPromptDialog(Gtk::Window& parent);
  virtual ~TagPromptDialog() = default;

  std::string tag() const;

private:
  void setupUi();

  Gtk::Entry _tagEntry;
  Gtk::Button _okButton;
  Gtk::Button _cancelButton;
};
