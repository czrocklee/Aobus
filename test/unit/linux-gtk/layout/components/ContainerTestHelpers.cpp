// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/linux-gtk/layout/components/ContainerTestHelpers.h"

#include "app/linux-gtk/layout/runtime/LayoutComponent.h"

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/paned.h>
#include <gtkmm/revealer.h>
#include <gtkmm/widget.h>

namespace ao::gtk::layout::test
{
  Gtk::Box* collapsibleSplitBox(LayoutComponent& component)
  {
    auto& root = component.widget();

    if (auto* const box = dynamic_cast<Gtk::Box*>(&root); box != nullptr)
    {
      return box;
    }

    return dynamic_cast<Gtk::Box*>(root.get_first_child());
  }

  Gtk::Paned* splitPaned(LayoutComponent& component)
  {
    auto& root = component.widget();

    if (auto* const paned = dynamic_cast<Gtk::Paned*>(&root); paned != nullptr)
    {
      return paned;
    }

    return dynamic_cast<Gtk::Paned*>(root.get_first_child());
  }

  Gtk::Revealer* endSideCollapsibleRevealer(Gtk::Box& box)
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

  Gtk::Button* endSideCollapsibleToggle(Gtk::Box& box)
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
