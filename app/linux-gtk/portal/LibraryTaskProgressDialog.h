// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <gtkmm/dialog.h>
#include <gtkmm/label.h>
#include <gtkmm/progressbar.h>

#include <cstdint>
#include <string>

namespace ao::gtk::portal
{
  /**
   * LibraryTaskProgressDialog shows a progress bar and label for long-running library operations.
   */
  class LibraryTaskProgressDialog final : public Gtk::Dialog
  {
  public:
    LibraryTaskProgressDialog(std::int32_t maxItems, Gtk::Window& parent);
    ~LibraryTaskProgressDialog() override;

    LibraryTaskProgressDialog(LibraryTaskProgressDialog const&) = delete;
    LibraryTaskProgressDialog& operator=(LibraryTaskProgressDialog const&) = delete;
    LibraryTaskProgressDialog(LibraryTaskProgressDialog&&) = delete;
    LibraryTaskProgressDialog& operator=(LibraryTaskProgressDialog&&) = delete;

    void updateProgress(std::string const& message, double fraction);
    void ready();

  private:
    void setupUi(std::int32_t maxItems);

    Gtk::Label _titleLabel;
    Gtk::Label _progressLabel;
    Gtk::ProgressBar _progressBar;
  };
} // namespace ao::gtk::portal
