// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "pch.h"
#include "platform/StringResources.h"
#include <ao/Error.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>

#include <format>
#include <memory>
#include <string>

namespace ao::winui::layout
{
  namespace
  {
    using winrt::Microsoft::UI::Xaml::FrameworkElement;
    using winrt::Microsoft::UI::Xaml::Automation::AutomationProperties;
    using winrt::Microsoft::UI::Xaml::Controls::Button;
    using winrt::Microsoft::UI::Xaml::Controls::FontIcon;
    using winrt::Microsoft::UI::Xaml::Controls::MenuFlyout;
    using winrt::Microsoft::UI::Xaml::Controls::TextBlock;
    using winrt::Microsoft::UI::Xaml::Controls::ToolTipService;

    /**
     * @brief The display string a node's `resourceKey` names.
     *
     * A key that no resource defines resolves to the key itself, which keeps a
     * localization gap visible in the shell instead of silently blanking a
     * control. The document is still structurally valid, so it must not reject
     * the candidate.
     */
    std::string resolvedText(uimodel::LayoutNode const& node)
    {
      auto const key = node.propertyOr<std::string>("resourceKey", {});
      return key.empty() ? std::string{} : resourceString(key);
    }

    /// Give @p button a glyph, and describe it by @p text when the glyph carries no words.
    void presentGlyphButton(Button const& button, std::string const& glyph, std::string const& text)
    {
      auto icon = FontIcon{};
      icon.Glyph(winrt::to_hstring(glyph));
      button.Content(icon);

      if (text.empty())
      {
        return;
      }

      auto const description = winrt::box_value(winrt::to_hstring(text));
      ToolTipService::SetToolTip(button, description);
      AutomationProperties::SetName(button, winrt::to_hstring(text));
    }

    /// Static text the shell does not update after construction.
    class LabelComponent final : public LayoutComponent
    {
    public:
      explicit LabelComponent(std::string const& text) { _text.Text(winrt::to_hstring(text)); }

      FrameworkElement element() const override { return _text; }

    private:
      TextBlock _text{};
    };

    /// A button whose whole behavior is the action its slots resolve to.
    class ActionButtonComponent final : public LayoutComponent
    {
    public:
      ActionButtonComponent(std::string const& glyph, std::string const& text)
      {
        if (glyph.empty())
        {
          _button.Content(winrt::box_value(winrt::to_hstring(text)));
        }
        else
        {
          presentGlyphButton(_button, glyph, text);
        }
      }

      FrameworkElement element() const override { return _button; }

    private:
      Button _button{};
    };

    /// A button that presents one of the shell's named menus.
    class MenuButtonComponent final : public LayoutComponent
    {
    public:
      MenuButtonComponent(std::string const& glyph, MenuFlyout const& flyout, std::string const& text)
      {
        presentGlyphButton(_button, glyph, text);
        _button.Flyout(flyout);
      }

      FrameworkElement element() const override { return _button; }

    private:
      Button _button{};
    };
  } // namespace

  void registerGenericComponents(ComponentRegistry& registry)
  {
    registry.registerComponent(
      "label",
      [](LayoutBuildContext& /*ctx*/, uimodel::LayoutNode const& node) -> Result<std::unique_ptr<LayoutComponent>>
      { return std::make_unique<LabelComponent>(resolvedText(node)); });

    registry.registerComponent(
      "actionButton",
      [](LayoutBuildContext& /*ctx*/, uimodel::LayoutNode const& node) -> Result<std::unique_ptr<LayoutComponent>>
      {
        return std::make_unique<ActionButtonComponent>(node.propertyOr<std::string>("glyph", {}), resolvedText(node));
      });

    registry.registerComponent(
      "menuButton",
      [](LayoutBuildContext& ctx, uimodel::LayoutNode const& node) -> Result<std::unique_ptr<LayoutComponent>>
      {
        auto const menuId = node.propertyOr<std::string>("menuId", "modernOverflow");
        auto flyout = ctx.menus.flyout ? ctx.menus.flyout(menuId) : MenuFlyout{nullptr};

        if (!flyout)
        {
          return makeError(
            Error::Code::NotFound,
            std::format("Node '{}' presents the menu '{}', which the shell does not offer", node.id, menuId));
        }

        return std::make_unique<MenuButtonComponent>(
          node.propertyOr<std::string>("glyph", {}), flyout, resolvedText(node));
      });
  }
} // namespace ao::winui::layout
