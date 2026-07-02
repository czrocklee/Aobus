// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/linux-gtk/layout/runtime/ILayoutComponent.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"

#include <glibmm/main.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/paned.h>
#include <gtkmm/revealer.h>
#include <gtkmm/widget.h>

namespace Gtk
{
  class Box;
  class Button;
  class Paned;
  class Revealer;
}

namespace ao::gtk::layout::test
{
  using ao::gtk::test::AllocationHost;
  using ao::gtk::test::drainGtkEventsFor;
  using ao::gtk::test::measureWidget;
  using ao::gtk::test::WidgetMeasure;

  inline Gtk::Box* collapsibleSplitBox(ILayoutComponent& component)
  {
    auto& root = component.widget();

    if (auto* const box = dynamic_cast<Gtk::Box*>(&root); box != nullptr)
    {
      return box;
    }

    return dynamic_cast<Gtk::Box*>(root.get_first_child());
  }

  inline Gtk::Paned* splitPaned(ILayoutComponent& component)
  {
    auto& root = component.widget();

    if (auto* const paned = dynamic_cast<Gtk::Paned*>(&root); paned != nullptr)
    {
      return paned;
    }

    return dynamic_cast<Gtk::Paned*>(root.get_first_child());
  }

  inline Gtk::Revealer* endSideCollapsibleRevealer(Gtk::Box& box)
  {
    auto* const workspace = box.get_first_child();

    if (workspace == nullptr)
    {
      return nullptr;
    }

    auto* const gutterBox = workspace->get_next_sibling();

    if (gutterBox == nullptr)
    {
      return nullptr;
    }

    return dynamic_cast<Gtk::Revealer*>(gutterBox->get_next_sibling());
  }

  inline Gtk::Button* endSideCollapsibleToggle(Gtk::Box& box)
  {
    auto* const workspace = box.get_first_child();

    if (workspace == nullptr)
    {
      return nullptr;
    }

    auto* const gutterBox = workspace->get_next_sibling();

    if (gutterBox == nullptr)
    {
      return nullptr;
    }

    return dynamic_cast<Gtk::Button*>(gutterBox->get_first_child());
  }
} // namespace ao::gtk::layout::test
