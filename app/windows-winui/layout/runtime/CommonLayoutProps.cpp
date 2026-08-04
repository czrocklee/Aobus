// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/CommonLayoutProps.h"

#include "layout/runtime/LayoutComponent.h"
#include "pch.h"
#include <ao/Error.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/winui/layout/ElementKind.h>
#include <ao/winui/layout/PlacementPlan.h>
#include <ao/winui/layout/StyleLookup.h>
#include <ao/winui/layout/ThemeSurface.h>

#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.h>

#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace ao::winui::layout
{
  namespace
  {
    using winrt::Microsoft::UI::Xaml::FrameworkElement;
    using winrt::Microsoft::UI::Xaml::GridLength;
    using winrt::Microsoft::UI::Xaml::GridUnitType;
    using winrt::Microsoft::UI::Xaml::ResourceDictionary;
    using winrt::Microsoft::UI::Xaml::Visibility;

    winrt::Microsoft::UI::Xaml::HorizontalAlignment toNative(HorizontalAlignment const alignment) noexcept
    {
      switch (alignment)
      {
        case HorizontalAlignment::Left: return winrt::Microsoft::UI::Xaml::HorizontalAlignment::Left;
        case HorizontalAlignment::Center: return winrt::Microsoft::UI::Xaml::HorizontalAlignment::Center;
        case HorizontalAlignment::Right: return winrt::Microsoft::UI::Xaml::HorizontalAlignment::Right;
        case HorizontalAlignment::Stretch: return winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch;
      }

      return winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch;
    }

    winrt::Microsoft::UI::Xaml::VerticalAlignment toNative(VerticalAlignment const alignment) noexcept
    {
      switch (alignment)
      {
        case VerticalAlignment::Top: return winrt::Microsoft::UI::Xaml::VerticalAlignment::Top;
        case VerticalAlignment::Center: return winrt::Microsoft::UI::Xaml::VerticalAlignment::Center;
        case VerticalAlignment::Bottom: return winrt::Microsoft::UI::Xaml::VerticalAlignment::Bottom;
        case VerticalAlignment::Stretch: return winrt::Microsoft::UI::Xaml::VerticalAlignment::Stretch;
      }

      return winrt::Microsoft::UI::Xaml::VerticalAlignment::Stretch;
    }

    /// The XAML type name a Style declares, reduced to its unqualified spelling.
    std::string_view unqualifiedTypeName(std::string_view const name) noexcept
    {
      auto const separator = name.rfind('.');
      return separator == std::string_view::npos ? name : name.substr(separator + 1);
    }

    std::optional<ElementKind> styleTargetKind(winrt::Microsoft::UI::Xaml::Style const& style)
    {
      auto const targetName = winrt::to_string(style.TargetType().Name);
      return elementKindFromString(unqualifiedTypeName(targetName));
    }

    StyleScope scopeOf(ResourceDictionary const& resources, winrt::hstring const& key)
    {
      if (resources && resources.HasKey(winrt::box_value(key)))
      {
        return StyleScope::RootGridResources;
      }

      auto const application = winrt::Microsoft::UI::Xaml::Application::Current();

      if (application && application.Resources() && application.Resources().HasKey(winrt::box_value(key)))
      {
        return StyleScope::ApplicationResources;
      }

      return StyleScope::Unresolved;
    }

    Result<> applyStyle(FrameworkElement const& element,
                        StyleLookupPlan const& plan,
                        ResourceDictionary const& resources)
    {
      auto const key = winrt::to_hstring(plan.key);
      auto const scope = scopeOf(resources, key);
      auto optStyle = std::optional<winrt::Microsoft::UI::Xaml::Style>{};
      auto optTarget = std::optional<ElementKind>{};

      if (scope == StyleScope::RootGridResources)
      {
        optStyle = resources.Lookup(winrt::box_value(key)).try_as<winrt::Microsoft::UI::Xaml::Style>();

        if (optStyle)
        {
          optTarget = styleTargetKind(*optStyle);
        }
      }

      switch (resolveStyle(plan, scope, optTarget))
      {
        case StyleResolution::Applied: element.Style(*optStyle); return {};
        case StyleResolution::MissingKey:
          return makeError(Error::Code::NotFound,
                           std::format("styleKey '{}' does not name a Style in the window resources", plan.key));
        case StyleResolution::IncompatibleTarget:
          return makeError(Error::Code::FormatRejected,
                           std::format("styleKey '{}' targets '{}', which does not accept a {}",
                                       plan.key,
                                       optTarget ? toString(*optTarget) : std::string_view{"<unknown>"},
                                       toString(plan.elementKind)));
        case StyleResolution::NoStyleAuthored: return {};
      }

      return {};
    }

    /**
     * @brief Whether @p element really is the @p kind its catalog entry declares.
     *
     * The declared kind decides which styles a document may name and whether a
     * themed surface can land, so a component that builds something else makes
     * both decisions on false evidence. Checking it here keeps the catalog's
     * native-element column enforced rather than merely documented.
     */
    bool elementIsKind(FrameworkElement const& element, ElementKind const kind)
    {
      using namespace winrt::Microsoft::UI::Xaml;

      switch (kind)
      {
        case ElementKind::FrameworkElement: return true;
        case ElementKind::Panel: return element.try_as<Controls::Panel>() != nullptr;
        case ElementKind::Grid: return element.try_as<Controls::Grid>() != nullptr;
        case ElementKind::Border: return element.try_as<Controls::Border>() != nullptr;
        case ElementKind::TextBlock: return element.try_as<Controls::TextBlock>() != nullptr;
        case ElementKind::Control: return element.try_as<Controls::Control>() != nullptr;
        case ElementKind::ContentControl: return element.try_as<Controls::ContentControl>() != nullptr;
        case ElementKind::ButtonBase: return element.try_as<Controls::Primitives::ButtonBase>() != nullptr;
        case ElementKind::Button: return element.try_as<Controls::Button>() != nullptr;
        case ElementKind::ItemsControl: return element.try_as<Controls::ItemsControl>() != nullptr;
        case ElementKind::ListView: return element.try_as<Controls::ListView>() != nullptr;
        case ElementKind::ScrollViewer: return element.try_as<Controls::ScrollViewer>() != nullptr;
        case ElementKind::Slider: return element.try_as<Controls::Slider>() != nullptr;
        case ElementKind::AutoSuggestBox: return element.try_as<Controls::AutoSuggestBox>() != nullptr;
        case ElementKind::NavigationView: return element.try_as<Controls::NavigationView>() != nullptr;
        case ElementKind::TreeView: return element.try_as<Controls::TreeView>() != nullptr;
        case ElementKind::MenuBar: return element.try_as<Controls::MenuBar>() != nullptr;
      }

      return false;
    }

    /// Paint @p brush behind @p element, whichever of the three background owners it is.
    void applyBackground(FrameworkElement const& element, winrt::Microsoft::UI::Xaml::Media::Brush const& brush)
    {
      if (auto const panel = element.try_as<winrt::Microsoft::UI::Xaml::Controls::Panel>(); panel)
      {
        panel.Background(brush);
        return;
      }

      if (auto const border = element.try_as<winrt::Microsoft::UI::Xaml::Controls::Border>(); border)
      {
        border.Background(brush);
        return;
      }

      if (auto const control = element.try_as<winrt::Microsoft::UI::Xaml::Controls::Control>(); control)
      {
        control.Background(brush);
      }
    }

    GridLength slotLength(SlotSizing const sizing) noexcept
    {
      return sizing == SlotSizing::Star ? GridLength{.Value = 1.0, .GridUnitType = GridUnitType::Star}
                                        : GridLength{.Value = 0.0, .GridUnitType = GridUnitType::Auto};
    }

    /// What a slot measures while the element in it is collapsed: nothing at all.
    constexpr auto kCollapsedSlot = GridLength{.Value = 0.0, .GridUnitType = GridUnitType::Pixel};

    /**
     * @brief Run @p apply whenever @p element becomes visible or collapsed.
     *
     * A proportional slot keeps its share even when the element in it is
     * collapsed, so a component that hides itself would leave a hole rather
     * than give the space back. The container therefore follows its children's
     * visibility instead of the children reaching up to re-place themselves:
     * only the parent knows which slot a child was given.
     *
     * The registration lives as long as the element, which the generation owns
     * together with the definition the handler holds, so both die together.
     */
    void followVisibility(FrameworkElement const& element, std::function<void(bool)> apply)
    {
      apply(element.Visibility() == Visibility::Visible);
      element.RegisterPropertyChangedCallback(
        winrt::Microsoft::UI::Xaml::UIElement::VisibilityProperty(),
        [apply = std::move(apply)](winrt::Microsoft::UI::Xaml::DependencyObject const& sender,
                                   winrt::Microsoft::UI::Xaml::DependencyProperty const&)
        {
          if (auto const changed = sender.try_as<FrameworkElement>(); changed)
          {
            apply(changed.Visibility() == Visibility::Visible);
          }
        });
    }
  } // namespace

  Result<> applyCommonProps(FrameworkElement const& element,
                            uimodel::LayoutNode const& node,
                            PlacementPlan const& plan,
                            ElementKind const kind,
                            ResourceDictionary const& resources,
                            SurfaceBrushResolver const& surfaceBrush)
  {
    if (!elementIsKind(element, kind))
    {
      return makeError(Error::Code::FormatRejected,
                       std::format("Component '{}' constructs an element that is not the {} its catalog entry declares",
                                   node.type,
                                   toString(kind)));
    }

    if (auto const optPlan = planStyleLookup(node, kind); optPlan)
    {
      if (auto applied = applyStyle(element, *optPlan, resources); !applied)
      {
        return applied;
      }
    }

    if (auto const optSurface = planThemeSurface(node); optSurface && surfaceBrush)
    {
      if (auto const brush = surfaceBrush(*optSurface); brush)
      {
        applyBackground(element, brush);
      }
    }

    if (plan.optHorizontalAlignment)
    {
      element.HorizontalAlignment(toNative(*plan.optHorizontalAlignment));
    }

    if (plan.optVerticalAlignment)
    {
      element.VerticalAlignment(toNative(*plan.optVerticalAlignment));
    }

    if (plan.optMinWidth)
    {
      element.MinWidth(*plan.optMinWidth);
    }

    if (plan.optMinHeight)
    {
      element.MinHeight(*plan.optMinHeight);
    }

    if (!plan.authoredVisible)
    {
      element.Visibility(Visibility::Collapsed);
    }

    return {};
  }

  void placeChildrenInGrid(winrt::Microsoft::UI::Xaml::Controls::Grid const& grid,
                           std::span<PlacedChild const> const children,
                           bool const vertical)
  {
    using winrt::Microsoft::UI::Xaml::Controls::ColumnDefinition;
    using winrt::Microsoft::UI::Xaml::Controls::Grid;
    using winrt::Microsoft::UI::Xaml::Controls::RowDefinition;

    std::int32_t index = 0;

    for (auto const& child : children)
    {
      auto const element = child.componentPtr->element();

      if (vertical)
      {
        auto definition = RowDefinition{};
        auto const authored = slotLength(child.placement.verticalSlot);
        definition.Height(authored);
        grid.RowDefinitions().Append(definition);
        Grid::SetRow(element, index);
        followVisibility(element,
                         [definition, authored](bool const visible)
                         { definition.Height(visible ? authored : kCollapsedSlot); });
      }
      else
      {
        auto definition = ColumnDefinition{};
        auto const authored = slotLength(child.placement.horizontalSlot);
        definition.Width(authored);
        grid.ColumnDefinitions().Append(definition);
        Grid::SetColumn(element, index);
        followVisibility(element,
                         [definition, authored](bool const visible)
                         { definition.Width(visible ? authored : kCollapsedSlot); });
      }

      grid.Children().Append(element);
      ++index;
    }
  }
} // namespace ao::winui::layout
