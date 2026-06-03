// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "PlaybackComponentRegistrations.h"
#include "app/AobusSoul.h"
#include "layout/document/LayoutNode.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include "playback/TransportButton.h"
#include <ao/uimodel/layout/ActionTypes.h>
#include <ao/uimodel/layout/ComponentActionPolicy.h>
#include <ao/uimodel/playback/AobusSoulViewModel.h>
#include <ao/uimodel/playback/TransportViewModel.h>

#include <gtkmm/button.h>
#include <gtkmm/enums.h>
#include <gtkmm/widget.h>

#include <memory>

namespace ao::gtk::layout
{
  namespace
  {
    using uimodel::layout::ActionSlot;
    using uimodel::layout::ComponentActionPolicy;
    using uimodel::layout::slotBit;

    constexpr double kDefaultStrokeWidth = 9.0;

    /**
     * @brief playback.soulPlayPauseButton
     */
    class SoulTransportButtonComponent final : public ILayoutComponent
    {
    public:
      SoulTransportButtonComponent(LayoutContext& ctx, LayoutNode const& node)
        : _transportController{ctx.runtime.playback(),
                               ctx.playback.queueModel,
                               TransportButton::Action::PlayPause,
                               getTransportCallback(ctx, TransportButton::Action::PlayPause),
                               false,
                               [this](uimodel::playback::TransportViewState const& state)
                               { applyTransportState(state); }}
        , _soulController{ctx.runtime.playback(),
                          [this](uimodel::playback::AobusSoulViewState const& state) { applySoulState(state); }}
        , _hasComplexTooltip{static_cast<bool>(node.optTooltip)}
      {
        _button.set_child(_soul);
        _button.set_has_frame(false);
        _button.add_css_class("ao-soul-button");
        _button.set_valign(Gtk::Align::CENTER);
        _button.set_halign(Gtk::Align::CENTER);

        _soul.set_halign(Gtk::Align::FILL);
        _soul.set_valign(Gtk::Align::FILL);

        if (auto const strokeWidth = node.getProp<double>("strokeWidth", 0.0); strokeWidth > 0.0)
        {
          _soul.setBaseStrokeWidth(static_cast<float>(strokeWidth));
        }

        if (auto const glyphScale = node.getProp<double>("glyphScale", 0.0); glyphScale > 0.0)
        {
          _soul.setInnerGlyphScale(static_cast<float>(glyphScale));
        }

        _button.signal_clicked().connect([this] { _transportController.handleClick(); });
      }

      Gtk::Widget& widget() override { return _button; }

    private:
      void applyTransportState(uimodel::playback::TransportViewState const& view)
      {
        using Icon = uimodel::playback::TransportIcon;

        if (view.icon == Icon::Pause)
        {
          _soul.setInnerGlyph(AobusSoul::InnerGlyph::Seal);
        }
        else if (view.icon == Icon::Play)
        {
          _soul.setInnerGlyph(AobusSoul::InnerGlyph::Sigil);
        }
        else
        {
          _soul.setInnerGlyph(AobusSoul::InnerGlyph::None);
        }

        _button.set_sensitive(view.enabled);

        if (!_hasComplexTooltip)
        {
          _button.set_tooltip_text(view.tooltip);
        }
      }

      void applySoulState(uimodel::playback::AobusSoulViewState const& view)
      {
        _soul.breathe(view.isBreathing);
        _soul.setAura(AobusSoul::mapAuraColor(view.auraColor));
      }

      Gtk::Button _button;
      AobusSoul _soul;
      uimodel::playback::TransportViewModel _transportController;
      uimodel::playback::AobusSoulViewModel _soulController;
      bool _hasComplexTooltip = false;
    };

    std::unique_ptr<ILayoutComponent> createSoulPlayPauseButton(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<SoulTransportButtonComponent>(ctx, node);
    }
  } // namespace

  void registerSoulTransportButtonComponent(ComponentRegistry& registry)
  {
    registry.registerComponent(
      {.type = "playback.soulPlayPauseButton",
       .displayName = "Soul Play/Pause Button",
       .category = "Playback",
       .container = false,
       .props = {{.name = "strokeWidth",
                  .kind = PropertyKind::Double,
                  .label = "Stroke Width",
                  .defaultValue = LayoutValue{kDefaultStrokeWidth}},
                 {.name = "glyphScale",
                  .kind = PropertyKind::Double,
                  .label = "Glyph Scale",
                  .defaultValue = LayoutValue{1.0}}},
       .layoutProps = {},
       .minChildren = 0,
       .optMaxChildren = 0,
       .actionPolicy = ComponentActionPolicy{.slotMask = slotBit(ActionSlot::SecondaryClick) |
                                                         slotBit(ActionSlot::SecondaryLongPress),
                                             .defaultActionIds = {{ActionSlot::SecondaryLongPress, "shell.showSoul"}}}},
      createSoulPlayPauseButton);
  }
} // namespace ao::gtk::layout
