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
    ~AllocationObserver() override;

    AllocationObserver(AllocationObserver const&) = delete;
    AllocationObserver& operator=(AllocationObserver const&) = delete;
    AllocationObserver(AllocationObserver&&) = delete;
    AllocationObserver& operator=(AllocationObserver&&) = delete;

    void setChild(Gtk::Widget& child);
    void clearChild();

    void setAllocatedCallback(AllocatedCallback callback) { _callback = std::move(callback); }

  protected:
    Gtk::SizeRequestMode get_request_mode_vfunc() const override;

    void measure_vfunc(Gtk::Orientation orientation,
                       int forSize,
                       int& minimum,
                       int& natural,
                       int& minimumBaseline,
                       int& naturalBaseline) const override;

    void size_allocate_vfunc(int width, int height, int baseline) override;

  private:
    Gtk::Widget* _child = nullptr;
    AllocatedCallback _callback;
  };
} // namespace ao::gtk::layout
