// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <runtime/CorePrimitives.h>

#include <gtkmm/box.h>
#include <gtkmm/label.h>
#include <gtkmm/progressbar.h>

namespace ao::rt
{
  class LibraryMutationService;
}

namespace ao::gtk
{
  /**
   * ImportProgressIndicator shows a progress bar and label during active library imports.
   * It self-subscribes to LibraryMutationService events.
   */
  class ImportProgressIndicator final
  {
  public:
    explicit ImportProgressIndicator(rt::LibraryMutationService& mutationService);
    ~ImportProgressIndicator();

    // Not copyable or movable
    ImportProgressIndicator(ImportProgressIndicator const&) = delete;
    ImportProgressIndicator& operator=(ImportProgressIndicator const&) = delete;
    ImportProgressIndicator(ImportProgressIndicator&&) = delete;
    ImportProgressIndicator& operator=(ImportProgressIndicator&&) = delete;

    Gtk::Widget& widget() { return _container; }

  private:
    rt::LibraryMutationService& _mutationService;
    Gtk::Box _container{Gtk::Orientation::HORIZONTAL};
    Gtk::Label _label;
    Gtk::ProgressBar _progressBar;

    rt::Subscription _progressSub;
    rt::Subscription _completedSub;

    static constexpr int kProgressBarWidth = 200;
  };
} // namespace ao::gtk
