// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

namespace ao::gtk::layout
{
  class LayoutComponent;
}

namespace Gtk
{
  class Box;
  class Button;
  class Paned;
  class Revealer;
}

namespace ao::gtk::layout::test
{
  Gtk::Box* collapsibleSplitBox(LayoutComponent& component);
  Gtk::Paned* splitPaned(LayoutComponent& component);
  Gtk::Revealer* endSideCollapsibleRevealer(Gtk::Box& box);
  Gtk::Button* endSideCollapsibleToggle(Gtk::Box& box);
} // namespace ao::gtk::layout::test
