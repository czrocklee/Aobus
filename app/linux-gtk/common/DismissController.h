// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <glibmm/refptr.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <functional>
#include <initializer_list>
#include <span>
#include <vector>

namespace ao::gtk
{
  // True when `target`, or any of its ancestors, is one of `insideWidgets`. Pure widget-tree walk
  // (no geometry/allocation) — the geometric hit-test (`Gtk::Window::pick`) stays at the call site.
  bool isWidgetWithinAny(Gtk::Widget const* target, std::span<Gtk::Widget* const> insideWidgets);

  class DismissController final
  {
  public:
    DismissController() = default;
    ~DismissController();

    DismissController(DismissController const&) = delete;
    DismissController& operator=(DismissController const&) = delete;
    DismissController(DismissController&&) = delete;
    DismissController& operator=(DismissController&&) = delete;

    void install(Gtk::Widget& rootedWidget,
                 std::initializer_list<Gtk::Widget*> insideWidgets,
                 std::function<void()> onDismiss);
    void remove();

  private:
    bool pressIsInside(Gtk::Widget const* target) const;

    Glib::RefPtr<Gtk::GestureClick> _clickPtr;
    Gtk::Window* _watchedWindow = nullptr;
    std::vector<Gtk::Widget*> _insideWidgets;
    std::function<void()> _onDismiss;
  };
} // namespace ao::gtk
