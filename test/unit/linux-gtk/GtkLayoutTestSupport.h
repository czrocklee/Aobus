// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <gtkmm/enums.h>
#include <gtkmm/widget.h>

#include <cstdint>

namespace ao::gtk::test
{
  class AllocationHost final : public Gtk::Widget
  {
  public:
    explicit AllocationHost(Gtk::Widget& child);
    ~AllocationHost() override;

    AllocationHost(AllocationHost const&) = delete;
    AllocationHost& operator=(AllocationHost const&) = delete;
    AllocationHost(AllocationHost&&) = delete;
    AllocationHost& operator=(AllocationHost&&) = delete;

    void allocateChild(std::int32_t width, std::int32_t height);

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
    std::int32_t _width = 0;
    std::int32_t _height = 0;
  };

  struct WidgetMeasure final
  {
    std::int32_t minimum = 0;
    std::int32_t natural = 0;
    std::int32_t minimumBaseline = -1;
    std::int32_t naturalBaseline = -1;
  };

  WidgetMeasure measureWidget(Gtk::Widget& widget, Gtk::Orientation orientation, std::int32_t forSize = -1);
} // namespace ao::gtk::test
