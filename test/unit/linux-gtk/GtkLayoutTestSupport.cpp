// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "GtkLayoutTestSupport.h"

#include <gtkmm/enums.h>
#include <gtkmm/widget.h>

#include <cstdint>

namespace ao::gtk::test
{
  AllocationHost::AllocationHost(Gtk::Widget& child)
    : _child{&child}
  {
    _child->set_parent(*this);
  }

  AllocationHost::~AllocationHost()
  {
    if (_child != nullptr)
    {
      _child->unparent();
    }
  }

  void AllocationHost::allocateChild(std::int32_t const width, std::int32_t const height)
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

  Gtk::SizeRequestMode AllocationHost::get_request_mode_vfunc() const
  {
    return Gtk::SizeRequestMode::CONSTANT_SIZE;
  }

  void AllocationHost::measure_vfunc(Gtk::Orientation const orientation,
                                     int /*forSize*/,
                                     int& minimum,
                                     int& natural,
                                     int& minimumBaseline,
                                     int& naturalBaseline) const
  {
    minimum = orientation == Gtk::Orientation::HORIZONTAL ? _width : _height;
    natural = minimum;
    minimumBaseline = -1;
    naturalBaseline = -1;
  }

  void AllocationHost::size_allocate_vfunc(int const width, int const height, int /*baseline*/)
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

  WidgetMeasure measureWidget(Gtk::Widget& widget, Gtk::Orientation const orientation, std::int32_t const forSize)
  {
    auto result = WidgetMeasure{};
    widget.measure(
      orientation, forSize, result.minimum, result.natural, result.minimumBaseline, result.naturalBaseline);
    return result;
  }
} // namespace ao::gtk::test
