// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <gtkmm/popover.h>
#include <gtkmm/widget.h>
#include <sigc++/scoped_connection.h>

#include <memory>
#include <vector>

namespace ao::gtk
{
  /**
   * Owns a short-lived popover attached to an anchor widget.
   *
   * Closing the popover or unmapping its anchor unparents the popover
   * immediately, but defers final destruction until the current GTK dispatch
   * stack has returned. Stable member popovers and popovers with multi-stage
   * state belong in their owning controller.
   */
  class PopoverAttachment final
  {
  public:
    PopoverAttachment() = default;
    ~PopoverAttachment();

    PopoverAttachment(PopoverAttachment const&) = delete;
    PopoverAttachment& operator=(PopoverAttachment const&) = delete;
    PopoverAttachment(PopoverAttachment&&) = delete;
    PopoverAttachment& operator=(PopoverAttachment&&) = delete;

    bool hasPopover() const;
    void attach(std::unique_ptr<Gtk::Popover> popoverPtr, Gtk::Widget& anchor);
    void popup();
    void detach();

  private:
    void retireAfterDispatch();
    void detachActivePopover();

    std::unique_ptr<Gtk::Popover> _popoverPtr;
    sigc::scoped_connection _closedConnection;
    sigc::scoped_connection _anchorUnmapConnection;
    std::vector<std::unique_ptr<Gtk::Popover>> _retiredPopoverPtrs;
    sigc::scoped_connection _retirementConnection;
  };
} // namespace ao::gtk
