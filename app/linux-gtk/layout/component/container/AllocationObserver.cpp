// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "AllocationObserver.h"

#include <gtkmm/enums.h>
#include <gtkmm/widget.h>

#include <cstdint>

namespace ao::gtk::layout
{
  AllocationObserver::~AllocationObserver()
  {
    clearChild();
  }

  void AllocationObserver::setChild(Gtk::Widget& child)
  {
    clearChild();
    _child = &child;
    _child->set_parent(*this);
  }

  void AllocationObserver::clearChild()
  {
    if (_child == nullptr)
    {
      return;
    }

    _child->unparent();
    _child = nullptr;
  }

  Gtk::SizeRequestMode AllocationObserver::get_request_mode_vfunc() const
  {
    if (_child != nullptr)
    {
      return _child->get_request_mode();
    }

    return Gtk::SizeRequestMode::CONSTANT_SIZE;
  }

  void AllocationObserver::measure_vfunc(Gtk::Orientation const orientation,
                                         int const forSize,
                                         int& minimum,
                                         int& natural,
                                         int& minimumBaseline,
                                         int& naturalBaseline) const
  {
    if (_child != nullptr)
    {
      _child->measure(orientation, forSize, minimum, natural, minimumBaseline, naturalBaseline);
      return;
    }

    minimum = 0;
    natural = 0;
    minimumBaseline = -1;
    naturalBaseline = -1;
  }

  void AllocationObserver::size_allocate_vfunc(int const width, int const height, int const baseline)
  {
    if (_callback)
    {
      _callback(width, height);
    }

    if (_child != nullptr)
    {
      std::int32_t minimum = 0;
      std::int32_t natural = 0;
      std::int32_t minimumBaseline = -1;
      std::int32_t naturalBaseline = -1;
      _child->measure(Gtk::Orientation::HORIZONTAL, -1, minimum, natural, minimumBaseline, naturalBaseline);
      _child->measure(Gtk::Orientation::VERTICAL, width, minimum, natural, minimumBaseline, naturalBaseline);
      _child->size_allocate({0, 0, width, height}, baseline);
    }
  }
} // namespace ao::gtk::layout
