// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "MouseBindings.h"

#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>

namespace ao::tui
{
  namespace
  {
    class LayoutRegionNode final : public ftxui::Node
    {
    public:
      LayoutRegionNode(ftxui::Element elementPtr, ftxui::Box& target)
        : Node{{std::move(elementPtr)}}, _target{target}
      {
        _target = kEmptyMouseBox;
      }

      void ComputeRequirement() override
      {
        Node::ComputeRequirement();
        requirement_ = children_.front()->requirement();
      }

      void SetBox(ftxui::Box box) override
      {
        _target = box;
        Node::SetBox(box);
        children_.front()->SetBox(box);
      }

    private:
      ftxui::Box& _target;
    };
  } // namespace

  bool isLeftPress(ftxui::Mouse const& mouse)
  {
    return mouse.button == ftxui::Mouse::Left && mouse.motion == ftxui::Mouse::Pressed;
  }

  std::int32_t mouseWheelDirection(ftxui::Mouse const& mouse)
  {
    if (mouse.motion != ftxui::Mouse::Pressed)
    {
      return 0;
    }

    if (mouse.button == ftxui::Mouse::WheelUp)
    {
      return -1;
    }

    return mouse.button == ftxui::Mouse::WheelDown ? 1 : 0;
  }

  bool containsMouse(ftxui::Box const& box, ftxui::Mouse const& mouse)
  {
    return !box.IsEmpty() && box.Contain(mouse.x, mouse.y);
  }

  std::optional<std::size_t> mouseRowAt(std::span<ftxui::Box const> rows, ftxui::Mouse const& mouse)
  {
    for (std::size_t index = 0; index < rows.size(); ++index)
    {
      if (containsMouse(rows[index], mouse))
      {
        return index;
      }
    }

    return std::nullopt;
  }

  ftxui::Decorator reflectLayout(ftxui::Box& box)
  {
    return [&box](ftxui::Element elementPtr) { return std::make_shared<LayoutRegionNode>(std::move(elementPtr), box); };
  }

  ftxui::Element mousePanel(ftxui::Element panelPtr, PanelMouseRegions& regions, std::int32_t const scrollRow)
  {
    using namespace ftxui;

    if (scrollRow >= 0)
    {
      // Let spare virtual rows belong to the filler, keeping the bottom border inside the viewport.
      panelPtr = vbox({std::move(panelPtr) | reflectLayout(regions.contentBox) | notflex | yflex_shrink, filler()}) |
                 notflex | yflex_shrink | focusPosition(0, scrollRow) | yframe;
    }

    return std::move(panelPtr) | ftxui::reflect(regions.box);
  }

  void MouseBindings::clear()
  {
    _bindings.clear();
  }

  ftxui::Element MouseBindings::bind(ftxui::Element elementPtr, ftxui::Event event)
  {
    _bindings.push_back(Binding{.event = std::move(event)});
    return std::move(elementPtr) | ftxui::reflect(_bindings.back().box);
  }

  std::optional<ftxui::Event> MouseBindings::eventAt(ftxui::Mouse const& mouse) const
  {
    if (isLeftPress(mouse))
    {
      for (auto const& binding : _bindings)
      {
        if (containsMouse(binding.box, mouse))
        {
          return binding.event;
        }
      }
    }

    return std::nullopt;
  }
} // namespace ao::tui
