// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <gtkmm.h>

class ImportProgressDialog final : public Gtk::Dialog
{
public:
  ImportProgressDialog(int maxItems, Gtk::Window& parent);
  virtual ~ImportProgressDialog();

  void onNewTrack(std::string const& path, int itemIndex);
  void ready();

private:
  void setupUi(int maxItems);

  Gtk::Label _progressLabel;
  Gtk::ProgressBar _progressBar;
  Gtk::Button _okButton;
  int _maxItems;
};
