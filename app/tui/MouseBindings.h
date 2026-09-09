// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>

namespace ao::tui
{
  inline constexpr auto kEmptyMouseBox = ftxui::Box{.x_max = -1, .y_max = -1};

  bool isLeftPress(ftxui::Mouse const& mouse);
  std::int32_t mouseWheelDirection(ftxui::Mouse const& mouse);
  bool containsMouse(ftxui::Box const& box, ftxui::Mouse const& mouse);
  std::optional<std::size_t> mouseRowAt(std::span<ftxui::Box const> rows, ftxui::Mouse const& mouse);
  /// Records the unscrolled layout origin, for cell-to-cursor mapping and scroll bounds.
  /// Storage must outlive the rendered node and remain at a stable address.
  ftxui::Decorator reflectLayout(ftxui::Box& box);

  /// Visible shortcuts route clicks through their owner's existing key protocol.
  class MouseBindings final
  {
  public:
    void clear();
    ftxui::Element bind(ftxui::Element elementPtr, ftxui::Event event);
    std::optional<ftxui::Event> eventAt(ftxui::Mouse const& mouse) const;

  private:
    struct Binding final
    {
      ftxui::Event event;
      ftxui::Box box = kEmptyMouseBox;
    };

    std::deque<Binding> _bindings;
  };
} // namespace ao::tui
