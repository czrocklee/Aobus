// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <gtkmm/button.h>
#include <gtkmm/dialog.h>
#include <gtkmm/label.h>
#include <gtkmm/progressbar.h>
#include <gtkmm/window.h>

#include <cstdint>
#include <string>

namespace ao::gtk::portal
{
  class ImportProgressDialog final : public Gtk::Dialog
  {
  public:
    ImportProgressDialog(std::int32_t maxItems, Gtk::Window& parent);
    ~ImportProgressDialog() override;

    // Not copyable or movable
    ImportProgressDialog(ImportProgressDialog const&) = delete;
    ImportProgressDialog& operator=(ImportProgressDialog const&) = delete;
    ImportProgressDialog(ImportProgressDialog&&) = delete;
    ImportProgressDialog& operator=(ImportProgressDialog&&) = delete;

    void onNewTrack(std::string const& path, std::int32_t itemIndex);
    void ready();

  private:
    void setupUi(std::int32_t maxItems);

    Gtk::Label _progressLabel;
    Gtk::ProgressBar _progressBar;
    Gtk::Button _okButton;
    std::int32_t _maxItems;
  };
} // namespace ao::gtk::portal
