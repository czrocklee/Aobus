// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "pch.h"
#include "platform/StringResources.h"
#include <ao/Error.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>
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
     * @brief The words @p node asks to show.
     *
     * The shared `text` property is those words verbatim, so a document setting
     * it reads the same in every shell. `textResourceKey` is this shell's own
     * way to name a localized string instead; it wins when present, and falls
     * back to `text` when the dictionary does not define it, which keeps a
     * localization gap visible rather than blank.
     *
     * The document stays structurally valid either way, so this must not reject
     * the candidate.
     */
    std::string resolvedText(uimodel::LayoutNode const& node)
    {
      // Not const: the trailing return moves out of it.
      auto text = node.propertyOr<std::string>(uimodel::kTextProp, {});

      if (auto const key = node.propertyOr<std::string>("textResourceKey", {}); !key.empty())
      {
        auto resolved = resourceString(key);
        return resolved == key && !text.empty() ? text : resolved;
      }

      return text;
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

  void registerGenericComponents(ComponentRegistry& registry, MenuComposer menus)
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
      [menus = std::move(menus)](
        LayoutBuildContext& /*ctx*/, uimodel::LayoutNode const& node) -> Result<std::unique_ptr<LayoutComponent>>
      {
        auto const menuId = node.propertyOr<std::string>("menuId", "modernOverflow");
        auto flyout = menus.flyout ? menus.flyout(menuId) : MenuFlyout{nullptr};

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
