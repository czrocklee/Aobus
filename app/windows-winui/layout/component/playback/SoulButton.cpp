// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/component/playback/SoulButton.h"

#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "layout/runtime/UiSubscription.h"
#include "pch.h"
#include "playback/AobusSoulControl.h"
#include "playback/AudioPipelineToolTip.h"
#include "playback/SoulTransportButton.h"
#include <ao/Error.h>
#include <ao/uimodel/layout/action/LayoutActionSlot.h>
#include <ao/uimodel/layout/action/LayoutActionSlotResolution.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/component/SharedLayoutComponentType.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.UI.h>

#include <format>
#include <memory>

namespace ao::winui::layout
{
  namespace
  {
    using winrt::Microsoft::UI::Xaml::FrameworkElement;
    using winrt::Microsoft::UI::Xaml::HorizontalAlignment;
    using winrt::Microsoft::UI::Xaml::Thickness;
    using winrt::Microsoft::UI::Xaml::VerticalAlignment;
    using winrt::Microsoft::UI::Xaml::Controls::Button;
    using winrt::Microsoft::UI::Xaml::Media::SolidColorBrush;

    /**
     * @brief The soul disc, as a button.
     *
     * The soul is a single visual with two jobs. It always renders what playback
     * is doing, and it runs play/pause when the document has not spent its
     * primary click on a shell action; the component asks the catalog which of
     * those it is rather than inferring it from what the disc draws.
     */
    class SoulButtonComponent final : public LayoutComponent
    {
    public:
      SoulButtonComponent(LayoutBuildContext& ctx, uimodel::LayoutNode const& node, bool const activatesOnClick)
      {
        // The soul draws its own disc edge to edge, so the button contributes
        // nothing but the hit region and the gesture.
        _button.Padding(Thickness{});
        _button.BorderThickness(Thickness{});
        _button.Background(SolidColorBrush{winrt::Windows::UI::Colors::Transparent()});
        _button.HorizontalContentAlignment(HorizontalAlignment::Stretch);
        _button.VerticalContentAlignment(VerticalAlignment::Stretch);
        _button.Content(_soul);

        auto* const soul = winrt::get_self<winrt::Aobus::implementation::AobusSoulControl>(_soul);

        if (auto const strokeWidth = node.propertyOr<double>(uimodel::kStrokeWidthProp, 0.0); strokeWidth > 0.0)
        {
          soul->setBaseStrokeWidth(strokeWidth);
        }

        if (auto const glyphScale = node.propertyOr<double>(uimodel::kGlyphScaleProp, 0.0); glyphScale > 0.0)
        {
          soul->setInnerGlyphScale(glyphScale);
        }

        _transportPtr = std::make_unique<SoulTransportButton>(SoulTransportButtonConfig{
          .button = _button,
          .soul = _soul.as<winrt::Microsoft::UI::Xaml::Controls::ContentControl>(),
          .textCatalog = ctx.textCatalog,
          // The pipeline explanation occupies the tooltip in every shell, so the
          // transport never writes its own.
          .hasComplexTooltip = true,
          // Windows draws one inner mark, so it answers whether the soul wears
          // a glyph at all rather than which of the two the vocabulary names.
          .showGlyph = node.propertyOr<bool>("showGlyph", true),
          .activatesOnClick = activatesOnClick,
        });
        _toolTipPtr = std::make_unique<AudioPipelineToolTip>(
          AudioPipelineToolTipConfig{.anchor = _button, .textCatalog = ctx.textCatalog});

        _transportPtr->bind(ctx.playback, ctx.playbackActions);
        _toolTipPtr->bind(ctx.playback);
        applyWindowActivity(ctx.windowActivity);
        _windowActivitySub = subscribeUiUpdate(ctx.windowActivityChanged,
                                               "SoulButtonComponent",
                                               [this](WindowActivityState const state) { applyWindowActivity(state); });
      }

      FrameworkElement element() const override { return _button; }

    private:
      void applyWindowActivity(WindowActivityState const& state)
      {
        winrt::get_self<winrt::Aobus::implementation::AobusSoulControl>(_soul)->setWindowActivity(
          state.visible, state.minimized);
      }

      Button _button{};
      winrt::Aobus::AobusSoulControl _soul{};
      std::unique_ptr<SoulTransportButton> _transportPtr;
      std::unique_ptr<AudioPipelineToolTip> _toolTipPtr;
      async::Subscription _windowActivitySub;
    };
  } // namespace

  Result<std::unique_ptr<LayoutComponent>> makeSoulButton(LayoutBuildContext& ctx, uimodel::LayoutNode const& node)
  {
    auto const optDescriptor = ctx.catalog.descriptor(node.type);

    if (!optDescriptor)
    {
      return makeError(
        Error::Code::NotSupported, std::format("Node '{}' has no descriptor for '{}'", node.id, node.type));
    }

    // A bound primary click belongs to the shell action, and the soul must not
    // answer the same gesture twice.
    auto const activatesOnClick =
      !uimodel::isActionSlotBound(optDescriptor->actionPolicy, node, uimodel::LayoutActionSlot::PrimaryClick);
    return std::make_unique<SoulButtonComponent>(ctx, node, activatesOnClick);
  }
} // namespace ao::winui::layout
