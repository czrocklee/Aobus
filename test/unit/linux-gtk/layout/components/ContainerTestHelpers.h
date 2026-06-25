// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/linux-gtk/layout/runtime/ILayoutComponent.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"

#include <glibmm/main.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/enums.h>
#include <gtkmm/paned.h>
#include <gtkmm/revealer.h>
#include <gtkmm/widget.h>

#include <cstdint>

namespace Gtk
{
  class Box;
  class Button;
  class Paned;
  class Revealer;
}

namespace ao::gtk::layout::test
{
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

  class AllocationHost final : public Gtk::Widget
  {
  public:
    explicit AllocationHost(Gtk::Widget& child)
      : _child{&child}
    {
      _child->set_parent(*this);
    }

    ~AllocationHost() override
    {
      if (_child != nullptr)
      {
        _child->unparent();
      }
    }

    AllocationHost(AllocationHost const&) = delete;
    AllocationHost& operator=(AllocationHost const&) = delete;
    AllocationHost(AllocationHost&&) = delete;
    AllocationHost& operator=(AllocationHost&&) = delete;

    void allocateChild(std::int32_t width, std::int32_t height)
    {
      _width = width;
      _height = height;

      std::int32_t minimum = 0;
      std::int32_t natural = 0;
      std::int32_t minimumBaseline = -1;
      std::int32_t naturalBaseline = -1;
      measure(Gtk::Orientation::HORIZONTAL, -1, minimum, natural, minimumBaseline, naturalBaseline);
      measure(Gtk::Orientation::VERTICAL, width, minimum, natural, minimumBaseline, naturalBaseline);

      size_allocate(Gtk::Allocation{0, 0, width, height}, -1);
    }

  protected:
    Gtk::SizeRequestMode get_request_mode_vfunc() const override { return Gtk::SizeRequestMode::CONSTANT_SIZE; }

    void measure_vfunc(Gtk::Orientation orientation,
                       int /*forSize*/,
                       int& minimum,
                       int& natural,
                       int& minimumBaseline,
                       int& naturalBaseline) const override
    {
      minimum = orientation == Gtk::Orientation::HORIZONTAL ? _width : _height;
      natural = minimum;
      minimumBaseline = -1;
      naturalBaseline = -1;
    }

    void size_allocate_vfunc(int width, int height, int /*baseline*/) override
    {
      if (_child == nullptr)
      {
        return;
      }

      std::int32_t minimum = 0;
      std::int32_t natural = 0;
      std::int32_t minimumBaseline = -1;
      std::int32_t naturalBaseline = -1;
      _child->measure(Gtk::Orientation::HORIZONTAL, -1, minimum, natural, minimumBaseline, naturalBaseline);
      _child->measure(Gtk::Orientation::VERTICAL, width, minimum, natural, minimumBaseline, naturalBaseline);

      _child->size_allocate(Gtk::Allocation{0, 0, width, height}, -1);
    }

  private:
    Gtk::Widget* _child = nullptr;
    std::int32_t _width = 0;
    std::int32_t _height = 0;
  };
} // namespace ao::gtk::layout::test
