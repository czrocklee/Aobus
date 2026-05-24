// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/CorePrimitives.h>
#include <ao/rt/LibraryMutationService.h>

#include <gtkmm/box.h>
#include <gtkmm/label.h>
#include <gtkmm/progressbar.h>

namespace ao::gtk::portal
{
  /**
   * LibraryTaskProgressIndicator shows a progress bar and label during active library operations.
   */
  class LibraryTaskProgressIndicator final : public Gtk::Box
  {
  public:
    explicit LibraryTaskProgressIndicator(rt::LibraryMutationService& mutationService);
    ~LibraryTaskProgressIndicator() override;

    LibraryTaskProgressIndicator(LibraryTaskProgressIndicator const&) = delete;
    LibraryTaskProgressIndicator& operator=(LibraryTaskProgressIndicator const&) = delete;
    LibraryTaskProgressIndicator(LibraryTaskProgressIndicator&&) = delete;
    LibraryTaskProgressIndicator& operator=(LibraryTaskProgressIndicator&&) = delete;

  private:
    void setupUi();

    rt::LibraryMutationService& _mutationService;

    Gtk::Label _label;
    Gtk::ProgressBar _progressBar;

    rt::Subscription _progressSub;
    rt::Subscription _completedSub;
  };
} // namespace ao::gtk::portal
