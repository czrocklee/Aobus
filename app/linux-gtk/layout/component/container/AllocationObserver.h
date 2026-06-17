// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <gtkmm/enums.h>
#include <gtkmm/widget.h>

#include <cstdint>
#include <functional>
#include <utility>

namespace ao::gtk::layout
{
  class AllocationObserver final : public Gtk::Widget
  {
  public:
    using AllocatedCallback = std::function<void(std::int32_t, std::int32_t)>;

    AllocationObserver() = default;
    ~AllocationObserver() override { clearChild(); }

    AllocationObserver(AllocationObserver const&) = delete;
    AllocationObserver& operator=(AllocationObserver const&) = delete;
    AllocationObserver(AllocationObserver&&) = delete;
    AllocationObserver& operator=(AllocationObserver&&) = delete;

    void setChild(Gtk::Widget& child)
    {
      clearChild();
      _child = &child;
      _child->set_parent(*this);
    }

    void clearChild()
    {
      if (_child == nullptr)
      {
        return;
      }

      _child->unparent();
      _child = nullptr;
    }

    void setAllocatedCallback(AllocatedCallback callback) { _callback = std::move(callback); }

  protected:
    Gtk::SizeRequestMode get_request_mode_vfunc() const override
    {
      if (_child != nullptr)
      {
        return _child->get_request_mode();
      }

      return Gtk::SizeRequestMode::CONSTANT_SIZE;
    }

    void measure_vfunc(Gtk::Orientation orientation,
                       int forSize,
                       int& minimum,
                       int& natural,
                       int& minimumBaseline,
                       int& naturalBaseline) const override
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

    void size_allocate_vfunc(int width, int height, int baseline) override
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

  private:
    Gtk::Widget* _child = nullptr;
    AllocatedCallback _callback;
  };
} // namespace ao::gtk::layout
