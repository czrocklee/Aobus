// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <runtime/CorePrimitives.h>

#include <gtkmm/label.h>

#include <chrono>
#include <optional>
#include <string>

namespace ao::rt
{
  class ViewService;
}

namespace ao::gtk
{
  /**
   * SelectionInfoLabel displays "N items selected" with optional total duration.
   * It self-subscribes to ViewService selection changes.
   */
  class SelectionInfoLabel final
  {
  public:
    explicit SelectionInfoLabel(rt::ViewService& viewService);
    ~SelectionInfoLabel();

    Gtk::Widget& widget() { return _label; }

  private:
    void updateState(std::size_t count, std::optional<std::chrono::milliseconds> totalDuration = std::nullopt);

    rt::ViewService& _viewService;
    Gtk::Label _label;
    rt::Subscription _selectionChangedSub;
  };
} // namespace ao::gtk
