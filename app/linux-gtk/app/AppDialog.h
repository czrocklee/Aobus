// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/enums.h>
#include <gtkmm/headerbar.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>
#include <sigc++/signal.h>

#include <cstdint>
#include <string>

namespace ao::gtk
{
  /**
   * AppDialog provides a consistent UI/UX for dialogs in Aobus.
   *
   * It uses a Gtk::HeaderBar for actions and provides a Gtk::Dialog-compatible
   * response API to ensure smooth migration.
   */
  class AppDialog : public Gtk::Window
  {
  public:
    AppDialog();
    ~AppDialog() override;

    AppDialog(AppDialog const&) = delete;
    AppDialog& operator=(AppDialog const&) = delete;
    AppDialog(AppDialog&&) = delete;
    AppDialog& operator=(AppDialog&&) = delete;

    /**
     * Emits signal_response with the given ID.
     */
    void response(std::int32_t id);

    /**
     * Compatibility signal that matches Gtk::Dialog's response signal.
     */
    sigc::signal<void(std::int32_t)> signal_response();

    /**
     * Sets the main content widget of the dialog.
     * It will be wrapped in a container with standard dialog padding.
     */
    void setContentWidget(Gtk::Widget& widget);

    /**
     * Adds a primary (suggested) action to the right side of the header bar.
     */
    Gtk::Button* addPrimaryAction(std::string const& label, std::int32_t responseId);

    /**
     * Adds a cancel action to the left side of the header bar.
     */
    Gtk::Button* addCancelAction(std::string const& label, std::int32_t responseId);

  private:
    Gtk::HeaderBar _headerBar;
    Gtk::Box _rootBox{Gtk::Orientation::VERTICAL};
    Gtk::Box _contentWrapper;
    sigc::signal<void(std::int32_t)> _signalResponse;
  };
} // namespace ao::gtk
