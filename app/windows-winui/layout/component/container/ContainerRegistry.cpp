// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/CommonLayoutProps.h"
#include "layout/runtime/ComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "pch.h"
#include <ao/Error.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::winui::layout
{
  namespace
  {
    using winrt::Microsoft::UI::Xaml::FrameworkElement;
    using winrt::Microsoft::UI::Xaml::GridLength;
    using winrt::Microsoft::UI::Xaml::GridUnitType;
    using winrt::Microsoft::UI::Xaml::Controls::ColumnDefinition;
    using winrt::Microsoft::UI::Xaml::Controls::Grid;
    using winrt::Microsoft::UI::Xaml::Controls::RowDefinition;

    constexpr double kMinimumSplitWeight = 0.05;
    constexpr double kMaximumSplitWeight = 0.95;
    constexpr double kDefaultSplitWeight = 0.5;

    bool isVertical(uimodel::LayoutNode const& node, std::string_view const fallback)
    {
      auto const it = node.props.find("orientation");
      auto const* const value = it == node.props.end() ? nullptr : it->second.getIf<std::string>();
      return (value == nullptr ? fallback : std::string_view{*value}) == "vertical";
    }

    /// A numeric property, accepting either YAML spelling the catalog admits.
    double numberProp(uimodel::LayoutNode const& node, std::string_view const name, double const fallback)
    {
      auto const it = node.props.find(name);
      return it == node.props.end() || !it->second.isNumber() ? fallback : it->second.asDouble();
    }

    GridLength starLength(double const weight) noexcept
    {
      return {.Value = weight, .GridUnitType = GridUnitType::Star};
    }

    /**
     * @brief A structural container that gives each child its own grid slot.
     *
     * WinUI allocates remaining space on a row or column definition, so `box`
     * builds a single-axis Grid rather than a stacking panel: that is the only
     * way an authored `hexpand` or `vexpand` can mean what it means elsewhere.
     */
    class BoxComponent final : public LayoutContainer
    {
    public:
      BoxComponent(bool const vertical, double const spacing)
        : _vertical{vertical}
      {
        if (_vertical)
        {
          _grid.RowSpacing(spacing);
        }
        else
        {
          _grid.ColumnSpacing(spacing);
        }
      }

      FrameworkElement element() const override { return _grid; }

      void adopt(std::vector<PlacedChild> children) override
      {
        placeChildrenInGrid(_grid, children, _vertical);
        _children = std::move(children);
      }

    private:
      Grid _grid{};
      bool _vertical = true;
      std::vector<PlacedChild> _children;
    };

    /**
     * @brief Two proportional regions along one axis.
     *
     * The divider is authored, not draggable: Windows pane boundaries that the
     * user can drag belong to the navigation and inspector components, which
     * own their persisted widths in `DesktopSettings`. A generic split
     * therefore persists nothing.
     */
    class SplitComponent final : public LayoutContainer
    {
    public:
      SplitComponent(bool const vertical, double const positionPercent)
        : _vertical{vertical}, _leadingWeight{std::clamp(positionPercent, kMinimumSplitWeight, kMaximumSplitWeight)}
      {
      }

      FrameworkElement element() const override { return _grid; }

      void adopt(std::vector<PlacedChild> children) override
      {
        std::int32_t index = 0;

        for (auto const& child : children)
        {
          auto const weight = index == 0 ? _leadingWeight : 1.0 - _leadingWeight;
          auto const childElement = child.componentPtr->element();

          if (_vertical)
          {
            auto definition = RowDefinition{};
            definition.Height(starLength(weight));
            _grid.RowDefinitions().Append(definition);
            Grid::SetRow(childElement, index);
          }
          else
          {
            auto definition = ColumnDefinition{};
            definition.Width(starLength(weight));
            _grid.ColumnDefinitions().Append(definition);
            Grid::SetColumn(childElement, index);
          }

          _grid.Children().Append(childElement);
          ++index;
        }

        _children = std::move(children);
      }

    private:
      Grid _grid{};
      bool _vertical = false;
      double _leadingWeight = kDefaultSplitWeight;
      std::vector<PlacedChild> _children;
    };

    /// A bare grid region the window frame styles, used by the title and status bars.
    class ChromeBarComponent final : public LayoutContainer
    {
    public:
      FrameworkElement element() const override { return _grid; }

      void adopt(std::vector<PlacedChild> children) override
      {
        placeChildrenInGrid(_grid, children, false);
        _children = std::move(children);
      }

    private:
      Grid _grid{};
      std::vector<PlacedChild> _children;
    };
  } // namespace

  void registerContainerComponents(ComponentRegistry& registry)
  {
    registry.registerComponent(
      "box",
      [](LayoutBuildContext& /*ctx*/, uimodel::LayoutNode const& node) -> Result<std::unique_ptr<LayoutComponent>>
      { return std::make_unique<BoxComponent>(isVertical(node, "vertical"), numberProp(node, "spacing", 0.0)); });

    registry.registerComponent(
      "split",
      [](LayoutBuildContext& /*ctx*/, uimodel::LayoutNode const& node) -> Result<std::unique_ptr<LayoutComponent>>
      {
        return std::make_unique<SplitComponent>(
          isVertical(node, "horizontal"), numberProp(node, "initialPositionPercent", kDefaultSplitWeight));
      });

    registry.registerComponent(
      "windows.titleBar",
      [](LayoutBuildContext& ctx, uimodel::LayoutNode const& /*node*/) -> Result<std::unique_ptr<LayoutComponent>>
      {
        auto componentPtr = std::make_unique<ChromeBarComponent>();
        // The system drag region is the window's to hand over, not the
        // component's to claim, so the bar only records which element it is.
        ctx.titleBarSlot = componentPtr->element();
        return componentPtr;
      });

    registry.registerComponent(
      "windows.statusBar",
      [](LayoutBuildContext& /*ctx*/, uimodel::LayoutNode const& /*node*/) -> Result<std::unique_ptr<LayoutComponent>>
      { return std::make_unique<ChromeBarComponent>(); });
  }
} // namespace ao::winui::layout
