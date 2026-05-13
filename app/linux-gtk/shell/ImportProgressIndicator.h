// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <runtime/CorePrimitives.h>

#include <gtkmm/box.h>
#include <gtkmm/label.h>
#include <gtkmm/progressbar.h>

namespace ao::rt
{
  class AppSession;
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
    explicit ImportProgressIndicator(ao::rt::AppSession& session);
    ~ImportProgressIndicator();

    Gtk::Widget& widget() { return _container; }

  private:
    ao::rt::AppSession& _session;
    Gtk::Box _container{Gtk::Orientation::HORIZONTAL};
    Gtk::Label _label;
    Gtk::ProgressBar _progressBar;

    ao::rt::Subscription _progressSub;
    ao::rt::Subscription _completedSub;

    static constexpr int kProgressBarWidth = 200;
  };
} // namespace ao::gtk
