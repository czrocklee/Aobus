// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "tui/MouseBindings.h"

#include "test/unit/tui/RenderTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace ao::tui::test
{
  TEST_CASE("MouseBindings - only painted cells activate shortcuts", "[tui][unit][mouse]")
  {
    auto bindings = MouseBindings{};
    auto elementPtr = bindings.bind(ftxui::text("Run"), ftxui::Event::Return);
    auto mouse = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 0, .y = 0};
    CHECK_FALSE(bindings.eventAt(mouse));
    auto const rendered = renderElement(std::move(elementPtr), 3, 1);
    CHECK(rendered.text == "Run");
    CHECK(bindings.eventAt(mouse) == ftxui::Event::Return);
    mouse.motion = ftxui::Mouse::Released;
    CHECK_FALSE(bindings.eventAt(mouse));
    mouse.motion = ftxui::Mouse::Moved;
    CHECK_FALSE(bindings.eventAt(mouse));
    mouse.motion = ftxui::Mouse::Pressed;
    mouse.button = ftxui::Mouse::Right;
    CHECK_FALSE(bindings.eventAt(mouse));
    bindings.clear();
    mouse.button = ftxui::Mouse::Left;
    CHECK_FALSE(bindings.eventAt(mouse));
  }

  TEST_CASE("MouseBindings - scroll frames retire off-screen row targets", "[tui][regression][mouse][render]")
  {
    using namespace ftxui;
    auto boxes = std::array<Box, 20>{};
    auto rows = Elements{};

    for (std::size_t index = 0; index < boxes.size(); ++index)
    {
      rows.push_back(text(std::to_string(index)) | ftxui::reflect(boxes[index]));
    }

    auto const rendered = renderElement(vbox(std::move(rows)) | focusPosition(0, 19) | yframe, 10, 3);
    CHECK(rendered.text.contains("19"));
    CHECK(boxes.front().IsEmpty());
    CHECK(std::ranges::count_if(boxes, [](Box const& box) { return !box.IsEmpty(); }) == 3);

    for (auto const& box : boxes)
    {
      if (!box.IsEmpty())
      {
        CHECK(box.y_min >= 0);
        CHECK(box.y_max < 3);
      }
    }
  }

  TEST_CASE("MouseBindings - fitting scroll panel keeps all four border corners", "[tui][regression][mouse][render]")
  {
    using namespace ftxui;
    auto regions = PanelMouseRegions{};
    auto const rendered = renderElement(mousePanel(text("Body") | border, regions, 0), 20, 3);
    CHECK(rendered.text.contains("╭"));
    CHECK(rendered.text.contains("╮"));
    CHECK(rendered.text.contains("╰"));
    CHECK(rendered.text.contains("╯"));
    CHECK(regions.contentBox.y_min == regions.box.y_min);
    CHECK(regions.contentBox.y_max == regions.box.y_max);
  }

  TEST_CASE("MouseBindings - scrolling panel keeps its hit region in the visible viewport",
            "[tui][unit][mouse][render]")
  {
    using namespace ftxui;
    auto regions = PanelMouseRegions{};
    auto rows = Elements{};

    for (std::int32_t index = 0; index < 30; ++index)
    {
      rows.push_back(text(std::to_string(index)));
    }

    auto const rendered = renderElement(mousePanel(vbox(std::move(rows)) | border, regions, 29), 20, 5);
    CHECK(rendered.text.contains("29"));
    CHECK_FALSE(rendered.text.contains("×"));
    CHECK(regions.box.y_min == 0);
    CHECK(regions.box.y_max == 4);
    CHECK(regions.contentBox.y_max - regions.contentBox.y_min > 5);
  }
} // namespace ao::tui::test
