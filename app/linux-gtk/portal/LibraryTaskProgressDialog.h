// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/AppDialog.h"

#include <gtkmm/label.h>
#include <gtkmm/progressbar.h>
#include <gtkmm/window.h>

#include <cstdint>
#include <string>

namespace Gtk
{
  class Button;
}

namespace ao::gtk::portal
{
  /**
   * LibraryTaskProgressDialog shows a progress bar and label for long-running library operations.
   */
  class LibraryTaskProgressDialog final : public AppDialog
  {
  public:
    LibraryTaskProgressDialog(std::int32_t maxItems, Gtk::Window& parent);
    ~LibraryTaskProgressDialog() override;

    LibraryTaskProgressDialog(LibraryTaskProgressDialog const&) = delete;
    LibraryTaskProgressDialog& operator=(LibraryTaskProgressDialog const&) = delete;
    LibraryTaskProgressDialog(LibraryTaskProgressDialog&&) = delete;
    LibraryTaskProgressDialog& operator=(LibraryTaskProgressDialog&&) = delete;

    // Restores the in-progress state (running title, "Starting...", 0% progress, disabled OK) so the dialog can be
    // reused for a new operation without showing the previous run's terminal state.
    void beginTask();
    void updateProgress(std::string const& message, double fraction);
    void ready();
    void failed(std::string const& message);

  private:
    void setupUi(std::int32_t maxItems);
    void enableOk();

    Gtk::Label _titleLabel;
    Gtk::Label _progressLabel;
    Gtk::ProgressBar _progressBar;
    Gtk::Button* _okButton = nullptr;
  };
} // namespace ao::gtk::portal
